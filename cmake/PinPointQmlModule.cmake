# The PinPointStudio QML module, declared ONCE and consumed TWICE.
#
# The app compiles this module into its binary; `qml_ui_test` compiles the same module into
# itself so it can press real components without building the app. Before this file existed
# the test borrowed the app's module through a staged, rewritten copy — which is why it could
# only run from an app build, and why it ran in NO release gate on any platform.
#
# ⚠ THESE THREE LISTS MOVE TOGETHER OR THE SUITE FAILS IN WAYS THAT DO NOT RESEMBLE THE CAUSE.
# That is the whole reason they are in one file rather than three places.
#
#   PP_QML_FILES        the .qml sources
#   PP_QML_SINGLETONS   which of them are `pragma Singleton`
#   PP_QML_SOURCES      the C++ that registers QML_ELEMENT types into the module
#
# The singleton list is the one that bites. Omit it and everything still builds, every C++
# type still resolves, the module still imports — and seventy tests fail with
# "Property 'sp' of object Theme is not a function", which points nowhere near the cause.
# It sat twelve lines above qt_add_qml_module in the root CMakeLists, close enough to look
# like part of it and far enough to be left behind. It was, once, and cost an afternoon.
#
# ⚠ ADDING A .qml FILE: add it to PP_QML_FILES, and if it carries `pragma Singleton` add it to
# PP_QML_SINGLETONS as well. Adding a C++ QML type: add it to PP_QML_SOURCES, and check the
# test target still links — the test compiles these sources itself and does not have the app's
# link closure behind it.
#
# Paths are relative to the repo root; each consumer prefixes them with its own.

# `pragma Singleton` files. Qt needs QT_QML_SINGLETON_TYPE on each or the type registers as
# an ordinary component and every `Theme.foo()` call becomes a property lookup on nothing.
set(PP_QML_SINGLETONS
    src/Gui/theme/Theme.qml
    src/Gui/components/ClubFormat.qml
    src/Gui/settings/SettingsIndex.qml
    src/Gui/session/ViewLayout.qml
    src/Gui/session/SessionMode.qml
    src/Gui/cameras/AnnotationTool.qml
    src/Gui/review/MetricRoute.qml
)

