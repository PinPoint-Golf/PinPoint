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
# reference_bands.cpp now hosts NormBandProvider, so it pulls in the norm stack (the corridors it
# used to compile in are content). Every test that links the wrist engine needs these, and needs
# PINPOINT_CORE_NORMS / PINPOINT_CORE_CONTEXTS to find the shipped files — see pp_norm_env below.
# reference_bands.h now pulls Diagnostics -> Metrics -> Analysis transitively (metric_reducer.h
# includes swing_analysis.h non-relatively), so any test touching it needs all three on the path.
set(NORM_INC ${SRC}/Diagnostics ${SRC}/Metrics ${ANALYSIS})
set(NORM_SRC ${SRC}/Diagnostics/norm_pack.cpp ${SRC}/Diagnostics/context_tree.cpp
             ${SRC}/Diagnostics/characteristic_pack.cpp ${SRC}/Diagnostics/characteristic.cpp
             # The pack loader resolves a citation against the bibliography to decide whether a
             # peer tier is resting on a BOOK, so every target that compiles characteristic_pack.cpp
             # now links reference_pack.cpp too. Where no registry is on the path the shared set is
             # empty, nothing resolves, and the gate is a no-op — the link is the only requirement.
             ${SRC}/Diagnostics/reference_pack.cpp
             ${SRC}/Diagnostics/measure_facets.cpp ${SRC}/Diagnostics/anatomy_vocabulary.cpp
             ${SRC}/Diagnostics/resource_norm_provider.cpp
             ${SRC}/Diagnostics/file_norm_provider.cpp
             ${SRC}/Diagnostics/merged_norm_provider.cpp)

# The shipped norm set and context tree live in Qt resources, which exist only inside the app
# binary. Every standalone test that can reach NormBandProvider is pointed at the reviewable JSON.
function(pp_norm_env target)
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT "PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json")
endfunction()

set(WRIST_ENGINE ${ANALYSIS}/reference_bands.cpp
                 ${ANALYSIS}/wrist_assessment_engine.cpp
                 ${ANALYSIS}/assessment_rules.cpp
                 ${NORM_SRC})

pp_add_test(reference_bands_test    SOURCES reference_bands_test.cpp ${ANALYSIS}/reference_bands.cpp ${NORM_SRC}
    INCLUDE ${NORM_INC})
pp_add_test(tier1_banding_test      SOURCES tier1_banding_test.cpp   ${WRIST_ENGINE} INCLUDE ${NORM_INC})
pp_add_test(wrist_analysis_adapter_test
    SOURCES wrist_analysis_adapter_test.cpp ${ANALYSIS}/wrist_analysis_adapter.cpp ${WRIST_ENGINE}
    INCLUDE ${NORM_INC})
pp_add_test(fault_rules_test        SOURCES fault_rules_test.cpp        ${WRIST_ENGINE} INCLUDE ${NORM_INC})
pp_add_test(strengths_test          SOURCES strengths_test.cpp          ${WRIST_ENGINE} INCLUDE ${NORM_INC})
pp_add_test(relationship_rules_test SOURCES relationship_rules_test.cpp ${WRIST_ENGINE} INCLUDE ${NORM_INC})
pp_add_test(confidence_test         SOURCES confidence_test.cpp         ${WRIST_ENGINE} INCLUDE ${NORM_INC})
pp_add_test(composite_score_v2_test SOURCES composite_score_v2_test.cpp ${WRIST_ENGINE} INCLUDE ${NORM_INC})

foreach(t reference_bands_test tier1_banding_test wrist_analysis_adapter_test
          fault_rules_test strengths_test relationship_rules_test confidence_test
          composite_score_v2_test)
    pp_norm_env(${t})
endforeach()

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
pp_add_test(tuning_overrides_test SOURCES tuning_overrides_test.cpp ${ANALYSIS}/swing_scorer.cpp
                                          ${ANALYSIS}/reference_bands.cpp ${NORM_SRC}
                                  INCLUDE ${NORM_INC})
pp_norm_env(tuning_overrides_test)

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

