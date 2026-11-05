// SPDX-License-Identifier: GPL-3.0-or-later
// time:u, time:a, time:s, time:r (S3c): the standard clocks, the steady clock, the time zone service and the time shared memory.
// Adapted from ryubing (Ryujinx, MIT) src/Ryujinx.HLE/HOS/Services/Time: TimeSharedMemory.cs (shared memory offsets, paddings and the
// lifo index rule), StaticService/IStaticServiceForGlue.cs (IStaticService command ids and the time:u/a/r split), StaticService/
// ISystemClock.cs, ISteadyClock.cs, ITimeZoneServiceForGlue.cs and ITimeZoneServiceForPsc.cs (command ids and output layouts),
// Types/TimePermissions.cs (the write masks per flavour). ScarletNX src/nx/hle/services/time.swipc (GPL-3.0-or-later) was used to
// cross-check the command ids and struct sizes. The Switchbrew "Services API" page was used for the list of time services.
//
// Model (one process-wide state, guarded by one mutex):
//   * posix time of a clock = the base clock + that clock's offset. The user and the local clock share the local offset (on the console
//     the user clock is the local clock); the network and the ephemeral network clock have their own.
//   * base clock (nxrt/clock.h, the F1 Date and Time tab): host UTC (GetSystemTimePreciseAsFileTime) in System mode; in Custom mode an
//     anchored guest time that runs 1440 / day_minutes times as fast as the tick counter, published in steps: every 250 ms (clock_tick)
//     the published time jumps to the anchored clock, and in between it runs at real speed like the steady clock the guest adds to the
//     shared memory contexts. IPC and the shared memory so read the same clock, and it never goes back (the speed is at least 1).
//   * steady clock: the raw tick counter (kernel::system_tick, 19.2 MHz, 1 tick = 625/12 ns; it counts from the host's boot) + the
//     internal offset + the test offset (both in ns, TimeSpanType; no fixed epoch, see kSteadyEpochSeconds). Seconds are the floor of that sum; the sequence is
//     monotonic. The guest's SDK computes the same sum itself from CNTPCT and the shared memory's internal offset, so no start tick is
//     taken off (that made the game's clock run ahead by the host's uptime).
//   * every change of an offset rewrites the shared memory, so the guest's fast GetCurrentTime path sees the same values.
//
// Time shared memory (0x1000 bytes, offsets as TimeSharedMemory.cs writes them). A block is a u32 index at `off`, `pad` bytes, then two
// slots; the live slot is (index & 1). Writers bump the index after writing the free slot.
//   0x000 SteadyClockContext x2 (pad 4; slots 0x008, 0x020; 0x18 each): u64 internal_offset (= epoch + internal + test, ns), uuid source
//   0x038 Local SystemClockContext x2 (pad 4; slots 0x040, 0x060; 0x20 each): i64 offset, {i64 time_point, uuid}
//   0x080 Network SystemClockContext x2 (pad 4; slots 0x088, 0x0A8)
//   0x0C8 automatic correction flag x2 (pad 0; slots 0x0CC, 0x0CD; 1 byte each)
//   0x0D0 ContinuousAdjustmentTimePoint x2 (pad 4; slots 0x0D8, 0x110; 0x38 each): u64 clock_offset (= start tick in ns),
//         i64 multiplier (1), i64 divisor_log2 (0), SystemClockContext {offset 0, {time_point 0, uuid}}
// The user clock has no slot of its own (the layout of the console has none): it is the local context.
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "clock_math.h"
#include "nxrt/clock.h"
#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/tt.h"
#include "services_internal.h"
#include "timezone.h"

