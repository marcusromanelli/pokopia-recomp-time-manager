# Date and Time Tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An F1 "Date and Time" tab that lets the player run the game on the PC clock or on a custom clock that starts at a chosen date and can run up to 1440× faster, persisted across runs, editable only before the player first enters the world.

**Architecture:** The time service (`runtime/src/services/time.cpp`) gets a "guest base clock" that replaces `host_unix_now()` for every system clock: the PC clock in System mode, `anchor + elapsed_mono × speed` in Custom mode. Pure math and the state-file format live in `clock_math.*` (unit tested). A public API `nxrt/clock.h` is called by play mode: start/apply/tick/flush. The settings travel in a new `[clock]` section of `controls.ini` through the existing `Settings` pipeline; the page draws them in a new `play_ui_clock.cpp`.

**Tech Stack:** C++20, CMake/MSVC, Dear ImGui, in-repo `NXTEST` framework (`nxrt_tests`, `nxrt_gfx_tests`).

**Spec:** `docs/superpowers/specs/2026-11-05-date-and-time-design.md`

## Global Constraints

- Day length: integer minutes, **1 … 1440**, default **1440**. Speed = `1440.0 / day_minutes` (always ≥ 1).
- Steady clock, `kernel::system_tick`, `CNTPCT/CNTVCT` are **never** scaled.
- Guest wall time must never go backwards within a session (shared memory and IPC).
- Shared-memory rewrite period in Custom mode with speed > 1: **≤ 250 ms**.
- State-file save cadence: when game time advanced **≥ 60 game s** or **≥ 10 real s** since the last write, at most **once per real second**; also on exit. Atomic (temp + rename).
- Lock: editable only until the player first enters the world this session (Pokopia: follow camera ever updated); fallback `input_calls == 0`. Locked text: **"Can only be changed on the title screen."**
- Checkbox label exactly: **"Keep time passing while you are away"**. Button: **"Reset to default"**. Tab: **"Date and Time"**, between Audio and Game.
- Custom date entry: day / month / year, hour : minute, local time under the device rule; seconds = 0; year **2000 … 2099** (designer call).
- Hours text: up to 2 decimals, trailing zeros trimmed, singular for exactly 1: `1440 → "24 hours"`, `60 → "1 hour"`, `20 → "0.33 hours"`, `90 → "1.5 hours"`.
- Code style: match the surrounding files (one-line `//` comments at the end of declarations, `NXLOG_*` tags, GPL header line).
- Build/test: `pwsh runtime\scripts\build.ps1 -Test -Filter <regex>` (CTest entry = test group name). Direct: `runtime\build\release\nxrt_tests.exe <group>`, `nxrt_gfx_tests.exe <group>`.

## Review Focus

1. **Impossible date typed** (31/02/2026, 29/02/2027, year 1999) → Save refused with a notice; the clock is unchanged. Test in Task 5.
2. **Hand-edited `controls.ini`** with `day_minutes = 0`, `day_minutes = 5000`, `mode = banana`, `start = -5` → load reports the bad line like other sections do and keeps the default for that key; nothing crashes. Test in Task 3.
3. **PC clock moved backwards between runs** with keep-passing on → game time resumes at the last game time, never earlier. Test in Task 1.
4. **A clock change reaching play mode while locked** (stale page copy applied after entering the world) → the clock part is ignored, other settings still apply. Test in Task 1 (`clock_accept`), wired in Task 4.
5. **State file missing, empty or garbage**, or from an older Save → clock starts from the saved custom start, no crash. Tests in Task 1 (parse) and Task 2 (start).

---

### Task 1: Clock math and state-file format (pure)

**Files:**
- Create: `runtime/include/nxrt/clock.h` (only `ClockConfig` + `LocalTime` structs in this task; functions added in Task 2)
- Create: `runtime/src/services/clock_math.h`, `runtime/src/services/clock_math.cpp`
- Test: `runtime/tests/test_clock_math.cpp` (group `clock_math`)

