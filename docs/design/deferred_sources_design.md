# Deferred sources — folding late-arriving samples into a read-only SwingWindow

**Status:** design note. Nothing built. Written 2026-08-14.
**Scope:** a PinPoint capture/analysis concern. The motivating case is a wrist sensor whose
high-rate record can only be retrieved after the swing, but **nothing here is specific to that
device**, and the protocol work behind it belongs to a separate team and is deliberately not
described here.

---

## 1. The gap in the current model

Every source PinPoint captures today is a **live producer**: it writes into its `SourceRing` as
the data happens, the merger drains it into the timeline index, and by the time `ShotProcessor`
freezes the window everything that will ever exist for that shot is already in the ring.

A **deferred source** breaks that assumption. It is a source whose samples for a shot are not on
the host when the window would be frozen, and can only be obtained afterwards by asking for them.
Three shapes we can already name:

- **A device that buffers internally and replays on request.** The motivating case: the live link
  carries a decimated view (a downswing gets a handful of samples) while the device holds the real
  record and hands it over after the fact. This is the one that forced the design.
- **A measurement instrument that reports on its own schedule.** The launch-monitor connector
  already hit the weak version of this — a reading that lands while analysis is still running.
- **Anything batch-retrieved over a slow link.** A future high-speed camera burst, an external
  force plate, a phone-hosted sensor.

The common property is that **arrival time carries no information**. A deferred source's samples
arrive thousands at a time, out of any relation to when they were measured, so they must carry
their own host-clock timestamps or they cannot be placed on the swing timeline at all.

### 1a. Why the launch-monitor precedent does not cover it

`ShotProcessor` already handles a late arrival: a launch-monitor reading is *parked* and flushed at
the analysis/export join, landing in `swing.json`. That works because the reading is **one
point-in-time value that nothing in the analysis reads**.

A deferred source is a **stream the analysis consumes**. The segmentation pre-stage fuses it, the
metrics are computed from it, the replay draws it. It has to be *inside the window*, before
anything runs. Parking it at the join is too late by the entire pipeline.

---

## 2. The invariant, restated rather than weakened

The window stays read-only. The restatement is the whole design:

> **A `SwingWindow` is constructed once, after every contributing source has delivered everything
> it will deliver for this shot, and is `const` for its entire lifetime. What changes is that
> "delivered" is no longer simultaneous with the *freeze* instant. A source may be deferred: the
> rings freeze on time, and the window is built when the deferred sources have reported or their
> deadline has passed.**

So we split two things `captureWindowAndLaunch()` (`shot_processor.cpp:562`) currently does in one
breath:

| Today | Proposed |
|---|---|
| `pauseBuffer()` — rings freeze | `pauseBuffer()` — rings freeze, unchanged, same instant |
| `captureSwingWindow(4 s trailing)` — window built in the same breath | window built once the deferred sources report or time out |

Nothing becomes mutable. No stage sees a window that grows under it. **The `SwingWindow` type does
not change at all.**

---

## 3. Impact on the buffer subsystem

**The headline: a deferred source is not a ring producer, so almost none of the buffer changes.**
It never calls `registerSource()`, `acquireWriteSlot()` or `publish()`. Everything that makes the
buffer delicate — the lock-free SPSC ring, the merger, the producer stop-barrier contract, the
pause-around-register requirement — is simply not on its path.

### 3.1 The seam already exists, and re-analysis proves it

`SwingPayloadSource` (`src/Buffer/swing_payload_source.h`) was built for exactly this: a pluggable
backing so the same concrete `SwingWindow` can be fed by the live ring **or** by something else.
And the offline re-analysis path has been running the "something else" in production for months —
`SwingDiskSource` (`swing_reanalyzer.cpp:194`) is, for IMU data, precisely the in-RAM source a
deferred source needs:

```cpp
void addImu(SourceId id, FormatDescriptor fd, std::vector<ImuSample> samples);
SourceRing::ReadHandle payloadOf(SourceId id, uint64_t seq) const noexcept override;
bool validate(SourceId, const SourceRing::ReadHandle&) const noexcept override { return true; }
```

and `SwingDiskLoader` already synthesizes the index rows the same way we would:

```cpp
const SourceId id = nextId++;
for (size_t i = 0; i < tUs.size(); ++i)
    entries.push_back(IndexEntry{ tUs[i], id, uint64_t(i) });   // swing_reanalyzer.cpp:459
...
std::stable_sort(entries.begin(), entries.end(), byTimestamp);  // :551
```