namespace nxrt::services {
namespace {

// ---- constants ----------------------------------------------------------------------------------------------------------------
constexpr uint64_t kSteadyEpochSeconds = 0;   // no epoch: Pokopia reads the wall clock as the context offset + the raw counter's seconds, so any
                                              // fixed epoch here made its clock that much behind (it was 3600 s: one hour)
constexpr uint64_t kSteadyEpochNs = kSteadyEpochSeconds * 1000000000ull;
constexpr uint8_t kSourceId[16] = {'n', 'x', 'r', 't', '-', 's', 't', 'e', 'a', 'd', 'y', '-', 'c', 'l', 'k', '1'};   // clock_source_id
constexpr uint32_t kShmSize = 0x1000;
constexpr int64_t kRtcEpochUnix = 946684800;                            // 2000-01-01T00:00:00Z in posix seconds (RTC counts from here)

constexpr uint32_t kOffSteady = 0x000, kPadSteady = 4;
constexpr uint32_t kOffLocal = 0x038, kPadLocal = 4;
constexpr uint32_t kOffNetwork = 0x080, kPadNetwork = 4;
constexpr uint32_t kOffAutoCorr = 0x0C8, kPadAutoCorr = 0;
constexpr uint32_t kOffAdjust = 0x0D0, kPadAdjust = 4;

// Write masks (Ryujinx TimePermissions): what a flavour may change.
enum : uint32_t {
    kWriteLocal = 0x1,
    kWriteUser = 0x2,
    kWriteNetwork = 0x4,
    kWriteTimeZone = 0x8,
    kWriteSteady = 0x10,
};

enum class Clock { User, Local, Network, Ephemeral };

// Horizon time-module results (module 116, description in bits 9 and up): Ryujinx ResultCode.
uint32_t err_perm() { return res(116, 1); }                 // PermissionDenied
uint32_t err_out_of_range() { return res(116, 902); }       // OutOfRange: a buffer of the wrong size
uint32_t err_tz_conversion() { return res(116, 903); }      // TimeZoneConversionFailed: missing or invalid rule
uint32_t err_tz_not_found() { return res(116, 989); }       // TimeZoneNotFound: a location name we cannot load

// ---- structs written into the shared memory (sizes asserted) ----------------------------------------------------------------
struct SteadyCtx {
    uint64_t internal_offset;
    uint8_t source[16];
};
struct SysCtx {
    int64_t offset;
    int64_t time_point;
    uint8_t source[16];
};
struct Adjust {
    uint64_t clock_offset;
    int64_t multiplier;
    int64_t divisor_log2;
    SysCtx ctx;
};
static_assert(sizeof(SteadyCtx) == 0x18);
static_assert(sizeof(SysCtx) == 0x20);
static_assert(sizeof(Adjust) == 0x38);

// ---- state --------------------------------------------------------------------------------------------------------------------
struct State {
    bool created = false;
    uint64_t tick0_ns = 0;                                // raw tick at start-up, in ns (the ContinuousAdjustment clock_offset)
    int64_t internal_ns = 0, test_ns = 0;                 // steady clock offsets (TimeSpanType, ns)
    int64_t local_delta = 0, network_delta = 0, ephemeral_delta = 0;   // seconds added to host UTC per clock
    bool auto_correction = false;
    bool device_utc = false;                              // [time] zone = UTC
    bool hosts_ready = false;
    std::string host_name;
    std::vector<uint8_t> host_rule, utc_rule;
    int64_t location_updated = 0;                         // steady seconds when the device location was last set
    std::unique_ptr<ServiceSharedMemory> shm;
    ServiceEvent clock_event, tz_event;                   // never signaled
    // The base clock (nxrt/clock.h). Kept by init_locked (play mode sets it, possibly before the first service use); time_reset clears it.
    ClockConfig clock;
    clockm::Anchor anchor;                                // Custom mode: the guest time at a tick
    std::string state_path;                               // the state file ("" = none)
    double saved_guest = 0;                               // the guest time and tick of the last state file write
    uint64_t saved_mono = 0, last_shm_mono = 0;           // ... and of the last shared memory refresh
    double pub_game = 0;                                  // Custom mode: the published guest time at pub_mono (runs at real speed from there)
    uint64_t pub_mono = 0;
};

std::mutex& mu() {
    static std::mutex m;
    return m;
}
State& raw_state() {
    static State s;
    return s;
}

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string str_of(const char* p, size_t n) {
    size_t l = 0;
    while (l < n && p[l]) ++l;
    return std::string(p, l);
}

// ---- clocks -------------------------------------------------------------------------------------------------------------------
// Console ticks to ns (19.2 MHz: 625 / 12 ns per tick; 31.25 MHz: 32 ns per tick), split so the multiplication cannot overflow.
uint64_t raw_ticks_ns() {
    const uint64_t t = kernel::system_tick(), hz = kernel::tick_hz();
    return (t / hz) * 1000000000ull + (t % hz) * 1000000000ull / hz;
}

int64_t host_unix_now() {
#if defined(_WIN32)
    const uint64_t t = tt::wall_filetime();   // 100 ns ticks since 1601-01-01 (the host clock; a fixed epoch + virtual time in deterministic test mode)
    return static_cast<int64_t>((t - 116444736000000000ull) / 10000000ull);
#else
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#endif
}

// The base clock in posix seconds: the PC clock, or the published custom clock (a step at each refresh, real speed in between).
double base_now(const State& s) {
    if (!s.clock.custom) return static_cast<double>(host_unix_now());
    const uint64_t mono = raw_ticks_ns();
    return mono <= s.pub_mono ? s.pub_game : s.pub_game + static_cast<double>(mono - s.pub_mono) * 1e-9;
}

int64_t steady_total_ns(const State& s) {
    return static_cast<int64_t>(raw_ticks_ns()) + static_cast<int64_t>(kSteadyEpochNs) + s.internal_ns + s.test_ns;
}
int64_t steady_secs(const State& s) { return steady_total_ns(s) / 1000000000; }

struct Now {
    int64_t host, steady;
    bool custom = false;                                  // Custom mode: the contexts' offsets come from the exact values below
    double host_x = 0, steady_x = 0;                      // the base clock and the steady clock in seconds, unrounded
};
Now now_of(const State& s) {
    Now n{};
    n.host_x = base_now(s);
    n.steady_x = static_cast<double>(steady_total_ns(s)) * 1e-9;
    n.host = static_cast<int64_t>(std::floor(n.host_x));
    n.steady = steady_secs(s);
    n.custom = s.clock.custom;
    return n;
}

int64_t& delta_of(State& s, Clock c) {
    switch (c) {
        case Clock::Network: return s.network_delta;
        case Clock::Ephemeral: return s.ephemeral_delta;
        default: return s.local_delta;   // user and local share the local offset
    }
}
uint32_t write_bit_of(Clock c) {
    switch (c) {
        case Clock::User: return kWriteUser;
        case Clock::Local: return kWriteLocal;
        default: return kWriteNetwork;   // network and ephemeral network
    }
}

SysCtx sys_ctx(const Now& n, int64_t delta) {
    SysCtx c{};
    // Custom mode: base - steady from the exact values, so a refresh never lowers offset + floor(steady) (rounding both apart could).
    c.offset = n.custom ? static_cast<int64_t>(std::floor(n.host_x - n.steady_x)) + delta : n.host + delta - n.steady;
    c.time_point = n.steady;
    std::memcpy(c.source, kSourceId, 16);
    return c;
}

// ---- time zone rules ----------------------------------------------------------------------------------------------------------
void ensure_hosts(State& s) {
    if (s.hosts_ready) return;
    s.host_rule.assign(tz::kRuleSize, 0);
    s.utc_rule.assign(tz::kRuleSize, 0);
    tz::make_utc_rule(s.utc_rule.data());
    std::string name;
    tz::make_host_rule(s.host_rule.data(), &name);
    s.host_name = name.empty() ? std::string("UTC") : name;
    s.hosts_ready = true;
}
const uint8_t* device_rule(State& s) {
    ensure_hosts(s);
    return s.device_utc ? s.utc_rule.data() : s.host_rule.data();
}
std::string device_name(State& s) {
    ensure_hosts(s);
    return s.device_utc ? std::string("UTC") : s.host_name;
}
// The location names this runtime can load: "UTC" and the host zone. No tz database is shipped, so these are the only two.
std::vector<std::string> location_names(State& s) {
    ensure_hosts(s);
    std::vector<std::string> v{"UTC"};
    if (s.host_name != "UTC") v.push_back(s.host_name);
    return v;
}
const std::vector<uint8_t>* rule_for_name(State& s, const std::string& name) {
    ensure_hosts(s);
    if (name == "UTC") return &s.utc_rule;
    if (name == s.host_name) return &s.host_rule;
    return nullptr;
}

// ---- shared memory ------------------------------------------------------------------------------------------------------------
uint8_t* shm_base(State& s) { return (s.shm && s.shm->ok()) ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(s.shm->host())) : nullptr; }

// Ryujinx TimeSharedMemory.WriteObjectToSharedMemory: write the free slot, then publish the new index.
void lifo_write(uint8_t* base, uint32_t off, uint32_t pad, const void* obj, uint32_t size) {
    uint32_t idx = 0;
    std::memcpy(&idx, base + off, 4);
    const uint32_t n = idx + 1;
    std::memcpy(base + off + 4 + pad + (n & 1) * size, obj, size);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::memcpy(base + off, &n, 4);
}

void write_shm(State& s) {
    uint8_t* b = shm_base(s);
    if (!b) return;
    const Now n = now_of(s);

    SteadyCtx sc{};
    sc.internal_offset = static_cast<uint64_t>(static_cast<int64_t>(kSteadyEpochNs) + s.internal_ns + s.test_ns);
    std::memcpy(sc.source, kSourceId, 16);
    lifo_write(b, kOffSteady, kPadSteady, &sc, sizeof sc);

    const SysCtx lc = sys_ctx(n, s.local_delta);
    lifo_write(b, kOffLocal, kPadLocal, &lc, sizeof lc);
    const SysCtx nc = sys_ctx(n, s.network_delta);
    lifo_write(b, kOffNetwork, kPadNetwork, &nc, sizeof nc);

    const uint8_t ac = s.auto_correction ? 1 : 0;
    lifo_write(b, kOffAutoCorr, kPadAutoCorr, &ac, 1);

    Adjust adj{};
    adj.clock_offset = s.tick0_ns;
    adj.multiplier = 1;
    adj.divisor_log2 = 0;
    std::memcpy(adj.ctx.source, kSourceId, 16);
    lifo_write(b, kOffAdjust, kPadAdjust, &adj, sizeof adj);
}

// Start-up state: every offset zero, the start tick taken now, the shared memory zeroed and written once.
void init_locked(State& s) {
    s.tick0_ns = raw_ticks_ns();
    s.internal_ns = 0;
    s.test_ns = 0;
    s.local_delta = 0;
    s.network_delta = 0;
    s.ephemeral_delta = 0;
    s.auto_correction = false;
    s.device_utc = ascii_lower(config_get_str("time", "zone", "host")) == "utc";
    s.hosts_ready = false;
    s.host_name.clear();
    s.host_rule.clear();
    s.utc_rule.clear();
    if (!s.shm) s.shm = std::make_unique<ServiceSharedMemory>(kShmSize, "time");
    s.location_updated = steady_secs(s);
    if (uint8_t* b = shm_base(s)) {
        std::memset(b, 0, kShmSize);
        write_shm(s);
    }
}

// Lazily brings the state up (the shared memory needs the kernel region, so nothing is allocated before the first service use).
State& state_locked() {
    State& s = raw_state();
    if (!s.created) {
        s.created = true;
        init_locked(s);
    }
    return s;
}

// nn::time::sf::ClockSnapshot (0xD0 bytes): Ryubing Time/Clock/Types/ClockSnapshot.cs.
struct ClockSnapshot {
    SysCtx user_ctx, network_ctx;                 // 0x00, 0x20
    int64_t user_time, network_time;              // 0x40, 0x48
    tz::CalendarTime user_cal, network_cal;       // 0x50, 0x58
    tz::CalendarAdditionalInfo user_info, network_info;   // 0x60, 0x78
    int64_t steady_time_point;                    // 0x90: SteadyClockTimePoint {time_point, clock source}
    uint8_t steady_source[16];
    char location[0x24];                          // 0xA8
    uint8_t auto_correction;                      // 0xCC
    uint8_t type;                                 // 0xCD
    uint16_t unknown;
};
static_assert(sizeof(ClockSnapshot) == 0xD0);

// Ryubing IStaticServiceForPsc.GetClockSnapshotFromSystemClockContextInternal: the steady time point now, both contexts, the times
// they give at that point (a context from another clock source gives none: the user one fails, the network one is 0) and the
// calendar times of both under the device rule. Called with mu() held.
uint32_t make_snapshot(State& st, const SysCtx& user, const SysCtx& network, uint8_t type, ClockSnapshot* out) {
    ClockSnapshot cs{};
    const int64_t steady = steady_secs(st);
    cs.auto_correction = st.auto_correction ? 1 : 0;
    cs.user_ctx = user;
    cs.network_ctx = network;
    cs.steady_time_point = steady;
    std::memcpy(cs.steady_source, kSourceId, 16);
    const std::string name = device_name(st);
    std::memcpy(cs.location, name.data(), name.size() < sizeof cs.location - 1 ? name.size() : sizeof cs.location - 1);
    if (std::memcmp(user.source, kSourceId, 16) != 0) return res(116, 102);   // ClockSnapshot.GetCurrentTime: TimeMismatch
    cs.user_time = user.offset + steady;
    if (!tz::to_calendar(device_rule(st), cs.user_time, &cs.user_cal, &cs.user_info)) return err_tz_conversion();
    cs.network_time = std::memcmp(network.source, kSourceId, 16) == 0 ? network.offset + steady : 0;
    if (!tz::to_calendar(device_rule(st), cs.network_time, &cs.network_cal, &cs.network_info)) return err_tz_conversion();
    cs.type = type;
    *out = cs;
    return 0;
}

void push_uuid(ipc::Response& rsp) { rsp.out.insert(rsp.out.end(), kSourceId, kSourceId + 16); }

// ---- service objects ----------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_clock(Clock kind, uint32_t perm) {
    return make_iface("ISystemClock", [kind, perm](ipc::Service& s) {
        s.add_cmd(0, "GetCurrentTime", [kind](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const Now n = now_of(st);
            rsp.push<int64_t>(n.host + delta_of(st, kind));
            return 0;
        });
        s.add_cmd(1, "SetCurrentTime", [kind, perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & write_bit_of(kind))) return err_perm();
            const int64_t posix = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const Now n = now_of(st);
            delta_of(st, kind) = posix - n.host;
            write_shm(st);
            return 0;
        });
        s.add_cmd(2, "GetSystemClockContext", [kind](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const SysCtx c = sys_ctx(now_of(st), delta_of(st, kind));
            rsp.push(c);
            return 0;
        });
        s.add_cmd(4, "GetOperationEventReadableHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            rsp.out_copy.push_back(st.clock_event.new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_steady(uint32_t perm) {
    return make_iface("ISteadyClock", [perm](ipc::Service& s) {
        s.add_cmd(0, "GetCurrentTimePoint", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            rsp.push<int64_t>(steady_secs(st));
            push_uuid(rsp);
            return 0;
        });
        s.add_cmd(2, "GetTestOffset", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<int64_t>(state_locked().test_ns);
            return 0;
        });
        s.add_cmd(3, "SetTestOffset", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteSteady)) return err_perm();
            const int64_t v = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            st.test_ns = v;
            write_shm(st);
            return 0;
        });
        s.add_cmd(100, "GetRtcValue", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // Seconds since 2000-01-01 of the host clock (the console keeps the RTC the same way; no RTC hardware here).
            const int64_t host = host_unix_now();
            rsp.push<uint64_t>(static_cast<uint64_t>(host - kRtcEpochUnix));
            return 0;
        });
        s.add_cmd(101, "IsRtcResetDetected", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        s.add_cmd(102, "GetSetupResultValue", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(200, "GetInternalOffset", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<int64_t>(state_locked().internal_ns);
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_tz(uint32_t perm) {
    return make_iface("ITimeZoneService", [perm](ipc::Service& s) {
        s.add_cmd(0, "GetDeviceLocationName", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            char name[0x24] = {};
            const std::string n = device_name(st);
            std::memcpy(name, n.data(), n.size() < sizeof name - 1 ? n.size() : sizeof name - 1);
            rsp.out.insert(rsp.out.end(), name, name + sizeof name);
            return 0;
        });
        s.add_cmd(1, "SetDeviceLocationName", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteTimeZone)) return err_perm();
            const auto raw = req.arg<std::array<char, 0x24>>(0);
            const std::string name = str_of(raw.data(), raw.size());
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            ensure_hosts(st);
            if (name == "UTC") {
                st.device_utc = true;
            } else if (name == st.host_name) {
                st.device_utc = false;
            } else {
                return err_tz_not_found();
            }
            st.location_updated = steady_secs(st);
            return 0;
        });
        s.add_cmd(2, "GetTotalLocationNameCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<int32_t>(static_cast<int32_t>(location_names(state_locked()).size()));
            return 0;
        });
        s.add_cmd(3, "LoadLocationNameList", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t index = req.arg<uint32_t>(0);
            const ipc::Buffer out = req.out_buf(0);
            std::lock_guard<std::mutex> lk(mu());
            const std::vector<std::string> names = location_names(state_locked());
            const size_t cap = out.size / 0x24 < 6 ? static_cast<size_t>(out.size / 0x24) : 6;
            std::vector<char> data;
            uint32_t count = 0;
            for (size_t i = index; i < names.size() && count < cap; ++i, ++count) {
                char entry[0x24] = {};
                std::memcpy(entry, names[i].data(), names[i].size() < sizeof entry - 1 ? names[i].size() : sizeof entry - 1);
                data.insert(data.end(), entry, entry + sizeof entry);
            }
            if (!data.empty()) buf_write(out, data.data(), data.size());
            rsp.push<uint32_t>(count);
            return 0;
        });
        s.add_cmd(4, "LoadTimeZoneRule", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const auto raw = req.arg<std::array<char, 0x24>>(0);
            const ipc::Buffer out = req.out_buf(0);
            if (out.size != tz::kRuleSize) return err_out_of_range();
            const std::string name = str_of(raw.data(), raw.size());
            std::lock_guard<std::mutex> lk(mu());
            const std::vector<uint8_t>* rule = rule_for_name(state_locked(), name);
            if (!rule) return err_tz_not_found();
            buf_write(out, rule->data(), tz::kRuleSize);
            return 0;
        });
        s.add_cmd(5, "GetTimeZoneRuleVersion", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // No tz database is shipped, so there is no rule version to report: 16 zero bytes.
            rsp.out.insert(rsp.out.end(), 0x10, uint8_t{0});
            return 0;
        });
        s.add_cmd(6, "GetDeviceLocationNameAndUpdatedTime", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            char name[0x24] = {};
            const std::string n = device_name(st);
            std::memcpy(name, n.data(), n.size() < sizeof name - 1 ? n.size() : sizeof name - 1);
            rsp.out.insert(rsp.out.end(), name, name + sizeof name);
            rsp.out.insert(rsp.out.end(), 4, uint8_t{0});          // padding before the 8-aligned time point
            rsp.push<int64_t>(st.location_updated);
            push_uuid(rsp);
            return 0;
        });
        s.add_cmd(20, "GetDeviceLocationNameOperationEventReadableHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.out_copy.push_back(state_locked().tz_event.new_handle());
            return 0;
        });
        s.add_cmd(100, "ToCalendarTime", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int64_t posix = req.arg<int64_t>(0);
            const ipc::Buffer rule = req.in_buf(0);
            if (!rule.addr || rule.size < tz::kRuleSize) return err_tz_conversion();
            tz::CalendarTime cal{};
            tz::CalendarAdditionalInfo info{};
            if (!tz::to_calendar(buf_ptr(rule), posix, &cal, &info)) return err_tz_conversion();
            rsp.push(cal);
            rsp.push(info);
            return 0;
        });
        s.add_cmd(101, "ToCalendarTimeWithMyRule", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int64_t posix = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            tz::CalendarTime cal{};
            tz::CalendarAdditionalInfo info{};
            if (!tz::to_calendar(device_rule(state_locked()), posix, &cal, &info)) return err_tz_conversion();
            rsp.push(cal);
            rsp.push(info);
            return 0;
        });
        s.add_cmd(201, "ToPosixTime", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const auto cal = req.arg<tz::CalendarTime>(0);
            const ipc::Buffer rule = req.in_buf(0);
            if (!rule.addr || rule.size < tz::kRuleSize) return err_tz_conversion();
            int64_t times[2] = {0, 0};
            int n = tz::to_posix(buf_ptr(rule), cal, times);
            if (n < 0 || n > 2) n = 0;
            if (n > 0) buf_write(req.out_buf(0), times, static_cast<uint64_t>(n) * 8);
            rsp.push<uint32_t>(static_cast<uint32_t>(n));
            return 0;
        });
        s.add_cmd(202, "ToPosixTimeWithMyRule", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const auto cal = req.arg<tz::CalendarTime>(0);
            std::lock_guard<std::mutex> lk(mu());
            int64_t times[2] = {0, 0};
            int n = tz::to_posix(device_rule(state_locked()), cal, times);
            if (n < 0 || n > 2) n = 0;
            if (n > 0) buf_write(req.out_buf(0), times, static_cast<uint64_t>(n) * 8);
            rsp.push<uint32_t>(static_cast<uint32_t>(n));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_static(const char* name, uint32_t perm) {
    return make_iface(name, [perm](ipc::Service& s) {
        s.add_cmd(0, "GetStandardUserSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::User, perm));
            return 0;
        });
        s.add_cmd(1, "GetStandardNetworkSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::Network, perm));
            return 0;
        });
        s.add_cmd(2, "GetStandardSteadyClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_steady(perm));
            return 0;
        });
        s.add_cmd(3, "GetTimeZoneService", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_tz(perm));
            return 0;
        });
        s.add_cmd(4, "GetStandardLocalSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::Local, perm));
            return 0;
        });
        s.add_cmd(5, "GetEphemeralNetworkSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::Ephemeral, perm));
            return 0;
        });
        s.add_cmd(20, "GetSharedMemoryNativeHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            rsp.out_copy.push_back(st.shm->new_handle());
            return 0;
        });
        s.add_cmd(50, "SetStandardSteadyClockInternalOffset", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteSteady)) return err_perm();
            const int64_t v = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            st.internal_ns = v;
            write_shm(st);
            return 0;
        });
        // GetClockSnapshot(u8 type) -> buffer<ClockSnapshot, 0x1a>: from the user and network clock contexts now.
        s.add_cmd(400, "GetClockSnapshot", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const Now n = now_of(st);
            ClockSnapshot cs{};
            const uint32_t r = make_snapshot(st, sys_ctx(n, st.local_delta), sys_ctx(n, st.network_delta), type, &cs);
            if (r) return r;
            buf_write(req.out_buf(0), &cs, sizeof cs);
            return 0;
        });
        // GetClockSnapshotFromSystemClockContext(u8 type, SystemClockContext user, SystemClockContext network) -> buffer 0x1a.
        // The u8 comes first and the contexts are 8-byte aligned after it (Ryubing reads type, skips 7 bytes, then the contexts).
        s.add_cmd(401, "GetClockSnapshotFromSystemClockContext", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            const SysCtx user = req.arg<SysCtx>(8), network = req.arg<SysCtx>(8 + sizeof(SysCtx));
            std::lock_guard<std::mutex> lk(mu());
            ClockSnapshot cs{};
            const uint32_t r = make_snapshot(state_locked(), user, network, type, &cs);
            if (r) return r;
            buf_write(req.out_buf(0), &cs, sizeof cs);
            return 0;
        });
        s.add_cmd(100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(state_locked().auto_correction ? 1 : 0);
            return 0;
        });
        s.add_cmd(101, "SetStandardUserSystemClockAutomaticCorrectionEnabled", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteUser)) return err_perm();
            const bool on = req.arg<uint8_t>(0) != 0;
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            st.auto_correction = on;
            write_shm(st);
            return 0;
        });
    });
}

}  // namespace

