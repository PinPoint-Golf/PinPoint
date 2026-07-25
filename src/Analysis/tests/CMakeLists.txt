# CTest suite for the M1 shot-analyzer (spans src/Analysis + Export persistence +
# the IMU driver stack + ShaftTracker). Self-contained — NOT part of the app
# build. Runs standalone or under the tests/ umbrella; shared infra (Qt prefix,
# Eigen via pp_find_eigen, sanitizers, pp_add_test) lives in tests/cmake/PinPointTests.cmake.
#
#   cmake -S src/Analysis/tests -B build/analyzer-tests
#   cmake --build build/analyzer-tests -j && ctest --test-dir build/analyzer-tests --output-on-failure
#
# The tests are self-contained (own main() + CHECK_NEAR; no googletest). They
# compile the handful of source .cpp directly — there is no pinpoint_analysis
# library. PP_BUFFER (types.h, pulled in by swing_analysis.h) is on every test's
# include path via pp_add_test.

if(NOT COMMAND pp_add_test)
    cmake_minimum_required(VERSION 3.16)
    project(pinpoint_analyzer_tests LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../../../tests/cmake)
    include(PinPointTests)
    enable_testing()
endif()

set(ANALYSIS ${CMAKE_CURRENT_SOURCE_DIR}/..)   # src/Analysis
set(SRC      ${ANALYSIS}/..)                    # src

if(MSVC)
    # The shaft_tracker/shaft_track timing targets request -O2 (→ /O2 on cl.exe),
    # which is incompatible with the /RTC1 runtime checks a Debug configuration
    # injects (cl error D8016). Drop /RTC1 for this test suite. (_USE_MATH_DEFINES
    # is provided globally by tests/cmake/PinPointTests.cmake.)
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

# --- Pure angle math (header-only) -------------------------------------------
pp_add_test(wrist_angles_test         SOURCES wrist_angles_test.cpp)
pp_add_test(wrist_angle_sampling_test SOURCES wrist_angle_sampling_test.cpp)

# --- Wrist Motion assessment engine ------------------------------------------
# The engine .cpp pulls in the rule registry, so every engine-linked test
# compiles both.
set(WRIST_ENGINE ${ANALYSIS}/reference_bands.cpp
                 ${ANALYSIS}/wrist_assessment_engine.cpp
                 ${ANALYSIS}/assessment_rules.cpp)

pp_add_test(reference_bands_test    SOURCES reference_bands_test.cpp ${ANALYSIS}/reference_bands.cpp)
pp_add_test(archetype_bands_test    SOURCES archetype_bands_test.cpp ${WRIST_ENGINE})
pp_add_test(tier1_banding_test      SOURCES tier1_banding_test.cpp   ${WRIST_ENGINE})
pp_add_test(wrist_analysis_adapter_test
    SOURCES wrist_analysis_adapter_test.cpp ${ANALYSIS}/wrist_analysis_adapter.cpp ${WRIST_ENGINE})
pp_add_test(fault_rules_test        SOURCES fault_rules_test.cpp        ${WRIST_ENGINE})
pp_add_test(strengths_test          SOURCES strengths_test.cpp          ${WRIST_ENGINE})
pp_add_test(relationship_rules_test SOURCES relationship_rules_test.cpp ${WRIST_ENGINE})
pp_add_test(confidence_test         SOURCES confidence_test.cpp         ${WRIST_ENGINE})
pp_add_test(composite_score_v2_test SOURCES composite_score_v2_test.cpp ${WRIST_ENGINE})

# --- Header-only math contracts ----------------------------------------------
# Kinematic-sequence reduction (kinematic_sequence.h) — peak-of-largest-magnitude
# selection, ordering by peak time + inter-node gaps, empty-series drop. The
# degrade-gracefully backing for the dashboard Sequence zone (ChartMetrics::
# sequenceNodes façade). Header-only, Qt-only (QString), no fixture.
pp_add_test(kinematic_sequence_test SOURCES kinematic_sequence_test.cpp)

