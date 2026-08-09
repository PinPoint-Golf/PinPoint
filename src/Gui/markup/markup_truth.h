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

#pragma once

// markup_truth — pure Qt-Core (no GUI / OpenCV) read/write of a SwingLab
// `truth.json` ground-truth sidecar, plus the minimal face-on geometry parse
// from a swing's `swing.json`. Kept dependency-light so it is standalone
// unit-testable (tools/swinglab `score.py` is the byte-compatibility oracle).
//
// The on-disk truth.json is the exact shape `tools/swinglab/swinglab/label.py`
// writes and `score.py` reads (Tier-3):
//   { "shaft": [ { "t_us": <face-on frames.t_us[i], window-relative>,
//                  "grip": [px,py], "head": [px,py],   // PIXELS @ source W×H
//                  "theta": atan2(hy-gy, hx-gx), "len": hypot } ... ],
//     "events": { "t0_us": frames.t_us[0],
//                 "<name>_s": (frame_t_us - t0)/1e6 },     // only marked events
//     "ball":   [px,py],                                   // stationary ball
//                                                     // centre; additive, omit if unset
//     "meta":   { "lighting":, "shaft":, "club":, "scope":, "tempo":,
//                 "contact":, "clubLeavesFrame": } }       // additive; only set
//                                                     // fields, omitted if none
//
// In memory labels are held NORMALIZED (0..1) so the UI is resolution-agnostic;
// the pixel conversion happens only at write time, against FaceOnInfo dims.

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pinpoint::markup {

// The labellable positions, in ladder order — the golf P-system P1..P10. The
// truth.json key is `<name>_s` (p1_s .. p10_s). score.py maps each to the
// analyzer Phase enum where one exists: p1→Address(0), p3→MidBackswing(8),
// p4→Top(2), p5→ArmParallelDown(13), p6→Delivery(9), p7→Impact(5),
// p8→ShaftParallelThrough(14), p9→FollowThrough(11), p10→Finish(7); p2 is a
// shaft-parallel instant with no analyzer event (it carries club-angle truth +
// the parallel cross-check, as do p5/p8 alongside their events). Legacy truth.json
// (address/takeaway/top/impact/release/finish) is still read via aliases below.
inline QStringList eventNames()
{
    return { QStringLiteral("p1"), QStringLiteral("p2"), QStringLiteral("p3"),
             QStringLiteral("p4"), QStringLiteral("p5"), QStringLiteral("p6"),
             QStringLiteral("p7"), QStringLiteral("p8"), QStringLiteral("p9"),
             QStringLiteral("p10") };
}

// Pre-P-system event keys mapped onto P-positions (for reading old/synth/lab.py
// truth.json into the markup tool). Only the unambiguous overlaps are mapped.
inline QMap<QString, QString> legacyEventAliases()
{
    return { { QStringLiteral("address"), QStringLiteral("p1") },
             { QStringLiteral("top"),     QStringLiteral("p4") },
             { QStringLiteral("impact"),  QStringLiteral("p7") },
             { QStringLiteral("finish"),  QStringLiteral("p10") } };
}

// Face-on video geometry resolved from a swing's swing.json. `frameTimesUs` is
// window-relative (matches the replay playhead domain) and one entry per frame.
struct FaceOnInfo {
    bool            ok = false;
    QString         videoFile;        // relative file name, e.g. "Face-On.mp4"
    QString         alias;            // stream alias
    int             srcWidth = 0;
    int             srcHeight = 0;
    QVector<qint64> frameTimesUs;     // window-relative µs, ascending
    double          playbackFps = 30.0;  // MP4 encode fps (sequential PTS); for frame→ms seeks

    int frameCount() const { return frameTimesUs.size(); }
};

// One labelled shaft frame, normalized [0..1] in the displayed video space.
struct ShaftLabel {
    double gripNx = 0.0, gripNy = 0.0;
    double headNx = 0.0, headNy = 0.0;
};

// The stationary ball centre, normalized [0..1] in the displayed video space.
// The ball does not move during a swing, so this is a single per-swing point
// (marked once), NOT a per-frame track like ShaftLabel. `has` is false until
// placed — the "ball" key is then omitted on write (additive, like TruthMeta).
// This is the ground truth for the ball-detector v2 position gate
// (docs/design/ball_detection_v2.md §9.1).
struct BallLabel {
    double nx = 0.0, ny = 0.0;
    bool   has = false;
};

// Capture-conditions metadata, persisted additively under truth.json "meta".
// Free-form strings so the SwingLab corpus can be filtered/segmented by capture
// conditions; canonical lowercase for the enums, a label for club. An empty
// string (or false bool) means "unset" — that key is omitted on write, preserving
// the legacy (shaft/events-only) byte-shape score.py was proven against.
//
// `scope` is the one SwingLab validation actively *consumes*: full-swing-only
// checks (track.downswing_sweep, seg.tempo_ratio) are skipped when it isn't
// "full", so a pitch/chip/putt doesn't spuriously fail the full-swing bounds.
struct TruthMeta {
    QString lighting;   // "bright" | "normal" | "dark" | ""
    QString shaft;      // "graphite" | "steel" | ""
    QString club;       // e.g. "DRIVER", "7 IRON" (see club_vocabulary.h); "" = unset
    QString scope;      // "full" | "pitch" | "chip" | "putt" | ""
    QString tempo;      // "slow" | "normal" | "fast" | ""
    QString contact;    // "ball" | "air" | "mishit" | ""
    bool    clubLeavesFrame = false;   // head exits frame mid-swing (explains low coverage)

    bool isEmpty() const {
        return lighting.isEmpty() && shaft.isEmpty() && club.isEmpty()
            && scope.isEmpty() && tempo.isEmpty() && contact.isEmpty()
            && !clubLeavesFrame;
    }
};

// The full in-memory label set for one swing. Keyed by face-on frame index.
struct TruthDoc {
    QMap<int, ShaftLabel> shaft;   // frameIndex -> endpoints
    QMap<QString, int>    events;  // eventName  -> frameIndex
    BallLabel             ball;    // stationary ball centre (per-swing, optional)
    TruthMeta             meta;    // capture conditions (lighting / shaft / club)
};

// Parse the face-on stream from <swingDir>/swing.json. Face-on selection:
// setup.perspective == 2, else alias containing `faceNeedle` (case-insensitive),
// else the first video stream. ok=false if no video stream / no frames.
FaceOnInfo readFaceOn(const QString &swingDir, const QString &faceNeedle = QStringLiteral("Face"));

// Nearest face-on frame index for a window-relative timestamp (binary search).
// Returns -1 if there are no frames.
int frameIndexForUs(const FaceOnInfo &fo, qint64 us);

// Build the truth.json object from normalized labels + face-on geometry. Out-of-
// range frame indices are skipped. Matches label.py byte-shape (theta rounded to
// 5 dp, len to 1 dp, event seconds to 4 dp; grip/head kept full precision).
QJsonObject toJson(const TruthDoc &doc, const FaceOnInfo &fo);

// Atomic write of <swingDir>/truth.json. Returns false (and sets *error) on
// failure. An empty doc still writes a valid (shaft:[], events:{t0_us}) file.
bool writeTruth(const QString &swingDir, const TruthDoc &doc, const FaceOnInfo &fo,
                QString *error = nullptr);

// Read <swingDir>/truth.json back into a TruthDoc (pixels -> normalized via fo
// dims; t_us / seconds -> nearest frame index). Empty TruthDoc if no file.
TruthDoc readTruth(const QString &swingDir, const FaceOnInfo &fo);

// The user's editable club for a swing, read from <swingDir>/swing.json's
// review.club (the shot-carousel swing-edit popover's field). Empty if there is
// no swing.json or no review.club yet. Used to seed the markup meta.club default
// so both sidecars start from the same club without re-typing.
QString readSwingReviewClub(const QString &swingDir);

// Quick facts for the queue UI without resolving frame indices.
struct TruthSummary {
    bool exists     = false;
    int  shaftCount = 0;
    int  eventCount = 0;
};
TruthSummary summarize(const QString &swingDir);

// ── Recorded pose overlay (read-only display; NOT part of truth.json) ─────────
// The analyzer's face-on 2D pose, replayed under the labelled shaft so the
// operator can see how the (independent) grip/head relate to the skeleton —
// especially the lead/trail HAND points. Read from analysis.pose2d in swing.json.

// One pose sample. `kp` is 17 COCO keypoints × (x, y, conf), normalized [0..1].
// `lead`/`trail` are the lead/trail hand points (normalized). `relUs` is
// window-relative (pose t_us is absolute; we subtract clock.t0_us so it shares
// the face-on frames.t_us domain).
struct PoseFrame {
    qint64         relUs = 0;
    QVector<float> kp;                 // 51 floats: x0,y0,c0, x1,y1,c1, ...
    float          leadX = 0, leadY = 0,  trailX = 0, trailY = 0, handConf = 0;
    bool           hasLead = false, hasTrail = false;
};

struct PoseTrack {
    bool               ok = false;
    QVector<PoseFrame> frames;         // ascending by relUs
};

// Read analysis.pose2d (+ clock.t0_us) from swing.json. ok=false if absent.
PoseTrack readPose2d(const QString &swingDir);

// Nearest pose-frame index for a window-relative timestamp (or -1 if empty).
int poseIndexForUs(const PoseTrack &track, qint64 relUs);

} // namespace pinpoint::markup
