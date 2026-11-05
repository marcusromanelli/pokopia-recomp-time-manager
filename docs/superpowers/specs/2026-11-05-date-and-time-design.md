# Date and Time tab (F1 overlay) — design

Date: 2026-11-05
Status: draft, awaiting approval

User decisions: faster days only (1–1440 min); lock = until the player first enters the world, then locked until restart.

## Intent

Pokopia gates some activities on specific weekdays. Players don't want to wait 3–4 real days to reach them. This feature lets the player
make the game believe days have passed: either jump to a chosen date, or let game days run faster than real days.

Success: from the title screen, the player picks a date (or a short day length), loads their save, and the game behaves as if that
date/weekday is the current one, with the clock moving forward from there.

## Goals

- New top-level tab **"Date and Time"** in the F1 settings page.
- Two modes:
  - **System time** — the game sees the PC clock (today's behaviour). The tab shows the live date, month, year, weekday and time.
  - **Custom date** — the player enters a date and time; on **Save** the game clock jumps there and runs forward from it. The tab shows
    the live game date, month, year, weekday and time.
- With Custom date saved, an extra option **Day length**: number of real minutes one game day lasts, 1 … 1440 (faster days only;
  decided by the user — slower days would force backward steps on the shared-memory path).
  - Input in minutes; next to it, the equivalent in hours (1440 → "24 hours").
  - **Reset to default** button → 1440 minutes (24 hours).
- **"Keep time passing while you are away"** checkbox (Custom mode). On next launch:
  - on: game time = last game time + (real time elapsed since last run × speed), speed = 1440 / day_length_minutes;
  - off: game time resumes exactly at the last game time.
- The tab can only be **changed while the game is on the start/title menu**. Otherwise it is read-only (still shows the live clock).
- All settings and the clock state persist across runs.

## Non-goals

- Changing the game's internal timers, animation, physics or audio speed. Only the calendar/wall clock is affected.
- Time zones beyond what exists today (`[time] zone = host | utc`).
- Cheats or memory editing of game save data.
- Launcher (Tauri) UI for this. F1 overlay only.

## Anti-goals (failure even if it "works")

- Speeding up the steady clock / tick counter (`CNTPCT`, `kernel::system_tick`, steady clock) — that would make the whole game run fast
  or break timers.
- Letting the player change the clock mid-gameplay (save corruption / time-travel penalties risk).
- Losing the custom clock position on a crash.

## Current state (recon)

- Guest wall time comes from `host_unix_now()` in `runtime/src/services/time.cpp:147` (Windows `GetSystemTimePreciseAsFileTime` via
  `tt::wall_filetime()`), plus a per-clock offset (`local_delta`, `network_delta`, `ephemeral_delta`, all 0 at boot).
- Two guest read paths, both must agree:
  1. IPC: `ISystemClock::GetCurrentTime` → `host_unix_now() + delta`.
  2. Time shared memory: guest computes `context.offset + steady_seconds`; `write_shm()` publishes contexts with
     `offset = host + delta − steady`. The steady clock runs at real speed.
- `GetClockSnapshot` (cmd 400) builds from the same contexts.
- F1 page: `runtime/src/gfx/play/play_ui.cpp` `draw_settings_page()` — top-level tab bar `settings_pages` (Display, Graphics, Controls,
  Audio, Game, About). Settings flow: working copy `Page::edit` → `UiHost::apply(Settings)` → main thread applies + writes `controls.ini`
  (`play_config.cpp`). Pattern for a self-contained group: `play_fastboot.h` / `play_ui_fastboot.cpp`.
- Title vs world signal for Pokopia: `games/pkp/pkp_play.cpp` `play_follow_live()` (follow camera updated within the last second → the
  player is in the world). Generic fallback: `LiveStatus::input_calls` (0 = game has not reached gameplay).

## Design

### 1. Clock model (runtime, `services/time.cpp` + new `nxrt/clock.h`)

Introduce a "guest wall clock" that replaces direct use of `host_unix_now()` for the user/local clock:

```
System mode:  guest_now = host_now                                  (unchanged behaviour)
Custom mode:  guest_now = anchor_game + (mono_now − anchor_mono) × speed
              speed     = 1440 / day_minutes
```

- `anchor_game` (posix seconds, can be fractional internally) and `anchor_mono` (monotonic ns, from the tick counter) are re-taken on every
  change (Save, day-length change, mode switch, load at start-up) so the clock is continuous.
- Within a session use the monotonic tick counter, not the PC wall clock, so a PC clock change mid-session doesn't jump the game.
- The `local_delta` mechanism stays: `GetCurrentTime` returns `guest_now + delta`; the game's own `SetCurrentTime` keeps working on top.
- **Network / ephemeral clocks**: follow the same guest_now in Custom mode (designer call) so a game that cross-checks
  user vs network time doesn't see a mismatch.
- **Shared memory**: because the guest's fast path computes `offset + steady_seconds` with a real-speed steady clock, a scaled clock
  drifts between rewrites. In Custom mode with speed ≠ 1, rewrite the local (and network) contexts periodically (target ≤ 250 ms; tick
  from the frame boundary or a light timer) via the existing `lifo_write`. Between rewrites the guest sees real-speed advance, then a
  small forward step — always monotonic forward. With speed = 1, one write per change is enough (current behaviour).
- New public API (`runtime/include/nxrt/clock.h`), thread safe, used by the play layer:
  - `struct ClockConfig { bool custom; int64_t custom_start_posix; int day_minutes; bool keep_passing; }`
  - `void clock_apply(const ClockConfig&)` — re-anchors and rewrites shared memory.
  - `int64_t clock_guest_now()` — for the UI display: posix seconds including local_delta, i.e. exactly what the game sees.
  - `void clock_save_state()` / start-up load (see §3).

### 2. F1 tab (`play_ui.cpp` + new `play_ui_clock.cpp`)

New top-level tab "Date and Time" between Audio and Game.

```
( ) System time
      Wednesday, 5 November 2026   14:32:07
( ) Custom date
      Date  [05] / [11] / [2026]   Time [14]:[32]      Weekday: Wednesday (computed)
      [ Save ]
      Game clock now: Friday, 7 November 2026   03:10:44

      Day length  [ 1440 ] minutes   = 24 hours        [ Reset to default ]
      [x] Keep time passing while you are away
```

- Date fields: day / month / year order (as requested); weekday computed live from the entered fields, invalid dates (31/02) rejected
  with a notice.
- Entered date/time is **local time** under the device time zone rule (same rule the game uses for calendar conversion; `tz::` helpers).
- Day length and the away checkbox are shown only once a custom date has been saved (per request). Day length range: 1 … 1440 minutes
  (user decision: faster days only). Hours display: up to 2 decimals, trailing zeros trimmed ("24 hours", "0.33 hours", "1 hour").
- Changes take effect on **Save** (custom date) or immediately for day length / checkbox / mode — all only while unlocked.
- **Lock**: when the game is not on the title, all controls are `BeginDisabled`, with a line: "Can only be changed on the title screen."
  The live clocks keep updating.
- Live display refreshes every frame from `clock_guest_now()` (System mode: host time under the device rule).

### 3. Persistence

- **Settings** (mode, custom start, day length, keep-passing): new `[clock]` section of `controls.ini`, through the existing Settings
  pipeline, following the `play_fastboot.h` pattern (struct + apply/format/ui functions).
- **Clock position** (last guest time + host wall time when written): separate small state file next to `controls.ini`
  (e.g. `clock_state.ini`), written atomically (temp file + rename) in Custom mode whenever the game clock has advanced ≥ 1 game
  minute since the last write or 10 real seconds have passed, whichever comes first, but at most once per real second; plus on clean
  exit. At 1-minute days (1440×) a crash loses at most ~24 game minutes; at 24-hour days at most 10 s. Kept out of `controls.ini` to
  avoid rewriting the user's settings file constantly.
- **Start-up** in Custom mode:
  - keep_passing on: `anchor_game = last_guest + (host_wall_now − last_host_wall) × speed` (clamped to ≥ last_guest if the PC clock went
    backwards).
  - keep_passing off: `anchor_game = last_guest`.
  - no state file yet: `anchor_game = custom_start_posix`.

### 4. Title-screen detection

- New optional `GamePlay` hook `bool (*clock_editable)()`. Pokopia implements it as "the player has not entered the world this session"
  (the follow camera has never been live since start-up), using the `g_follow_ns` signal behind `play_follow_live()`.
- Expose through `LiveStatus` (e.g. `bool clock_editable`) so the page stays testable without a game.
- Fallback for games without the hook: editable while `input_calls == 0`.
- Rationale: this is stricter than "on the title right now" (returning to the title after playing keeps it locked until restart),
  but guarantees the game hasn't read and cached the clock for a loaded save. **Approved by the user.**

## Risks

- **Time travel**: saving a custom date earlier than one the game has already recorded may trigger the game's own anti-clock-change
  logic. Unknown how Pokopia reacts. The tab shows a warning when the new custom date is earlier than the last guest time.
- **Which read path Pokopia uses** (IPC vs shared memory vs snapshot) is unknown; the design covers all three. Verify with a log line
  on first use of each path.
- Shared-memory step artifacts at high speeds (e.g. 1-minute days = 1440×): with 250 ms rewrites the guest sees ~6-minute forward steps.
  Acceptable for weekday purposes.

## Testing

- Unit tests in `runtime/tests/test_svc_time.cpp`: System mode unchanged; Custom start; speed scaling; re-anchor continuity; away
  calculation both ways; PC clock moved backwards; shared memory context matches IPC within one rewrite period.
- UI tests alongside `runtime/tests/gfx/test_play_ui.cpp`: lock state disables controls; minutes → hours text; Reset to default; invalid
  date rejected; `[clock]` round-trips through `controls.ini`.
- Manual: from title, set a custom weekday, load save, confirm the game reflects the weekday; 20-minute day passes in 20 min; quit and
  relaunch with the checkbox on/off.

## Calls made by the designer (check these)

- Network/ephemeral clocks follow the custom clock.
- Hours shown to 2 decimals, trailing zeros trimmed.
- Within-session timing uses the monotonic counter; PC wall clock used only for the away calculation.
- Clock position stored in a separate state file; write cadence ≥ 1 game minute or 10 s, max 1/s, atomic.
- Custom date/time entered in local time; seconds start at 00.
- Warning (not a block) when the custom date is earlier than the last game time.