# Lower-body frontal-plane metrics (lower_body_metrics.{h,cpp}): the hip-referenced
# lead-knee drift and why it is a difference, the lead-positive sign resolved from
# the address geometry rather than assumed, the absolute hip-line tilt, and the
# stance-span denominator floor. Reads COCO body 11–16 only, so it also pins that
# a legacy 17-kp track produces a full set. Qt-only, no OpenCV; synthetic track.
pp_add_test(lower_body_metrics_test SOURCES lower_body_metrics_test.cpp
            ${ANALYSIS}/lower_body_metrics.cpp)

# Upper-body frontal-plane metrics (upper_body_metrics.{h,cpp}): the trail-positive
# axis tilt against the lead-positive everything-else, spine side bend as the
# shoulder line AGAINST the hip line (so a whole-body lean reads zero), the
# absolute — not address-referenced — thorax drift the pack's Delta reducer needs,
# and the denominator floor. Every sign is asserted a second time through a
# MIRRORED camera with a left-handed golfer, which is what a synthetic track can
# pin and a corpus cannot. Links anatomy_vocabulary.cpp: this producer resolves
# points and segments through the vocabulary rather than spelling keypoint indices,
# so the geometry it measures is the geometry the content facets name.
pp_add_test(upper_body_metrics_test SOURCES upper_body_metrics_test.cpp
            ${ANALYSIS}/upper_body_metrics.cpp ${SRC}/Diagnostics/anatomy_vocabulary.cpp)

# Axial body rotation (body_rotation.{h,cpp}): the per-segment tier choice (a bound
# Pelvis/Thorax stream beats the camera, independently for each), the cosine
# inversion the camera tier is built on, and the MAGNITUDE convention — positive at
# the top AND positive at impact, which is the only reading consistent with the
# shipped corridors and the thing that would grade every downswing backwards if it
# were wrong. Qt-only; synthetic pose tracks and synthetic quaternion streams.
pp_add_test(body_rotation_test SOURCES body_rotation_test.cpp
            ${ANALYSIS}/body_rotation.cpp)

# Face-on club delivery (club_delivery.{h,cpp}): past-parallel / short-of-parallel
# sign, upward-is-positive attack angle, the sub-frame parabola vertex the low point
# needs (a whole frame is inches at impact speeds), and — the one that matters most —
# that a PROJECTED clubhead produces nothing at all rather than something plausible.
pp_add_test(club_delivery_test SOURCES club_delivery_test.cpp
            ${ANALYSIS}/club_delivery.cpp)

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
            ${ANALYSIS}/reference_bands.cpp ${NORM_SRC}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})
pp_norm_env(metric_catalogue_test)

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

# ── Diagnostics: swing-characteristics pack (src/Diagnostics) ─────────────────
# Authoring/maintenance surface for the diagnostic rule library. Pure C++, no
# Qt-GUI, and deliberately NOT wired into the live analysis path — the engine
# runs against synthetic input only (see docs/design/swing_characteristics_impl_plan.md).
set(DIAG ${SRC}/Diagnostics)
# The shipped diagnostics content moved to src/Resources/diagnostics/ (alongside the icons
# resource) when norms and the context tree joined the pack. Tests read the reviewable JSON
# directly — the Qt resource exists only inside the app binary.
set(DIAGRES ${SRC}/Resources/diagnostics)

# Anatomy vocabulary: role metadata, handedness resolution both ways, derived
# midpoints, both pose layouts (17-kp live / 133-kp wholebody), and the hard
# guarantee that no admitted role reaches the face (23-90) or finger (91-132)
# ranges. Qt-only (QPointF), no fixture.
pp_add_test(anatomy_vocabulary_test
    SOURCES ${DIAG}/tests/anatomy_vocabulary_test.cpp ${DIAG}/anatomy_vocabulary.cpp
    INCLUDE ${DIAG})

