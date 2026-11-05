// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's wall clock as the F1 Date and Time tab sets it (docs/superpowers/specs/2026-11-05-date-and-time-design.md).
//   System mode: the game sees the PC clock (the time services' old behaviour).
//   Custom mode: the game clock starts at a saved date and runs 1440 / day_minutes times as fast as real time. Only the wall clocks
//                (user, local, network) change; the steady clock and the tick counter always run at real speed.
// The settings are the [clock] section of controls.ini (play mode); the clock's position between runs is a small state file next to it.
#pragma once
#include <cstdint>

namespace nxrt {

struct ClockConfig {
    bool custom = false;        // [clock] mode = system | custom
    int64_t start = 0;          // [clock] start: posix seconds of the saved custom date (0 = never saved)
    int64_t saved_at = 0;       // [clock] saved_at: host posix seconds when Save was pressed (a new Save of the same date restarts it)
    int day_minutes = 1440;     // [clock] day_minutes, 1..1440
    bool keep_passing = false;  // [clock] keep_passing
    bool operator==(const ClockConfig&) const = default;
};

struct LocalTime {              // a calendar time under the device time zone rule
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;   // month 1..12
    int weekday = 0;            // 0 = Sunday
};

}  // namespace nxrt
