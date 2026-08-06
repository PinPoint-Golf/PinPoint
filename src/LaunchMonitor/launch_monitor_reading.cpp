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

#include "launch_monitor_reading.h"

namespace pinpoint::lm {

const std::vector<const char *> &fieldGroups()
{
    static const std::vector<const char *> groups = { "Club", "Strike", "Launch", "Spin", "Flight" };
    return groups;
}

const std::vector<FieldDef> &fieldDefs()
{
    using R = LaunchMonitorReading;
    static const std::vector<FieldDef> defs = {
        // Measured where we also estimate — the validation pairs. The labels carry
        // "(measured)" so that a table showing both cannot be misread.
        { "lm.clubheadSpeed",   "clubheadSpeed",   "Clubhead speed (measured)", "mph",   "Club",   "CLUB SPEED",  &R::clubheadSpeed   },
        { "lm.ballSpeed",       "ballSpeed",       "Ball speed (measured)",     "mph",   "Launch", "BALL SPEED",  &R::ballSpeed       },
        { "lm.smashFactor",     "smashFactor",     "Smash factor",              "ratio", "Strike", "SMASH FAC.",  &R::smashFactor     },
        { "lm.attackAngle",     "attackAngle",     "Attack angle (measured)",   "°",     "Club",   "ATTACK ANG.", &R::attackAngle     },
        { "lm.clubPath",        "clubPath",        "Club path (measured)",      "°",     "Club",   "CLUB PATH",   &R::clubPath        },
        { "lm.launchAngle",     "launchAngle",     "Launch angle (measured)",   "°",     "Launch", "LAUNCH ANG.", &R::launchAngle     },
        { "lm.launchDirection", "launchDirection", "Start direction (measured)","°",     "Launch", "START DIR.",  &R::launchDirection },
        { "lm.lowPointAhead",   "lowPointAhead",   "Low point (measured)",      "in",    "Club",   "LOW POINT",   &R::lowPointAhead   },

        // Club delivery the cameras cannot resolve.
        { "lm.faceAngle",       "faceAngle",       "Face angle",    "°",   "Club", "FACE ANG.",    &R::faceAngle    },
        { "lm.faceToPath",      "faceToPath",      "Face to path",  "°",   "Club", "FACE TO PATH", &R::faceToPath   },
        { "lm.dynamicLoft",     "dynamicLoft",     "Dynamic loft",  "°",   "Club", "DYN. LOFT",    &R::dynamicLoft  },
        { "lm.spinLoft",        "spinLoft",        "Spin loft",     "°",   "Club", "SPIN LOFT",    &R::spinLoft     },
        { "lm.lieAngle",        "lieAngle",        "Lie angle",     "°",   "Club", "LIE ANG.",     &R::lieAngle     },
        { "lm.closureRate",     "closureRate",     "Closure rate",  "°/s", "Club", "CLOSURE",      &R::closureRate  },

        // Strike.
        { "lm.strikeLocation",  "strikeLocation",  "Strike location", "mm", "Strike", "STRIKE LOC.", &R::strikeLocation },
        { "lm.strikeHeight",    "strikeHeight",    "Strike height",   "mm", "Strike", "STRIKE HT.",  &R::strikeHeight   },

        // Spin.
        { "lm.spinRate",        "spinRate",        "Spin rate", "rpm", "Spin", "SPIN RATE", &R::spinRate },
        { "lm.backSpin",        "backSpin",        "Back spin", "rpm", "Spin", "BACK SPIN", &R::backSpin },
        { "lm.sideSpin",        "sideSpin",        "Side spin", "rpm", "Spin", "SIDE SPIN", &R::sideSpin },
        { "lm.spinAxis",        "spinAxis",        "Spin axis", "°",   "Spin", "SPIN AXIS", &R::spinAxis },

        // Flight-model outputs.
        { "lm.carryDistance",   "carryDistance",   "Carry",            "yd", "Flight", "CARRY",        &R::carryDistance },
        { "lm.totalDistance",   "totalDistance",   "Total distance",   "yd", "Flight", "TOTAL",        &R::totalDistance },
        { "lm.offline",         "offline",         "Offline",          "yd", "Flight", "OFFLINE",      &R::offline       },
        { "lm.peakHeight",      "peakHeight",      "Peak height",      "ft", "Flight", "PEAK HT.",     &R::peakHeight    },
        { "lm.descentAngle",    "descentAngle",    "Descent angle",    "°",  "Flight", "DESCENT ANG.", &R::descentAngle  },
        { "lm.distanceToPin",   "distanceToPin",   "Distance to pin",  "yd", "Flight", "TO PIN",       &R::distanceToPin },
    };
    return defs;
}

bool LaunchMonitorReading::hasAnyValue() const
{
    for (const FieldDef &f : fieldDefs())
        if ((this->*f.member).has_value())
            return true;
    return false;
}

} // namespace pinpoint::lm