# Vocabulary <-> skeleton-overlay parity. Pins the shared resolver to what
# video_overlay_pose.cpp draws today (neck/pelvis midpoint formulas, the 0.25
# admission threshold, min-of-parents confidence, and the bone topology that is
# hand-duplicated in PpCameraFrame.qml's kBlueprintBones), so re-pointing the
# overlay at the resolver is provably a no-op and drift on either side fails.
pp_add_test(anatomy_overlay_parity_test
    SOURCES ${DIAG}/tests/anatomy_overlay_parity_test.cpp ${DIAG}/anatomy_vocabulary.cpp
    INCLUDE ${DIAG})

# Measure facets: the validity table indexed on (what-class, quantity,
# reference-class), deterministic canonical naming across series AND reducer,
# and structural duplicate / near-duplicate detection on the series tuple.
pp_add_test(measure_facets_test
    SOURCES ${DIAG}/tests/measure_facets_test.cpp ${DIAG}/measure_facets.cpp ${DIAG}/anatomy_vocabulary.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})

# Pack schema, loader and validator. Every rejection path is exercised: a validator whose
# negative cases are untested is a validator that passes everything.
set(DIAG_PACK ${DIAG}/characteristic_pack.cpp ${DIAG}/characteristic.cpp
              # See NORM_SRC above: the loader's book-citation tier gate resolves through the
              # bibliography, so the two travel together on every link line.
              ${DIAG}/reference_pack.cpp
              ${DIAG}/measure_facets.cpp ${DIAG}/anatomy_vocabulary.cpp)

pp_add_test(characteristic_pack_test
    SOURCES ${DIAG}/tests/characteristic_pack_test.cpp ${DIAG_PACK}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})

# The SHIPPED seed pack (packs/core.json). Asserts the cause-coverage concentration, the
# structural edge-orientation check (which a coverage count cannot make — totals are identical
# under edge reversal), the capture-gap classification, and that no commercial brand has leaked
# into the content.
#
# PINPOINT_CORE_REFERENCES is DELIBERATELY absent from this target's environment, so the shared
# bibliography stays empty here. With the registry live, loadPack() would DEMOTE a book citation
# sitting at a peer tier before this test ever looked at it, and the shipped-content check would
# pass by watching the fix rather than the fault. It reads references.json itself, through
# PP_CORE_REFERENCES_PATH, precisely so the validator and the content are gated separately.
pp_add_test(core_pack_test
    SOURCES ${DIAG}/tests/core_pack_test.cpp ${DIAG_PACK} ${DIAG}/norm_pack.cpp ${DIAG}/context_tree.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS}
    COMPILE_OPTIONS -DPP_CORE_PACK_PATH="${DIAGRES}/core.json"
                    -DPP_CORE_NORMS_PATH="${DIAGRES}/norms.json"
                    -DPP_CORE_REFERENCES_PATH="${DIAGRES}/references.json")

# Detection + explanation passes, DARK: synthetic measure source only, never invoked on a real
# swing. Asserts the rule the whole library depends on — an absent measure resolves Unavailable and
# never as a pass — plus greedy explanation ranking, the "a leaf is never a root" rule, and that
# Behavioural causes are offered but never concluded.
# context_tree.cpp joined the link line at stage 7: detect() resolves each condition's context
# bindings, so a condition switched off for this kind of shot is never asked rather than reported
# as absent.
pp_add_test(characteristic_engine_test
    SOURCES ${DIAG}/tests/characteristic_engine_test.cpp
            ${DIAG}/relation_resolver.cpp ${DIAG}/characteristic_engine.cpp
            ${DIAG}/context_tree.cpp ${DIAG_PACK}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})

# Qml for the QML_ELEMENT façades under test (ModelBrowser is a QObject with Q_PROPERTY/Q_INVOKABLE,
# so those targets also need AUTOMOC).
find_package(Qt6 REQUIRED COMPONENTS Qml)
# Quick, for inspector_refresh_test: it counts the delegates a Repeater actually builds, which is
# the one link in the panel's refresh chain that cannot be checked by reading a map.
find_package(Qt6 REQUIRED COMPONENTS Quick)
# Concurrent: ModelBrowser scans the swing library on a worker rather than stalling the
# GUI thread for the seconds a first pass over a real library costs.
find_package(Qt6 REQUIRED COMPONENTS Concurrent)
set(CMAKE_AUTOMOC ON)