# Dashboard band-rail reductions (dashboard_reductions.h) — phase-keyed sample↔
# corridor join, the value→y domain (one-sided speeds + the always-present 0
# line), and playhead interpolation with endpoint clamping. The backing for
# PpBandRail (ChartMetrics::railCheckpoints/railRange/valueAtUs façade).
pp_add_test(dashboard_reductions_test SOURCES dashboard_reductions_test.cpp)
pp_add_test(imu_calibration_test  SOURCES imu_calibration_test.cpp)   # A·q·M solve + keystone golden
pp_add_test(live_wrist_angles_test SOURCES live_wrist_angles_test.cpp) # live tick() math contract
pp_add_test(imu_sample_test       SOURCES imu_sample_test.cpp)         # exported swing.json vector frame
pp_add_test(viz_frame_test        SOURCES viz_frame_test.cpp)         # worldToScene + ArmVizView bone math

# --- Segmentation v2/v3 + metric extraction ----------------------------------
pp_add_test(phase_signals_test
    SOURCES phase_signals_test.cpp ${ANALYSIS}/phase_signals.cpp)
pp_add_test(phase_segmenter_test
    SOURCES phase_segmenter_test.cpp ${ANALYSIS}/phase_segmenter.cpp ${ANALYSIS}/phase_signals.cpp)

# Phase segmenter + metric extractor over a synthetic swing. metric_extractor.cpp
# carries resource-profiler instrumentation, so it pulls in the profiler core
# (pp_profiler/pp_os_metrics/PpStatsLog) and a PpLogStream (imu_test_stubs).
# imu_test_stubs.cpp pulls event_buffer.h → swing_window.h (std::span) → C++20.
pp_add_test(pipeline_test
    SOURCES pipeline_test.cpp
            ${ANALYSIS}/phase_segmenter.cpp
            ${ANALYSIS}/phase_signals.cpp
            ${ANALYSIS}/metric_extractor.cpp
            ${SRC}/Core/pp_profiler.cpp
            ${SRC}/Core/pp_os_metrics.cpp
            ${SRC}/Core/PpStatsLog.cpp
            imu_test_stubs.cpp
    INCLUDE ${SRC}/Core   # pp_debug.h for the stub
    STD 20)

# --- Banded weighted-geometric-mean scorer (Swing/GRF adherence) -------------
pp_add_test(swing_scorer_test SOURCES swing_scorer_test.cpp ${ANALYSIS}/swing_scorer.cpp)

# --- Per-archetype resemblance scorer + uncertainty interval (Wrist, §B.0a/§B.7)
pp_add_test(wrist_resemblance_test SOURCES wrist_resemblance_test.cpp
    ${ANALYSIS}/wrist_resemblance.cpp ${ANALYSIS}/score_uncertainty.cpp)

# --- Tunable-parameter plumbing: frozen-constant parity + dotted-key overrides
pp_add_test(tuned_constants_parity_test SOURCES tuned_constants_parity_test.cpp)

# WB1 person-crop geometry (pose_crop.h): union bbox → margin → 3:4 aspect → clamp
# → integer pixels, the full-frame fallback guards, and fromOverrides plumbing.
pp_add_test(pose_crop_test SOURCES pose_crop_test.cpp)
pp_add_test(tuning_overrides_test SOURCES tuning_overrides_test.cpp ${ANALYSIS}/swing_scorer.cpp ${ANALYSIS}/reference_bands.cpp)

# Capability-gated stage pipeline (analysis_stage.h): authored-order orchestration,
# canRun/skipReason gating, the halted short-circuit, product gating between stages via
# the shared AnalysisContext, and CaptureCapabilities::fromJob/hasRole/hasRoles. Header-only
# — the transitive shot_analyzer.h/swing_analysis.h/imu_vision_fuser.h/pose_runner.h
# includes are type-only on this path (fake stages, window = nullptr), so no extra .cpp.
pp_add_test(analysis_stage_test SOURCES analysis_stage_test.cpp)

