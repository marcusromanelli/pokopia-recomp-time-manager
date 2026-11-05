// SPDX-License-Identifier: GPL-3.0-or-later
// The custom guest clock's pure parts (src/services/clock_math.h): speed, the anchored clock, resuming after a run, the state file and the
// save cadence, the lock rule and the hours text of the F1 Date and Time tab.
#include "test_framework.h"

#include <string>

#include "../src/services/clock_math.h"
#include "nxrt/clock.h"

NXTEST_GROUP(clock_math)

using namespace nxrt;
using namespace nxrt::services::clockm;

NXTEST(speed_clamps) {
    CHECK(speed_of(1440) == 1.0);
    CHECK(speed_of(20) == 72.0);
    CHECK(speed_of(0) == 1440.0);
    CHECK(speed_of(5000) == 1.0);
}

NXTEST(guest_at_scales) {
    Anchor a{1000.0, 5'000'000'000ull, 72.0};
    CHECK(guest_at(a, 6'000'000'000ull) == 1072.0);
    CHECK(guest_at(a, 4'000'000'000ull) == 1000.0);   // a tick before the anchor never goes back
}

NXTEST(state_round_trip) {
    State s{1762353120, 1762300000, 1762000000, 1762100000};
    State o;
    REQUIRE(parse_state(format_state(s), &o));
    CHECK(o.guest == s.guest && o.host == s.host && o.start == s.start && o.saved_at == s.saved_at);
}

NXTEST(state_garbage_rejected) {
    State o;
    CHECK(!parse_state("", &o));
    CHECK(!parse_state("[clock]\nguest = x\n", &o));
    CHECK(!parse_state("\x01\x02junk", &o));
    CHECK(!parse_state("[clock]\nguest = 5\nhost = 6\nstart = 7\n", &o));   // saved_at missing
}

NXTEST(resume_no_state_uses_start) {
    ClockConfig c;
    c.custom = true;
    c.start = 500;
    CHECK(resume(c, nullptr, false, 9999) == 500.0);
}

NXTEST(resume_new_save_wins) {
    ClockConfig c;
    c.custom = true;
    c.start = 500;
    c.saved_at = 2;
    State s{900, 100, 500, 1};
    CHECK(resume(c, &s, true, 200) == 500.0);
}

NXTEST(resume_keep_off) {
    ClockConfig c;
    c.custom = true;
    c.start = 500;
    State s{900, 100, 500, 0};
    CHECK(resume(c, &s, true, 200) == 900.0);
}

NXTEST(resume_keep_on_scaled) {
    ClockConfig c;
    c.custom = true;
    c.start = 500;
    c.day_minutes = 20;
    c.keep_passing = true;
    State s{900, 100, 500, 0};
    CHECK(resume(c, &s, true, 110) == 900.0 + 10 * 72.0);
}

NXTEST(resume_pc_clock_backwards) {
    ClockConfig c;
    c.custom = true;
    c.start = 500;
    c.keep_passing = true;
    State s{900, 100, 500, 0};
    CHECK(resume(c, &s, true, 50) == 900.0);
}

NXTEST(save_cadence) {
    CHECK(!should_save(1000, 1000, 500'000'000ull, 0));     // under 1 s since the last write
    CHECK(should_save(1060, 1000, 1'000'000'000ull, 0));    // 60 game s
    CHECK(!should_save(1010, 1000, 5'000'000'000ull, 0));   // neither
    CHECK(should_save(1010, 1000, 10'000'000'000ull, 0));   // 10 real s
}

NXTEST(accept_respects_lock) {
    ClockConfig a, b;
    b.custom = true;
    CHECK(clock_accept(a, b, false) == a);
    CHECK(clock_accept(a, b, true) == b);
}

NXTEST(hours_text_copy) {
    CHECK(hours_text(1440) == "24 hours");
    CHECK(hours_text(60) == "1 hour");
    CHECK(hours_text(20) == "0.33 hours");
    CHECK(hours_text(90) == "1.5 hours");
}
