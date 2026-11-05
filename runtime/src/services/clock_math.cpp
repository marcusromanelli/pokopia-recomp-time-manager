// SPDX-License-Identifier: GPL-3.0-or-later
// The custom guest clock's pure parts (clock_math.h).
#include "clock_math.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace nxrt::services::clockm {

double speed_of(int day_minutes) { return static_cast<double>(kDayMinutes) / std::clamp(day_minutes, 1, kDayMinutes); }

double guest_at(const Anchor& a, uint64_t mono_ns) {
    if (mono_ns <= a.mono_ns) return a.game;
    return a.game + static_cast<double>(mono_ns - a.mono_ns) * 1e-9 * a.speed;
}

std::string format_state(const State& s) {
    char buf[192];
    std::snprintf(buf, sizeof buf, "[clock]\nguest = %lld\nhost = %lld\nstart = %lld\nsaved_at = %lld\n", static_cast<long long>(s.guest),
                  static_cast<long long>(s.host), static_cast<long long>(s.start), static_cast<long long>(s.saved_at));
    return buf;
}

bool parse_state(const std::string& text, State* out) {
    State s;
    unsigned seen = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;   // the section header, blank lines
        auto trim = [](std::string v) {
            const size_t b = v.find_first_not_of(" \t"), e = v.find_last_not_of(" \t");
            return b == std::string::npos ? std::string() : v.substr(b, e - b + 1);
        };
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (v.empty()) return false;
        char* end = nullptr;
        const long long n = std::strtoll(v.c_str(), &end, 10);
        if (*end) return false;
        if (k == "guest") s.guest = n, seen |= 1;
        else if (k == "host") s.host = n, seen |= 2;
        else if (k == "start") s.start = n, seen |= 4;
        else if (k == "saved_at") s.saved_at = n, seen |= 8;
    }
    if (seen != 15) return false;
    *out = s;
    return true;
}

double resume(const ClockConfig& c, const State* s, bool have_state, int64_t host_now) {
    if (!have_state || !s || s->start != c.start || s->saved_at != c.saved_at) return static_cast<double>(c.start);
    if (!c.keep_passing) return static_cast<double>(s->guest);
    const int64_t away = std::max<int64_t>(0, host_now - s->host);
    return static_cast<double>(s->guest) + static_cast<double>(away) * speed_of(c.day_minutes);
}

bool should_save(double guest, double saved_guest, uint64_t mono_ns, uint64_t saved_mono_ns) {
    const uint64_t real = mono_ns > saved_mono_ns ? mono_ns - saved_mono_ns : 0;
    if (real < 1'000'000'000ull) return false;
    return guest - saved_guest >= 60.0 || real >= 10'000'000'000ull;
}

ClockConfig clock_accept(const ClockConfig& cur, const ClockConfig& want, bool editable) { return editable ? want : cur; }

std::string hours_text(int minutes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", minutes / 60.0);
    std::string s = buf;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s + (s == "1" ? " hour" : " hours");
}

}  // namespace nxrt::services::clockm