// time:u gets no write rights; time:a the local and user clock and the time zone; time:s the network clock; time:r the steady clock.
void register_time() {
    struct Flavour {
        const char* name;
        uint32_t perm;
    };
    static constexpr Flavour kFlavours[] = {
        {"time:u", 0},
        {"time:a", kWriteLocal | kWriteUser | kWriteTimeZone},
        {"time:s", kWriteNetwork},
        {"time:r", kWriteSteady},
    };
    for (const Flavour& f : kFlavours) {
        const char* name = f.name;
        const uint32_t perm = f.perm;
        add(name, [name, perm] { return make_static(name, perm); });
    }
}

void time_reset() {
    std::lock_guard<std::mutex> lk(mu());
    State& s = raw_state();
    s.clock = ClockConfig{};
    s.anchor = clockm::Anchor{};
    s.state_path.clear();
    s.saved_guest = 0;
    s.saved_mono = s.last_shm_mono = 0;
    s.pub_game = 0;
    s.pub_mono = 0;
    if (s.created) init_locked(s);
}

namespace {

constexpr uint64_t kShmRefreshNs = 250'000'000ull;   // a fast clock's shared memory contexts are at most this old

// Writes the state file (Custom mode with a path): temp file, then a rename over the old one, so a crash never leaves half a file.
void write_state_locked(State& s) {
    if (!s.clock.custom || s.state_path.empty()) return;
    const uint64_t mono = raw_ticks_ns();
    const double guest = base_now(s);
    clockm::State st;
    st.guest = static_cast<int64_t>(std::floor(guest));
    st.host = host_unix_now();
    st.start = s.clock.start;
    st.saved_at = s.clock.saved_at;
    namespace fs = std::filesystem;
    const std::string tmp = s.state_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << clockm::format_state(st);
        out.flush();
        if (!out) return;
    }
    std::error_code ec;
    fs::rename(tmp, fs::path(s.state_path), ec);   // replaces an existing file on Windows
    if (ec) {
        fs::remove(tmp, ec);
        return;
    }
    s.saved_guest = guest;
    s.saved_mono = mono;
}

