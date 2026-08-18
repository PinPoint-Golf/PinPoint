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

#pragma once

#include "types.h"
#include "source_descriptor.h"
#include "source_ring.h"
#include "source_stats.h"
#include "timeline_index.h"
#include "wait_flag.h"
#include "event_buffer_config.h"
#include "swing_window.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pinpoint {

// The ring-backed SwingPayloadSource (defined in event_buffer.cpp). It owns the
// swing_window_live_ resume guard and validates read handles against the live ring,
// so it needs friend access to those private members.
class RingPayloadSource;

enum class LogSeverity { Info, Warn, Error };
using LogCallback = std::function<void(LogSeverity, const char *)>;

enum class BufferState {
    Idle,        // constructed, not started; no merger thread
    Capturing,   // merger running, producers active
    Paused,      // merger quiesced, rings frozen, safe for analysis
    Stopping,    // teardown in progress (transient)
};

class EventBuffer {
public:
    static constexpr size_t MAX_SOURCES = 16;

    explicit EventBuffer(EventBufferConfig cfg = {});
    ~EventBuffer();

    // Register a log callback before calling start(). If none is set, messages
    // go to stderr. The callback may be invoked from the merger thread.
    void setLogCallback(LogCallback cb);

    // Optional hook invoked once from the merger thread at loop entry, so the app
    // can register that thread with its resource profiler WITHOUT coupling this
    // buffer library to it (the standalone Buffer tests never set it). Set before
    // start(); the argument is a stable label for the thread.
    void setThreadRegisterHook(std::function<void(const char *)> hook)
    {
        m_threadRegisterHook = std::move(hook);
    }

    EventBuffer(const EventBuffer&)            = delete;
    EventBuffer& operator=(const EventBuffer&) = delete;

    // --- Registration ---
    // Callable while Idle or Paused. NOT callable while Capturing or Stopping.
    SourceId registerSource(SourceDescriptor desc);

    // Deregister a source and immediately free its ring memory.
    // ONLY callable while Paused.
    // Asserts no SwingWindow is currently live (caller must destroy SwingWindow
    // before deregistering any source it references).
    // After this call, 'id' is invalid. Any further calls with 'id' are no-ops.
    void deregisterSource(SourceId id);

    // Number of currently active (registered) sources.
    size_t activeSourceCount() const noexcept;

    // --- Producer API ---
    // acquireWriteSlot returns valid=false when state != Capturing.
    SourceRing::WriteSlot acquireWriteSlot(SourceId id) noexcept;
    SourceRing::WriteSlot getSlotByIndex(SourceId id, size_t slot_idx) noexcept;
    void                  publish(SourceId id, uint64_t sequence) noexcept;

    // DMA registration helpers
    std::vector<std::byte*> getSlotPointers(SourceId id) const;
    size_t                  getSlotCapacity(SourceId id)  const;
    size_t                  getSlotCount(SourceId id)     const;

    // --- Consumer API ---
    class Subscription {
    public:
        bool     waitNext(IndexEntry& out, std::chrono::microseconds timeout);
        bool     tryNext(IndexEntry& out);
        void     resetToLatest();
        uint64_t overrunsSinceLastRead() const noexcept { return overruns_; }

    private:
        friend class EventBuffer;
        Subscription(EventBuffer* buf, uint64_t start_seq, uint64_t gen);

        EventBuffer* buffer_;
        uint64_t     next_seq_;
        uint64_t     gen_;        // sub_gen_ snapshot; mismatch means timeline was reset
        uint64_t     overruns_ = 0;
    };

    Subscription subscribe();

    SourceRing::ReadHandle  acquireReadHandle(SourceId id, uint64_t source_seq) const noexcept;
    const FormatDescriptor& formatOf(SourceId id) const;
    std::vector<IndexEntry> snapshot(int64_t t_start_us, int64_t t_end_us);

    // --- Lifecycle ---
    void        start();
    void        pause();
    void        resume();
    void        stop();
    BufferState state() const noexcept;
    bool        isCapturing() const noexcept;
    // True when the buffer is Paused solely because it has no registered
    // sources. Cleared when resume() succeeds. Use this to distinguish the
    // "waiting for first device" state from a deliberate swing-analysis pause.
    bool        isWaitingForSources() const noexcept { return no_source_paused_; }
    // True while a SwingWindow captured from this buffer is live anywhere in
    // the app. Resume/deregister gating in the GUI layer keys off this —
    // window ownership lives in ShotProcessor, not CameraManager.
    bool        swingWindowLive() const noexcept { return swing_window_live_.load(std::memory_order_acquire); }

    // Update the format metadata for an already-registered source.
    // Safe to call in any state — touches only the descriptor, not ring memory.
    void updateSourceFormat(SourceId id, const FormatDescriptor &fmt);

    // SwingWindow — only callable in Paused state (asserts otherwise)
    SwingWindow captureSwingWindow(int64_t t_start_us, int64_t t_end_us);
    SwingWindow captureSwingWindow(std::chrono::milliseconds trailing_duration);

