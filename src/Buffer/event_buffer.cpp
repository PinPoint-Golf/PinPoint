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

#include "event_buffer.h"
#include "swing_payload_source.h"
#include "thread_policy.h"
#include "platform.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(PINPOINT_PLATFORM_WINDOWS)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <timeapi.h>
  #pragma comment(lib, "winmm.lib")
#endif

namespace pinpoint {

// ---------------------------------------------------------------------------
// Internal MergerState — not exposed in header
// ---------------------------------------------------------------------------

struct PendingEvent {
    SourceId source_id    = kInvalidSourceId;
    uint64_t source_seq   = 0;
    int64_t  timestamp_us = 0;
    bool     valid        = false;
};

class MergerState {
public:
    static constexpr size_t MAX = EventBuffer::MAX_SOURCES;

    std::array<PendingEvent, MAX> heads{};
    std::array<int64_t, MAX>      last_emitted_ts{};

    MergerState() { last_emitted_ts.fill(INT64_MIN); }

    // Linear scan — faster than a heap for N <= 16 (cache-hot, no allocation).
    int findMinValidIndex() const noexcept {
        int    best    = -1;
        int64_t min_ts = INT64_MAX;
        for (int i = 0; i < static_cast<int>(MAX); ++i) {
            if (heads[i].valid && heads[i].timestamp_us < min_ts) {
                best   = i;
                min_ts = heads[i].timestamp_us;
            }
        }
        return best;
    }

    void setHead(int idx, PendingEvent ev) noexcept { heads[idx] = ev; }
    void clearHead(int idx)               noexcept { heads[idx].valid = false; }

