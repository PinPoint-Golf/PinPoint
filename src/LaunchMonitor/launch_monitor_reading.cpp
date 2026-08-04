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

const std::vector<FieldDef> &fieldDefs()
{
    using R = LaunchMonitorReading;
    static const std::vector<FieldDef> defs = {
        // Measured where we also estimate — the validation pairs. The labels carry
        // "(measured)" so that a table showing both cannot be misread.
        { "lm.clubheadSpeed",   "clubheadSpeed",   "Clubhead speed (measured)", "mph",   &R::clubheadSpeed   },
        { "lm.ballSpeed",       "ballSpeed",       "Ball speed (measured)",     "mph",   &R::ballSpeed       },
        { "lm.smashFactor",     "smashFactor",     "Smash factor",              "ratio", &R::smashFactor     },
        { "lm.attackAngle",     "attackAngle",     "Attack angle (measured)",   "°",     &R::attackAngle     },
        { "lm.clubPath",        "clubPath",        "Club path (measured)",      "°",     &R::clubPath        },
        { "lm.launchAngle",     "launchAngle",     "Launch angle (measured)",   "°",     &R::launchAngle     },
        { "lm.launchDirection", "launchDirection", "Start direction (measured)","°",     &R::launchDirection },

        // Club delivery the cameras cannot resolve.
        { "lm.faceAngle",       "faceAngle",       "Face angle",    "°",   &R::faceAngle    },
        { "lm.faceToPath",      "faceToPath",      "Face to path",  "°",   &R::faceToPath   },
        { "lm.dynamicLoft",     "dynamicLoft",     "Dynamic loft",  "°",   &R::dynamicLoft  },
        { "lm.spinLoft",        "spinLoft",        "Spin loft",     "°",   &R::spinLoft     },
        { "lm.lieAngle",        "lieAngle",        "Lie angle",     "°",   &R::lieAngle     },
        { "lm.closureRate",     "closureRate",     "Closure rate",  "°/s", &R::closureRate  },

        // Strike.
        { "lm.strikeLocation",  "strikeLocation",  "Strike location", "mm", &R::strikeLocation },
        { "lm.strikeHeight",    "strikeHeight",    "Strike height",   "mm", &R::strikeHeight   },

        // Spin.
        { "lm.spinRate",        "spinRate",        "Spin rate", "rpm", &R::spinRate },
        { "lm.backSpin",        "backSpin",        "Back spin", "rpm", &R::backSpin },
        { "lm.sideSpin",        "sideSpin",        "Side spin", "rpm", &R::sideSpin },
        { "lm.spinAxis",        "spinAxis",        "Spin axis", "°",   &R::spinAxis },

        // Flight-model outputs.
        { "lm.carryDistance",   "carryDistance",   "Carry",            "yd", &R::carryDistance },
        { "lm.totalDistance",   "totalDistance",   "Total distance",   "yd", &R::totalDistance },
        { "lm.offline",         "offline",         "Offline",          "yd", &R::offline       },
        { "lm.peakHeight",      "peakHeight",      "Peak height",      "ft", &R::peakHeight    },
        { "lm.descentAngle",    "descentAngle",    "Descent angle",    "°",  &R::descentAngle  },
        { "lm.distanceToPin",   "distanceToPin",   "Distance to pin",  "yd", &R::distanceToPin },
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
