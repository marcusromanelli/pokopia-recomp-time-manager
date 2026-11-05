// SPDX-License-Identifier: GPL-3.0-or-later
// The F1 page's Date and Time tab (play_ui_clock.h).
#include "play_ui_clock.h"

#include <algorithm>
#include <cstdio>

#include "../../services/clock_math.h"
#include "imgui.h"

namespace nxrt::play {
namespace {

constexpr const char* kWeekdays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr const char* kMonths[12] = {"January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November", "December"};
const ImVec4 kOrange(1.0f, 0.65f, 0.2f, 1.0f);

void tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("%s", text);
}

nxrt::LocalTime fields_of(const ClockUi& ui) {
    nxrt::LocalTime t;
    t.year = ui.year, t.month = ui.month, t.day = ui.day, t.hour = ui.hour, t.minute = ui.minute;
    return t;
}

// A whole-number field of the given width in font units.
bool int_field(const char* id, int* v, float width_em) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width_em);
    return ImGui::InputInt(id, v, 0, 0);
}

}  // namespace

std::string clock_text(int64_t posix) {
    nxrt::LocalTime t;
    if (!nxrt::clock_to_local(posix, &t) || t.month < 1 || t.month > 12 || t.weekday < 0 || t.weekday > 6) return "";
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s, %d %s %d   %02d:%02d:%02d", kWeekdays[t.weekday], t.day, kMonths[t.month - 1], t.year, t.hour, t.minute,
                  t.second);
    return buf;
}

bool clock_ui_save(nxrt::ClockConfig& c, const LiveStatus& live, ClockUi& ui) {
    int64_t posix = 0;
    if (!nxrt::clock_from_local(fields_of(ui), &posix)) {
        ui.notice = "That date does not exist.";
        ui.notice_warn = true;
        return false;
    }
    c.custom = true;
    c.start = posix;
    c.saved_at = live.clock_host;
    if (posix < live.clock_guest) {
        ui.notice = "This date is earlier than the game's current date. The game may not like time going backwards.";
        ui.notice_warn = true;
    } else {
        ui.notice = "Saved. The game's clock runs from this date.";
        ui.notice_warn = false;
    }
    return true;
}

bool clock_ui(nxrt::ClockConfig& c, const LiveStatus& live, ClockUi& ui) {
    bool changed = false;
    if (!ui.filled) {   // the fields start at the game's clock, minutes rounded down
        ui.picked_custom = c.custom;
        nxrt::LocalTime t;
        if (nxrt::clock_to_local(live.clock_guest ? live.clock_guest : live.clock_host, &t))
            ui.day = t.day, ui.month = t.month, ui.year = t.year, ui.hour = t.hour, ui.minute = t.minute;
        ui.filled = true;
    }

    if (!live.clock_editable) ImGui::TextColored(kOrange, "Can only be changed on the title screen.");
    ImGui::BeginDisabled(!live.clock_editable);

    if (ImGui::RadioButton("System time", !ui.picked_custom)) {
        ui.picked_custom = false;
        if (c.custom) {
            c.custom = false;
            changed = true;
        }
    }
    tip("The game uses your PC's date and time.");
    ImGui::Indent();
    ImGui::TextUnformatted(clock_text(live.clock_host).c_str());
    ImGui::Unindent();

    if (ImGui::RadioButton("Custom date", ui.picked_custom)) ui.picked_custom = true;
    tip("The game's clock starts at the date you save and keeps running from there.");
    if (ui.picked_custom) {
        ImGui::Indent();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Date");
        ImGui::SameLine();
        int_field("##day", &ui.day, 2.5f);
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        int_field("##month", &ui.month, 2.5f);
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        int_field("##year", &ui.year, 4.0f);
        ImGui::SameLine();
        ImGui::TextUnformatted("   Time");
        ImGui::SameLine();
        int_field("##hour", &ui.hour, 2.5f);
        ImGui::SameLine();
        ImGui::TextUnformatted(":");
        ImGui::SameLine();
        int_field("##minute", &ui.minute, 2.5f);
        ImGui::SameLine();
        int64_t posix = 0;
        nxrt::LocalTime t;
        if (nxrt::clock_from_local(fields_of(ui), &posix) && nxrt::clock_to_local(posix, &t) && t.weekday >= 0 && t.weekday < 7)
            ImGui::Text("   %s", kWeekdays[t.weekday]);
        else
            ImGui::TextColored(kOrange, "   not a date");
        if (ImGui::Button("Save") && clock_ui_save(c, live, ui)) changed = true;
        tip("The game's clock jumps to this date and time and runs from there.");
        if (!ui.notice.empty()) {
            ImGui::SameLine();
            if (ui.notice_warn) ImGui::TextColored(kOrange, "%s", ui.notice.c_str());
            else ImGui::TextUnformatted(ui.notice.c_str());
        }

        if (c.custom && c.start != 0) {
            ImGui::Spacing();
            ImGui::Text("Game clock now: %s", clock_text(live.clock_guest).c_str());
            ImGui::Spacing();
            int m = c.day_minutes;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Day length");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::InputInt("##day_minutes", &m, 1, 60)) {
                m = std::clamp(m, 1, services::clockm::kDayMinutes);
                if (m != c.day_minutes) {
                    c.day_minutes = m;
                    changed = true;
                }
            }
            tip("How many real minutes one game day lasts (1440 = a normal day). Shorter days make the game's clock run faster.");
            ImGui::SameLine();
            ImGui::Text("minutes = %s", services::clockm::hours_text(c.day_minutes).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Reset to default") && c.day_minutes != services::clockm::kDayMinutes) {
                c.day_minutes = services::clockm::kDayMinutes;
                changed = true;
            }
            if (ImGui::Checkbox("Keep time passing while you are away", &c.keep_passing)) changed = true;
            tip("On: when you start the game again, the time you were away is added at the game's clock speed. Off: the clock continues "
                "where you left it.");
        }
        ImGui::Unindent();
    }

    ImGui::EndDisabled();
    return changed;
}

}  // namespace nxrt::play