# Analysis → profiler bridge (analysis_profiling.{h,cpp}): feeds ctx.trace into
# the resource-profiler scopes (Analysis.Stage.*) + the per-run AnalysisProfileLog
# ring. Links the profiler core (pp_profiler/pp_os_metrics/PpStatsLog +
# AnalysisProfileLog) and a PpLogStream (imu_test_stubs) — same provisioning as
# pipeline_test; imu_test_stubs → event_buffer.h (std::span) forces C++20.
pp_add_test(analysis_profiling_test
    SOURCES analysis_profiling_test.cpp
            ${ANALYSIS}/analysis_profiling.cpp
            ${SRC}/Core/pp_profiler.cpp
            ${SRC}/Core/pp_os_metrics.cpp
            ${SRC}/Core/PpStatsLog.cpp
            ${SRC}/Core/AnalysisProfileLog.cpp
            imu_test_stubs.cpp
    INCLUDE ${SRC}/Core
    STD 20)

# WB2 head tracking (head_track.{h,cpp}): conf-gated head centre/scale/tilt from the
# COCO-WholeBody head keypoints + Address-referenced sway/lift/tilt series (isotropic
# ×frame units, gap-bridged resample). Qt-only (QPointF/MetricSeries via
# swing_analysis.h), no OpenCV; synthetic track, no fixture.
pp_add_test(head_track_test SOURCES head_track_test.cpp ${ANALYSIS}/head_track.cpp)

# Club-kinematics display series (kinematic_series.{h,cpp}): clubhead/hand speed (mph)
# from the shaft track + lead-forearm-vs-shaft lag (°) from pose. Qt-only, synthetic
# constant-velocity tracks (exact mph scale + lag geometry) and the product-absent
# omission contract; no OpenCV, no fixture.
pp_add_test(kinematic_series_test SOURCES kinematic_series_test.cpp ${ANALYSIS}/kinematic_series.cpp)

# WB3 setup + footwork metrics (foot_metrics.{h,cpp}): conf-gated stance width /
# per-foot flare / toe-line angle (address, phaseSamples-only scalars) + the
# lead-heel-lift Address-referenced curve (isotropic ×frame, gap-bridged
# resample) — same conventions as head_track. Qt-only, no OpenCV; synthetic
# track, no fixture.
pp_add_test(foot_metrics_test SOURCES foot_metrics_test.cpp ${ANALYSIS}/foot_metrics.cpp)

# Ball position at address (ball_position.{h,cpp}): the heel-line projection, the
# order-independent median lock (leading detector mis-locks must not win), the
# ball-diameter px→mm ruler, and the refuse-don't-fabricate contract. Qt-only, no
# OpenCV — the ball-diameter constant lives in Core/pp_physical_constants.h
# precisely so this stays OpenCV-free. Synthetic tracks, no fixture.
pp_add_test(ball_position_test SOURCES ball_position_test.cpp ${ANALYSIS}/ball_position.cpp)

# Tempo metrics (tempo_metrics.{h,cpp}): the Address→Top basis (NOT the Takeaway→Top
# one the literature uses — asserted explicitly), every refusal gate, the Summary
# series shape the dashboard Verdict tile depends on, and the correlated uncertainty
# propagation in which Top dominates. Qt-only; synthetic ladders, no fixture.
pp_add_test(tempo_metrics_test SOURCES tempo_metrics_test.cpp ${ANALYSIS}/tempo_metrics.cpp)

# Metric Catalogue (metric_catalogue / manifest / providers / resolver): descriptor
# identity + metadata, per-shot availability resolution over the provider seam, and
# normative-corridor delegation to reference_bands. Qt-only; reuses wristCheckpoints()
# (wrist_analysis_adapter.h, header-only) for the Phase→P-position map; no fixture.
pp_add_test(metric_catalogue_test
    SOURCES ${SRC}/Metrics/tests/metric_catalogue_test.cpp
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${ANALYSIS}/reference_bands.cpp
    INCLUDE ${SRC}/Metrics ${ANALYSIS})

# WB4 shared hand geometry (hand_axis.h): handCentroid (byte-identical to the
# pose_runner original), computeGripAnchors (the pose.gripFromSmoothedHands
# recompute + wrist fallback) and handAxisDirection (the ShaftTracker θ prior).
# Header-only, Qt-only (QPointF via swing_analysis.h); synthetic, no fixture.
pp_add_test(hand_axis_test SOURCES hand_axis_test.cpp)

