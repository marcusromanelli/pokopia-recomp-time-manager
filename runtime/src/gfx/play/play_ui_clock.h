// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 page's Date and Time tab (play_ui_clock.cpp): the game sees the PC clock, or a custom clock that starts at a saved date and can run
// faster (a day of 1..1440 real minutes). Changes only while LiveStatus::clock_editable (the title, before the player entered the world).
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/clock.h"
#include "play_ui.h"

namespace nxrt::play {

struct ClockUi {                    // the tab's editing state, not saved
    bool picked_custom = false;     // the Custom date choice is picked (the clock changes only on Save)
    int day = 1, month = 1, year = 2026, hour = 0, minute = 0;
    bool filled = false;            // the fields were filled from the game's clock once
    std::string notice;             // the last Save's message
    bool notice_warn = false;
};

// Draws the tab into the current window. True when c changed (the page then applies the clock at once).
bool clock_ui(nxrt::ClockConfig& c, const LiveStatus& live, ClockUi& ui);
// The Save button: the date in the fields becomes the custom clock's start. False (c unchanged, a warning in ui.notice) for a date that does
// not exist or is outside 2000..2099; an earlier date than the game's clock is saved with a warning.
bool clock_ui_save(nxrt::ClockConfig& c, const LiveStatus& live, ClockUi& ui);
// "Wednesday, 5 November 2026   14:32:07" under the device time zone rule ("" when it cannot be converted).
std::string clock_text(int64_t posix);

}  // namespace nxrt::play