**Interfaces:**
- Produces (`nxrt/clock.h`, namespace `nxrt`):
  ```cpp
  struct ClockConfig {
      bool custom = false;        // [clock] mode = system | custom
      int64_t start = 0;          // [clock] start: posix seconds of the saved custom date (0 = never saved)
      int64_t saved_at = 0;       // [clock] saved_at: host posix seconds when Save was pressed (a new Save of the same date restarts it)
      int day_minutes = 1440;     // [clock] day_minutes, 1..1440
      bool keep_passing = false;  // [clock] keep_passing
      bool operator==(const ClockConfig&) const = default;
  };
  struct LocalTime { int year, month, day, hour, minute, second, weekday; };   // month 1..12, weekday 0 = Sunday
  ```
- Produces (`clock_math.h`, namespace `nxrt::services::clockm`):
  - `double speed_of(int day_minutes)` — clamps to 1..1440, returns `1440.0 / m`.
  - `struct Anchor { double game = 0; uint64_t mono_ns = 0; double speed = 1; };`
  - `double guest_at(const Anchor& a, uint64_t mono_ns)` — `a.game + (mono_ns - a.mono_ns) * 1e-9 * a.speed`; mono before anchor → `a.game`.
  - `struct State { int64_t guest = 0, host = 0, start = 0, saved_at = 0; };`
  - `std::string format_state(const State&)` / `bool parse_state(const std::string&, State*)` — ini text `[clock]\nguest = …\nhost = …\nstart = …\nsaved_at = …\n`; parse false for missing keys or non-numbers.
  - `double resume(const ClockConfig& c, const State* s, bool have_state, int64_t host_now)` — no state, or `s->start != c.start || s->saved_at != c.saved_at` → `c.start`; keep_passing off → `s->guest`; on → `s->guest + max(0, host_now - s->host) * speed_of(c.day_minutes)`.
  - `bool should_save(double guest, double saved_guest, uint64_t mono, uint64_t saved_mono)` — cadence from Global Constraints.
  - `ClockConfig clock_accept(const ClockConfig& cur, const ClockConfig& want, bool editable)` — `editable ? want : cur`.
  - `std::string hours_text(int minutes)` — copy rules from Global Constraints.

- [ ] **Step 1: Write the failing tests** in `runtime/tests/test_clock_math.cpp` (`NXTEST_GROUP(clock_math)`):
  ```cpp
  NXTEST(speed_clamps) { CHECK(speed_of(1440) == 1.0); CHECK(speed_of(20) == 72.0); CHECK(speed_of(0) == 1440.0); CHECK(speed_of(5000) == 1.0); }
  NXTEST(guest_at_scales) { Anchor a{1000.0, 5'000'000'000ull, 72.0}; CHECK(guest_at(a, 6'000'000'000ull) == 1072.0); CHECK(guest_at(a, 4'000'000'000ull) == 1000.0); }
  NXTEST(state_round_trip) { State s{1762353120, 1762300000, 1762000000, 1762100000}; State o; REQUIRE(parse_state(format_state(s), &o));
                             CHECK(o.guest == s.guest && o.host == s.host && o.start == s.start && o.saved_at == s.saved_at); }
  NXTEST(state_garbage_rejected) { State o; CHECK(!parse_state("", &o)); CHECK(!parse_state("[clock]\nguest = x\n", &o)); CHECK(!parse_state("\x01\x02junk", &o)); }
  NXTEST(resume_no_state_uses_start) { ClockConfig c; c.custom = true; c.start = 500; CHECK(resume(c, nullptr, false, 9999) == 500.0); }
  NXTEST(resume_new_save_wins) { ClockConfig c; c.custom = true; c.start = 500; c.saved_at = 2; State s{900, 100, 500, 1}; CHECK(resume(c, &s, true, 200) == 500.0); }
  NXTEST(resume_keep_off) { ClockConfig c; c.custom = true; c.start = 500; State s{900, 100, 500, 0}; CHECK(resume(c, &s, true, 200) == 900.0); }
  NXTEST(resume_keep_on_scaled) { ClockConfig c; c.custom = true; c.start = 500; c.day_minutes = 20; c.keep_passing = true;
                                  State s{900, 100, 500, 0}; CHECK(resume(c, &s, true, 110) == 900.0 + 10 * 72.0); }
  NXTEST(resume_pc_clock_backwards) { ClockConfig c; c.custom = true; c.start = 500; c.keep_passing = true; State s{900, 100, 500, 0}; CHECK(resume(c, &s, true, 50) == 900.0); }
  NXTEST(save_cadence) { CHECK(!should_save(1000, 1000, 500'000'000ull, 0));          // < 1 s
                         CHECK(should_save(1060, 1000, 1'000'000'000ull, 0));          // 60 game s
                         CHECK(!should_save(1010, 1000, 5'000'000'000ull, 0));         // neither
                         CHECK(should_save(1010, 1000, 10'000'000'000ull, 0)); }       // 10 real s
  NXTEST(accept_respects_lock) { ClockConfig a, b; b.custom = true; CHECK(clock_accept(a, b, false) == a); CHECK(clock_accept(a, b, true) == b); }
  NXTEST(hours_text_copy) { CHECK(hours_text(1440) == "24 hours"); CHECK(hours_text(60) == "1 hour"); CHECK(hours_text(20) == "0.33 hours"); CHECK(hours_text(90) == "1.5 hours"); }
  ```
