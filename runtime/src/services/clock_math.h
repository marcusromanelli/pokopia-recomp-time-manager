// SPDX-License-Identifier: GPL-3.0-or-later
// The custom guest clock's pure parts (nxrt/clock.h; time.cpp owns the state): speed, the anchored clock, where a new run resumes, the state
// file, how often it is written, the lock rule and the hours text of the F1 Date and Time tab. No I/O, no clocks read here.
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/clock.h"

namespace nxrt::services::clockm {

constexpr int kDayMinutes = 1440;   // a real day; also the slowest custom day (the shared memory cannot show a clock slower than real time)

double speed_of(int day_minutes);   // 1440 / day_minutes, day_minutes clamped to 1..1440

struct Anchor {
    double game = 0;                // guest posix seconds at the anchor
    uint64_t mono_ns = 0;           // the monotonic clock at the anchor
    double speed = 1;
};
double guest_at(const Anchor& a, uint64_t mono_ns);   // guest posix seconds at mono_ns (never before a.game)

struct State {                      // the state file: the clock's position when it was last written
    int64_t guest = 0;              // guest posix seconds
    int64_t host = 0;               // the PC clock (posix seconds) at that moment
    int64_t start = 0;              // the ClockConfig::start and saved_at in force then (a newer Save restarts from its date)
    int64_t saved_at = 0;
};
std::string format_state(const State& s);
bool parse_state(const std::string& text, State* out);   // false for a missing key or a value that is not a number

// The guest time a run starts at: c.start without a state or after a newer Save; else the last guest time, plus the time away at the clock's
// speed when keep_passing is on (never less than the last guest time when the PC clock went back).
double resume(const ClockConfig& c, const State* s, bool have_state, int64_t host_now);

// The state file is due: the game clock moved 60 s or 10 real seconds passed since the last write, and at least 1 real second did.
bool should_save(double guest, double saved_guest, uint64_t mono_ns, uint64_t saved_mono_ns);

ClockConfig clock_accept(const ClockConfig& cur, const ClockConfig& want, bool editable);   // the clock part of new settings while locked

std::string hours_text(int minutes);   // "24 hours", "1 hour", "0.33 hours", "1.5 hours"

}  // namespace nxrt::services::clockm
