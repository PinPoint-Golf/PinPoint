# Deferred sources — folding late-arriving samples into a read-only SwingWindow

**Status:** **AS BUILT.** Written 2026-08-14 as a design note; implemented 2026-08-18 as
HackMotion Phase E, its first real consumer. Every §6 open item is closed below.
⚠ Three things in the original text were WRONG and are corrected in place — §4.3's statistic,
§4.7's single-span stitch, and §3.3's reserved-id allocator, which turned out not to be needed
at all. Each correction says what it replaced and why, because the wrong version is the one a
reader would otherwise re-derive.
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

> ⚠ **AS BUILT: NONE OF THIS WAS NEEDED, AND THE REASON IS WORTH KEEPING.** This section assumes
> a deferred source has no ring. The motivating one does: a HackMotion registers its two
> EventBuffer sources at construction and records live into them all session, and the pull is a
> DENSER VERSION OF A LANE THAT ALREADY EXISTS. So the stitched lane reuses the id it already
> has, and the composite simply routes that id to RAM instead of to the ring. No allocator, no
> ring-less identity, no special case in the exporter or the resource monitor.
>
> The reserved-id question returns the day a deferred source arrives that was NEVER live — a
> force plate, a phone-hosted sensor, a camera burst. Until then, building it would have been
> machinery with no caller, and open item 4 below is closed on that basis rather than answered.

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

> **AS BUILT — MEASURED, and it landed first and alone.** `SwingWindow` builds a per-source lane
> vector in its constructor; `interpolateImu`, `entriesFor` and `frameCount` all binary-search it.
> RelWithDebInfo on an M4, both implementations timed **in the same run on the same window** —
> see the method note below, because the first version of this measurement was not:
>
> | window shape | entries | calls | linear scan | indexed | speed-up |
> |---|---|---|---|---|---|
> | deferred (high-rate span) | 5,520 | 6,402 | 16.6-27.1 ms | 0.23-0.30 ms | **73-91×** |
> | ordinary (2 cams, 2 IMUs @100 Hz, 200 Hz grid) | 2,720 | 1,602 | 1.8-2.4 ms | 0.05-0.07 ms | **~36×** |
>
> ⚠ **THE SECOND ROW IS THE ONE WORTH NOTICING.** It contains no deferred source at all — it is
> what every swing already in the library looks like. `interpolateImu` sits on the pre-stage that
> EVERY inertial capture runs through, so this was never a high-rate optimisation: it is a fix to
> the existing pipeline that this phase happened to force. Live captures and corpus re-analysis
> both get it, with or without a HackMotion. (The ordinary row is conservative: the indexed column
> also performs the full interpolation, while the scan column only finds the bracketing pair.)
>
> `swing_window_parity_test` reports `interpolateImu: 187 agree, 0 mismatch` across the RAM and
> disk backings, so the two construction paths still agree exactly.
>
> ⚠ **METHOD NOTE, and it is the same lesson twice in one phase.** The first version of this
> measurement compared a number from the old build against a number from the new one, written down
> separately. Machine load moves these timings by a factor of three between runs — comfortably
> enough to invent or erase a speed-up. Both implementations are now timed in ONE run on ONE
> window, and the reference scan is kept in the test permanently for that purpose.
>
> ⚠ **TWO THINGS THE IMPLEMENTATION FOUND THAT THIS SECTION DID NOT SAY.**
>
> 1. **`after - 1` is the wrong `prev`.** Among entries sharing the largest timestamp `<=` target
>    the old linear scan kept the FIRST encountered; `upper_bound() - 1` is the LAST. Duplicate
>    timestamps cannot occur on the live path — the merger enforces per-source monotonicity — but
>    they are expressible on the disk and stitched paths, where the other pick silently changes
>    the interpolated value. The lane is searched with a second `lower_bound` to preserve it.
> 2. **The ascending invariant is now made true rather than assumed.** Both production paths sort,
>    but the `SwingWindow` constructor is public and a binary search over an unsorted lane fails
>    plausibly where the scan simply worked. Each lane is `stable_sort`ed at construction — a
>    no-op on already-ordered data, and `stable_` because the tie order is load-bearing per (1).
>
> ⚠ **AND A METHOD NOTE, BECAUSE IT IS THE FAILURE THIS PHASE KEEPS REPEATING.** The first
> performance gate written for this was `EXPECT_LT(us, 150'000)`, from an estimate that a linear
> scan would take "hundreds of milliseconds". It takes 14 ms — so the check meant to catch the
> regression **passed for the unfixed code**. The threshold is now derived from the two measured
> numbers. A gate whose failing case you have not measured is not a gate.

### 4.3 The master grid rate must follow the data

`ImuVisionFuser::fuse(..., double gridHz = 200.0, ...)` resamples every bound segment onto one
fixed-rate grid. Leaving it at 200 Hz would discard most of what a deferred high-rate source cost
us to retrieve; raising it globally makes every ordinary capture pay 4× for nothing.

Recommend deriving the grid rate from the bound sources' **actual median sample interval over this
window**, clamped to a sane range, rather than a constant. That keeps a webcam-plus-two-IMUs
capture exactly where it is today and lets a high-rate capture use what it has.