    // Clamps non-monotonic timestamps; records the violation via the ring.
    void enforceMonotonicity(int idx, PendingEvent& ev, SourceRing& ring) noexcept {
        if (ev.timestamp_us <= last_emitted_ts[idx]) {
            ring.recordMonotonicityViolation();
            ev.timestamp_us = last_emitted_ts[idx] + 1;
        }
        last_emitted_ts[idx] = ev.timestamp_us;
    }
};

// ---------------------------------------------------------------------------
// EventBuffer
// ---------------------------------------------------------------------------

EventBuffer::EventBuffer(EventBufferConfig cfg)
    : config_(cfg)
    , index_(cfg.timeline_index_capacity)
{
}

EventBuffer::~EventBuffer() {
    if (state_.load(std::memory_order_acquire) != BufferState::Idle)
        stop();
}

void EventBuffer::setLogCallback(LogCallback cb)
{
    m_logCallback = std::move(cb);
}

void EventBuffer::logMsg(LogSeverity sev, const char *msg) const
{
    if (m_logCallback) {
        m_logCallback(sev, msg);
        return;
    }
    switch (sev) {
    case LogSeverity::Error: fprintf(stderr, "ERROR: %s\n",   msg); break;
    case LogSeverity::Warn:  fprintf(stderr, "WARNING: %s\n", msg); break;
    default:                 fprintf(stderr, "%s\n",           msg); break;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

SourceId EventBuffer::registerSource(SourceDescriptor desc) {
    auto st = state_.load(std::memory_order_acquire);
    assert((st == BufferState::Idle || st == BufferState::Paused)
           && "registerSource requires Idle or Paused state");
    if (st != BufferState::Idle && st != BufferState::Paused)
        return kInvalidSourceId;

    // Find a free slot: prefer reusing a deregistered slot, then extend hwm.
    SourceId id = kInvalidSourceId;
    for (size_t i = 0; i < slot_hwm_; ++i) {
        if (!sources_[i]) { id = static_cast<SourceId>(i); break; }
    }
    if (id == kInvalidSourceId) {
        if (slot_hwm_ >= MAX_SOURCES)
            throw std::runtime_error("EventBuffer: MAX_SOURCES exceeded");
        id = static_cast<SourceId>(slot_hwm_++);
    }

    desc.id = id;
    // Normalise: expose the registrar's device identifier (serial or opaque id)
    // through FormatDescriptor so window readers (formatOf) can attribute data
    // to a physical device without access to the SourceDescriptor.
    if (desc.format.device_serial.empty())
        desc.format.device_serial = desc.identifier;
    size_t slot_count = desc.computeSlotCount();
    size_t slot_bytes = desc.computeSlotBytes();

    auto slot  = std::make_unique<SourceSlot>();
    slot->desc = std::move(desc);
    slot->ring = std::make_unique<SourceRing>(id, slot_count, slot_bytes,
                                              &capturing_);
    sources_[id] = std::move(slot);
    ++active_sources_;
    return id;
}

void EventBuffer::foldLifetime(const SourceSlot &slot) {
    if (slot.desc.identifier.empty() || !slot.ring) return;
    const auto &s = slot.ring->stats();
    auto &L = lifetime_[slot.desc.identifier];
    L.bytes_written      += s.bytes_written_total.load(std::memory_order_relaxed);
    L.events_written     += s.events_written.load(std::memory_order_relaxed);
    L.events_overwritten += s.events_overwritten.load(std::memory_order_relaxed);
}

void EventBuffer::deregisterSource(SourceId id) {
    assert(state_.load(std::memory_order_acquire) == BufferState::Paused
           && "deregisterSource requires Paused state");
    assert(!swing_window_live_.load(std::memory_order_acquire)
           && "deregisterSource called while a SwingWindow is live — "
              "destroy the SwingWindow before deregistering sources");

    int idx = findSlotIndex(id);
    if (idx < 0) return;  // already deregistered or invalid — no-op

    // Preserve this source's totals for the rest of the session before the ring
    // (and its stats) are freed, so the resource monitor keeps showing them.
    if (sources_[id]) foldLifetime(*sources_[id]);

    // Destroy the unique_ptr — frees ring memory via AlignedDeleter.
    sources_[id].reset();
    --active_sources_;
    // slot_hwm_ is NOT decremented — the index may be reused by a future
    // registerSource() call (which scans for null entries).

    // If this was the last source, flag that the next registerSource() should
    // auto-resume (resume() will be blocked until then).
    if (active_sources_ == 0)
        no_source_paused_ = true;
}

size_t EventBuffer::activeSourceCount() const noexcept {
    return active_sources_;
}

// ---------------------------------------------------------------------------
// Producer API
// ---------------------------------------------------------------------------

int EventBuffer::findSlotIndex(SourceId id) const noexcept {
    if (id >= static_cast<SourceId>(slot_hwm_)) return -1;
    if (!sources_[id]) return -1;  // deregistered
    return static_cast<int>(id);
}

SourceRing::WriteSlot EventBuffer::acquireWriteSlot(SourceId id) noexcept {
    int idx = findSlotIndex(id);
    if (idx < 0) return {};
    return sources_[idx]->ring->acquireWriteSlot();
}

SourceRing::WriteSlot EventBuffer::getSlotByIndex(SourceId id,
                                                   size_t slot_idx) noexcept {
    int idx = findSlotIndex(id);
    if (idx < 0) return {};
    return sources_[idx]->ring->getSlotByIndex(slot_idx);
}

void EventBuffer::publish(SourceId id, uint64_t sequence) noexcept {
    int idx = findSlotIndex(id);
    if (idx < 0) return;
    sources_[idx]->ring->publish(sequence);
    source_published_.store(source_published_.load() + 1);
    source_published_.notifyAll();
}

std::vector<std::byte*> EventBuffer::getSlotPointers(SourceId id) const {
    int idx = findSlotIndex(id);
    if (idx < 0) return {};
    return sources_[idx]->ring->getSlotPointers();
}

size_t EventBuffer::getSlotCapacity(SourceId id) const {
    int idx = findSlotIndex(id);
    return (idx < 0) ? 0 : sources_[idx]->ring->slotCapacity();
}

size_t EventBuffer::getSlotCount(SourceId id) const {
    int idx = findSlotIndex(id);
    return (idx < 0) ? 0 : sources_[idx]->ring->slotCount();
}

// ---------------------------------------------------------------------------
// Consumer API
// ---------------------------------------------------------------------------

EventBuffer::Subscription EventBuffer::subscribe() {
    return Subscription(this,
                        index_.writeSequence(),
                        sub_gen_.load(std::memory_order_acquire));
}

SourceRing::ReadHandle EventBuffer::acquireReadHandle(SourceId id,
                                                       uint64_t source_seq) const noexcept {
    int idx = findSlotIndex(id);
    if (idx < 0) return {};
    return sources_[idx]->ring->acquireReadHandle(source_seq);
}

const FormatDescriptor& EventBuffer::formatOf(SourceId id) const {
    int idx = findSlotIndex(id);
    if (idx < 0) throw std::out_of_range("EventBuffer: unknown SourceId");
    return sources_[idx]->desc.format;
}

void EventBuffer::updateSourceFormat(SourceId id, const FormatDescriptor &fmt) {
    int idx = findSlotIndex(id);
    if (idx < 0) return;
    FormatDescriptor updated = fmt;
    // registerSource() normalised device_serial (identifier fallback) so
    // window readers can attribute payloads to a physical device. Re-stamps
    // built from frame data carry no serial — preserve the registered one.
    if (updated.device_serial.empty())
        updated.device_serial = sources_[idx]->desc.format.device_serial;
    sources_[idx]->desc.format = updated;
}

std::vector<IndexEntry> EventBuffer::snapshot(int64_t t_start_us,
                                               int64_t t_end_us) {
    return index_.snapshot(t_start_us, t_end_us);
}

// ---------------------------------------------------------------------------
// Observability
// ---------------------------------------------------------------------------

const SourceStats& EventBuffer::statsFor(SourceId id) const {
    int idx = findSlotIndex(id);
    if (idx < 0) throw std::out_of_range("EventBuffer: unknown SourceId");
    return sources_[idx]->ring->stats();
}

std::vector<SourceId> EventBuffer::stalledSources() const {
    std::vector<SourceId> result;
    for (size_t i = 0; i < slot_hwm_; ++i) {
        if (!sources_[i]) continue;
        if (sources_[i]->stalled.load(std::memory_order_acquire))
            result.push_back(sources_[i]->desc.id);
    }
    return result;
}

BufferState EventBuffer::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

bool EventBuffer::isCapturing() const noexcept {
    return capturing_.load(std::memory_order_acquire);
}

int64_t EventBuffer::nowMicros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// SwingWindow
// ---------------------------------------------------------------------------

// Ring-backed payload source: zero-copy reads from the paused buffer's rings, and
// the owner of the swing_window_live_ resume/deregister guard (set on construction,
// cleared on destruction — i.e. for the captured window's whole lifetime).
class RingPayloadSource final : public SwingPayloadSource {
public:
    explicit RingPayloadSource(EventBuffer* buffer) noexcept : buffer_(buffer) {
        buffer_->swing_window_live_.store(true, std::memory_order_release);
    }
    ~RingPayloadSource() override {
        buffer_->swing_window_live_.store(false, std::memory_order_release);
    }

    SourceRing::ReadHandle payloadOf(SourceId id, uint64_t sequence) const noexcept override {
        return buffer_->acquireReadHandle(id, sequence);
    }
    const FormatDescriptor& formatOf(SourceId id) const noexcept override {
        return buffer_->formatOf(id);
    }
    bool validate(SourceId id, const SourceRing::ReadHandle& h) const noexcept override {
        const auto& slot = buffer_->sources_[id];
        return slot && slot->ring && h.validate(*slot->ring);
    }

private:
    EventBuffer* buffer_;
};

SwingWindow EventBuffer::captureSwingWindow(int64_t t_start_us,
                                             int64_t t_end_us) {
    assert(state_.load(std::memory_order_acquire) == BufferState::Paused
           && "captureSwingWindow requires Paused state");
    auto entries = index_.snapshot(t_start_us, t_end_us);
    return SwingWindow(std::make_unique<RingPayloadSource>(this),  // sets swing_window_live_
                       std::move(entries), t_start_us, t_end_us);
}

std::unique_ptr<const SwingPayloadSource> EventBuffer::makeRingPayloadSource() {
    assert(state_.load(std::memory_order_acquire) == BufferState::Paused
           && "makeRingPayloadSource requires Paused state");
    // Called immediately after the pause that froze the rings, so the interval
    // in which they are frozen but unguarded is the width of these two calls
    // rather than the width of a deferred gather.
    return std::make_unique<RingPayloadSource>(this);   // sets swing_window_live_
}

SwingWindow EventBuffer::captureSwingWindow(
    std::chrono::milliseconds trailing_duration) {
    int64_t end_us   = nowMicros();
    int64_t start_us = end_us - trailing_duration.count() * 1000LL;
    return captureSwingWindow(start_us, end_us);
}

// ---------------------------------------------------------------------------
// Merger loop
// ---------------------------------------------------------------------------

void EventBuffer::mergerLoop() {
    // On Windows, raise the global 1 ms timer resolution only while actively
    // capturing (normal path), not for the whole thread lifetime. While paused
    // the merger parks on control_wait_ and has no need for fine granularity, so
    // we drop back to the system default to avoid the system-wide idle power cost
    // of holding timeBeginPeriod(1). hires_active tracks the begin/end balance;
    // every timeBeginPeriod must have exactly one matching timeEndPeriod.
    bool hires_active = false;
#if defined(PINPOINT_PLATFORM_WINDOWS)
    auto raiseTimerRes = [&] {
        if (!hires_active) { timeBeginPeriod(1); hires_active = true; }
    };
    auto lowerTimerRes = [&] {
        if (hires_active) { timeEndPeriod(1); hires_active = false; }
    };
#else
    auto raiseTimerRes = [&] { (void)hires_active; };
    auto lowerTimerRes = [&] {};
#endif

    ThreadPolicy::apply(ThreadRole::Merger);
    // Resource-profiler thread registration (decoupled — see setThreadRegisterHook).
    if (m_threadRegisterHook)
        m_threadRegisterHook("Buffer.Merger");
    logMsg(LogSeverity::Warn, (std::string("[pinpoint] merger thread priority: ")
                               + ThreadPolicy::lastApplyDescription()).c_str());

    if (config_.cpu_affinity_enabled)
        ThreadPolicy::pinToCore(1);

    MergerState state;

    // Drain each source ring into merger heads.
    // Returns true if any new event was pulled.
    auto drainSources = [&]() -> bool {
        bool any = false;
        for (size_t i = 0; i < slot_hwm_; ++i) {
            if (!sources_[i]) continue;         // deregistered slot
            auto& slot = *sources_[i];
            if (state.heads[i].valid) continue; // head already filled

            uint64_t write_seq = slot.ring->writeSequence();
            if (write_seq == 0) continue; // nothing written yet

            uint64_t& next = slot.next_seq;
            while (next < write_seq) {
                int64_t ts;
                if (slot.ring->peekTimestamp(next, ts)) {
                    state.setHead(static_cast<int>(i),
                                  PendingEvent{slot.desc.id, next, ts, true});
                    next++;
                    any = true;
                    break;
                } else {
                    // peekTimestamp returned false for one of two reasons:
                    //   (a) the slot is being written right now (gen=odd) — transient;
                    //       the producer just advanced write_seq before finishing the copy,
                    //       which can take several milliseconds on some platforms (e.g. macOS
                    //       AVFoundation frame mapping).  Do NOT skip: retry next iteration.
                    //   (b) genuine ring overrun: the producer lapped the merger by more than
                    //       slot_count entries.  Skip to the current write head.
                    uint64_t ws_now = slot.ring->writeSequence();
                    if ((ws_now - next) > slot.ring->slotCount()) {
                        // Genuine overrun. Resume at the OLDEST sequence still
                        // resident in the ring, not the write head: the
                        // backlog [ws_now - slot_count, ws_now) is published,
                        // still-valid data — skipping to ws_now would discard
                        // a full ring lap (~seconds of frames) on top of what
                        // the producer already overwrote. peekTimestamp's
                        // seqlock/lap checks reject any salvaged sequence the
                        // producer reaches while we drain.
                        next = ws_now - slot.ring->slotCount();
                        continue;
                    }
                    // else: slot being written — break and retry without advancing next
                    break;
                }
            }
        }
        return any;
    };

    // Safe-to-emit threshold: min(latest producer ts across all sources) minus
    // the reorder window. Uses ring stats to see past the current head.
    auto computeSafe = [&]() -> int64_t {
        int64_t min_ts = INT64_MAX;
        bool    any    = false;
        for (size_t i = 0; i < slot_hwm_; ++i) {
            if (!sources_[i]) continue;         // deregistered slot
            int64_t ts = sources_[i]->ring->stats()
                             .last_write_timestamp_us.load(std::memory_order_relaxed);
            if (ts == 0) continue; // source never written
            any    = true;
            min_ts = std::min(min_ts, ts);
        }
        return any ? (min_ts - static_cast<int64_t>(config_.reorder_window_us))
                   : INT64_MIN;
    };

    // Emit all heads with timestamp <= safe_until into the timeline index.
    // Returns true if anything was emitted.
    auto emitReady = [&](int64_t safe_until) -> bool {
        bool emitted = false;
        while (true) {
            int idx = state.findMinValidIndex();
            if (idx < 0) break;
            PendingEvent& ev = state.heads[idx];
            if (ev.timestamp_us > safe_until) break;

            state.enforceMonotonicity(idx, ev, *sources_[idx]->ring);

            IndexEntry entry{};
            entry.timestamp_us    = ev.timestamp_us;
            entry.source_id       = ev.source_id;
            entry.source_sequence = ev.source_seq;
            uint64_t seq = index_.append(entry);
            index_wait_.store(seq);
            index_wait_.notifyAll();

            state.clearHead(idx);
            emitted = true;
            drainSources(); // refill after consuming a head
        }
        return emitted;
    };

    // Drain and emit everything — used during pause().
    auto drainAll = [&] {
        drainSources();
        while (true) {
            int idx = state.findMinValidIndex();
            if (idx < 0) break;
            PendingEvent& ev = state.heads[idx];
            state.enforceMonotonicity(idx, ev, *sources_[idx]->ring);

            IndexEntry entry{};
            entry.timestamp_us    = ev.timestamp_us;
            entry.source_id       = ev.source_id;
            entry.source_sequence = ev.source_seq;
            uint64_t seq = index_.append(entry);
            index_wait_.store(seq);
            index_wait_.notifyAll();

            state.clearHead(idx);
            drainSources();
        }
    };

    while (running_.load(std::memory_order_acquire)) {

        // --- Draining path (pause() signalled) ---
        if (draining_.load(std::memory_order_acquire)) {
            drainAll();
            drained_.store(true, std::memory_order_release);

            // Entering the paused park — drop back to default timer resolution.
            lowerTimerRes();

            // Block until resumed or stopped — untimed, no polling.
            while (draining_.load(std::memory_order_acquire)
                   && running_.load(std::memory_order_acquire)) {
                uint64_t gen = control_wait_.load();
                // Re-check the predicate AFTER sampling gen so a resume()/stop()
                // that lands in this window has already bumped gen, making the
                // wait() below return immediately (no lost wakeup).
                if (!draining_.load(std::memory_order_acquire)
                    || !running_.load(std::memory_order_acquire))
                    break;
                control_wait_.wait(gen);
            }

            // Fresh capture session — reset merger state.
            state = MergerState{};
            continue;
        }

        // --- Normal path ---
        // Actively capturing: ensure 1 ms timer resolution (no-op if already on).
        raiseTimerRes();

        bool any_progress  = drainSources();
        int64_t safe_until = computeSafe();
        any_progress      |= emitReady(safe_until);

        if (!draining_.load(std::memory_order_acquire))
            maybeRunWatchdog();

        if (any_progress) {
            for (uint32_t i = 0; i < config_.merger_spin_iterations; ++i)
                PINPOINT_CPU_PAUSE();
        } else {
            uint64_t observed = source_published_.load();
            source_published_.waitFor(observed,
                                      std::chrono::microseconds(config_.merger_cold_timeout_us));
        }
    }

    // Balance any outstanding timeBeginPeriod on thread exit (e.g. stop() while
    // on the normal path). No-op if we exited from the paused park.
    lowerTimerRes();
}

void EventBuffer::maybeRunWatchdog() {
    auto now = std::chrono::steady_clock::now();
    if (now - last_watchdog_tick_ <
            std::chrono::milliseconds(config_.watchdog_interval_ms))
        return;
    last_watchdog_tick_ = now;

    int64_t now_us = EventBuffer::nowMicros();
    for (size_t i = 0; i < slot_hwm_; ++i) {
        if (!sources_[i]) continue;
        auto& src = *sources_[i];

        if (src.desc.expected_interarrival_us.count() == 0) continue;

        int64_t last_ts = src.ring->stats()
                              .last_write_timestamp_us
                              .load(std::memory_order_relaxed);
        if (last_ts == 0) continue;

        int64_t threshold_us = std::max(
            static_cast<int64_t>(src.desc.expected_interarrival_us.count())
                * static_cast<int64_t>(config_.stall_threshold_mult),
            static_cast<int64_t>(config_.stall_threshold_min_us));

        bool silent = (now_us - last_ts) > threshold_us;

        if (silent && !src.stalled.exchange(true, std::memory_order_acq_rel)) {
            IndexEntry marker{};
            marker.timestamp_us = now_us;
            marker.source_id    = src.desc.id;
            marker.flags        = IndexEntryFlags::SourceStalled;
            uint64_t seq = index_.append(marker);
            index_wait_.store(seq);
            index_wait_.notifyAll();
        } else if (!silent) {
            src.stalled.store(false, std::memory_order_release);
        }
    }
}

EventBuffer::DiagnosticsSnapshot EventBuffer::diagnostics() const {
    DiagnosticsSnapshot snap;
    snap.state = state_.load(std::memory_order_acquire);
    snap.snapshot_timestamp_us = nowMicros();
    snap.timeline_entries = index_.latestSequence();
    for (size_t i = 0; i < slot_hwm_; ++i) {
        if (!sources_[i]) continue;
        const auto& src = *sources_[i];
        const auto& s   = src.ring->stats();
        DiagnosticsSnapshot::SourceInfo info{};
        info.id                      = src.desc.id;
        info.name                    = src.desc.name;
        info.identifier              = src.desc.identifier;
        info.events_written          = s.events_written.load(std::memory_order_relaxed);
        info.events_overwritten      = s.events_overwritten.load(std::memory_order_relaxed);
        info.slot_count              = src.ring->slotCount();
        info.bytes_written_total     = s.bytes_written_total.load(std::memory_order_relaxed);
        info.last_write_timestamp_us = s.last_write_timestamp_us.load(std::memory_order_relaxed);
        info.max_inter_arrival_us    = s.max_inter_arrival_us.load(std::memory_order_relaxed);
        info.bounds_violations       = s.bounds_violations.load(std::memory_order_relaxed);
        info.monotonicity_violations = s.monotonicity_violations.load(std::memory_order_relaxed);
        info.stalled                 = src.stalled.load(std::memory_order_acquire);
        // Lifetime = folded history (if this identifier has been reset before)
        // plus the current ring counters.
        LifetimeCounters base{};
        if (auto it = lifetime_.find(src.desc.identifier); it != lifetime_.end())
            base = it->second;
        info.lifetime_bytes_written      = base.bytes_written      + info.bytes_written_total;
        info.lifetime_events_written     = base.events_written     + info.events_written;
        info.lifetime_events_overwritten = base.events_overwritten + info.events_overwritten;
        snap.sources.push_back(std::move(info));
    }
    // Folded totals for every identifier seen this session — covers sources that
    // have since been deregistered (no live SourceInfo above).
    for (const auto &[ident, L] : lifetime_) {
        snap.lifetime.push_back(DiagnosticsSnapshot::LifetimeInfo{
            ident, L.bytes_written, L.events_written, L.events_overwritten});
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void EventBuffer::start() {
    assert(state_.load(std::memory_order_acquire) == BufferState::Idle
           && "start() requires Idle state");
    if (state_.load(std::memory_order_acquire) != BufferState::Idle) return;

    running_.store(true, std::memory_order_release);
    drained_.store(false, std::memory_order_relaxed);

    if (active_sources_ == 0) {
        // No sources yet — start Paused so registerSource() can be called
        // safely. The first registerSource() will auto-resume.
        no_source_paused_ = true;
        capturing_.store(false, std::memory_order_release);
        draining_.store(true, std::memory_order_release);
        state_.store(BufferState::Paused, std::memory_order_release);
    } else {
        no_source_paused_ = false;
        capturing_.store(true, std::memory_order_release);
        draining_.store(false, std::memory_order_relaxed);
        state_.store(BufferState::Capturing, std::memory_order_release);
    }

    merger_thread_ = std::thread([this] { mergerLoop(); });
}

void EventBuffer::pause() {
    assert(state_.load(std::memory_order_acquire) == BufferState::Capturing
           && "pause() requires Capturing state");
    if (state_.load(std::memory_order_acquire) != BufferState::Capturing) return;

    capturing_.store(false, std::memory_order_release);

    draining_.store(true, std::memory_order_release);
    index_wait_.notifyAll(); // wake merger if sleeping

    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(config_.pause_drain_timeout_ms);
    while (!drained_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    state_.store(BufferState::Paused, std::memory_order_release);
}

void EventBuffer::resume() {
    assert(state_.load(std::memory_order_acquire) == BufferState::Paused
           && "resume() requires Paused state");
    if (state_.load(std::memory_order_acquire) != BufferState::Paused) return;

    // Refuse to resume with no sources — stay Paused until at least one
    // source registers, at which point registerSource() auto-resumes.
    if (active_sources_ == 0) {
        no_source_paused_ = true;
        return;
    }
    no_source_paused_ = false;

    if (config_.resume_clear_rings) {
        for (size_t i = 0; i < slot_hwm_; ++i) {
            if (!sources_[i]) continue;
            // Fold pre-reset totals into the session lifetime before clearing,
            // otherwise every pause→resume (e.g. each swing-replay cycle) would
            // silently discard the counters the resource monitor reports.
            foldLifetime(*sources_[i]);
            sources_[i]->ring->reset();
            sources_[i]->next_seq = 0;
        }
        index_.reset();
    }

    // Invalidate all live Subscription instances.
    sub_gen_.fetch_add(1, std::memory_order_release);

    draining_.store(false, std::memory_order_release);
    drained_.store(false, std::memory_order_relaxed);
    capturing_.store(true, std::memory_order_release);

    source_published_.store(0);

    // Reset the index-append signal for the fresh capture session.
    index_wait_.store(0);
    index_wait_.notifyAll();

    state_.store(BufferState::Capturing, std::memory_order_release);

    // Wake the merger out of its paused block — MUST be last, so the woken
    // merger observes fully-committed state (draining_ cleared, capturing_ set,
    // state_ == Capturing) and not a torn mid-resume view.
    control_wait_.store(control_wait_.load() + 1);
    control_wait_.notifyAll();
}

void EventBuffer::stop() {
    if (state_.load(std::memory_order_acquire) == BufferState::Capturing)
        pause();

    running_.store(false, std::memory_order_release);
    // Wake the merger whether it is parked on the pause gate (control_wait_) or
    // on the normal-path cold timeout (index_wait_/source_published_).
    control_wait_.store(control_wait_.load() + 1);
    control_wait_.notifyAll();
    index_wait_.notifyAll();

    if (merger_thread_.joinable())
        merger_thread_.join();

    state_.store(BufferState::Idle, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Subscription
// ---------------------------------------------------------------------------

EventBuffer::Subscription::Subscription(EventBuffer* buf,
                                         uint64_t start_seq,
                                         uint64_t gen)
    : buffer_(buf), next_seq_(start_seq), gen_(gen)
{}

void EventBuffer::Subscription::resetToLatest() {
    next_seq_ = buffer_->index_.writeSequence();
}

bool EventBuffer::Subscription::tryNext(IndexEntry& out) {
    // Generation check: timeline was reset by resume().
    uint64_t cur_gen = buffer_->sub_gen_.load(std::memory_order_acquire);
    if (cur_gen != gen_) {
        gen_      = cur_gen;
        next_seq_ = 0;
        return false;
    }

    if (buffer_->index_.tryRead(next_seq_, out)) {
        next_seq_++;
        return true;
    }

    // Overrun: write_seq_ has lapped next_seq_ by more than index capacity.
    uint64_t ws = buffer_->index_.writeSequence();
    if (ws > next_seq_ && (ws - next_seq_) > buffer_->index_.capacity()) {
        overruns_ += (ws - next_seq_);
        next_seq_  = ws;
    }

    return false;
}

bool EventBuffer::Subscription::waitNext(IndexEntry& out,
                                          std::chrono::microseconds timeout) {
    if (tryNext(out)) return true;
    uint64_t expected = buffer_->index_wait_.load();
    buffer_->index_wait_.waitFor(expected, timeout);
    return tryNext(out);
}

} // namespace pinpoint