# Cross-registry integrity: the diagnostics pack vs the metric catalogue. The catalogue's `usedBy`
# is hand-authored and will otherwise rot silently — a wrong reverse index is worse than an absent
# one because it looks authoritative. Checked in BOTH directions, plus that no measure claims a
# producer the catalogue does not have.
pp_add_test(diagnostics_catalogue_integrity_test
    SOURCES ${DIAG}/tests/diagnostics_catalogue_integrity_test.cpp ${DIAG_PACK}
            ${DIAG}/screen_pack.cpp ${DIAG}/drill_pack.cpp ${DIAG}/reference_pack.cpp
            ${SRC}/Gui/diagnosticmodel/model_browser.cpp
            ${DIAG}/dag_layout.cpp ${DIAG}/corridor_plot.cpp
            ${DIAG}/memory_pack_provider.cpp ${DIAG}/memory_norm_provider.cpp
            # The model's health list spans the norm set now, and its corpus check reads phase grids.
            ${DIAG}/diagnostics_health.cpp ${DIAG}/screen_pack.cpp ${DIAG}/drill_pack.cpp ${DIAG}/measure_sample.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${ANALYSIS}/reference_bands.cpp ${DIAG}/norm_pack.cpp ${DIAG}/context_tree.cpp
            ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS} ${SRC}/Gui/diagnosticmodel
    LINK    Qt6::Qml Qt6::Concurrent
    COMPILE_OPTIONS -DPP_CORE_PACK_PATH="${DIAGRES}/core.json")
# ModelBrowser assembles the pack through the provider, which normally reads the Qt resource — absent
# from a standalone binary. Point it at the reviewable JSON instead. PP_DIAG_QML_DIR is the panel
# whose Connections blocks are joined against the model's signals.
target_compile_definitions(diagnostics_catalogue_integrity_test PRIVATE
    PP_DIAG_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../Gui/diagnosticmodel")
set_tests_properties(diagnostics_catalogue_integrity_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json;PINPOINT_CORE_SCREENS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/screens.json;PINPOINT_CORE_DRILLS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/drills.json;PINPOINT_CORE_REFERENCES=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/references.json")

# The ASSEMBLED-LIBRARY health checks: the ones no single validator can make, because they span the
# pack, the norm set, the context tree and the metric catalogue at once. Run against the SHIPPED
# content as well as synthetic fixtures — a check that cannot fire on real content is a check nobody
# will notice is broken. Also gates the two rules that are easy to get backwards: the personal-layer
# scoping (unscoped, the n=0 check opens with 39 items of noise about migrated rows) and the
# single-tail axis, which must NOT be reported.
pp_add_test(diagnostics_health_test
    SOURCES ${DIAG}/tests/diagnostics_health_test.cpp ${DIAG}/diagnostics_health.cpp ${DIAG}/screen_pack.cpp ${DIAG}/drill_pack.cpp ${DIAG_PACK}
            ${DIAG}/norm_pack.cpp ${DIAG}/context_tree.cpp
            ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${ANALYSIS}/reference_bands.cpp
            ${DIAG}/screen_pack.cpp ${DIAG}/drill_pack.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})
set_tests_properties(diagnostics_health_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json;PINPOINT_CORE_SCREENS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/screens.json;PINPOINT_CORE_DRILLS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/drills.json")

# The two reference registries a characteristic points at: the physical screens behind `screenRef`
# and the drills behind `drills`. Both joins are EXACT STRING MATCHES into a separate file, so a
# typo fails silently — the condition still loads, still validates, still renders, and the panel
# that should tell a coach how to run the test is simply blank. This gates the shipped content in
# both directions and the validators in both, firing and staying silent.
pp_add_test(reference_sets_test
    SOURCES ${DIAG}/tests/reference_sets_test.cpp
            ${DIAG}/screen_pack.cpp ${DIAG}/drill_pack.cpp ${DIAG}/reference_pack.cpp ${DIAG_PACK}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})
set_tests_properties(reference_sets_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_SCREENS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/screens.json;PINPOINT_CORE_DRILLS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/drills.json;PINPOINT_CORE_REFERENCES=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/references.json")

# The causal DAG's layout. Everything about that view that can be wrong in a way a screenshot would
# not show: two boxes on top of each other (a hidden node reads as a graph that lacks it), a picture
# that reshuffles between visits, a depth bound that silently drops what it cut, and the empty state
# — a characteristic connected to nothing, which is every freshly authored one.
pp_add_test(dag_layout_test
    SOURCES ${DIAG}/tests/dag_layout_test.cpp ${DIAG}/dag_layout.cpp ${DIAG_PACK}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})

# ── Norms ───────────────────────────────────────────────────────────────────
set(DIAG_NORM ${DIAG}/norm_pack.cpp ${DIAG}/context_tree.cpp
              ${DIAG}/characteristic_pack.cpp ${DIAG}/characteristic.cpp
              ${DIAG}/reference_pack.cpp            # same pairing as DIAG_PACK
              ${DIAG}/measure_facets.cpp ${DIAG}/anatomy_vocabulary.cpp)

# The grade rule. Asserts z is computed per side (an asymmetric norm grades asymmetrically) and
# that an explicit monitor band dominates the z-derived outer edge in BOTH directions — the rule
# that makes the corridors migrated out of reference_bands.cpp reproduce exactly.
pp_add_test(norm_test
    SOURCES ${DIAG}/tests/norm_test.cpp ${DIAG}/norm_pack.cpp ${DIAG}/characteristic_pack.cpp
            ${DIAG}/characteristic.cpp ${DIAG}/measure_facets.cpp ${DIAG}/anatomy_vocabulary.cpp
            ${DIAG}/reference_pack.cpp   # spelled out rather than DIAG_NORM; keep the pairing
            ${DIAG}/context_tree.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})