    // Build the ring backing on its own, WITHOUT constructing a window —
    // deferred_sources_design.md §3.2/§3.5. For a capture that must wait for a
    // deferred source to report before the window can be built.
    //
    // ⚠ THE RESUME GUARD GOES UP THE MOMENT THIS RETURNS, and that is the whole
    // reason it exists. swing_window_live_ is set by the ring source's
    // CONSTRUCTOR, which in the ordinary captureSwingWindow() path is the same
    // instant as window construction. Splitting the freeze from the construction
    // opens a gap where the rings are frozen but the guard is not set — and
    // `resume_clear_rings` is true, so a resume landing in that gap does not
    // merely restart capture, it CLEARS THE RINGS WE ARE ABOUT TO SNAPSHOT.
    // That turns a race into silent, total data loss for the shot.
    //
    // So: call this at PAUSE, hold the returned source across the gather, and
    // move it into the composite when the window is finally constructed. The
    // guard is released when the window (and hence the source) is destroyed,
    // exactly as it is today.
    std::unique_ptr<const SwingPayloadSource> makeRingPayloadSource();

    // --- Observability ---
    const SourceStats&    statsFor(SourceId id) const;
    std::vector<SourceId> stalledSources() const;

    struct DiagnosticsSnapshot {
        struct SourceInfo {
            SourceId    id;
            std::string name;
            std::string identifier;
            uint64_t    events_written;
            uint64_t    events_overwritten;
            uint64_t    slot_count;
            uint64_t    bytes_written_total;
            int64_t     last_write_timestamp_us;
            int64_t     max_inter_arrival_us;
            uint64_t    bounds_violations;
            uint64_t    monotonicity_violations;
            bool        stalled;
            // Session-lifetime totals (folded history + current ring counters).
            // Unlike the *_total/written fields above, these survive ring resets
            // (pause→resume clears) and source deregistration, keyed by identifier.
            uint64_t    lifetime_bytes_written;
            uint64_t    lifetime_events_written;
            uint64_t    lifetime_events_overwritten;
        };
        // Session-lifetime totals for sources that have been folded down — i.e.
        // every source seen this session, including ones since deregistered.
        // Keyed by SourceDescriptor::identifier (stable per physical device).
        struct LifetimeInfo {
            std::string identifier;
            uint64_t    bytes_written;
            uint64_t    events_written;
            uint64_t    events_overwritten;
        };
        std::vector<SourceInfo>   sources;
        std::vector<LifetimeInfo> lifetime;
        uint64_t                timeline_entries;
        BufferState             state;
        int64_t                 snapshot_timestamp_us;
    };

    DiagnosticsSnapshot diagnostics() const;

    // --- Clock ---
    static int64_t nowMicros() noexcept;

private:
    friend class RingPayloadSource;

    struct SourceSlot {
        SourceDescriptor        desc;
        std::unique_ptr<SourceRing> ring;
        uint64_t                next_seq = 0; // next ring sequence to drain
        std::atomic<bool>       stalled{false};
    };

    // Session-lifetime byte/event totals, keyed by SourceDescriptor::identifier.
    // Survives ring resets (resume clears the rings) and source deregistration,
    // so the resource monitor can show stats for a device's whole session rather
    // than only since the last resume. Mutated only on the control plane
    // (resume()/deregisterSource(), both serialized with the buffer Paused) and
    // read in diagnostics(); never touched on the lock-free producer hot path.
    struct LifetimeCounters {
        uint64_t bytes_written      = 0;
        uint64_t events_written     = 0;
        uint64_t events_overwritten = 0;
    };
    std::unordered_map<std::string, LifetimeCounters> lifetime_;

    // Fold a source's current ring counters into lifetime_ before the ring is
    // reset or destroyed. No-op for sources with an empty identifier.
    void foldLifetime(const SourceSlot &slot);

    EventBufferConfig config_;

    std::array<std::unique_ptr<SourceSlot>, MAX_SOURCES> sources_;
    size_t slot_hwm_       = 0;   // highest index ever assigned + 1; merger iterates 0..slot_hwm_
    size_t active_sources_ = 0;   // count of non-null slots
    // True while the buffer is Paused solely because there are no registered
    // sources. registerSource() clears this and calls resume() automatically
    // when the first source is added. Set by start(), resume() (blocked), and
    // deregisterSource() when the last source is removed.
    bool   no_source_paused_ = false;

    TimelineIndex index_;
    WaitFlag      index_wait_;
    alignas(64) WaitFlag source_published_; // signalled by every publish(); merger cold-path waits on this

    // Generation counter bumped by resume()/stop() to wake the merger out of its
    // paused block via an untimed wait() (no polling while paused). Deliberately
    // NOT index_wait_: that flag's value means "last appended index seq", and
    // overloading it would force a parked waiter onto a hot-path instance.
    WaitFlag      control_wait_;

    alignas(64) std::atomic<bool>        capturing_{false};
    std::atomic<BufferState>             state_{BufferState::Idle};
    std::atomic<bool>                    running_{false};
    std::atomic<bool>                    draining_{false};
    std::atomic<bool>                    drained_{false};
    std::atomic<uint64_t>                sub_gen_{0};

    std::thread merger_thread_;
    std::chrono::steady_clock::time_point last_watchdog_tick_;

    // Set to true while a SwingWindow is live. deregisterSource() asserts this
    // is false. SwingWindow constructor sets it true; destructor sets it false.
    std::atomic<bool> swing_window_live_{false};

    LogCallback m_logCallback;
    std::function<void(const char *)> m_threadRegisterHook;

    void logMsg(LogSeverity sev, const char *msg) const;
    int findSlotIndex(SourceId id) const noexcept;
    void mergerLoop();
    void maybeRunWatchdog();
};

} // namespace pinpoint