bool read_state(const std::string& path, clockm::State* out) {
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return clockm::parse_state(ss.str(), out);
}

void anchor_locked(State& s, double game) {
    const uint64_t mono = raw_ticks_ns();
    s.anchor = clockm::Anchor{game, mono, clockm::speed_of(s.clock.day_minutes)};
    s.pub_game = game;
    s.pub_mono = mono;
    s.saved_guest = game;
    s.saved_mono = mono;
    s.last_shm_mono = mono;
    if (s.created) write_shm(s);
}

// The device rule without bringing the service up: before the first use, [time] zone decides as init_locked will.
const uint8_t* local_rule(State& s) {
    ensure_hosts(s);
    const bool utc = s.created ? s.device_utc : ascii_lower(config_get_str("time", "zone", "host")) == "utc";
    return utc ? s.utc_rule.data() : s.host_rule.data();
}

int days_in(int year, int month) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : kDays[month - 1];
}

// ---- nxrt/clock.h -----------------------------------------------------------------------------------------------------------------
// mode = custom without a saved date (a hand-edited controls.ini) runs on the PC clock.
ClockConfig effective(ClockConfig c) {
    if (c.custom && c.start == 0) c.custom = false;
    return c;
}

void clock_start_impl(const ClockConfig& cfg, const std::string& state_path) {
    std::lock_guard<std::mutex> lk(mu());
    State& s = raw_state();
    const ClockConfig c = effective(cfg);
    s.clock = c;
    s.state_path = state_path;
    clockm::State st;
    const bool have = read_state(state_path, &st);
    anchor_locked(s, c.custom ? clockm::resume(c, &st, have, host_unix_now()) : 0.0);
    write_state_locked(s);
}