set(PP_QML_FILES
    src/Gui/theme/Theme.qml
    src/Gui/components/ClubFormat.qml
    src/Gui/session/ViewLayout.qml
    src/Gui/session/SessionMode.qml
    src/Gui/shell/Main.qml
    src/Gui/media/AudioPage.qml
    src/Gui/imu/CapturePage.qml
    src/Gui/viz/ImuVizView.qml
    src/Gui/viz/ArmVizView.qml
    src/Gui/viz/BodyVizView.qml
    src/Gui/media/FilmPage.qml
    src/Gui/cameras/VideoPage.qml
    src/Gui/cameras/PpCameraFrame.qml
    src/Gui/cameras/AnnotationTool.qml
    src/Gui/cameras/PpAnnotationIcon.qml
    src/Gui/cameras/PpAnnotationLayer.qml
    src/Gui/cameras/PpAnnotationToolbar.qml
    src/Gui/components/PpSegmentedControl.qml
    src/Gui/session/PpStagePanel.qml
    src/Gui/session/PpViewPanel.qml
    src/Gui/session/PpMotionPanel.qml
    src/Gui/session/PpClubPanel.qml
    src/Gui/cameras/PpCameraTiles.qml
    src/Gui/session/PpModeStage.qml
    src/Gui/review/PpReplayCharts.qml
    src/Gui/review/PpTransitTimeline.qml
    src/Gui/review/PpReplayTransport.qml
    src/Gui/shell/PpRail.qml
    src/Gui/shell/PpRailButton.qml
    src/Gui/components/PpDivider.qml
    src/Gui/shell/PpHeader.qml
    src/Gui/shell/PpUpdateBanner.qml
    src/Gui/shell/PpDetectCluster.qml
    src/Gui/shell/PpAnalysingBadge.qml
    src/Gui/shell/PlayPage.qml
    src/Gui/shell/ScreenPlaceholder.qml
    src/Gui/athlete/ScreenAthleteForm.qml
    src/Gui/athlete/AthleteClubsSection.qml
    src/Gui/athlete/ScreenAthletePicker.qml
    src/Gui/components/PpTextField.qml
    src/Gui/components/PpComboBox.qml
    src/Gui/components/PpChipGroup.qml
    src/Gui/components/PpUnitToggle.qml
    src/Gui/components/PpButton.qml
    src/Gui/components/PpConnectingFrame.qml
    src/Gui/components/PpPressable.qml
    src/Gui/components/PpDisplayText.qml
    src/Gui/athlete/PpAthleteCard.qml
    src/Gui/athlete/PpAthleteRow.qml
    src/Gui/monitor/ScreenResourceMonitor.qml
    src/Gui/monitor/RmDeviceCard.qml
    src/Gui/monitor/RmSourceRow.qml
    src/Gui/monitor/RmProfilerRow.qml
    src/Gui/monitor/RmStatRow.qml
    src/Gui/monitor/RmAnalysisRunRow.qml
    src/Gui/monitor/RmTimelineChart.qml
    src/Gui/monitor/RmWarningNotice.qml
    src/Gui/home/ScreenHome.qml
    # H5 — the sessions a connected capture device offers, in the DEVICES area
    # of the home screen.  This is where H3's deleted "File -> Import Session…"
    # menu item went: no menus, no dialogs, and the user never picks a file.
    src/Gui/home/PpcpOfferList.qml
    # H6 — "Pair to my phone": the PPCP-RV pairing code as a QR, in a modal
    # opened by the one button in the DEVICES heading.  It replaced
    # PpcpPairPanel.qml, which was an always-visible inline section; the
    # remembered pairings it also carried are now Settings -> Phones, which is
    # where RV 7.4b's "visible and individually revocable" is satisfied.
    src/Gui/home/PpcpPairDialog.qml
    src/Gui/home/PpQrGlyph.qml
    src/Gui/settings/ScreenSettings.qml
    src/Gui/settings/AppearancePanel.qml
    src/Gui/settings/GeneralPanel.qml
    src/Gui/settings/DisplaysPanel.qml
    src/Gui/settings/CamerasPanel.qml
    src/Gui/settings/ImusPanel.qml
    src/Gui/settings/PhonesPanel.qml
    src/Gui/settings/MicrophonesPanel.qml
    src/Gui/settings/StoragePanel.qml
    src/Gui/settings/LaunchMonitorPanel.qml
    src/Gui/settings/SettingsIndex.qml
    src/Gui/home/HmTypeCard.qml
    src/Gui/session/ScreenSessionWizard.qml
    src/Gui/media/CoachPage.qml
    src/Gui/session/PpSessionToolbar.qml
    src/Gui/cameras/PpCameraPanel.qml
    src/Gui/imu/PpImuPanel.qml
    src/Gui/session/ScreenWrist.qml
    src/Gui/session/ScreenSessionMode.qml
    src/Gui/calibration/ImuCalibrationFlow.qml
    src/Gui/calibration/CameraCalibrationFlow.qml
    src/Gui/shot/PpShotCarousel.qml
    src/Gui/shot/PpShotActionBar.qml
    src/Gui/shot/PpExportOptionsSheet.qml
    src/Gui/shot/PpShotCard.qml
    src/Gui/shot/PpShotPipRow.qml
    src/Gui/session/PpSessionDrawer.qml
    src/Gui/shot/PpShotFilter.qml
    src/Gui/shot/PpSwingEditPanel.qml
    src/Gui/diagnostics/PpSessionDiagnosticsPanel.qml
    src/Gui/diagnostics/PpSessionDiagnosticsBody.qml
    src/Gui/diagnostics/PpThisShotStrip.qml
    src/Gui/diagnostics/PpReviewShotStrip.qml
    src/Gui/diagnostics/PpPatternCard.qml
    src/Gui/diagnostics/PpChainRail.qml
    src/Gui/diagnostics/PpChainNodeCard.qml
    src/Gui/diagnostics/PpChainLink.qml
    src/Gui/diagnostics/PpConditionDetail.qml
    src/Gui/diagnostics/PpDriverFooter.qml
    src/Gui/diagnostics/PpTickRun.qml
    src/Gui/diagnostics/PpWatchingRow.qml
    src/Gui/diagnostics/PpCoverageLine.qml
    src/Gui/diagnostics/PpDashedFrame.qml
    src/Gui/session/PpLaunchMonitorPanel.qml
    src/Gui/session/PpLmGraphicsBody.qml
    src/Gui/session/PpLmCard.qml
    src/Gui/session/PpLmDiagram.qml
    src/Gui/session/PpLmRule.qml
    src/Gui/session/PpLmRead.qml
    src/Gui/session/PpLmSpread.qml
    src/Gui/shot/PpSessionDiagnosticsWindow.qml
    src/Gui/components/PpQualityPill.qml
    src/Gui/components/PpStarRating.qml
    src/Gui/components/PpTrace.qml
    src/Gui/components/PpToolPill.qml
    src/Gui/components/PpTypePill.qml
    src/Gui/components/PpTopoBackground.qml
    src/Gui/review/PpMetricChart.qml
    src/Gui/review/PpChartPlot.qml
    src/Gui/review/PpSegmentBrush.qml
    src/Gui/review/PpChartSummary.qml
    src/Gui/review/PpSpeedSelector.qml
    src/Gui/components/PpToast.qml
    src/Gui/review/PpDataViewer.qml
    src/Gui/review/PpCoverageStrip.qml
    src/Gui/review/PpPropertiesPanel.qml
    src/Gui/review/MetricRow.qml
    src/Gui/review/NormativeBar.qml
    src/Gui/review/MetricDetail.qml
    src/Gui/review/MetricLibrary.qml
    src/Gui/review/MetricRoute.qml
    src/Gui/diagnosticmodel/DiagnosticModel.qml
    src/Gui/diagnosticmodel/ModelTypeRail.qml
    src/Gui/diagnosticmodel/ModelTable.qml
    src/Gui/diagnosticmodel/ModelInspector.qml
    src/Gui/diagnosticmodel/ModelTrail.qml
    src/Gui/diagnosticmodel/ModelGraph.qml
    src/Gui/diagnosticmodel/RingMenu.qml
    src/Gui/diagnosticmodel/ModelCausePicker.qml
    src/Gui/diagnosticmodel/ModelPicker.qml
    src/Gui/diagnosticmodel/ModelEdits.qml
    src/Gui/diagnosticmodel/ModelMint.qml
    src/Gui/diagnosticmodel/ModelChipRow.qml
    src/Gui/diagnosticmodel/ModelTools.qml
    src/Gui/diagnosticmodel/ModelBarButton.qml
    src/Gui/diagnosticmodel/ModelEnumField.qml
    src/Gui/diagnosticmodel/ModelConfirm.qml
    src/Gui/diagnosticmodel/ModelPolicyPicker.qml
    src/Gui/diagnosticmodel/ModelUnsaved.qml
    src/Gui/diagnosticmodel/ModelCorridorPlot.qml
    src/Gui/diagnostics/WristDiagnostics.qml
    src/Gui/diagnostics/DofTrajectoryStrip.qml
    src/Gui/diagnostics/PositionAngleGrid.qml
    src/Gui/diagnostics/WristScorePill.qml
    src/Gui/diagnostics/FindingCard.qml
    src/Gui/diagnostics/FindingsList.qml
    src/Gui/diagnostics/StrengthsList.qml
    src/Gui/session/PpMarkupPanel.qml
    src/Gui/shell/PpAboutDialog.qml
    src/Gui/shell/MacAboutMenu.qml
)