# Upward resolution through the context tree, plus the rule that an UNKNOWN context resolves to
# nothing rather than silently falling back to full_swing. Also gates the shipped contexts.json.
pp_add_test(context_tree_test
    SOURCES ${DIAG}/tests/context_tree_test.cpp ${DIAG_NORM}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS}
    COMPILE_OPTIONS -DPP_CONTEXTS_PATH="${DIAGRES}/contexts.json")

# Norm persistence, validation and layering. The load-bearing assertion is that CONTEXT SPECIFICITY
# BEATS LAYER PRECEDENCE: a user adjusting the general full-swing norm must not silently override
# every club-specific shipped norm beneath it.
pp_add_test(norm_pack_test
    SOURCES ${DIAG}/tests/norm_pack_test.cpp ${DIAG_NORM}
            ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})
# The shipped norm set and context tree live in Qt resources, absent from a standalone binary —
# same seam as PINPOINT_CORE_PACK.
set_tests_properties(norm_pack_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json")

# Reading a MEASURE off a stored swing — the half of the value/norm join that nothing implemented
# until the corridor editor needed real numbers under its histogram. The load-bearing assertions are
# that an unsegmented phase yields NOTHING (never a nearest-sample guess a sidecar would then cache),
# that Extremum finds a peak no endpoint sees, and that an anchored Extremum is the SIGNED deviation.
pp_add_test(measure_sample_test
    SOURCES ${DIAG}/tests/measure_sample_test.cpp ${DIAG}/measure_sample.cpp ${DIAG_NORM}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})

# Every corridor signal points the way its own condition claims. An inverted signal does not fail
# loudly — it fires on the wrong swings with correct-sounding text attached — so the fixture states
# the expected direction and the catalogue quote that decides it. A new signal with no fixture row
# fails, which is what stops the audit needing to be re-run by hand.
pp_add_test(axis_direction_test
    SOURCES ${DIAG}/tests/axis_direction_test.cpp ${DIAG_PACK}
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS}
    COMPILE_OPTIONS -DPP_CORE_PACK_PATH="${DIAGRES}/core.json")