# WB4 pose-derived wrist-angle source (pose_wrist_angle_source.{h,cpp}): apparent
# camera-plane lead-wrist FE/RUD through the windowed-median sampler, the
# left-handed image mirror, gap handling, default-OFF. Qt-only (contract +
# sampler headers), no OpenCV; synthetic pose track, no fixture.
pp_add_test(pose_wrist_angle_source_test
    SOURCES pose_wrist_angle_source_test.cpp ${ANALYSIS}/pose_wrist_angle_source.cpp)

# --- Header-only review/shot helpers -----------------------------------------
pp_add_test(session_summary_test SOURCES session_summary_test.cpp)   # session_summary.h
pp_add_test(arbiter_test         SOURCES arbiter_test.cpp)           # shot_arbiter.h

# --- Unified swing.json writer/reader round-trip (Export subsystem) ----------
pp_add_test(swing_doc_test
    SOURCES ${SRC}/Export/tests/swing_doc_test.cpp ${SRC}/Export/swing_doc.cpp)

# --- Orientation filters (Phase 0.4): Madgwick (header-only) + ESKF (Eigen) ---
# The ESKF .cpp pulls the vendored imu_ekf headers through the Arduino-style
# <Eigen.h>/<Wire.h> shims in imu_ekf_compat (which MUST precede imu_ekf on the
# include path), and those need real Eigen. pp_find_eigen() resolves it.
pp_find_eigen(EIGEN_DIR)
pp_add_test(orientation_filter_test
    SOURCES orientation_filter_test.cpp ${SRC}/IMU/eskf_orientation_filter.cpp
    INCLUDE ${PP_THIRD}/imu_ekf_compat ${PP_THIRD}/imu_ekf ${EIGEN_DIR})

# Offline orientation re-fusion + parity (the post-hoc tuning substrate; corpus-1
# pre-collection gate E1). Same ESKF/Eigen deps; the refuser is header-only.
pp_add_test(orientation_refuse_test
    SOURCES orientation_refuse_test.cpp ${SRC}/IMU/eskf_orientation_filter.cpp
    INCLUDE ${SRC}/IMU ${PP_THIRD}/imu_ekf_compat ${PP_THIRD}/imu_ekf ${EIGEN_DIR})

# --- IMU driver frame-parse + WT901BLE67 eulerToQuat override (Phase 0.3) -----
# Exercises the REAL driver stack. imu_test_stubs.cpp supplies EventBuffer::nowMicros
# and PpLogStream so we link neither the Buffer library nor pp_debug.cpp. The
# Q_OBJECT driver classes need moc (AUTOMOC); std::span → C++20.
find_package(Qt6 REQUIRED COMPONENTS Bluetooth)
pp_add_test(imu_driver_frame_test
    SOURCES imu_driver_frame_test.cpp
            imu_test_stubs.cpp
            ${SRC}/IMU/wt9011dcl_ble.cpp
            ${SRC}/IMU/ble_imu_transport.cpp
            ${SRC}/IMU/wt9011dcl_base.cpp
            ${SRC}/IMU/imu_base.cpp
            ${SRC}/IMU/eskf_orientation_filter.cpp
    INCLUDE ${SRC}/IMU ${SRC}/Core
            ${PP_THIRD}/imu_ekf_compat ${PP_THIRD}/imu_ekf ${EIGEN_DIR}
    LINK    Qt6::Bluetooth
    STD 20  AUTOMOC)

# --- ShaftTracker OpenCV targets ---------------------------------------------
# OpenCV (system 4.x, core imgproc only — no contrib). Both math sources are
# Qt-free; -O2 because the suites set no build type and assert real hot-loop cost.
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)

# Shared frame decode (src/Export/frame_decode, ShaftTracker S0).
pp_add_test(frame_decode_test
    SOURCES frame_decode_test.cpp ${SRC}/Export/frame_decode.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    NO_QT)

# Shaft v3.0-r1 evidence engines (E1 discrete band + E2 polarity ridge sweep) —
# Qt-free math core (shaft_tracker_math). -O2: the suites set no build type and
# these assert real hot-loop cost.
pp_add_test(shaft_evidence_test
    SOURCES shaft_evidence_test.cpp ${ANALYSIS}/shaft_tracker_math.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2
    NO_QT)