# The C++ types the module registers (QML_ELEMENT). Headers are listed alongside their .cpp
# so AUTOMOC sees them.
set(PP_QML_SOURCES
    src/Audio/ting_player.h
    src/Audio/ting_player.cpp
    src/Video/bayer_video_item.h
    src/Video/bayer_video_item.cpp
    src/Gui/viz/body_pose_adapter.h
    src/Gui/viz/body_pose_adapter.cpp
    src/Gui/shot/shot_filter_proxy_model.h
    src/Gui/shot/shot_filter_proxy_model.cpp
    src/Gui/review/swing_data_source.h
    src/Gui/review/swing_data_source.cpp
    src/Gui/review/swing_coverage_model.h
    src/Gui/review/swing_coverage_model.cpp
    src/Gui/review/swing_series_model.h
    src/Gui/review/swing_series_model.cpp
    src/Gui/review/timeline_labels.h
    src/Gui/review/timeline_labels.cpp
    src/Gui/review/chart_metrics.h
    src/Gui/review/chart_metrics.cpp
    src/Gui/review/metric_catalog.h
    src/Gui/review/metric_catalog.cpp
    src/Gui/launchmonitor/lm_session_model.h
    src/Gui/launchmonitor/lm_session_model.cpp
    src/Gui/diagnosticmodel/model_browser.h
    src/Gui/diagnosticmodel/model_browser.cpp
    src/Diagnostics/anatomy_vocabulary.h
    src/Diagnostics/anatomy_vocabulary.cpp
    src/Diagnostics/measure_facets.h
    src/Diagnostics/measure_facets.cpp
    src/Diagnostics/measure_vocabulary.h
    src/Diagnostics/characteristic.h
    src/Diagnostics/characteristic.cpp
    src/Diagnostics/pack_io.h
    src/Diagnostics/pack_io.cpp
    src/Diagnostics/characteristic_pack.h
    src/Diagnostics/characteristic_pack.cpp
    src/Diagnostics/pack_provider.h
    src/Diagnostics/resource_pack_provider.cpp
    src/Diagnostics/memory_pack_provider.cpp
    src/Diagnostics/memory_norm_provider.cpp
    src/Diagnostics/corridor_plot.h
    src/Diagnostics/corridor_plot.cpp
    src/Diagnostics/file_pack_provider.cpp
    src/Diagnostics/merged_pack_provider.cpp
    src/Diagnostics/characteristic_engine.h
    src/Diagnostics/characteristic_engine.cpp
    src/Diagnostics/relation_resolver.h
    src/Diagnostics/relation_resolver.cpp
    src/Diagnostics/dag_layout.h
    src/Diagnostics/dag_layout.cpp
    src/Diagnostics/norm.h
    src/Diagnostics/norm.cpp
    src/Diagnostics/context_tree.h
    src/Diagnostics/context_tree.cpp
    src/Diagnostics/norm_pack.h
    src/Diagnostics/norm_pack.cpp
    src/Diagnostics/norm_provider.h
    src/Diagnostics/norm_measure_source.h
    src/Diagnostics/metric_corridor.h
    src/Diagnostics/diagnostics_health.h
    src/Diagnostics/diagnostics_health.cpp
    src/Diagnostics/screen_pack.h
    src/Diagnostics/screen_pack.cpp
    src/Diagnostics/drill_pack.h
    src/Diagnostics/drill_pack.cpp
    src/Diagnostics/reference_pack.cpp
    src/Diagnostics/measure_sample.h
    src/Diagnostics/measure_sample.cpp
    src/Diagnostics/live_measure_source.h
    src/Diagnostics/live_measure_source.cpp
    src/Diagnostics/resource_norm_provider.cpp
    src/Diagnostics/file_norm_provider.cpp
    src/Diagnostics/merged_norm_provider.cpp
    src/Gui/diagnostics/wrist_diagnostics_model.h
    src/Gui/diagnostics/wrist_diagnostics_model.cpp
    src/Gui/diagnostics/session_diagnostics_model.h
    src/Gui/diagnostics/session_diagnostics_model.cpp
    src/Analysis/reference_bands.h
    src/Analysis/reference_bands.cpp
    src/Analysis/wrist_assessment_engine.h
    src/Analysis/wrist_assessment_engine.cpp
    src/Analysis/wrist_analysis_adapter.h
    src/Analysis/wrist_analysis_adapter.cpp
    src/Analysis/assessment_rule.h
    src/Analysis/assessment_rules.h
    src/Analysis/assessment_rules.cpp
)

