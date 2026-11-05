// SPDX-License-Identifier: GPL-3.0-or-later
// The guest's wall clock as the F1 Date and Time tab sets it (docs/superpowers/specs/2026-11-05-date-and-time-design.md).
//   System mode: the game sees the PC clock (the time services' old behaviour).
//   Custom mode: the game clock starts at a saved date and runs 1440 / day_minutes times as fast as real time. Only the wall clocks
//                (user, local, network) change; the steady clock and the tick counter always run at real speed.
// The settings are the [clock] section of controls.ini (play mode); the clock's position between runs is a small state file next to it.
#pragma once
#include <cstdint>
#include <string>

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

// All thread safe (services/time.cpp holds the state). None of them brings the time services up, so play mode may call them first.
void clock_start(const ClockConfig& c, const std::string& state_path);   // once at play start: reads the state file, anchors the clock
void clock_apply(const ClockConfig& c);     // new settings: re-anchors (a Save restarts from its date, other changes keep the time)
void clock_tick();                          // every frame: refreshes the shared memory of a fast clock, writes the state file when due
void clock_flush();                         // at exit: writes the state file (Custom mode)
int64_t clock_guest_now();                  // what the game's user clock says now (posix seconds)
int64_t clock_host_now();                   // the PC clock (posix seconds)
bool clock_to_local(int64_t posix, LocalTime* out);         // under the device time zone rule ([time] zone)
// False for a field out of range, a year outside 2000..2099 or a local time the zone skips (DST); an ambiguous one gives the earlier.
bool clock_from_local(const LocalTime& t, int64_t* posix);

}  // namespace nxrt