So this is **not a new mechanism**. It is promoting a proven offline one to the live path, and the
two paths converge rather than diverge — which is worth something on its own, because re-analysis
determinism depends on them agreeing.

What is missing is only the **composite**: today a window's backing is *either* ring *or* disk.
A deferred source needs *both at once*.

### 3.2 What gets added

| Component | Where | Size |
|---|---|---|
| `RamPayloadSource` — id-keyed `vector<ImuSample>` + `FormatDescriptor`, `validate()` true | `src/Buffer/` | ~40 lines, **lifted out of `SwingDiskSource`** so both paths share one implementation |
| `CompositePayloadSource` — routes `payloadOf`/`formatOf`/`validate` by `SourceId` | `src/Buffer/` | ~40 lines |
| Split window construction from ring freeze — expose `snapshotEntries(t0,t1)` and a ring-source factory so the caller composes and calls the (already public) `SwingWindow` constructor | `event_buffer.h/.cpp` | small |
| A reserved-id allocator for sources that have no ring | `event_buffer.h/.cpp` | small, see 3.3 |

### 3.3 Source identity — the one genuinely new question

Deferred ids must not collide with ring ids, and `SourceId` is used as a direct index into
`sources_[id]` with `slot_hwm_` bounding the merger's sweep. The offline path sidesteps this
entirely (all its ids are synthetic, starting at 0, with no EventBuffer in the process), which is
why it never had to answer it.

Recommend **`EventBuffer::reserveSourceId(SourceDescriptor)`**: allocates an id and stores the
descriptor, allocates **no ring**, and is never swept by the merger. Not a "virtual source" that
pretends to be live — a reserved identity, explicitly ring-less.

The alternative — allocating deferred ids downward from `kInvalidSourceId - 1` — avoids touching
the buffer at all, but then the exporter and resource monitor, which resolve device metadata by
source id, have an id the buffer has never heard of and each needs a special case. Reserving is
cheaper than two special cases.

**Consequence for the resource monitor:** a reserved id has no ring and therefore no
`SourceStats`. It must render as *deferred* rather than falling through to *stalled*, which is what
a zero-write source looks like today.

### 3.4 The timeline index is protected, and that is deliberate

`TimelineIndex` is **one global ring of 8192 entries shared by every source**
(`event_buffer_config.h`). Routing a high-rate lane through it would consume roughly 7 s of index
headroom against a 4 s window — margin that varies with camera count, i.e. a capture that works
with one camera and silently truncates with two.

Deferred entries never enter it. They are merge-sorted into the **window's own** `entries_` vector,
which is heap-allocated and unbounded. This is a large part of why the composite design beats
anything that writes into the buffer.

### 3.5 Two hazards that are specific to splitting freeze from construction

**⚠ `swing_window_live_` must be held across the gap.** The flag is set by `RingPayloadSource`'s
*constructor* (`event_buffer.cpp:335`), which today is the same instant as window construction.
Separate them and there is a window where the rings are frozen but the resume guard is not set —
and `CameraManager::resumeBuffer()`'s backstop reads exactly that flag.

**⚠ And `resume_clear_rings = true`.** So a resume landing in that gap does not merely restart
capture — it **clears the rings we are about to snapshot**. This turns a race into silent total
data loss for the shot.

Fix: construct the `RingPayloadSource` at *pause* time — the flag goes up there — hold it through
the gather, and move it into the composite at construction. Not optional.

### 3.6 Why the samples cannot simply be written into the ring

Stated explicitly because it is the obvious idea and it fails silently.

**`MergerState::enforceMonotonicity` (`event_buffer.cpp:84`) rewrites any entry whose timestamp is
`<= last_emitted_ts` for that source to `last_emitted_ts + 1` and counts a violation.** A burst of
back-dated samples arriving after the live ones would be clamped into a smear a few hundred
microseconds wide — present, plausible-looking, and completely wrong. Nothing downstream would
report an error.

Three further reasons, any one sufficient: the index pressure in 3.4; `registerSource()` /
`deregisterSource()` require a paused buffer, so a per-shot source means a per-shot pause dance
inside a pipeline already pausing for its own reasons; and a bulk write from an I/O completion
callback is a new producer shape for which the stop-barrier contract would have to be
re-established.

### 3.7 What does not change

Worth stating plainly, because this is a subsystem where "small change" and "safe change" are not
the same thing:

- `SourceRing` — untouched. No new locks, no change to the hot write path.
- The merger loop — untouched. It never sees a deferred source.
- The producer stop-barrier contract — untouched, and no new entry in its table.
- `EventBuffer` pause/resume/register/deregister semantics — untouched.
- `SwingWindow` — the public type is **unchanged**. `entriesFor()`, `payloadOf()`,
  `interpolateImu()`, `formatOf()` all behave identically; they simply resolve some ids through a
  different backing.
- Memory cost — bounded and small. A 2 s high-rate span for two segments is ~128 KB, against
  camera ring slots measured in megabytes.

---

## 4. Impact on the analysis pipeline

**The headline: every stage reads the window through `entriesFor()` and `interpolateImu()`, and
neither can tell a source was deferred.** No stage needs to know. What changes is *when* the
pipeline starts, one algorithmic cost, and how stages decide whether they can run.

### 4.1 Where the wait goes

`ShotProcessor` gains **`Gathering`** between `PostRoll` and `Processing`:

```
shotDetected ─► POSTROLL    per-source delay (1250 ms auto / 500 ms manual)
             ─► pauseBuffer()            ← rings freeze here, UNCHANGED instant
             ─► GATHERING                ← new: request deferred data, bounded deadline
             ─► construct SwingWindow    ← ring snapshot + RAM blocks, const from here
             ─► PROCESSING …             unchanged from here down
```

`busy()` is already `m_state != State::Idle`, so `ShotController` stays disarmed through the gather
for free, and `setProcessorBusy` already covers the whole span. The toolbar needs a distinct label
— the athlete must not tee up during it.

**The segmentation pre-stage moves with it, and nothing else does.** That stage
(`shot_processor.cpp:605-622`) fuses IMU + vision on the frozen window and its future *gates* both
heavy workers, so it simply starts later. Its inputs, its job value type, and its consumers are
unchanged.

### 4.2 ⚠ The one real algorithmic consequence: `interpolateImu` is a linear scan

`SwingWindow::interpolateImu()` (`swing_window.cpp:77`) finds its bracketing samples by scanning
**every entry in the window**, for every call. And `ImuVisionFuser` calls it **once per grid point
per binding** (`imu_vision_fuser.cpp:145`):

```cpp
for (const int64_t t : out.timeGrid)
    window.interpolateImu(bd.b->source, t, ...);
```

So the cost is `gridPoints × bindings × totalEntries`, and a deferred high-rate source inflates
*both* of the terms that matter:

| | entries in window | grid points (4 s) | bindings | entry visits |
|---|---|---|---|---|
| today (2 cams @240, 2 IMUs @100 Hz, 200 Hz grid) | ~2,700 | 800 | 2 | **4.3 M** |
| + 2 s high-rate span, grid raised to match | ~5,900 | 3,200 | 2 | **37.8 M** |

A ~9× increase, and it lands on the pre-stage that gates everything else. This is exactly the kind
of regression that surfaces months later as "re-analysis got slower" with no obvious cause.

**Fix, and it is small:** `entries_` is already sorted by timestamp on both construction paths
(`TimelineIndex::snapshot` sorts; `SwingDiskLoader` `stable_sort`s). Build a **per-source offset
table once at construction** and binary-search it. O(log n) per call, no API change, and it makes
today's path faster too. This should land *with* deferred sources, not after them.

### 4.3 The master grid rate must follow the data

`ImuVisionFuser::fuse(..., double gridHz = 200.0, ...)` resamples every bound segment onto one
fixed-rate grid. Leaving it at 200 Hz would discard most of what a deferred high-rate source cost
us to retrieve; raising it globally makes every ordinary capture pay 4× for nothing.

Recommend deriving the grid rate from the bound sources' **actual median sample interval over this
window**, clamped to a sane range, rather than a constant. That keeps a webcam-plus-two-IMUs
capture exactly where it is today and lets a high-rate capture use what it has.

⚠ **What rate the wrist metrics actually need is an open analysis question, not a plumbing one.**
Landing the mechanism does not answer it, and the mechanism should not pretend to.

### 4.4 Stages gate on data, never on the device

`CaptureCapabilities` (`analysis_stage.h:59`) is the established seam — stages call `canRun` against
it so a webcam-only or IMU-only capture skips what it cannot feed instead of branching inside
itself. A deferred source fits by **extending `BoundImu`**, not by adding a device check:

