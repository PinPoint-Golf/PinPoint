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

#include "body_rotation.h"

#include <QVector3D>

#include <algorithm>
#include <cmath>

namespace pinpoint::analysis {
namespace {

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kPi       = 3.14159265358979323846;
constexpr double kEps      = 1e-9;

// COCO body indices. Only the four span endpoints are needed, and both exist in either layout, so
// this module answers on a legacy 17-keypoint track exactly as it does on a WholeBody one.
constexpr int kLShoulder = 5, kRShoulder = 6, kLHip = 11, kRHip = 12;

double spanPx(const PoseFrame2D &f, int a, int b, int frameW, int frameH, double confMin)
{
    if (f.conf[size_t(a)] < confMin || f.conf[size_t(b)] < confMin)
        return -1.0;                                   // not measured this frame — NOT a zero span
    const double dx = (f.kp[size_t(b)].x() - f.kp[size_t(a)].x()) * frameW;
    const double dy = (f.kp[size_t(b)].y() - f.kp[size_t(a)].y()) * frameH;
    return std::sqrt(dx * dx + dy * dy);
}

// Wrap an angle difference into (−π, π]. Used only by the IMU tier, where the projected axis
// direction is a true angle and can cross the branch cut.
double wrapPi(double a)
{
    while (a >  kPi) a -= 2.0 * kPi;
    while (a <= -kPi) a += 2.0 * kPi;
    return a;
}

// ── The camera tier ────────────────────────────────────────────────────────────────────────────
//
// Turn from the collapse of an image span. `w0` is the address span; values above it clamp to zero
// turn rather than producing a NaN out of acos — a span that measures WIDER than address is noise
// or a golfer who was not square at address, and in both cases the honest reading is "no turn
// resolved", not an imaginary angle.
void fillForeshortening(RotationChannel &out, const std::vector<PoseFrame2D> &frames, int a, int b,
                        int frameW, int frameH, double w0, const BodyRotationConfig &cfg)
{
    if (w0 < cfg.minSpanPx)
        return;                                        // denominator below its floor — refuse

    std::vector<double> sigmas;
    sigmas.reserve(frames.size());
    for (const PoseFrame2D &f : frames) {
        const double w = spanPx(f, a, b, frameW, frameH, cfg.confMin);
        if (w < 0.0) continue;
        const double ratio = std::clamp(w / w0, 0.0, 1.0);
        const double theta = std::acos(ratio);          // radians, [0, π/2]
        out.turn.push(f.t_us, theta * kRadToDeg);

        // Propagate the span noise through dθ/dw = −1 / (w₀ · sin θ). The floor on sin θ is what
        // keeps this finite as the body squares up; without it the reported uncertainty near zero
        // turn is unbounded, which is true but useless.
        const double s = std::max(std::sin(theta), cfg.sinFloor);
        sigmas.push_back(cfg.spanNoisePx / (w0 * s) * kRadToDeg);
    }
    if (out.turn.empty())
        return;

    out.tier     = RotationTier::Foreshortening;
    out.sigmaDeg = medianOfCopy(sigmas);
}

// ── The IMU tier ───────────────────────────────────────────────────────────────────────────────
//
// The segment's medio-lateral axis is anatomical +X (imu_frame_contract.md §5: every segment shares
// the solveSegment construction, e_x = the flexion / medio-lateral axis). Carried into world by
// q_anat and projected into the horizontal plane — world is Z-up, so the horizontal plane is world
// XY — its direction angle IS the segment's axial orientation. Referenced to its own address
// direction and reported as a magnitude, the same convention the camera tier uses, so the two
// tiers are interchangeable to every reader.
void fillFromImu(RotationChannel &out, const SegmentStream &seg, const std::vector<int64_t> &timeGrid,
                 int64_t addressUs)
{
    if (seg.qAnat.size() != timeGrid.size() || timeGrid.empty())
        return;

    const auto axisAngle = [&](size_t i) {
        const QVector3D ml = seg.qAnat[i].rotatedVector(QVector3D(1.0f, 0.0f, 0.0f));
        return std::atan2(double(ml.y()), double(ml.x()));
    };

    // The address reference index: nearest grid sample to the Address instant, or the first sample
    // when the ladder never found one.
    size_t addrIdx = 0;
    if (addressUs >= 0)
        addrIdx = size_t(nearestIndex(timeGrid, addressUs));
    const double a0 = axisAngle(addrIdx);

    for (size_t i = 0; i < timeGrid.size(); ++i)
        out.turn.push(timeGrid[i], std::abs(wrapPi(axisAngle(i) - a0)) * kRadToDeg);

    if (!out.turn.empty())
        out.tier = RotationTier::Imu;   // sigma left unset: no error budget is propagated here
}

} // namespace

BodyRotationResult trackBodyRotation(const PoseTrack2D &pose, const FusedStreams &streams,
                                     int frameW, int frameH, bool leadIsLeft,
                                     const std::vector<PhaseEvent> &phases,
                                     const BodyRotationConfig &cfg)
{
    Q_UNUSED(leadIsLeft)   // the magnitude convention is handedness-free by construction

    BodyRotationResult res;

    const std::vector<PoseFrame2D> &frames = pose.smoothed.empty() ? pose.frames : pose.smoothed;
    for (const PoseFrame2D &f : frames)
        res.grid.push_back(f.t_us);

    const int64_t fallbackUs = res.grid.empty() ? -1 : res.grid.front();
    const int64_t addressUs  = phaseTime(phases, Phase::Address, fallbackUs);

    // ── Address spans, robustly ────────────────────────────────────────────────────────────────
    // Median over the confident frames inside the address window, falling back to the first N
    // usable frames — the same robust-reference shape every neighbouring producer uses. A single
    // address frame is one pose estimate and inherits all of its jitter, and this one number is the
    // denominator of every subsequent turn.
    if (frameW > 0 && frameH > 0 && !frames.empty()) {
        std::vector<double> hips, shoulders;
        const auto collect = [&](bool windowed) {
            for (const PoseFrame2D &f : frames) {
                if (windowed && (addressUs < 0 || std::llabs(f.t_us - addressUs) > cfg.addrWindowUs))
                    continue;
                const double h = spanPx(f, kLHip, kRHip, frameW, frameH, cfg.confMin);
                const double s = spanPx(f, kLShoulder, kRShoulder, frameW, frameH, cfg.confMin);
                if (h > 0.0) hips.push_back(h);
                if (s > 0.0) shoulders.push_back(s);
                if (!windowed && int(hips.size()) >= cfg.addrMinFrames
                    && int(shoulders.size()) >= cfg.addrMinFrames)
                    break;
            }
        };
        collect(true);
        if (hips.empty() && shoulders.empty())
            collect(false);
        res.addrHipSpanPx      = medianOfCopy(hips);
        res.addrShoulderSpanPx = medianOfCopy(shoulders);
    }

    // ── Per segment: the IMU if it is bound, the camera if it is not ───────────────────────────
    // Resolved INDEPENDENTLY per segment. A swing with a pelvis IMU and no thorax IMU gets a
    // measured pelvis and an estimated chest, which is the right answer — refusing the pair because
    // half of it could be better measured would throw away the half that could not.
    if (const SegmentStream *s = streams.streamFor(SegmentRole::Pelvis))
        fillFromImu(res.pelvis, *s, streams.timeGrid, addressUs);
    if (res.pelvis.tier == RotationTier::None && frameW > 0 && frameH > 0)
        fillForeshortening(res.pelvis, frames, kLHip, kRHip, frameW, frameH, res.addrHipSpanPx, cfg);

    if (const SegmentStream *s = streams.streamFor(SegmentRole::Thorax))
        fillFromImu(res.thorax, *s, streams.timeGrid, addressUs);
    if (res.thorax.tier == RotationTier::None && frameW > 0 && frameH > 0)
        fillForeshortening(res.thorax, frames, kLShoulder, kRShoulder, frameW, frameH,
                           res.addrShoulderSpanPx, cfg);

    // The IMU tier writes onto the stream time grid, which is not the pose grid. Adopt whichever
    // grid actually carries samples so the resample target is never empty.
    if (res.grid.empty()) {
        if (!res.pelvis.turn.empty())      res.grid = res.pelvis.turn.t_us;
        else if (!res.thorax.turn.empty()) res.grid = res.thorax.turn.t_us;
    }

    // ── X-factor, and the stretch ──────────────────────────────────────────────────────────────
    // Both segments are needed: separation is a relationship, and one half of it is not a partial
    // answer, it is a different quantity. Evaluated on the shared grid through the same interpolator
    // the series resample uses, so the difference is taken between values at the SAME instant even
    // when the two tiers disagree about the sampling rate.
    if (!res.pelvis.turn.empty() && !res.thorax.turn.empty() && !res.grid.empty()) {
        for (int64_t t : res.grid) {
            const double p = interpChannel(res.pelvis.turn.t_us, res.pelvis.turn.value, t);
            const double x = interpChannel(res.thorax.turn.t_us, res.thorax.turn.value, t);
            res.xFactor.push(t, x - p);
        }

        // The stretch is measured FROM THE TOP, so it needs a segmented Top. Without one the
        // channel is absent rather than anchored to something arbitrary — a stretch referenced to
        // the wrong instant is a plausible number about nothing.
        const int64_t topUs = phaseTime(phases, Phase::Top, -1);
        if (topUs >= 0) {
            const double atTop = interpChannel(res.xFactor.t_us, res.xFactor.value, topUs);
            for (size_t i = 0; i < res.xFactor.t_us.size(); ++i)
                res.xFactorStretch.push(res.xFactor.t_us[i], res.xFactor.value[i] - atTop);
        }
    }

    res.valid = !res.pelvis.turn.empty() || !res.thorax.turn.empty();
    return res;
}

std::vector<MetricSeries> buildBodyRotationSeries(const BodyRotationResult &res,
                                                  const std::vector<PhaseEvent> &phases)
{
    std::vector<MetricSeries> out;
    if (!res.valid || res.grid.empty())
        return out;

    const QString deg = QStringLiteral("°");

    const auto emitRotation = [&](const RotationChannel &ch, const char *key, const char *label) {
        MetricSeries m = buildChannelSeries(res.grid, ch.turn, QString::fromLatin1(key),
                                            QString::fromUtf8(label), deg, phases);
        if (m.key.isEmpty())
            return;
        // Carry the propagated uncertainty ONLY where one was actually computed. The field's
        // contract is explicit that absent means "not characterised" rather than "zero error", so
        // the IMU tier — which has no error budget through this path — leaves it unset rather than
        // claiming a perfect measurement.
        if (ch.sigmaDeg > 0.0)
            m.sigma = ch.sigmaDeg;
        out.push_back(std::move(m));
    };

    emitRotation(res.pelvis, "pelvisRotation", "Pelvis rotation");
    emitRotation(res.thorax, "thoraxRotation", "Thorax rotation");

    appendIfProduced(out, buildChannelSeries(res.grid, res.xFactor, QStringLiteral("xFactor"),
                                             QStringLiteral("X-factor"), deg, phases));
    appendIfProduced(out, buildChannelSeries(res.grid, res.xFactorStretch,
                                             QStringLiteral("xFactorStretch"),
                                             QStringLiteral("X-factor stretch"), deg, phases));
    return out;
}

} // namespace pinpoint::analysis