> ⚠ **AS BUILT — THE MEDIAN IS THE WRONG STATISTIC, AND IT WOULD HAVE THROWN AWAY THE PULL.**
>
> A stitched lane runs at ~100 Hz over a 3 s still pre-roll and up to ~799 Hz through the swing.
> Its median interval across a 4 s window is therefore dominated by the PRE-ROLL, so a
> median-derived grid lands near the base rate and discards precisely the dense span the ~4.5 s
> pull was performed to obtain. The library makes the identical point about its own `density`
> field: the block-level figure is the wrong scope, and a consumer should point the measurement
> at the sub-range it actually cares about (`history.h`, `hm_sample_step_density`).
>
> **What shipped:** `ImuVisionFuser::gridHzForWindow()` takes the **PEAK local rate**, measured by
> sliding a 250 ms probe (about a downswing) across each bound lane and taking the maximum. That
> finds the dense span wherever it is, without depending on the impact estimate being right.
> Clamped to **[200, 800] Hz**.
>
> ⚠ **THE FLOOR IS LOAD-BEARING AND IT IS NOT A ROUNDING CHOICE.** 200 Hz is what both call sites
> hardcoded before this existed. Deriving with no floor would take an ordinary Witmotion capture
> — whose lanes run at 100 Hz — DOWN to 100, changing every existing swing's metrics and moving
> the whole graded corpus underneath us. The floor never caps the device; it stops the corpus
> moving. `swing_window_parity_test` asserts `gridHzForWindow() == kGridHzMin` on an ordinary
> fixture, so a future change that moves it fails a test instead of quietly re-scoring the corpus.
>
> ⚠ **AND THE GRID MUST STAY UNIFORM.** A variable-density grid — dense through the swing, sparse
> either side — is the theoretically better answer and is ruled out: `phase_segmenter.cpp:224`
> derives its sample rate as `1e6 / (grid[1] - grid[0])`, i.e. it assumes uniform spacing. A
> non-uniform grid would hand it a wrong `fsHz` with nothing reporting an error.

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

> ⚠ **AS BUILT — THE THREE-PART PICTURE IS TOO SIMPLE, BECAUSE THE HOLED PULL IS THE NORMAL CASE.**
>
> `[live prefix] + [high-rate span] + [live suffix]` assumes the retrieval is ONE contiguous span.
> It usually is not: the device **holes** an over-wide request rather than clamping it — 33-58 %
> coverage with **no error reported** — so what comes back is several disjoint intervals with
> live-rate gaps between them.
>
> The shipped rule is therefore **per delivered interval**: take deferred samples INSIDE the
> block's `delivered[]` intervals and live samples OUTSIDE them. That is the same code for a clean
> pull and a holed one, and it fills the holes from the live lane instead of leaving them empty. A
> stitch written to the original picture would look perfect on a clean pull and silently drop the
> live samples that should have covered the holes.
>
> ⚠ **`delivered[]` IS HALF-OPEN, AND THAT IS WHERE THE OFF-BY-ONE IS BORN** — the library's own
> header says so. A sample exactly at an interval's `end_us` is OUTSIDE it and is served from
> live. `deferred_stitch.h`'s test asserts that specific instant rather than a sample count, so
> the boundary cannot drift unnoticed.
>
> ⚠ **STRICT ASCENDING IS ENFORCED LOCALLY.** These bytes never passed through the ring, so
> `MergerState::enforceMonotonicity` never saw them and the window's binary search would fail
> plausibly on a duplicate. A deferred and a live sample can legitimately land on one instant at
> an interval edge; the later one is dropped and **counted**, never silently interleaved.
>
> ⚠ **AND THE WHOLE LANE MOVES TO RAM, NOT JUST THE DENSE PART.** One `SourceId` cannot split its
> sequence space across two backings without the sequences colliding, so the stitched lane is
> served wholly from the RAM block and its ring entries are REPLACED, not added to.

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

## 6. Open — all four closed 2026-08-18 (HackMotion Phase E)

1. ~~**`interpolateImu` indexing (4.2)**~~ — **DONE, and it landed first and alone.** 14,385 µs →
   210 µs, a 68× reduction, on a window of the post-deferred shape. See the measured block in §4.2.
2. ~~**Grid rate policy (4.3)**~~ — **DONE, with the statistic corrected**: peak local rate over a
   250 ms sliding probe, clamped to [200, 800] Hz. The median this document originally recommended
   would have discarded the pull; see §4.3. ⚠ **The separate question it names remains genuinely
   open**: what rate the wrist metrics actually NEED is an analysis question, and landing the
   mechanism has not answered it.
3. ~~**Gather deadline and request width**~~ — **DONE, from the library's measured behaviour.**
   Request: §7.6's 3 s pre / 1.5 s post (the library's own default, taken by passing NULL rather
   than restating it). Gather deadline: **6 s past the pause**. ⚠ **The two deadlines are ordered
   deliberately** — the library's own deadline sits just INSIDE ours, so a slow pull materialises
   its block carrying whatever arrived, instead of being cancelled by our timeout and recording
   `CANCELLED` where the honest answer was `TIMED_OUT` with partial coverage.
4. ~~**Reserved-id ergonomics (3.3)**~~ — **CLOSED AS NOT NEEDED**, not answered. The motivating
   deferred source was already a live ring producer, so the stitched lane reuses its existing id
   and the composite reroutes it. The question returns for a deferred source that was never live.
   See the note in §3.3.

## 7. What Phase E did NOT deliver, stated plainly

- **No metric changed.** `shot_processor.cpp`'s binding loop casts to `ImuInstance*` and skips
  `HmInstance`, so no HackMotion lane is bound into analysis yet. That is Phase F, gated behind
  the `FusedStreams::streamFor` fix. A HackMotion-only capture still halts with "no IMU and no
  pose data in window" — expected, not a defect.
- **What rate the wrist metrics need** is still unanswered (open item 2 above).
- **Wire-byte recording** — `HM_BUILD_RECORD` is forced on and `hackmotion_record` is linked, but
  nothing calls it and no recording is ever opened, so every app-driven calibration has discarded
  its payload. Deliberately left out of this phase and recorded as a follow-up rather than drifted
  past.