# The join that lights the pack up: values + norms -> a graded reading, and the engine's corridor
# test on top of it. Asserts the three silent failures — firing on ordinary variation, grading an
# unknown context against the default, and an absent norm reading as a pass.
pp_add_test(norm_measure_source_test
    SOURCES ${DIAG}/tests/norm_measure_source_test.cpp ${DIAG}/characteristic_engine.cpp
            ${DIAG_NORM} ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp
    INCLUDE ${NORM_INC})

# The stage-2 migration gates (reference_bands_parity_test, wrist_render_parity_test) are GONE. They
# compared NormBandProvider against the compiled band table, and both were deleted at stage 9
# together with that table — a parity test outliving what it compares against pins the shipped norms
# to a frozen set of numbers, so the first legitimate corpus re-seat of a corridor would fail a gate
# nobody wanted (ledger C5). What was permanent in them moved: the projection rules and the
# grade↔RAG agreement sweep into reference_bands_test, the "an empty norm set greys the whole grid"
# guard into wrist_norm_render_test below.
#
# The engine rendered from the SHIPPED norm set — real bands, all three RAGs reached, findings
# produced — and the inverse that makes those assertions a gate: with no norm source every cell greys,
# which is exactly how a failed content read presents at runtime.
pp_add_test(wrist_norm_render_test
    SOURCES ${DIAG}/tests/wrist_norm_render_test.cpp ${WRIST_ENGINE}
    INCLUDE ${NORM_INC})
pp_norm_env(wrist_norm_render_test)

# Stage 9's own gate: the corridors the metric surfaces used to get from the manifest now come from
# the norm set through corridorForMetricAtPhase(), and nothing a surface drew went missing. Carries
# the last migration PIN in the suite — the four tempo numbers, which retire with ledger C9.
# A5's gate: the seed conversion, asserted on the SHIPPED content. m_smashFactor is the first and
# only one-sided measure PinPoint ships, so this is where the shapes machinery meets real numbers —
# which band a golfer's reading lands in, and which side of the corridor is open. Pins the shape of
# the answer and one physical cap, never a fitted mu or sigma: those are heuristics and are meant to
# move when a corpus re-seats them.
pp_add_test(seed_conversion_test
    SOURCES ${DIAG}/tests/seed_conversion_test.cpp
            ${DIAG_NORM} ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})
