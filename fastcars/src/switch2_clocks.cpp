// fastcars - Switch 2: CPU / GPU / memory clocks (clkrst).
//
// Logs the current and possible rates of the CPU, GPU and EMC at startup. --switch2_clocks
// raises them: "max" = the highest rate each module lists, "keep" (default) = leave them alone,
// or "cpu,gpu,emc" in MHz (0 = keep that one). The firmware's DVFS table bounds what it accepts.

#if defined(__SWITCH__)

#include <switch.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

// Default: the CPU at its boost clock (1702 MHz, the rate retail games use while loading); GPU and
// memory untouched, because raising either makes the firmware halve the CPU.
REXCVAR_DEFINE_STRING(switch2_clocks, "1702,0,0", "FASTCARS",
                      "Switch 2 clocks: keep, max, or cpu,gpu,emc in MHz (0 = keep that one)");
REXCVAR_DEFINE_INT32(switch2_clocks_log_s, 5, "FASTCARS",
                     "Switch 2: log CPU/GPU/EMC clocks and SoC temperature every n s (0 = off); "
                     "a set clock that drifted is set again");

namespace fastcars::switch2 {

namespace {

struct Module {
  const char* name;
  PcvModuleId id;
};

constexpr Module kModules[] = {
    {"CPU", PcvModuleId_CpuBus},
    {"GPU", PcvModuleId_GPU},
    {"EMC", PcvModuleId_EMC},
};

std::string Rates(const u32* rates, s32 count, PcvClockRatesListType type) {
  std::string s = type == PcvClockRatesListType_Range ? "range" : "list";
  for (s32 i = 0; i < count; ++i) s += " " + std::to_string(rates[i] / 1000000);
  return s;
}

// Effective CPU speed: 16 dependent adds per loop iteration (1 per cycle, loop overhead runs
// beside them), timed by the thread's own run time (svcGetInfo ThreadTickCount), so time slices
// spent off the core do not count. Shows throttling the reported clock rate does not.
double MeasureCpuMhz() {
  constexpr uint64_t kIters = 300000;  // 4.8 M adds: ~4 ms at 1.1 GHz
  uint64_t run0 = 0, run1 = 0;
  if (R_FAILED(svcGetInfo(&run0, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total)))
    return 0;
  uint64_t x = 0;
  for (uint64_t i = 0; i < kIters; ++i) {
    __asm__ volatile(
        "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
        "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
        "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
        "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
        : "+r"(x));
  }
  if (R_FAILED(svcGetInfo(&run1, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total)))
    return 0;
  const double secs = double(run1 - run0) / double(armGetSystemTickFreq());
  return secs > 0 ? kIters * 16 / secs / 1e6 : 0;
}

ClkrstSession g_sessions[3];
bool g_open[3] = {};
u32 g_target[3] = {};
u32 g_original[3] = {};
u32 g_achieved[3] = {};  // what the firmware snapped the target to (1702 -> 1689)

}  // namespace

// The rates stay set after the process exits (the next boot saw them): put them back.
void RestoreClocks() {
  for (int i = 0; i < 3; ++i) {
    if (!g_open[i] || !g_target[i] || !g_original[i]) continue;
    g_target[i] = 0;  // the monitor stops re-setting it
    clkrstSetClockRate(&g_sessions[i], g_original[i]);
    REXLOG_INFO("Switch 2 clocks: {} restored to {} MHz", kModules[i].name, g_original[i] / 1000000);
  }
}

// --switch2_clocks changed while running (the settings menu's CPU boost): each module goes to its
// new target, or back to the rate it had at start when the new mode leaves it alone.
void ReapplyClocks() {
  const std::string mode = REXCVAR_GET(switch2_clocks);
  u32 want_mhz[3] = {0, 0, 0};
  const bool want_max = mode == "max";
  if (!want_max && mode != "keep") std::sscanf(mode.c_str(), "%u,%u,%u", &want_mhz[0], &want_mhz[1], &want_mhz[2]);
  for (int i = 0; i < 3; ++i) {
    if (!g_open[i]) continue;
    u32 target = want_mhz[i] * 1000000u;
    if (want_max) {
      u32 rates[64] = {};
      s32 count = 0;
      PcvClockRatesListType type = PcvClockRatesListType_Invalid;
      if (R_SUCCEEDED(clkrstGetPossibleClockRates(&g_sessions[i], rates, 64, &type, &count)))
        for (s32 k = 0; k < count; ++k) target = std::max(target, rates[k]);
    }
    if (target == g_target[i]) continue;
    g_target[i] = 0;  // the monitor leaves it alone while it changes
    g_achieved[i] = 0;
    const u32 rate = target ? target : g_original[i];
    if (!rate) continue;
    const Result rs = clkrstSetClockRate(&g_sessions[i], rate);
    u32 now = 0;
    clkrstGetClockRate(&g_sessions[i], &now);
    if (target) g_achieved[i] = now;
    g_target[i] = target;
    REXLOG_INFO("Switch 2 clocks: {} {} {} MHz: rc 0x{:X}, now {} MHz", kModules[i].name,
                target ? "set" : "back to", rate / 1000000, rs, now / 1000000);
  }
}

void ApplyClocks() {
  const std::string mode = REXCVAR_GET(switch2_clocks);
  u32 want_mhz[3] = {0, 0, 0};
  bool want_max = mode == "max";
  if (!want_max && mode != "keep") {
    std::sscanf(mode.c_str(), "%u,%u,%u", &want_mhz[0], &want_mhz[1], &want_mhz[2]);
  }

  Result rc = clkrstInitialize();
  if (R_FAILED(rc)) {
    REXLOG_WARN("Switch 2 clocks: clkrstInitialize failed 0x{:X}", rc);
    return;
  }
  // Sessions stay open for the monitor (a closed session might also give its rate up).
  ClkrstSession* sessions = g_sessions;
  bool* open = g_open;
  u32* target = g_target;
  for (int i = 0; i < 3; ++i) {
    const Module& m = kModules[i];
    ClkrstSession& session = sessions[i];
    rc = clkrstOpenSession(&session, m.id, 3);
    if (R_FAILED(rc)) {
      REXLOG_WARN("Switch 2 clocks: {} open failed 0x{:X}", m.name, rc);
      continue;
    }
    open[i] = true;
    u32 hz = 0;
    clkrstGetClockRate(&session, &hz);
    u32 rates[64] = {};
    s32 count = 0;
    PcvClockRatesListType type = PcvClockRatesListType_Invalid;
    Result rr = clkrstGetPossibleClockRates(&session, rates, 64, &type, &count);
    u32 max_hz = 0;
    if (R_SUCCEEDED(rr)) {
      for (s32 k = 0; k < count; ++k) max_hz = std::max(max_hz, rates[k]);
    }
    REXLOG_INFO("Switch 2 clocks: {} at {} MHz; possible: {}", m.name, hz / 1000000,
                R_SUCCEEDED(rr) ? Rates(rates, count, type) : "query failed");

    target[i] = want_max ? max_hz : want_mhz[i] * 1000000u;
    g_original[i] = hz;
    if (target[i] && target[i] != hz) {
      Result rs = clkrstSetClockRate(&session, target[i]);
      u32 now = 0;
      clkrstGetClockRate(&session, &now);
      g_achieved[i] = now;
      REXLOG_INFO("Switch 2 clocks: {} set {} MHz: rc 0x{:X}, now {} MHz", m.name,
                  target[i] / 1000000, rs, now / 1000000);
    }
  }

  const int period_s = REXCVAR_GET(switch2_clocks_log_s);
  if (period_s <= 0) return;
  static TsSession ts_soc, ts_pcb;
  const bool have_ts = R_SUCCEEDED(tsInitialize());
  const bool have_soc = have_ts && R_SUCCEEDED(tsOpenSession(&ts_soc, TsDeviceCode_LocationExternal));
  const bool have_pcb = have_ts && R_SUCCEEDED(tsOpenSession(&ts_pcb, TsDeviceCode_LocationInternal));
  REXLOG_INFO("Switch 2 clocks: ts {}, SoC sensor {}, PCB sensor {}", have_ts, have_soc, have_pcb);
  std::thread([period_s, have_ts, have_soc, have_pcb] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(period_s));
      u32 hz[3] = {};
      std::string reset;
      for (int i = 0; i < 3; ++i) {
        if (!g_open[i]) continue;
        clkrstGetClockRate(&g_sessions[i], &hz[i]);
        // Within 2 MHz counts as set (rates snap to the DVFS table).
        const u32 want = g_achieved[i] ? g_achieved[i] : g_target[i];
        if (g_target[i] && (hz[i] + 2000000u < want || hz[i] > want + 2000000u)) {
          clkrstSetClockRate(&g_sessions[i], g_target[i]);
          reset += std::string(" ") + kModules[i].name + " was " + std::to_string(hz[i] / 1000000);
        }
      }
      float soc = 0, pcb = 0;
      if (have_soc) tsSessionGetTemperature(&ts_soc, &soc);
      if (have_pcb) tsSessionGetTemperature(&ts_pcb, &pcb);
      if (!have_soc && have_ts) {
        s32 mc = 0;
        if (R_SUCCEEDED(tsGetTemperatureMilliC(TsLocation_External, &mc))) soc = mc / 1000.0f;
      }
      static int tick = 0;
      const double eff = (tick++ % 6 == 0) ? MeasureCpuMhz() : 0;  // every 30 s
      u64 mem_total = 0, mem_used = 0;
      svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      // The heap is committed whole at start; malloc's own counts show what is in use.
      const struct mallinfo mi = mallinfo();
      REXLOG_INFO("Switch 2 memory: process {} of {} MB; malloc in use {} MB of {} MB taken from the heap",
                  mem_used >> 20, mem_total >> 20, size_t(mi.uordblks) >> 20, size_t(mi.arena) >> 20);
      REXLOG_INFO("Switch 2 clocks: CPU {} (measured {}) GPU {} EMC {} MHz, SoC {:.1f} C, "
                  "PCB {:.1f} C{}{}",
                  hz[0] / 1000000, eff > 0 ? std::to_string(int(eff)) : std::string("-"),
                  hz[1] / 1000000, hz[2] / 1000000, soc, pcb,
                  reset.empty() ? "" : "; set again:", reset);
    }
  }).detach();
}

}  // namespace fastcars::switch2

#endif  // __SWITCH__