void clock_apply_impl(const ClockConfig& cfg) {
    std::lock_guard<std::mutex> lk(mu());
    State& s = raw_state();
    const ClockConfig c = effective(cfg);
    const ClockConfig old = s.clock;
    if (old == c) return;
    double game = 0;
    if (c.custom) {
        const bool saved = !old.custom || old.start != c.start || old.saved_at != c.saved_at;
        game = saved ? static_cast<double>(c.start) : base_now(s);   // a Save restarts from its date; the other changes keep the time
    }
    s.clock = c;
    anchor_locked(s, game);
    write_state_locked(s);
}

void clock_tick_impl() {
    std::lock_guard<std::mutex> lk(mu());
    State& s = raw_state();
    if (!s.clock.custom) return;
    const uint64_t mono = raw_ticks_ns();
    if (s.anchor.speed > 1.0 && mono - s.last_shm_mono >= kShmRefreshNs) {   // publish the next step of a fast clock
        s.pub_game = clockm::guest_at(s.anchor, mono);
        s.pub_mono = mono;
        if (s.created) write_shm(s);
        s.last_shm_mono = mono;
    }
    if (clockm::should_save(base_now(s), s.saved_guest, mono, s.saved_mono)) write_state_locked(s);
}

void clock_flush_impl() {
    std::lock_guard<std::mutex> lk(mu());
    write_state_locked(raw_state());
}