set_tests_properties(seed_conversion_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json")

pp_add_test(manifest_migration_test
    SOURCES ${DIAG}/tests/manifest_migration_test.cpp
            ${DIAG_NORM} ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
    INCLUDE ${DIAG} ${SRC}/Metrics ${ANALYSIS})
set_tests_properties(manifest_migration_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json")


# The Diagnostic Model panel's facade. QML holds no rules, so every rule it hides is asserted
# here or nowhere. The load-bearing ones are the working copy (an unsaved edit has to be VISIBLE, or
# editing looks like nothing happening),
# undo as the inverse of copy-on-write (undoing a first edit to shipped content IS the reset), a
# bulk-set being ONE command, and legality being decided by the same function the graph drag asks.
#
# Writes go to QStandardPaths::AppDataLocation, so XDG_DATA_HOME is isolated: a suite that wrote into
# the developer's own diagnostics pack would be a test with a side effect on the product.
pp_add_test(model_browser_test
    SOURCES ${DIAG}/tests/model_browser_test.cpp
            ${SRC}/Gui/diagnosticmodel/model_browser.cpp
            ${DIAG}/corridor_plot.cpp ${DIAG}/measure_sample.cpp
            ${DIAG}/dag_layout.cpp ${DIAG}/diagnostics_health.cpp
            ${DIAG}/drill_pack.cpp ${DIAG}/screen_pack.cpp ${DIAG}/reference_pack.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp ${DIAG}/memory_pack_provider.cpp
            ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp ${DIAG}/memory_norm_provider.cpp ${DIAG_NORM}
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${ANALYSIS}/reference_bands.cpp
    INCLUDE ${NORM_INC} ${SRC}/Gui/diagnosticmodel
    LINK    Qt6::Qml Qt6::Concurrent
    COMPILE_OPTIONS -DPP_CORE_PACK_PATH="${DIAGRES}/core.json")
# The screen, drill and reference registries need their env overrides too: this panel reads all
# three ALONGSIDE the pack (a screen is only interesting for what it would settle), and without them
# the assembled library reports every screenRef and drill id as dangling — which would make the
# validation-strip assertions below pass for entirely the wrong reason.
set_tests_properties(model_browser_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json;PINPOINT_CORE_SCREENS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/screens.json;PINPOINT_CORE_DRILLS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/drills.json;PINPOINT_CORE_REFERENCES=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/references.json;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/model_browser_scratch")

# Does the details pane REDRAW after a write? model_browser_test proves the façade answers correctly;
# this proves the four-link chain between that answer and the screen — the command's signal, the
# panel's revision counter, the binding that reads it, and the hop across into the pane's own `var`
# property. A pane showing yesterday's answer while the table shows today's is the one failure this
# panel's "selecting a row IS opening its editor" premise cannot survive, and it is invisible in a
# façade test and in a screenshot that happens to be of a correct pane.
pp_add_test(inspector_refresh_test
    SOURCES ${DIAG}/tests/inspector_refresh_test.cpp
            ${SRC}/Gui/diagnosticmodel/model_browser.cpp
            ${DIAG}/corridor_plot.cpp ${DIAG}/measure_sample.cpp
            ${DIAG}/dag_layout.cpp ${DIAG}/diagnostics_health.cpp
            ${DIAG}/drill_pack.cpp ${DIAG}/screen_pack.cpp ${DIAG}/reference_pack.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp ${DIAG}/memory_pack_provider.cpp
            ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp ${DIAG}/memory_norm_provider.cpp ${DIAG_NORM}
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${ANALYSIS}/reference_bands.cpp
    INCLUDE ${NORM_INC} ${SRC}/Gui/diagnosticmodel
    LINK    Qt6::Qml Qt6::Quick Qt6::Concurrent
    COMPILE_OPTIONS -DPP_CORE_PACK_PATH="${DIAGRES}/core.json")
set_tests_properties(inspector_refresh_test PROPERTIES
    ENVIRONMENT "PINPOINT_CORE_PACK=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/core.json;PINPOINT_CORE_NORMS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/norms.json;PINPOINT_CORE_CONTEXTS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/contexts.json;PINPOINT_CORE_SCREENS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/screens.json;PINPOINT_CORE_DRILLS=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/drills.json;PINPOINT_CORE_REFERENCES=${CMAKE_CURRENT_SOURCE_DIR}/../../Resources/diagnostics/references.json;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/inspector_refresh_scratch")

# The corridor picture. Pure geometry over value types, which is the whole reason the layout lives
# in C++ — a curve computed inside delegate bindings is a curve nothing can assert. The load-bearing
# claims are that the curve is SPLIT-normal (the tolerances are asymmetric by design), that the
# bands drawn are bandEdgesOf()'s bands under whatever policy is active, and that implausible
# readings are counted apart rather than graded.
pp_add_test(corridor_plot_test
    SOURCES ${DIAG}/tests/corridor_plot_test.cpp
            ${DIAG}/corridor_plot.cpp ${DIAG}/diagnostics_health.cpp
            ${DIAG}/drill_pack.cpp ${DIAG}/screen_pack.cpp ${DIAG}/reference_pack.cpp
            ${DIAG}/resource_pack_provider.cpp ${DIAG}/file_pack_provider.cpp
            ${DIAG}/merged_pack_provider.cpp
            ${DIAG}/resource_norm_provider.cpp ${DIAG}/file_norm_provider.cpp
            ${DIAG}/merged_norm_provider.cpp ${DIAG_NORM}
            ${SRC}/Metrics/metric_catalogue.cpp
            ${SRC}/Metrics/metric_resolver.cpp
            ${SRC}/Metrics/metric_catalogue_manifest.cpp
            ${SRC}/Metrics/metric_providers.cpp
            ${ANALYSIS}/reference_bands.cpp
    INCLUDE ${NORM_INC}
    LINK    Qt6::Qml)
