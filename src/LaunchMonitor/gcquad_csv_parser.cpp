/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "gcquad_csv_parser.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <cmath>

namespace pinpoint::lm {
namespace {

// Spelled out rather than M_PI: that macro needs _USE_MATH_DEFINES before the
// first <cmath> on MSVC, which the app target sets and the test targets do not.
// The same constant is declared locally in shaft_positions.h for this reason.
constexpr double kPi = 3.14159265358979323846;

// The quantity family a column belongs to, which is what decides whether a
// declared unit is convertible into the catalogue's.
enum class Dim { Speed, Distance, Angle, AngRate, Spin };

struct ColumnDef {
    const char *token;        // normalised header token
    const char *defaultUnit;  // assumed when the header declares none
    Dim         dim;
    const char *targetUnit;   // the catalogue's unit for this metric
    // The widest value this quantity can physically take, IN targetUnit. This is a
    // sentinel filter and not a validator: every bound is set several times beyond
    // anything golf produces, so a genuine outlier survives and a "no data" marker
    // — which arrives six orders of magnitude out — does not. See kNoData below.
    double      lo;
    double      hi;
    std::optional<double> LaunchMonitorReading::*member;
};

// Every column we consume, plus the aliases Foresight has been seen to use for
// the same quantity. A token absent from here is ignored, not an error.
const std::vector<ColumnDef> &columnDefs()
{
    using R = LaunchMonitorReading;
    static const std::vector<ColumnDef> defs = {
        { "clubheadspeed",   "m/s",   Dim::Speed,    "mph",       0.0,    300.0, &R::clubheadSpeed    },
        { "clubspeed",       "m/s",   Dim::Speed,    "mph",       0.0,    300.0, &R::clubheadSpeed    },
        { "ballspeed",       "m/s",   Dim::Speed,    "mph",       0.0,    400.0, &R::ballSpeed        },

        { "launchangle",     "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::launchAngle      },
        { "azimuth",         "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::launchDirection  },
        { "launchdirection", "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::launchDirection  },

        // Foresight states club path on two axes. The VERTICAL one is what the
        // rest of golf calls attack angle, and it is the twin of our camera
        // estimate; the horizontal one is club path proper.
        { "vertpath",        "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::attackAngle      },
        { "attackangle",     "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::attackAngle      },
        { "horizpath",       "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::clubPath         },
        { "clubpath",        "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::clubPath         },

        { "facetotarget",    "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::faceAngle        },
        { "faceangle",       "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::faceAngle        },
        { "facetopath",      "deg",   Dim::Angle,    "deg",    -180.0,    180.0, &R::faceToPath       },
        { "loft",            "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::dynamicLoft      },
        { "dynamicloft",     "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::dynamicLoft      },
        { "lie",             "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::lieAngle         },
        { "closurerate",     "deg/s", Dim::AngRate,  "deg/s", -50000.0,  50000.0, &R::closureRate     },

        { "sidespin",        "rpm",   Dim::Spin,     "rpm",  -30000.0,  30000.0, &R::sideSpin         },
        { "backspin",        "rpm",   Dim::Spin,     "rpm",  -30000.0,  30000.0, &R::backSpin         },
        { "totalspin",       "rpm",   Dim::Spin,     "rpm",       0.0,  30000.0, &R::spinRate         },
        { "spinrate",        "rpm",   Dim::Spin,     "rpm",       0.0,  30000.0, &R::spinRate         },

        { "horizimpact",     "mm",    Dim::Distance, "mm",     -500.0,    500.0, &R::strikeLocation   },
        { "vertimpact",      "mm",    Dim::Distance, "mm",     -500.0,    500.0, &R::strikeHeight     },

        { "carry",           "m",     Dim::Distance, "yd",        0.0,   1000.0, &R::carryDistance    },
        { "carrydistance",   "m",     Dim::Distance, "yd",        0.0,   1000.0, &R::carryDistance    },
        { "totaldistance",   "m",     Dim::Distance, "yd",        0.0,   1000.0, &R::totalDistance    },
        { "offline",         "m",     Dim::Distance, "yd",    -1000.0,   1000.0, &R::offline          },
        { "peakheight",      "m",     Dim::Distance, "ft",        0.0,   2000.0, &R::peakHeight       },
        { "apex",            "m",     Dim::Distance, "ft",        0.0,   2000.0, &R::peakHeight       },
        { "descentangle",    "deg",   Dim::Angle,    "deg",     -90.0,     90.0, &R::descentAngle     },
        { "distancetopin",   "m",     Dim::Distance, "yd",        0.0,   2000.0, &R::distanceToPin    },
    };
    return defs;
}

// ── "The device did not measure this" ──────────────────────────────────────────
//
// A GCQuad reads the CLUB from reflective stickers on it. With no stickers on the
// face — or none it could see — the ball numbers are still measured optically and
// are perfectly good, while every club quantity is unknown. FSX2020 does not leave
// those cells empty: it writes 16777215 into them. That is 0xFFFFFF, and also the
// largest integer a float32 represents exactly, which is what it is being used as
// — an in-band "no value" marker.
//
// TAKEN AT FACE VALUE THE ROW IS NOT MERELY WRONG, IT IS PLAUSIBLE-LOOKING NONSENSE
// that then propagates: 16777215° of dynamic loft, and a smash factor of 0.000007
// from the derivation below.
//
// THE MARKER IS UNIT-CONVERTED ON ITS WAY OUT, so it does not always arrive as
// 16777215. In a metric-configured installation club head speed reads 7500086 —
// which is 16777215 mph × 0.44704, i.e. the same marker held internally in mph and
// converted to m/s like a real measurement. So matching the constant alone is not
// enough, and the ColumnDef range above is the net that actually catches it: any
// scaling of a 16.7-million marker lands astronomically outside golf.
constexpr double kNoData = 16777215.0;

bool isNoDataMarker(double raw)
{
    return std::abs(std::abs(raw) - kNoData) < 1.0;
}

// Multiplier taking `unit` into the family's base unit (m/s, m, deg, deg/s, rpm).
// Returns 0 for a unit we do not know, which the caller reads as "fall back".
double toBaseFactor(Dim dim, const QString &u)
{
    switch (dim) {
    case Dim::Speed:
        if (u == QLatin1String("m/s") || u == QLatin1String("mps"))    return 1.0;
        if (u == QLatin1String("mph"))                                 return 0.44704;
        if (u == QLatin1String("kph") || u == QLatin1String("km/h"))   return 1.0 / 3.6;
        if (u == QLatin1String("fps") || u == QLatin1String("ft/s"))   return 0.3048;
        return 0.0;
    case Dim::Distance:
        if (u == QLatin1String("m"))                                   return 1.0;
        if (u == QLatin1String("yd") || u == QLatin1String("yds")
            || u == QLatin1String("yards") || u == QLatin1String("yard")) return 0.9144;
        if (u == QLatin1String("ft") || u == QLatin1String("feet"))    return 0.3048;
        if (u == QLatin1String("in") || u == QLatin1String("inches"))  return 0.0254;
        if (u == QLatin1String("cm"))                                  return 0.01;
        if (u == QLatin1String("mm"))                                  return 0.001;
        return 0.0;
    case Dim::Angle:
        if (u == QLatin1String("deg") || u == QLatin1String("degrees")
            || u == QLatin1String("°"))                                return 1.0;
        if (u == QLatin1String("rad") || u == QLatin1String("radians")) return 180.0 / kPi;
        return 0.0;
    case Dim::AngRate:
        if (u == QLatin1String("deg/s") || u == QLatin1String("degs")
            || u == QLatin1String("°/s") || u == QLatin1String("dps"))  return 1.0;
        if (u == QLatin1String("rad/s"))                               return 180.0 / kPi;
        // Foresight documents closure rate as reported in "degrees per second (dps) OR revolutions
        // per minute (rpm)" — the same quantity, two units, chosen by the installation. rpm here is
        // an ANGULAR RATE and not the spin unit: one revolution is 360° over a minute, so 6 °/s.
        // Without this the header's own "rpm" is unrecognised, falls back to the deg/s default and
        // the value is read SIX TIMES TOO LOW — an error that lands inside a plausible range and
        // would never look wrong.
        if (u == QLatin1String("rpm") || u == QLatin1String("rev/min"))  return 6.0;
        return 0.0;
    case Dim::Spin:
        if (u == QLatin1String("rpm"))                                 return 1.0;
        return 0.0;
    }
    return 0.0;
}

// Convert a value the file declared in `from` into the catalogue's `to`. When the
// header declares a unit we do not recognise we fall back to the column's
// documented default rather than dropping the number — a mis-scaled reading is
// visible and correctable, a silently absent one is neither.
double convertUnits(double v, Dim dim, const QString &from, const char *defaultUnit,
                    const char *to)
{
    double fromF = toBaseFactor(dim, from);
    if (fromF == 0.0)
        fromF = toBaseFactor(dim, QString::fromLatin1(defaultUnit));
    const double toF = toBaseFactor(dim, QString::fromLatin1(to));
    if (fromF == 0.0 || toF == 0.0)
        return v;
    return v * fromF / toF;
}

} // namespace

void normaliseHeaderCell(const QString &cell, QString *token, QString *unit)
{
    const int paren = cell.indexOf(QLatin1Char('('));
    const QString namePart = paren >= 0 ? cell.left(paren) : cell;

    QString t;
    t.reserve(namePart.size());
    for (const QChar c : namePart)
        if (c.isLetterOrNumber())
            t.append(c.toLower());
    if (token) *token = t;

    if (!unit)
        return;
    unit->clear();
    if (paren < 0)
        return;
    const int close = cell.indexOf(QLatin1Char(')'), paren);
    *unit = cell.mid(paren + 1, (close > paren ? close : cell.size()) - paren - 1)
                .trimmed().toLower();
}

QStringList splitCsvLine(const QString &line)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuotes) {
            // "" inside a quoted field is a literal quote.
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    cur.append(QLatin1Char('"'));
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cur.append(c);
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            out.append(cur.trimmed());
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    out.append(cur.trimmed());
    return out;
}

std::optional<LaunchMonitorReading> parseLastShotCsv(const QByteArray &bytes, QString *error)
{
    const auto fail = [error](const QString &why) -> std::optional<LaunchMonitorReading> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (error) error->clear();

    // Tolerate CRLF, bare CR and a UTF-8 BOM; FSX2020 writes Windows line endings
    // and the file may be read across an SMB share.
    QString text = QString::fromUtf8(bytes);
    if (!text.isEmpty() && text.at(0) == QChar(0xFEFF))
        text.remove(0, 1);
    const QStringList rawLines = text.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));

    QStringList lines;
    for (const QString &l : rawLines)
        if (!l.trimmed().isEmpty())
            lines.append(l);

    if (lines.size() < 2)
        return fail(QStringLiteral("expected a header row and a shot row, got %1 non-empty line(s)")
                        .arg(lines.size()));

    // ── Header ──────────────────────────────────────────────────────────────
    const QStringList headerCells = splitCsvLine(lines.at(0));

    QHash<QString, const ColumnDef *> byToken;
    for (const ColumnDef &d : columnDefs())
        byToken.insert(QString::fromLatin1(d.token), &d);

    struct Bound { const ColumnDef *def; QString unit; };
    QHash<int, Bound> boundColumns;     // column index → what it feeds
    int shotIdCol = -1;
    int clubCol   = -1;

    for (int i = 0; i < headerCells.size(); ++i) {
        QString token, unit;
        normaliseHeaderCell(headerCells.at(i), &token, &unit);
        if (token.isEmpty())
            continue;
        if (token == QLatin1String("shotid")) { shotIdCol = i; continue; }
        if (token == QLatin1String("club"))   { clubCol   = i; continue; }
        if (const ColumnDef *d = byToken.value(token, nullptr))
            boundColumns.insert(i, Bound{ d, unit });
    }

    if (shotIdCol < 0)
        return fail(QStringLiteral("no 'Shot ID' column in the header"));
    if (boundColumns.isEmpty())
        return fail(QStringLiteral("header carried no recognised measurement columns"));

    // ── Shot row ────────────────────────────────────────────────────────────
    const QStringList cells = splitCsvLine(lines.at(1));

    LaunchMonitorReading r;
    if (shotIdCol >= cells.size() || cells.at(shotIdCol).isEmpty())
        return fail(QStringLiteral("shot row has no Shot ID"));
    r.deviceShotId = cells.at(shotIdCol);
    if (clubCol >= 0 && clubCol < cells.size())
        r.deviceClub = cells.at(clubCol);

    // Which fields the device explicitly declined to measure. Needed after the loop
    // because one printed column is itself derived from two others — see below.
    bool faceAngleUnmeasured = false;
    bool clubPathUnmeasured  = false;

    for (auto it = boundColumns.cbegin(); it != boundColumns.cend(); ++it) {
        const int idx = it.key();
        if (idx >= cells.size())
            continue;                       // short row: absent, not zero
        const QString cell = cells.at(idx);
        if (cell.isEmpty())
            continue;
        bool ok = false;
        const double raw = cell.toDouble(&ok);
        if (!ok || !std::isfinite(raw))
            continue;                       // a junk cell loses its own value, not the row
        const ColumnDef *d = it.value().def;
        const double v = convertUnits(raw, d->dim, it.value().unit, d->defaultUnit,
                                      d->targetUnit);

        // Unmeasured stays ABSENT, exactly like a short row or a junk cell. The
        // board then shows nothing for it, which is the truth, instead of a number
        // no golfer can act on.
        if (isNoDataMarker(raw) || !std::isfinite(v) || v < d->lo || v > d->hi) {
            if (d->member == &LaunchMonitorReading::faceAngle) faceAngleUnmeasured = true;
            if (d->member == &LaunchMonitorReading::clubPath)  clubPathUnmeasured  = true;
            continue;
        }
        r.*(d->member) = v;
    }

    // FSX2020 DERIVES Face to Path from face and path, so when both are unmeasured
    // it prints their difference — a clean 0.000000 that is inside every plausible
    // range and survives the filter above. It is the one unmeasured club value that
    // does not look unmeasured, and a face square to the path is precisely what a
    // golfer would read it as. Drop it only when we positively saw BOTH ingredients
    // marked unmeasured; a file that simply omits those columns keeps its value.
    if (r.faceToPath && faceAngleUnmeasured && clubPathUnmeasured)
        r.faceToPath.reset();

    // ── Values the device states implicitly ─────────────────────────────────
    // Foresight prints the ingredients but not these three, and they are the ones
    // a golfer actually reads. Every input is the device's, so the results are
    // the device's too and carry the same lm. prefix — see launch_monitor_reading.h.

    // Spin axis: the tilt of the rotation axis, positive tilting right for a
    // right-handed golfer, which is the same sign convention as side spin.
    if (!r.spinAxis && r.sideSpin && r.backSpin) {
        const double side = *r.sideSpin, back = *r.backSpin;
        if (side != 0.0 || back != 0.0)
            r.spinAxis = std::atan2(side, back) * 180.0 / kPi;
    }

    // Smash factor: unit-free, so it only needs the two speeds to share a unit,
    // which they do by the time we get here.
    if (!r.smashFactor && r.ballSpeed && r.clubheadSpeed && *r.clubheadSpeed > 0.0)
        r.smashFactor = *r.ballSpeed / *r.clubheadSpeed;

    // Spin loft: the angle between delivered loft and the direction of travel.
    if (!r.spinLoft && r.dynamicLoft && r.attackAngle)
        r.spinLoft = *r.dynamicLoft - *r.attackAngle;

    if (!r.hasAnyValue())
        return fail(QStringLiteral("shot row carried no numeric values"));

    return r;
}

} // namespace pinpoint::lm
