# Phase 5 (motion-adaptive smoother window) contracts — pinned by the orchestrator. Code against these; if one is wrong, say so and STOP on that item.

Repo: /Users/markliversedge/Projects/PinPointStudio (main at 9cb276b)
Background: docs/design/metric_presentation_honesty.md §5.4 and the tracker Log entries of
2026-09-05 (why a global legs scale failed: P7 samples move 3–4 σ at scale 0.1 because the hips
move fast through impact). Plan: ~/.claude/plans/buzzing-frolicking-valley.md (copied below in
spirit). Smoother facts verified: Kf3::predict(dt) builds Q(dt, m_q) with a fixed m_q; the RTS
uses the STORED Pp/dt and never recomputes Q; the RTS holds the full state x[k] (p, v, a).

## C13. The hook (owner W1) — src/Analysis/pose_smoother.{h,cpp}
- `Kf3::predict(double dt, double qScale = 1.0)` scales m_q for THAT step only (Q(dt, m_q·qScale));
  commit/rts unchanged (they read the stored Pp and dt).
- `smoothKeypoint(...)` takes an optional per-frame `const std::vector<double> *qScale` (nullptr
  or empty ⇒ 1.0 everywhere, byte-identical output). The scale for step f applies to the predict
  INTO frame f (the transition f-1 → f).
- `Kf3::rts(pOut, varOut, std::vector<double> *aOut = nullptr)` additionally returns the smoothed
  acceleration (px/s²) when asked. Nothing new is persisted.

## C14. Two policies behind dark keys (owner W1) — keys `poseSmooth.adapt.*`
PoseSmootherConfig gains `AdaptConfig adapt { mode, group, minScale, aRefPxS2, expo, leadFrames,
innovRef, innovRun }` with defaults: mode "off", group "legs" (kp 11–16; "body" = 0–16; tail
groups NEVER adapt), minScale 0.05, aRefPxS2 = tuned::pose::smoother::adapt::kARefPxS2
(placeholder 20000.0 until Phase 0 measures it — a comment says so), expo 1.0, leadFrames 3,
innovRef 4.0, innovRun 3. Read in PoseSmoothStage (wrist_analyzer.cpp) via tuning::apply exactly
like the legs keys; `fromOverrides` extended. tuned constants under
`pinpoint::tuned::pose::smoother::adapt::` with why-comments; documented in
docs/validation/tunable_parameters_reference.md.
- **accel** (two-pass, deterministic): pass 1 = today's smoother for the keypoint (with the group's
  static scales); read |a_k| = hypot(a_x, a_y) from rts's aOut per frame (0 where no smoothed
  value); s_k = clamp((|a_k| / aRef)^expo, minScale, 1.0); then a symmetric running MAX over
  ±leadFrames frames (so the scale rises ahead of the acceleration and decays after it); pass 2 =
  the smoother with qScale = s. Frames outside every segment keep 1.0.
- **innov** (single forward pass): before each predict, s = clamp(max over the last innovRun
  ACCEPTED steps of (innov²/S) / innovRef, minScale, 1.0), where innov²/S is the normalised
  innovation the gate already computes; the first innovRun steps of a segment use 1.0; a coasted
  step contributes nothing (the run is of accepted steps only).
- Both policies leave the 3σ gate, the coast budget, segmentation, the confirmed-run marking and
  the per-group static scales untouched. mode "off" ⇒ the qScale vector is never built.
- Test hook: a function `adaptScalesForTest(frames, W, H, cfg, kp) -> std::vector<double>` (or an
  optional output on PoseSmootherOutput guarded by a config flag) so tests assert the scale vector.

## C15. Gate criteria (owner W2 for the tooling; I judge)
`tools/metrics/sweep_adapt.sh <out_root> <swinglab_run> <settings-file>` runs the 11-swing subset
once per line of a settings file (each line = a JSON object of dotted keys, e.g.
{"poseSmooth.adapt.mode":"accel","poseSmooth.adapt.minScale":0.05,"poseSmooth.adapt.aRefPxS2":20000})
into <out_root>/<setting-name>/, then `series_noise.py --file result.json` per setting.
`tools/metrics/sweep_summary.py` gains `--gate CONTROL_DIR` mode printing per setting, for
pelvisSway, hipLineTilt, plumbBobDistance, leadKneeDrift, pelvisLift: (2) median and max over
swings of |ΔP4|/σ and |ΔP7|/σ versus the control (σ = that result's own persisted `sigma`; a
missing phase counts as a failure and is listed), (3) median P1–P7 excursion ratio vs control,
(4) still-address p95 jitter ratio vs control (window [Address−300 ms, Address], from
series_noise's pk_rate_address column AND a new jitter_address column — add it), (5) median σ
ratio vs control inside the domain; and a PASS/FAIL per criterion with the thresholds from the
plan (2: median < 1 σ and max < 2 σ; 3: within ±3 %; 4: ≥ 20 % reduction on the three named
series; 5: σ ratio < 1). Criterion (1) parity is run by me with parity_diff.py.

## Rules (unchanged)
Workers never build/run/commit; own only listed files; byte-identical when off; comments say WHY;
report files, tests and what each proves, contract problems with evidence, anything left undone.