- [ ] **Step 2: Run** `pwsh runtime\scripts\build.ps1 -Test -Filter clock_math` — expected: build fails (`clock_math.h` not found).
- [ ] **Step 3: Implement** `clock.h` structs and `clock_math.h/.cpp` per the Interfaces block. `src/*.cpp` is globbed (`CONFIGURE_DEPENDS`), no CMake edit.
- [ ] **Step 4: Run** the same command — expected: `clock_math` passes.
- [ ] **Step 5: Commit** — `feat(clock): clock math and state file format`.

---

### Task 2: Guest base clock in the time service

**Files:**
- Modify: `runtime/include/nxrt/clock.h` (add the functions below)
- Modify: `runtime/src/services/time.cpp` (`host_unix_now` users at `now_of()` :164, `GetCurrentTime`/`SetCurrentTime` lambdas, `init_locked` :263, `time_reset` :650)
- Test: `runtime/tests/test_svc_time.cpp`

**Interfaces:**
- Consumes: Task 1 `clockm::*`, `ClockConfig`, `LocalTime`.
- Produces (`nxrt/clock.h`, all thread safe, guarded by time.cpp's `mu()`):
  ```cpp
  void clock_start(const ClockConfig& c, const std::string& state_path);   // once at play start: reads the state file, anchors, rewrites shm
  void clock_apply(const ClockConfig& c);     // config changed: re-anchors (see below), rewrites shm, writes the state file
  void clock_tick();                          // frame boundary: shm rewrite every ≤250 ms when speed > 1; state save per should_save
  void clock_flush();                         // on exit: writes the state file in Custom mode
  int64_t clock_guest_now();                  // what GetCurrentTime of the user clock returns now (base + local_delta)
  int64_t clock_host_now();                   // the PC clock, posix seconds
  bool clock_to_local(int64_t posix, LocalTime* out);         // device rule ([time] zone), weekday filled
  bool clock_from_local(const LocalTime& t, int64_t* posix);  // false: invalid fields, year outside 2000..2099, or a local time that does not exist (DST gap); ambiguous → the earlier
  ```
- Behaviour:
  - `State` gains `ClockConfig clock; clockm::Anchor anchor; std::string state_path; double saved_guest; uint64_t saved_mono, last_shm_mono;`.
  - New `double base_now(State&)`: System → `host_unix_now()`; Custom → `clockm::guest_at(anchor, raw_ticks_ns())`. `now_of()` uses `floor(base_now)` for `n.host` — so local, network and ephemeral clocks all follow (spec §1).
  - `clock_apply` re-anchor rules: System → System: nothing. Into System: anchor unused. `start`/`saved_at` changed (a Save): `anchor.game = c.start`. Only `day_minutes`/`keep_passing` changed: `anchor.game = base_now()` (continuity). Always `anchor.mono_ns = raw_ticks_ns()`, `anchor.speed = speed_of(...)`.
  - `clock_start`: reads the file (missing/garbage → `have_state = false`), `anchor.game = clockm::resume(...)`.
  - State writes: `<path>.tmp` then `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`. `host` field = `host_unix_now()`.
  - `clock_to_local/from_local` use `device_rule()` and must not create the shared memory (call `ensure_hosts`, not `state_locked`).
  - `time_reset()` puts the clock back to System with an empty state path.

- [ ] **Step 1: Write the failing tests** (append to `test_svc_time.cpp`, use a temp file under `%TEMP%` and `nxrt::services::reset_all()` between):
  - `svc_time_clock_system_matches_host`: after `clock_apply(ClockConfig{})`, user clock cmd 0 within 2 s of `host_unix()`.
  - `svc_time_clock_custom_start`: `start = 1893456000` (2030-01-01Z), `saved_at = 1`, `custom = true` → cmd 0 returns `1893456000 ± 1`.
  - `svc_time_clock_custom_speed`: same with `day_minutes = 1` (1440×), sleep 100 ms → advanced ≥ 140 s and ≤ 160 s.
  - `svc_time_clock_network_follows`: Custom 2030 → network clock (time:s) within 1 s of user clock.
  - `svc_time_clock_shm_matches_ipc`: Custom 1440×, call `clock_tick()`, then local context `offset + steady_secs` within 400 s (≤250 ms × 1440 + slack) of cmd 0, and never below the previous read over 10 ticks 50 ms apart.
  - `svc_time_clock_day_change_continuous`: Custom 2030 at 1440×, sleep 50 ms, `clock_apply` with `day_minutes = 1440` → cmd 0 did not jump back to 1893456000.
  - `svc_time_clock_state_persist_keep_off`: Custom, `clock_tick`/`clock_flush`, `reset_all()`, `clock_start` with same config + path, `keep_passing = false` → resumes within 2 s of the flushed guest.
  - `svc_time_clock_state_garbage`: write `"\x01junk"` to the path, `clock_start` Custom `start = 1893456000` → cmd 0 = `1893456000 ± 1`.
  - `svc_time_clock_local_round_trip_needs_tz`: `[time] zone = utc` → `clock_from_local({2050,6,15,12,30,0,0})` then `clock_to_local` gives the same fields, weekday 3 (Wednesday); `{2026,2,31,…}` and `{1999,1,1,…}` → false.
- [ ] **Step 2: Run** `pwsh runtime\scripts\build.ps1 -Test -Filter svc_time` — expected: compile errors for `clock_*`.
- [ ] **Step 3: Implement** per Interfaces/Behaviour. Existing `svc_time_*` tests must keep passing (System mode = old behaviour).
- [ ] **Step 4: Run** `-Filter "svc_time|clock_math"` — expected: all pass.
- [ ] **Step 5: Commit** — `feat(time): custom guest clock with day length`.

---

### Task 3: `[clock]` section in controls.ini

**Files:**
- Modify: `runtime/src/gfx/play/play_types.h:274` (`Settings` gets `nxrt::ClockConfig clock;  // [clock] (nxrt/clock.h): the Date and Time tab`)
- Modify: `runtime/src/gfx/play/play_config.cpp` (`enum class Sec` :153 add `Clock`; `sec_label`; `fixed_sec` :619 `"clock"`; `apply_fixed`; writer after `[tweaks]` :897)
- Test: `runtime/tests/gfx/test_play_config.cpp`

**Interfaces:**
- Consumes: `nxrt::ClockConfig` (Task 1).
- Keys: `mode = system | custom`, `start = <int ≥ 0>`, `saved_at = <int ≥ 0>`, `day_minutes = <int 1..1440>`, `keep_passing = 0|1`. Written always, in that order.

- [ ] **Step 1: Write the failing tests:**
  - `clock_section_round_trip`: Settings with `custom = true, start = 1893456000, saved_at = 7, day_minutes = 20, keep_passing = true` → format → load → `== `.
  - `clock_section_defaults_when_absent`: a file without `[clock]` loads `ClockConfig{}`.
  - `clock_section_bad_values`: `day_minutes = 0`, `day_minutes = 5000`, `mode = banana`, `start = -5` each → load reports an error naming the key (same mechanism other sections use) and that key keeps its default.
- [ ] **Step 2: Run** `pwsh runtime\scripts\build.ps1 -Test -Filter play_config` — expected: fail.
- [ ] **Step 3: Implement** with the existing `set_bool`/range helpers in `apply_fixed`.
- [ ] **Step 4: Run** same — expected: pass.
- [ ] **Step 5: Commit** — `feat(play): [clock] settings section`.

---

### Task 4: Play mode wiring and the lock

**Files:**
- Modify: `runtime/src/gfx/play/play_ui.h` (`LiveStatus` gets `bool clock_editable = true; int64_t clock_guest = 0, clock_host = 0;`)
- Modify: `runtime/src/gfx/play/play_game.h` (`GamePlay` gets `bool (*clock_editable)() = nullptr;  // F1 Date and Time: the clock may change now (the title, before the world); nullptr: until the first input tick`)
- Modify: `runtime/src/gfx/play/play.cpp` — settings load ~:1173 (`clock_start(st.settings.clock, <dir of st.path>/clock_state.ini)`), pending-settings block ~:747 (guard + `clock_apply` when changed), frame boundary after `frame_tick` (`clock_tick()`), `exit_process_impl` :405 (`clock_flush()`), `status()` :1015 (fill the three fields)
- Modify: `runtime/games/pkp/pkp_play.h/.cpp` (`bool play_clock_editable()` → `g_follow_ns.load() == 0`), `runtime/games/pkp/pkp_game.cpp:52` (`p.clock_editable = &play_clock_editable;`)

**Interfaces:**
- Consumes: Task 1 `clockm::clock_accept`, Task 2 `clock_start/apply/tick/flush/guest_now/host_now`, Task 3 `Settings::clock`.
- Produces: `LiveStatus::clock_editable`, `clock_guest`, `clock_host` for Task 5.
- Guard in the pending block: `s.clock = clockm::clock_accept(st.settings.clock, s.clock, editable_now())` before `st.settings = s`; log at Info when a clock change is dropped. `editable_now()` = hook if set, else `pi_calls == 0`.

- [ ] **Step 1:** Build (`pwsh runtime\scripts\build.ps1 -Test -Filter "play_ui|play_config|svc_time|clock_math"`) — expected: all pass (no behaviour change visible to existing tests; the guard's logic is covered by `accept_respects_lock`).
- [ ] **Step 2: Commit** — `feat(play): wire the custom clock and the title-screen lock`.

---

### Task 5: The "Date and Time" tab

**Files:**
- Create: `runtime/src/gfx/play/play_ui_clock.h`, `runtime/src/gfx/play/play_ui_clock.cpp`
- Modify: `runtime/src/gfx/play/play_ui.cpp` (`Page` gets `ClockUi clock_ui;`; tab between Audio :1538 and Game :1542)
- Test: `runtime/tests/gfx/test_play_ui.cpp`

**Interfaces:**
- Consumes: `LiveStatus` fields (Task 4), `nxrt::clock_to_local/from_local` (Task 2), `clockm::hours_text` (Task 1).
- Produces:
  ```cpp
  struct ClockUi {                    // page-local editing state, not saved
      bool picked_custom = false;     // the Custom radio is selected (the clock changes only on Save)
      int day = 1, month = 1, year = 2026, hour = 0, minute = 0;
      bool filled = false;            // fields prefilled from the live game clock once
      std::string notice; bool notice_warn = false;
  };
  // true when c changed (the page then marks itself dirty and applies).
  bool clock_ui(nxrt::ClockConfig& c, const LiveStatus& live, ClockUi& ui);
  ```
- Layout and rules (spec §2 mock):
  - Locked: everything inside `BeginDisabled(!live.clock_editable)`; line "Can only be changed on the title screen." above. Live clock lines keep updating.
  - System radio: `c.custom = false` immediately. Line: live `clock_host` as `"<Weekday>, <d> <Month> <yyyy>   <hh:mm:ss>"`.
  - Custom radio: shows fields (day/month/year, hour:minute, live computed weekday) and **Save**. Save → `clock_from_local`; false → notice "That date does not exist." (warn), `c` unchanged; true → `c.custom = true, c.start = posix, c.saved_at = live.clock_host`. If `posix < live.clock_guest` → notice "This date is earlier than the game's current date. The game may not like time going backwards." (warn) — still saved.
  - Once `c.custom && c.start != 0`: "Game clock now: …" (from `clock_guest`), Day length `InputInt` clamped 1..1440 + `hours_text` + **Reset to default** (→ 1440), and the keep-passing checkbox.

- [ ] **Step 1: Write the failing tests** (existing `FakeHost`, open the tab with `TabBarQueueFocus` as other tests do):
  - `clock_tab_draws_locked_and_unlocked`: draw 5 frames with `status_.clock_editable` false then true; no crash; `applies == 0`.
  - `clock_ui_save_invalid_date_refused`: call `clock_ui` inside a frame with `ui.picked_custom = true, day = 31, month = 2, year = 2026` and a simulated Save (expose `bool clock_ui_save(nxrt::ClockConfig&, const LiveStatus&, ClockUi&)` from `play_ui_clock.h` and test it directly) → returns false, `c == ClockConfig{}`, `ui.notice_warn`.
  - `clock_ui_save_valid_sets_config`: `[time] zone = utc` via `config_set`, 15/06/2050 12:30 → returns true, `c.custom`, `c.start == 2538909000`, `c.saved_at == live.clock_host`.
  - `clock_ui_save_earlier_warns`: `live.clock_guest = 2538909000 + 100` → saved, `ui.notice_warn`.
  - `clock_ui_year_out_of_range`: year 1999 → refused.
- [ ] **Step 2: Run** `pwsh runtime\scripts\build.ps1 -Test -Filter play_ui` — expected: fail.
- [ ] **Step 3: Implement** `clock_ui`, `clock_ui_save` and the tab.
- [ ] **Step 4: Run** `-Filter "play_ui|play_config|svc_time|clock_math"` — expected: all pass.
- [ ] **Step 5: Commit** — `feat(play): Date and Time tab in the F1 page`.

---

### Task 6: In-game verification and docs

**Files:**
- Modify: `README.md` (Features → "In-game settings": one bullet in player terms), `CHANGELOG.md` (Unreleased entry)

- [ ] **Step 1:** Full build of the game per the repo's normal flow; launch; on the title open F1 → Date and Time. Check: System shows the PC clock ticking; Custom 15/06/2030 10:00 Save → "Game clock now" ticks from it; Day length 20 → "0.33 hours", clock moves ~72 s per s; Reset to default → 1440 / "24 hours".
- [ ] **Step 2:** Load the save; confirm the game reflects the custom weekday (whatever weekday-gated content applies); confirm the tab is disabled with the locked line. Check the log for which read path the game uses (IPC cmd 0 / shm / snapshot).
- [ ] **Step 3:** Quit; relaunch with keep-passing off → resumes at the last game time; with it on and 20-minute days, wait 1 real minute before relaunch → ~72 game minutes later.
- [ ] **Step 4:** Update README/CHANGELOG; commit — `docs: Date and Time tab`.