int64_t clock_guest_now_impl() {
    std::lock_guard<std::mutex> lk(mu());
    const State& s = raw_state();
    return static_cast<int64_t>(std::floor(base_now(s))) + (s.created ? s.local_delta : 0);
}

int64_t clock_host_now_impl() { return host_unix_now(); }

bool clock_to_local_impl(int64_t posix, LocalTime* out) {
    std::lock_guard<std::mutex> lk(mu());
    tz::CalendarTime cal{};
    tz::CalendarAdditionalInfo info{};
    if (!tz::to_calendar(local_rule(raw_state()), posix, &cal, &info)) return false;
    out->year = cal.year, out->month = cal.month, out->day = cal.day;
    out->hour = cal.hour, out->minute = cal.minute, out->second = cal.second;
    out->weekday = static_cast<int>(info.day_of_week);
    return true;
}

bool clock_from_local_impl(const LocalTime& t, int64_t* posix) {
    if (t.year < 2000 || t.year > 2099 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > days_in(t.year, t.month)) return false;
    if (t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 || t.second < 0 || t.second > 59) return false;
    tz::CalendarTime cal{};
    cal.year = static_cast<int16_t>(t.year);
    cal.month = static_cast<int8_t>(t.month), cal.day = static_cast<int8_t>(t.day);
    cal.hour = static_cast<int8_t>(t.hour), cal.minute = static_cast<int8_t>(t.minute), cal.second = static_cast<int8_t>(t.second);
    std::lock_guard<std::mutex> lk(mu());
    int64_t found[2] = {};
    const int n = tz::to_posix(local_rule(raw_state()), cal, found);
    if (n <= 0) return false;
    *posix = n == 2 ? std::min(found[0], found[1]) : found[0];
    return true;
}

}  // namespace

}  // namespace nxrt::services

namespace nxrt {

void clock_start(const ClockConfig& c, const std::string& state_path) { services::clock_start_impl(c, state_path); }
void clock_apply(const ClockConfig& c) { services::clock_apply_impl(c); }
void clock_tick() { services::clock_tick_impl(); }
void clock_flush() { services::clock_flush_impl(); }
int64_t clock_guest_now() { return services::clock_guest_now_impl(); }
int64_t clock_host_now() { return services::clock_host_now_impl(); }
bool clock_to_local(int64_t posix, LocalTime* out) { return services::clock_to_local_impl(posix, out); }
bool clock_from_local(const LocalTime& t, int64_t* posix) { return services::clock_from_local_impl(t, posix); }

}  // namespace nxrt
