/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

pragma Singleton
import QtQuick
import PinPointStudio

// Full-text search index for all settings panels.
//
// panelIndex → panel name mapping:
//   0  General        5  Microphone
//   1  Appearance     6  Launch Monitor (placeholder — no entries yet)
//   2  Displays       7  Storage
//   3  Cameras        8  Archiving      (placeholder — no entries yet)
//   4  IMUs           9  Diagnostic Model (the whole content set)
//
// label / subtitle / groupLabel / panelLabel are wrapped in qsTr() so they
// are extracted by lupdate and searched against the user's active locale.
// Language changes require restart; the singleton re-evaluates with the
// new locale on next launch.
//
// actions is intentionally NOT translated — it is a bag of supplementary
// English keyword hints that help catch button and chip labels. When
// translations are added, per-language keyword bags can be added here.
//
// itemId is the objectName on the searchable row in its panel file.
// An empty itemId navigates to the panel without scrolling.
// If a row's objectName changes, the entry here must change to match.

QtObject {
    id: root

    readonly property var entries: [

        // ── General (panelIndex: 0) ───────────────────────────────────────────

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Localisation"),
          label: qsTr("Language"),                        subtitle: qsTr("Restart required to apply"),
          itemId: "setting_language" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Localisation"),
          label: qsTr("Units"),                           subtitle: qsTr("Speed and distance displayed throughout"),
          itemId: "setting_units" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Session behaviour"),
          label: qsTr("Auto-detect swing start"),         subtitle: qsTr("Begins capture when motion exceeds threshold"),
          itemId: "setting_autoDetect" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Session behaviour"),
          label: qsTr("Swing detection sensitivity"),     subtitle: qsTr("Lower values trigger on slower movements"),
          itemId: "setting_swingSensitivity" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Session behaviour"),
          label: qsTr("AI coaching on session end"),      subtitle: qsTr("Automatically generate a Claude observation"),
          itemId: "setting_aiCoaching" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Motion capture"),
          label: qsTr("Motion capture quality"),          subtitle: qsTr("Detail level for pose and swing tracking during analysis"),
          actions: "motion capture quality pose tracking model low medium high detail accuracy",
          itemId: "setting_motionCapture" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Application"),
          label: qsTr("Check for updates automatically"), subtitle: qsTr("Checks on launch — never downloads without confirmation"),
          itemId: "setting_checkUpdates" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Application"),
          label: qsTr("Send anonymous diagnostics"),      subtitle: qsTr("Crash reports and performance data only"),
          itemId: "setting_diagnostics" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Application"),
          label: qsTr("Version"),                         subtitle: qsTr("Installed application version and update status"),
          actions: "Version app version up to date release build number check now download install update available restart upgrade",
          itemId: "setting_version" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Cloud fallback"),
          label: qsTr("Speech-to-text cloud fallback"),  subtitle: qsTr("Transcribe with Azure cloud speech recognition"),
          actions: "cloud fallback STT Azure speech recognition transcription GPU",
          itemId: "setting_cloudStt" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Cloud fallback"),
          label: qsTr("Text-to-speech cloud fallback"),  subtitle: qsTr("Synthesise speech with Azure cloud voices"),
          actions: "cloud fallback TTS Azure text to speech synthesis voice GPU",
          itemId: "setting_cloudTts" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Cloud fallback"),
          label: qsTr("AI Coach cloud fallback"),         subtitle: qsTr("Run the Google Gemini cloud model"),
          actions: "cloud fallback LLM AI coach Gemini Google model GPU",
          itemId: "setting_cloudLlm" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Cloud fallback"),
          label: qsTr("Azure Speech key"),                subtitle: qsTr("API key for cloud speech-to-text and text-to-speech"),
          actions: "Azure cognitive services API key secret credentials cloud STT TTS",
          itemId: "setting_azureKey" },

        { panelIndex: 0, panelLabel: qsTr("General"),    groupLabel: qsTr("Cloud fallback"),
          label: qsTr("Gemini API key"),                  subtitle: qsTr("API key for the Google Gemini AI Coach"),
          actions: "Gemini Google AI studio API key secret credentials cloud LLM coach",
          itemId: "setting_geminiKey" },

        // ── Appearance (panelIndex: 1) ────────────────────────────────────────

        { panelIndex: 1, panelLabel: qsTr("Appearance"),  groupLabel: qsTr("Aesthetic & colour mode"),
          label: qsTr("Aesthetic"),                        subtitle: qsTr("Visual theme applied across all screens"),
          itemId: "setting_aesthetic" },

        { panelIndex: 1, panelLabel: qsTr("Appearance"),  groupLabel: qsTr("Type scale"),
          label: qsTr("Text size"),                        subtitle: qsTr("Scales all fonts and spacing proportionally"),
          itemId: "setting_textSize" },

        { panelIndex: 1, panelLabel: qsTr("Appearance"),  groupLabel: qsTr("Interface"),
          label: qsTr("Density"),                          subtitle: qsTr("Compact reduces rail and panel padding"),
          itemId: "setting_density" },

        { panelIndex: 1, panelLabel: qsTr("Appearance"),  groupLabel: qsTr("Interface"),
          label: qsTr("Reduce motion"),                    subtitle: qsTr("Disables transitions and animations throughout"),
          itemId: "setting_reduceMotion" },

        { panelIndex: 1, panelLabel: qsTr("Appearance"),  groupLabel: qsTr("Interface"),
          label: qsTr("Overlay opacity"),                  subtitle: qsTr("Skeleton and angle guide overlays"),
          itemId: "setting_overlayOpacity" },

        // ── Displays (panelIndex: 2) ──────────────────────────────────────────

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Main window"),
          label: qsTr("Launch on"),                        subtitle: qsTr("Which display opens the main Pinpoint window"),
          itemId: "setting_launchOn" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Main window"),
          label: qsTr("Remember window size and position"), subtitle: qsTr("Restores last session geometry on next launch"),
          itemId: "setting_rememberGeometry" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Main window"),
          label: qsTr("Launch in full screen"),            subtitle: qsTr("Overrides saved window size if enabled"),
          itemId: "setting_fullScreen" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Post-shot display"),
          label: qsTr("Secondary display"),                subtitle: qsTr("Golfer-facing screen the session diagnostics panel is cast to"),
          itemId: "setting_secondaryDisplay" },

        // NO "Post-shot content" ROW. The cast surface is the session diagnostics panel and
        // there is no longer a content choice to index — see DisplaysPanel's group note. The
        // AppSettings key survives for stored profiles; a search hit that led to a control
        // that no longer exists would not.
        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Post-shot display"),
          label: qsTr("Post-shot display mode"),           subtitle: qsTr("A persistent window, one that pops after each swing, or a full-screen kiosk"),
          itemId: "setting_postShotDisplayMode" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Post-shot display"),
          label: qsTr("Display delay"),                    subtitle: qsTr("Pause after a swing before the cast window appears"),
          itemId: "setting_postShotDelay" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Post-shot display"),
          label: qsTr("Mirror main window"),               subtitle: qsTr("Flips the cast horizontally, for a coach standing opposite the athlete"),
          itemId: "setting_mirrorMain" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Rendering"),
          label: qsTr("Frame rate cap"),                   subtitle: qsTr("UI rendering — independent of camera capture rate"),
          itemId: "setting_frameRateCap" },

        { panelIndex: 2, panelLabel: qsTr("Displays"),    groupLabel: qsTr("Rendering"),
          label: qsTr("Hardware acceleration"),            subtitle: qsTr("Uses GPU for video decode and overlay rendering"),
          itemId: "setting_hwAccel" },

        // ── Cameras (panelIndex: 3) ───────────────────────────────────────────

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Global capture"),
          label: qsTr("Pre-roll buffer"),                  subtitle: qsTr("Seconds of frames held before swing trigger fires"),
          actions: "Refresh enumerate rescan detected cameras hardware",
          itemId: "setting_preroll" },

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Global capture"),
          label: qsTr("Synchronise cameras"),              subtitle: qsTr("Lock frame timing across all enabled cameras"),
          itemId: "setting_camSync" },

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Camera device settings"),
          label: qsTr("Camera view assignment"),           subtitle: qsTr("Assign each camera to a perspective"),
          actions: "View Face-on Down-the-line perspective unassigned Other",
          itemId: "" },

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Camera device settings"),
          label: qsTr("Camera frame rate"),                subtitle: qsTr("Set the capture frame rate for each camera"),
          actions: "Frame rate fps 30 60 120 240",
          itemId: "" },

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Camera device settings"),
          label: qsTr("Camera trigger mode"),              subtitle: qsTr("Choose between free-run and hardware-synchronised capture"),
          actions: "Trigger free-run HW sync hardware synchronise",
          itemId: "" },

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Camera device settings"),
          label: qsTr("Fixed in place"),                   subtitle: qsTr("Mark a camera as wall-mounted"),
          actions: "Fixed in place wall mounted immovable",
          itemId: "" },

        { panelIndex: 3, panelLabel: qsTr("Cameras"),     groupLabel: qsTr("Camera crop & ROI"),
          label: qsTr("Camera crop region"),               subtitle: qsTr("Define a crop to reduce frame storage and ring buffer allocation"),
          actions: "Set crop Reset to full frame Default crop Full frame 16:9 ROI region origin size presets",
          itemId: "" },

        // ── IMUs (panelIndex: 4) ──────────────────────────────────────────────

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("Global IMU settings"),
          label: qsTr("Auto-connect on session start"),    subtitle: qsTr("Connects all enabled devices before recording begins"),
          actions: "enumerated devices hardware",
          itemId: "setting_imuAutoConnect" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("Global IMU settings"),
          label: qsTr("Auto-reconnect on signal loss"),    subtitle: qsTr("Attempts reconnect if the BLE link drops during a session"),
          itemId: "setting_imuAutoReconnect" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("Global IMU settings"),
          label: qsTr("Save calibration to device flash"), subtitle: qsTr("Persists zero-orientation and mag calibration across power cycles"),
          itemId: "setting_imuFlash" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("Global IMU settings"),
          label: qsTr("Use 9-axis fusion by default"),     subtitle: qsTr("Magnetometer-aided orientation"),
          itemId: "setting_imuFusion" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("IMU device settings"),
          label: qsTr("IMU body placement"),               subtitle: qsTr("Assign each IMU to a body segment"),
          actions: "Placement body thorax lumbar spine shoulder wrist hand segment A B C D",
          itemId: "" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("IMU device settings"),
          label: qsTr("IMU output rate"),                  subtitle: qsTr("Set the data rate for each IMU device in Hz"),
          actions: "Output rate Hz 50 100 200 500",
          itemId: "" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("IMU device settings"),
          label: qsTr("IMU fusion algorithm"),             subtitle: qsTr("Choose between 6-axis and 9-axis magnetometer-aided fusion"),
          actions: "Fusion 9-axis 6-axis magnetometer gyro algorithm",
          itemId: "" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("IMU device settings"),
          label: qsTr("IMU mount orientation"),            subtitle: qsTr("Specify whether the device is mounted horizontally or vertically"),
          actions: "Mount orientation horizontal vertical",
          itemId: "" },

        { panelIndex: 4, panelLabel: qsTr("IMUs"),        groupLabel: qsTr("IMU test panel"),
          label: qsTr("Test IMU connection"),              subtitle: qsTr("Verify orientation and live sensor data from a connected device"),
          actions: "Test Connect Disconnect Zero orientation Calibrate magnetometer Save to flash Scan Euler angles roll pitch yaw battery calibration",
          itemId: "" },

        // ── Microphone (panelIndex: 5) ────────────────────────────────────────

        { panelIndex: 5, panelLabel: qsTr("Microphone"),  groupLabel: qsTr("Shot detection"),
          label: qsTr("Use microphone for shot detection"), subtitle: qsTr("Detect impact from the strike sound, fused with IMU and vision"),
          actions: "acoustic audio onset enable disable sound",
          itemId: "setting_acousticShotDetection" },

        { panelIndex: 5, panelLabel: qsTr("Microphone"),  groupLabel: qsTr("Input device"),
          label: qsTr("Audio input device"),              subtitle: qsTr("Select which microphone is used for capture and shot detection"),
          actions: "mic select default usb webcam rescan",
          itemId: "" },

        { panelIndex: 5, panelLabel: qsTr("Microphone"),  groupLabel: qsTr("Calibration"),
          label: qsTr("Microphone sensitivity"),          subtitle: qsTr("Tune the acoustic detection threshold so every shot is detected"),
          actions: "calibrate sensitivity threshold level meter tune",
          itemId: "" },

        // ── Storage (panelIndex: 7) ───────────────────────────────────────────

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Athlete library"),
          label: qsTr("Library location"),                 subtitle: qsTr("Root directory for all athlete profiles and session archives"),
          actions: "Change Reveal open folder",
          itemId: "setting_libraryPath" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Athlete library"),
          label: qsTr("Session folder naming"),            subtitle: qsTr("Pattern used when creating a new session directory"),
          itemId: "setting_sessionNaming" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Athlete library"),
          label: qsTr("Auto-save session on completion"),  subtitle: qsTr("Writes session data to the library immediately when recording ends"),
          itemId: "setting_autoSave" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Video recording"),
          label: qsTr("Recording resolution"),             subtitle: qsTr("Applies to all cameras — must be within sensor ROI bounds"),
          itemId: "setting_videoRes" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Video recording"),
          label: qsTr("Video codec"),                      subtitle: qsTr("Encoding applied when saving swing clips to disk"),
          itemId: "setting_videoCodec" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Video recording"),
          label: qsTr("Encoding quality"),                 subtitle: qsTr("Higher quality produces larger files"),
          itemId: "setting_videoQuality" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Video recording"),
          label: qsTr("Save raw camera frames"),           subtitle: qsTr("Stores unprocessed Bayer data alongside encoded clips"),
          itemId: "setting_saveRaw" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Video recording"),
          label: qsTr("Container format"),                 subtitle: qsTr("File format for saved swing clips"),
          itemId: "setting_container" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Sensor data"),
          label: qsTr("Save pose keypoints"),              subtitle: qsTr("MoveNet skeleton data stored as JSON"),
          itemId: "setting_savePose" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Sensor data"),
          label: qsTr("Save IMU streams"),                 subtitle: qsTr("Full quaternion and accelerometer data for all enabled IMUs"),
          itemId: "setting_saveImu" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Sensor data"),
          label: qsTr("IMU data format"),                  subtitle: qsTr("File format for saved IMU streams"),
          itemId: "setting_imuFormat" },

        { panelIndex: 7, panelLabel: qsTr("Storage"),     groupLabel: qsTr("Sensor data"),
          label: qsTr("Save launch monitor data"),         subtitle: qsTr("Ball-flight data from connected launch monitor"),
          itemId: "setting_saveLaunchMon" },

        // ── Diagnostic Model (panelIndex: 9) ──────────────────────────────────
        //
        // Metrics was panelIndex 9 and Diagnostics 10 until the metric catalogue became a VIEW
        // inside Diagnostics rather than a panel beside it. Its search entries stayed — the words
        // people look for did not change, only where they land.
        //
        // The same thing has happened again: Diagnostics is hidden and these now land on Diagnostic
        // Model. Repointed rather than deleted for exactly the reason above — a golfer searching
        // "glossary" or "over the top" is asking about content that still exists. Left at 9 they
        // would open a panel with no row highlighted in the sidenav, which reads as a bug.

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Swing diagnostics"),                subtitle: qsTr("Named faults and their causes — what each costs the golfer"),
          actions: "diagnostics characteristics faults causes diagnosis library swing early extension loss of posture over the top casting scooping sway slide hanging back chicken wing flying elbow reverse spine s-posture c-posture ball position stance width alignment tempo sequence x-factor",
          itemId: "" },

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Reference"),
          label: qsTr("Measures and corridors"),           subtitle: qsTr("What the app can read, and what good looks like for each"),
          actions: "metric metrics catalogue directory reference normative corridor tour wrist bow cup hinge roll elbow clubhead hand speed lag stance foot flare toe heel",
          itemId: "" },

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Physical screens"),                 subtitle: qsTr("Causes the app cannot measure — which screens would explain the most"),
          actions: "screen screening physical mobility hip internal rotation thoracic rotation pelvic disassociation core stability balance ankle dorsiflexion shoulder flexion wrist mobility",
          itemId: "" },

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Drills"),                           subtitle: qsTr("What a golfer does about a characteristic, and what it is trying to change"),
          actions: "drill drills practice rehearsal feel gate towel step pump hold the finish low point standoff",
          itemId: "" },

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Glossary"),                         subtitle: qsTr("What a coaching term means — search by the word you were taught"),
          actions: "glossary terms vocabulary jargon flip flipping ott over the top standing up early release casting shank chunk fat thin slice hook pull push block",
          itemId: "" },

        // ── Diagnostic Model, continued ───────────────────────────────────────
        //
        // The same content set as Diagnostics above, one screen instead of eight views. Both are
        // searchable while both ship — a search for "measures" should offer either.

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Diagnostic model"),                 subtitle: qsTr("The whole content set as one graph — navigate and edit it in place"),
          actions: "diagnostic model graph dag navigator content set browse edit author authoring inline table library characteristics causes measures signals causal links screens drills references",
          itemId: "" },

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Causal links"),                     subtitle: qsTr("Every edge between conditions — relation, strength and the evidence behind it"),
          actions: "causal link links edge edges cause causes corroborates excludes strength evidence tier citation acyclic cycle relation",
          itemId: "" },

        { panelIndex: 9,  panelLabel: qsTr("Diagnostic Model"), groupLabel: qsTr("Swing diagnostics"),
          label: qsTr("Signals"),                          subtitle: qsTr("What each condition is detected by — the test, its direction and the measure it reads"),
          actions: "signal signals detection detected by test direction threshold measure reads",
          itemId: "" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Connection"),
          label: qsTr("Device"),                           subtitle: qsTr("Which launch monitor to read — Foresight GC Quad via FSX2020, or none"),
          actions: "launch monitor device gcquad gc quad foresight fsx2020 fsx connect ball club data",
          itemId: "setting_lmDevice" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Connection"),
          label: qsTr("Shot data folder"),                 subtitle: qsTr("The folder FSX2020 writes LastShot.CSV into — a local path or a share"),
          actions: "launch monitor folder path directory lastshot csv fsx2020 share network location",
          itemId: "setting_lmPath" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Connection"),
          label: qsTr("Connection status"),                subtitle: qsTr("Whether the configured folder can be read, and what was last read from it"),
          actions: "launch monitor status connected waiting error not reading last shot",
          itemId: "setting_lmStatus" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Behaviour"),
          label: qsTr("Chime when a reading arrives"),     subtitle: qsTr("A short quiet tone when the monitor's data is folded into the swing"),
          actions: "launch monitor chime sound ting audio beep tone notify silence mute arrival",
          itemId: "setting_lmChime" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Behaviour"),
          label: qsTr("Store launch monitor data with each swing"), subtitle: qsTr("Off means readings are read and discarded rather than saved to the swing"),
          actions: "launch monitor store save data swing json record keep discard",
          itemId: "setting_lmStore" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Behaviour"),
          label: qsTr("Check for new shots every"),        subtitle: qsTr("How often the shot data folder is re-read — raise it only for a slow share"),
          actions: "launch monitor poll interval frequency check rate refresh share slow",
          itemId: "setting_lmPoll" },

        { panelIndex: 6,  panelLabel: qsTr("Launch Monitor"), groupLabel: qsTr("Behaviour"),
          label: qsTr("Record shots the monitor sees on its own"), subtitle: qsTr("Create a swing from the monitor's reading alone, with no video and no analysis"),
          actions: "launch monitor standalone device only no camera solo shots without cameras record alone",
          itemId: "setting_lmStandalone" }

        // TODO: add entries when Archiving panel is implemented (panelIndex: 8)
    ]
}
