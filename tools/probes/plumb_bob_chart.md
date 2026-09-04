# plumb_bob_chart.qml — run it

One line (the probe path is absolute; `PBPROBE` is the grep handle):

```sh
QT_QPA_PLATFORM=offscreen PINPOINT_LOG_STDERR=1 /Users/markliversedge/Projects/PinPointStudio/build/Qt_6_11_1_for_macOS_Debug/PinPointStudio.app/Contents/MacOS/PinPointStudio --probe-qml /private/tmp/claude-501/-Users-markliversedge-Projects-PinPointStudio/672a7582-f617-424e-812b-29a11fd5bd33/scratchpad/probe/plumb_bob_chart.qml 2>&1 | grep -E 'PBPROBE|ShotReplay'
```

Runs ~20 s (6 steps × 2500 ms + teardown), then `Qt.quit()`s itself.

## Env, and what is NOT needed

- `QT_QPA_PLATFORM=offscreen` — required by the brief. View3D renders nothing; this probe
  reports data only.
- `PINPOINT_LOG_STDERR=1` — echoes the app log (and `console.warn`) to stderr
  (`src/Core/pp_debug.cpp:309`). Without it the lines only reach the in-app log.
- **No library-root configuration is needed.** The library root is the QSettings key
  `General/athleteLibraryPath` (`src/Gui/app/app_settings.h:146`, read at
  `app_settings.h:392`) and is used only to *enumerate* sessions
  (`SessionReviewController::refresh`). The probe calls `loadSession(<absolute session dir>)`
  — "a session's id IS its directory path" (`session_review_controller.h:63`) — so it finds
  the swing regardless of the setting. The probe prints the setting's current value for
  context and never writes it.
- No `PINPOINT_CORE_NORMS` / `PINPOINT_CORE_CONTEXTS`: those are the diagnostics norm
  registry's test overrides, and nothing on the chart path reads them.
- `appInfo.devBuild` must be true or the `--probe-qml` Loader stays inert. Verified:
  `PP_SHIPPING_BUILD:BOOL=OFF` in this build dir's `CMakeCache.txt`.

## Overrides

| flag | default |
|---|---|
| `--probe-swing <abs swing dir>` | `/mnt/swingdata/Mark-Liversedge/2026-08-18_Mark-Liversedge_Wrist_01/swing_0001` |
| `--probe-preset <name>` | `Plumb Bob` |
| `--probe-session-type <n>` | `1` (Wrist → screen index 2) |
| `--probe-step-ms <n>` | `2500` |

The app parses no `QCommandLineParser`, so unknown flags are inert — every arg is read ad hoc
out of `Qt.application.arguments`.

## What it will not do

It does **not** turn the Charts panel on. `PpModeStage` only instantiates panels that
`ViewLayout.isPanelOn(mode, "charts")` returns true for (`PpModeStage.qml:59`), and
`ViewLayout.setPanel` writes a persisted user setting. Instead the probe drives its **own**
`PpMetricChart` with `sessionType: -1`, which is contractually forbidden from persisting
(`_persistPref` / `_persistSection` return early), and separately walks the live tree
read-only to report whether an on-screen chart exists and which preset it is on. If you want
the two compared, turn the Charts panel on in View by hand before the run.