# Shaft v3.0-r1 deciding half (phase model, geometry, emission, banded Viterbi
# DP, ψ-isotonic reconcile, tiering + the decideTrack core, which calls the E1/E2
# engines) — needs Qt (QVariantMap config) + OpenCV. ball_anchor.cpp supplies
# medianGripBallLenPx (A1), now referenced by decideTrack's length ladder.
pp_add_test(shaft_decide_test
    SOURCES shaft_decide_test.cpp ${ANALYSIS}/shaft_track_assembly.cpp ${ANALYSIS}/shaft_tracker_math.cpp ${ANALYSIS}/ball_anchor.cpp ${ANALYSIS}/clubhead_track.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# WB4 hand-axis θ prior DP unary (shaft_track_assembly addHandAxisPrior): the
# conf-gated / wrap-aware off-axis penalty, disabled ⇒ zero contribution, plus
# the shaft.handAxisPrior.* override plumbing. Same link set as shaft_decide_test.
pp_add_test(shaft_hand_axis_prior_test
    SOURCES shaft_hand_axis_prior_test.cpp ${ANALYSIS}/shaft_track_assembly.cpp ${ANALYSIS}/shaft_tracker_math.cpp ${ANALYSIS}/ball_anchor.cpp ${ANALYSIS}/clubhead_track.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# Ball anchor (v3.4): medianGripBallLenPx (median/jump-gate/stillness/window) +
# applyBallAnchor's A3 head recompute and no-op contracts — synthetic, no fixture.
pp_add_test(ball_anchor_test
    SOURCES ball_anchor_test.cpp ${ANALYSIS}/ball_anchor.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# Late-pipeline event refinement (event_refine.{h,cpp}): the three-tier at-ball
# evidence, LAST-departure/no-return takeaway (the s0002 lesson), flicker debounce,
# tier confidence + minConf/maxShift abstention, Club-provenance/version-3 mutation,
# monotone-ladder invariant, enabled=false byte-identity, swingStartUs coupling.
# Pulls ball_anchor.cpp for the shared buildThetaBallSeries; addressHoldEndFrame is
# header-only (shaft_positions.h). Same OpenCV link set as ball_anchor_test (the
# transitive shaft_track_assembly.h drags in cv:: types).
pp_add_test(event_refine_test
    SOURCES event_refine_test.cpp ${ANALYSIS}/event_refine.cpp ${ANALYSIS}/ball_anchor.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# W3 club-corridor activity helper (ball_activity.h): the annulus + per-pixel
# temporal-median math (quiet ⇒ 0, uniform-annulus diff ⇒ mean/σ, inner-disc
# exclusion, median-ref outlier robustness, the -1 absent contracts). Header-only,
# OpenCV core only, no Qt, no fixture.
pp_add_test(ball_activity_test
    SOURCES ball_activity_test.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    NO_QT)

# Persisted live-baseline blob loader (ball_baseline_io.h) — happy path + the
# truncated/missing/wrong-dims rejects that force BallRunner's self-seed fallback.
# Header-only; Qt (QFile/QTemporaryDir) + OpenCV (CV_32F Mat); HAVE_OPENCV un-guards
# the header.
pp_add_test(ball_baseline_io_test
    SOURCES ball_baseline_io_test.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    DEFINES HAVE_OPENCV)

# Clubhead Stage-2 (Phase B): the per-frame gap-tolerant on-axis terminus
# (measureHeadRadius, rayEdgeRadius, the B2 ball-length model) and the segmented
# KF + per-segment RTS temporal model (HeadKf1D, runHeadTemporal). Both synthetic
# — the measure test builds tiny CV_32F frames; the temporal test is frameless.
pp_add_test(clubhead_measure_test
    SOURCES clubhead_measure_test.cpp ${ANALYSIS}/clubhead_track.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)
pp_add_test(clubhead_temporal_test
    SOURCES clubhead_temporal_test.cpp ${ANALYSIS}/clubhead_track.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# Multi-estimator club-length fusion + persistent prior (club_length_fusion.h):
# inverse-variance fuse, leave-one-out outlier rejection, confidence, sanity
# clipping, EMA convergence, kσ gate + reset, determinism. Header-only, Qt only
# (QVariantMap for fromOverrides) — no OpenCV, no fixture.
pp_add_test(club_length_fusion_test SOURCES club_length_fusion_test.cpp)

# Layer B P-time location (shaft_positions.h): image-plane shaft/arm parallel
# crossings of θ(t)/φ(t) with hysteresis — full swing (all 8), abbreviated punch
# (no P8), NaN-φ (no P3/P5), noisy horizontal (fires once). Header-only, no fixture.
pp_add_test(shaft_positions_test SOURCES shaft_positions_test.cpp)

# Phase-1 motion-overlay pose smoother (pose_smoother.{h,cpp}): offline non-causal
# RTS smoother for the 17 COCO keypoints — a 3-state [p,v,a] white-jerk sibling of
# the clubhead HeadKf1D + runHeadTemporal, with variable dt + pixel-domain +
# coast-budget-in-time segmentation. Qt-only (QPointF via swing_analysis.h), no
# OpenCV; synthetic noisy circular arc, no fixture.
pp_add_test(pose_smoother_test SOURCES pose_smoother_test.cpp ${ANALYSIS}/pose_smoother.cpp)

# Layer C synthesis (shaft_synthesis.h): C¹ Hermite interpolation between P-anchors
# — C⁰ continuity at anchors, conf decay shape, 200° unwrap onto the track branch,
# strictly-between emission window, absent-anchor subsets. Header-only, Qt (QPointF/
# ShaftSample2D) only — no OpenCV, no fixture.
pp_add_test(shaft_synthesis_test SOURCES shaft_synthesis_test.cpp)

# Motion-overlay pose synthesis (pose_synthesis.h): C¹ Hermite / Catmull-Rom
# temporal upsampling of the smoothed skeleton — endpoint C⁰, linear reproduction,
# Off-joint stays Off, window/monotonic, disabled/degenerate. Header-only, Qt
# (QPointF/PoseFrame2D) only — no OpenCV, no fixture.
pp_add_test(pose_synthesis_test SOURCES pose_synthesis_test.cpp)

# Layer B milestone fitter (shaft_position_fit.h): ±k-frame shift-and-stack +
# joint (grip,θ,L) fit over the shared ridge integral — still P1, blur-gap P7 with
# a 180°-flipped init recovered via wide sector + ball, pure-noise reject,
# determinism. Header-only (inline warpAffine); Qt (QPointF) + OpenCV, no fixture.
pp_add_test(shaft_position_fit_test
    SOURCES shaft_position_fit_test.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# Stage A true-onset segmentation (swing_span_bounding_plan.md §4/§7.1): the bs0
# walk-back (A1 speed + A2 φ), the impact clamp (A3), the swLow=0 legacy path,
# and the estimateSwingSpanUs Stage B helper — synthetic grip tracks, no fixture.
# ball_anchor.cpp resolves medianGripBallLenPx, referenced from shaft_track_assembly.
pp_add_test(swing_onset_test
    SOURCES swing_onset_test.cpp ${ANALYSIS}/shaft_track_assembly.cpp ${ANALYSIS}/shaft_tracker_math.cpp ${ANALYSIS}/ball_anchor.cpp ${ANALYSIS}/clubhead_track.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# Stage-A "no-return" veto (W1) + additive vision Takeaway event (W2): the
# fidget-proof onset (a club bob that departs and returns no longer drags the
# onset back through the fidget) and the shaft.emitTakeaway ladder. Same link set
# as swing_onset_test (segmentPhases + phasesToSegmentation are pure).
pp_add_test(shaft_onset_test
    SOURCES shaft_onset_test.cpp ${ANALYSIS}/shaft_track_assembly.cpp ${ANALYSIS}/shaft_tracker_math.cpp ${ANALYSIS}/ball_anchor.cpp ${ANALYSIS}/clubhead_track.cpp
    INCLUDE ${OpenCV_INCLUDE_DIRS}
    LINK    ${OpenCV_LIBS}
    COMPILE_OPTIONS -O2)

# shaft_parity_test (numeric parity vs the Python exemplar) was retired
# 2026-07-09: the C++ tracker is the development reference and has diverged
# (onset v2). Parity was the port-time acceptance gate only; regression
# baselines are now C++-vs-C++ (SwingLab runs on recorded corpora).