```cpp
struct BoundImu {
    SegmentRole role;
    bool        calibValid;
    double      calibAgeSec;
    double      effectiveHz     = 0.0;   // measured over this window, not declared
    int64_t     highRateSpanUs[2]{};     // the span that exceeded the base rate, if any
};
```

This obeys the standing rule that **stages gate on devices and data, never on session type**, and
it means a stage needing high-rate input can land dark: it skips with a reason recorded in
`StageTraceEntry::skipReason` until the data is actually present. `effectiveHz` is *measured from
the window*, not taken from a device declaration, so a pull that silently under-delivered gates the
same way as no pull at all.

### 4.5 Degradation is the normal path, not the error path

If the deferred source misses its deadline, fails, or was never connected, the window is built with
no deferred entries — **byte-identical to what the pipeline produces today**. Segmentation, the
analyzer, the exporter and the replay all behave exactly as they do now. There is no failure branch
to write, only a capability that is absent and a stage trace that says so.

This is the property that makes the whole thing safe to land incrementally.

### 4.6 Provenance and re-analysis determinism

A stitched trace changes sample rate partway through the window. Nothing in the sample data records
that, and re-analysis must reproduce the same result, so it has to be written down:

- **`swing.json` gains a per-stream provenance block** — the high-rate span, its effective rate, and
  the timestamp uncertainty the supplier reported. Without it, a trace that is 800 Hz for 2 s and
  25 Hz either side is indistinguishable from one that is not, and `effectiveHz` in 4.4 would be
  computed from a window whose provenance nobody can audit.
- **The round-trip works with no loader change**, because the exporter already writes real
  per-sample `t_us` arrays and `SwingDiskLoader` already builds `ImuStream` from them. A
  variable-rate stream is just a `t_us` array with uneven spacing.
- **Use the csv/binary sidecar for high-rate streams** (`SwingExportJob::imuFormat`). 2 s × 800 Hz ×
  2 units is ~128 KB binary but on the order of a megabyte inlined as JSON, against a corpus where
  `swing.json` size is already a live complaint.

### 4.7 One lane, stitched — not two

A deferred pull typically covers a span *around impact*, not the whole window. So the high-rate
block does not replace the live lane, it fills part of it.

Two options were considered. **Two source ids per segment** (a live lane and a high-rate lane) is
honest and lets us compare decimated against dense — the same "reference instrument beside our own
estimate" pattern the launch-monitor work established. But it doubles the streams and forces the
fuser to choose between two sources carrying the same `SegmentRole`.

**Recommended: one id, stitched** — `[live prefix] + [high-rate span] + [live suffix]`, one
ascending variable-rate trace in the RAM block. Every consumer is unchanged: `interpolateImu()` is
timestamp-driven, the fuser resamples onto its grid, the exporter writes one stream.

**Nothing is lost by stitching**, provided the deferred samples are a superset of the live ones over
the same span — which is the case when the live lane is a decimation of the same underlying record.
If a supplier ever delivers a *different* measurement rather than a denser one, that is two lanes,
not one, and the choice has to be revisited.

---

## 5. Requirements this places on any deferred source

Generic, and worth stating because they are what a supplier has to provide before its data can be
folded in at all:

1. **Samples must carry host-clock timestamps.** Arrival order is meaningless for batch-retrieved
   data; the supplier is the only layer positioned to map its own clock onto ours while the data is
   being recorded.
2. **Per-sample timing uncertainty**, so we can refuse data whose alignment is worse than a camera
   frame and record the refusal, rather than silently misaligning a trace against video. One camera
   frame at 240 fps is 4.17 ms — that is the bar.
3. **A bounded, cancellable request with a caller-supplied deadline**, and an honest report of what
   was delivered versus what was asked for. A short delivery that quietly drops the backswing must
   not look like a swing that started late.
4. **A hard stop barrier on teardown** — the request must return only once no further callback can
   fire. `finishNowBlocking()` is the stop barrier for camera deselect and both destructors, and it
   must be able to join the gather. Without a supplier-side guarantee we need our own token guard.

---

## 6. Open

1. **`interpolateImu` indexing (4.2)** — should land with, or before, the first deferred source.
2. **Grid rate policy (4.3)** — derive from data; the analysis question of what rate the metrics
   need is separate and unanswered.
3. **Gather deadline and request width** — both depend on measured supplier behaviour and should be
   set from real captures, not guessed.
4. **Reserved-id ergonomics (3.3)** — confirm the resource monitor and exporter paths read cleanly
   against a ring-less id before committing to it.
