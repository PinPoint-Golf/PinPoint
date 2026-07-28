/*
 * Copyright (C) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "source_ring.h"
#include "source_descriptor.h"

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace pinpoint {

static size_t nextPow2(size_t n) {
    if (n == 0) return 1;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

static size_t alignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

SourceRing::SourceRing(SourceId id, size_t slot_count, size_t slot_max_bytes,
                       std::atomic<bool>* capturing)
    : source_id_(id)
    , slot_count_(nextPow2(slot_count))
    , slot_mask_(slot_count_ - 1)
    , slot_max_bytes_(slot_max_bytes)
    , slot_stride_(alignUp(sizeof(SlotHeader) + slot_max_bytes, 64))
    , capturing_(capturing)
{
    assert((slot_count_ & (slot_count_ - 1)) == 0 && "slot_count_ must be power of 2");

    // On MSVC std::aligned_alloc requires size to be a multiple of alignment.
    size_t total = alignUp(slot_count_ * slot_stride_, 64);

#ifdef _MSC_VER
    std::byte* raw = static_cast<std::byte*>(_aligned_malloc(total, 64));
#else
    std::byte* raw = static_cast<std::byte*>(std::aligned_alloc(64, total));
#endif
    if (!raw) throw std::bad_alloc{};

    storage_.reset(raw);

    // Placement-construct all SlotHeaders to zero-initialise atomics.
    for (size_t i = 0; i < slot_count_; ++i) {
        new (storage_.get() + i * slot_stride_) SlotHeader{};
    }
}

SourceRing::WriteSlot SourceRing::acquireWriteSlot() noexcept {
    if (capturing_ && !capturing_->load(std::memory_order_acquire)) {
        return WriteSlot{};
    }

    // Read write_seq_ before advancing: SPSC, no other producer races.
    uint64_t seq = write_seq_.load(std::memory_order_relaxed);
    size_t   idx = seq & slot_mask_;
    SlotHeader& hdr = slotHeaderAt(idx);

    // Mark slot as in-progress (odd generation) BEFORE advancing write_seq_.
    // Combined with the release store of write_seq_ below, this ensures that any
    // consumer who acquires write_seq_ > seq also sees gen=odd (ARM-safe).
    uint64_t prev_gen = hdr.generation.load(std::memory_order_relaxed);
    hdr.generation.store((prev_gen | 1u), std::memory_order_relaxed);

    if (prev_gen > 0) {
        stats_.events_overwritten.fetch_add(1, std::memory_order_relaxed);
    }

    // Release: all stores above (including gen=odd) are visible to threads that
    // subsequently load write_seq_ with acquire and see the incremented value.
    write_seq_.store(seq + 1, std::memory_order_release);

    // The store-release above is a ONE-WAY barrier: it orders everything before it, and stops
    // nothing after it from moving up. The caller's payload stores land after this returns, so
    // without a second barrier they may become visible BEFORE the gen=odd marker they are supposed
    // to be covered by — and a consumer recycling this slot would then copy half-written bytes
    // while the generation still looks stable, i.e. a torn read that validate() calls clean.
    // On x86-TSO store-store ordering is free and this compiles to nothing; on arm64 it is the
    // difference between the seqlock working and silently not.
    std::atomic_thread_fence(std::memory_order_release);

    return WriteSlot{
        .data          = payloadAt(idx),
        .capacity      = slot_max_bytes_,
        .sequence      = seq,
        .bytes_written = &hdr.bytes_written,
        .timestamp_us  = &hdr.timestamp_us,
        .valid         = true,
    };
}

SourceRing::WriteSlot SourceRing::getSlotByIndex(size_t slot_idx) noexcept {
    if (capturing_ && !capturing_->load(std::memory_order_acquire)) {
        return WriteSlot{};
    }

    uint64_t wseq = write_seq_.load(std::memory_order_relaxed);
    uint64_t seq  = (wseq & ~slot_mask_) | slot_idx;
    if (seq < wseq) seq += slot_count_;

    SlotHeader& hdr = slotHeaderAt(slot_idx);

    // Mark gen=odd BEFORE advancing write_seq_ (same release-acquire reasoning as acquireWriteSlot).
    uint64_t prev_gen = hdr.generation.load(std::memory_order_relaxed);
    hdr.generation.store((prev_gen | 1u), std::memory_order_relaxed);

    if (prev_gen > 0) {
        stats_.events_overwritten.fetch_add(1, std::memory_order_relaxed);
    }

    write_seq_.store(seq + 1, std::memory_order_release);

    // Same one-way-barrier reasoning as acquireWriteSlot(): keep the caller's payload stores from
    // being reordered above the gen=odd marker.
    std::atomic_thread_fence(std::memory_order_release);

    return WriteSlot{
        .data          = payloadAt(slot_idx),
        .capacity      = slot_max_bytes_,
        .sequence      = seq,
        .bytes_written = &hdr.bytes_written,
        .timestamp_us  = &hdr.timestamp_us,
        .valid         = true,
    };
}

void SourceRing::publish(uint64_t sequence) noexcept {
    size_t      idx = sequence & slot_mask_;
    SlotHeader& hdr = slotHeaderAt(idx);

    // Hard clamp — defence in depth.
    if (hdr.bytes_written > static_cast<uint32_t>(slot_max_bytes_)) {
        hdr.bytes_written = static_cast<uint32_t>(slot_max_bytes_);
        stats_.bounds_violations.fetch_add(1, std::memory_order_relaxed);
    }

    stats_.events_written.fetch_add(1, std::memory_order_relaxed);
    stats_.bytes_written_total.fetch_add(hdr.bytes_written, std::memory_order_relaxed);
    stats_.updateInterArrival(hdr.timestamp_us);

    // Release: makes all payload writes visible to consumers checking generation.
    hdr.generation.fetch_add(1, std::memory_order_release);
}

SourceRing::ReadHandle SourceRing::acquireReadHandle(uint64_t sequence) const noexcept {
    size_t            idx = sequence & slot_mask_;
    const SlotHeader& hdr = slotHeaderAt(idx);

    // Load write_seq_ FIRST with acquire.  This synchronises with the release store in
    // acquireWriteSlot(), ensuring the gen=odd store (done before that release) is visible.
    uint64_t write_seq_first = write_seq_.load(std::memory_order_acquire);

    // Sequence not yet acquired — slot still contains data from a previous ring cycle.
    if (write_seq_first <= sequence) {
        return ReadHandle{};
    }

    // Now load gen.  Because write_seq_ acquire synced with acquireWriteSlot's release,
    // gen is at least odd for this cycle.  If it is even, publish() has completed and
    // the acquire here synchronises with publish()'s release, making payload bytes visible.
    uint64_t gen_before = hdr.generation.load(std::memory_order_acquire);

    // Slot is being written — odd generation.
    if (gen_before & 1u) {
        return ReadHandle{};
    }

    // Fresh overrun check: load write_seq_ again *after* gen_before so that the value
    // reflects writes that completed after we first loaded write_seq_.  Without this
    // second load, a slow consumer could observe gen from a later ring cycle while
    // using a stale write_seq_ for the range check.
    uint64_t write_seq_now = write_seq_.load(std::memory_order_acquire);
    if ((write_seq_now - sequence) > slot_count_) {
        return ReadHandle{};
    }

    ReadHandle h;
    h.data                = payloadAt(idx);
    h.bytes               = hdr.bytes_written;
    h.timestamp_us        = hdr.timestamp_us;
    h.generation_snapshot = gen_before;
    h.ring_               = this;
    h.slot_idx_           = idx;
    return h;
}

void SourceRing::ReadHandle::copyBytesRacy(void* dst, size_t len) const noexcept {
    if (!data || !dst || len == 0) return;
    std::memcpy(dst, data, len);
}

bool SourceRing::ReadHandle::validate(const SourceRing& ring) const noexcept {
    if (!data) return false;
    const SlotHeader& hdr = ring.slotHeaderAt(slot_idx_);

    // The read side of the seqlock needs this fence, and an acquire LOAD cannot supply it: acquire
    // stops later accesses from floating up, whereas what has to be pinned here is the payload copy
    // that already happened — copyBytesRacy()'s loads must not sink below the check that is meant to
    // vet them. An acquire FENCE is the barrier with that direction (no prior load may cross it), so
    // the generation is re-read strictly after the bytes were taken. Without it the CPU is free to
    // satisfy the copy late, from a slot the producer has since recycled, and this function then
    // certifies those bytes against a generation that was read before they were fetched.
    std::atomic_thread_fence(std::memory_order_acquire);

    uint64_t gen_after = hdr.generation.load(std::memory_order_relaxed);
    return gen_after == generation_snapshot;
}

uint64_t SourceRing::latestSequence() const noexcept {
    // write_seq_ is the *next* sequence to be acquired, so latest published is write_seq_ - 1.
    uint64_t ws = write_seq_.load(std::memory_order_acquire);
    return ws == 0 ? 0 : ws - 1;
}

bool SourceRing::peekTimestamp(uint64_t sequence, int64_t& out) const noexcept {
    size_t            idx = sequence & slot_mask_;
    const SlotHeader& hdr = slotHeaderAt(idx);

    uint64_t write_seq_first = write_seq_.load(std::memory_order_acquire);
    if (write_seq_first <= sequence) return false;

    uint64_t gen_before = hdr.generation.load(std::memory_order_acquire);
    if (gen_before & 1u) return false;

    uint64_t write_seq_now = write_seq_.load(std::memory_order_acquire);
    if ((write_seq_now - sequence) > slot_count_) return false;

    out = hdr.timestamp_us;

    uint64_t gen_after = hdr.generation.load(std::memory_order_acquire);
    return gen_after == gen_before;
}

void SourceRing::reset() noexcept {
    write_seq_.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < slot_count_; ++i)
        slotHeaderAt(i).generation.store(0, std::memory_order_relaxed);
    stats_.events_written.store(0, std::memory_order_relaxed);
    stats_.events_overwritten.store(0, std::memory_order_relaxed);
    stats_.bytes_written_total.store(0, std::memory_order_relaxed);
    stats_.last_write_timestamp_us.store(0, std::memory_order_relaxed);
    stats_.max_inter_arrival_us.store(0, std::memory_order_relaxed);
    stats_.bounds_violations.store(0, std::memory_order_relaxed);
    stats_.monotonicity_violations.store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

std::vector<std::byte*> SourceRing::getSlotPointers() const {
    std::vector<std::byte*> ptrs;
    ptrs.reserve(slot_count_);
    for (size_t i = 0; i < slot_count_; ++i) {
        ptrs.push_back(storage_.get() + i * slot_stride_ + sizeof(SlotHeader));
    }
    return ptrs;
}

// --- SourceDescriptor helpers ---

size_t SourceDescriptor::computeSlotBytes() const {
    return std::visit([](const auto& f) -> size_t {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, CameraFormat>) {
            return f.max_payload_bytes;
        } else {
            // Align up to next multiple of 8.
            size_t b = f.packet_bytes;
            return (b + 7u) & ~7u;
        }
    }, format.format);
}

size_t SourceDescriptor::computeSlotCount() const {
    double rate = std::visit([](const auto& f) -> double {
        using T = std::decay_t<decltype(f)>;
        if constexpr (std::is_same_v<T, CameraFormat>) {
            if (f.fps_denominator == 0) return 0.0;
            return static_cast<double>(f.fps_numerator) / f.fps_denominator;
        } else {
            return static_cast<double>(f.sample_rate_hz);
        }
    }, format.format);

    double window_s = window_duration.count() / 1000.0;
    size_t n = static_cast<size_t>(std::ceil(rate * window_s));
    if (n == 0) n = 1;

    // Round up to next power of 2.
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

} // namespace pinpoint
