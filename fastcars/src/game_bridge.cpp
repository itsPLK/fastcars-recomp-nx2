// fastcars - runtime interface for the game DLL (FASTCARS_GAME_DLL build, see fs/fastcars_pch.h).
//
// The recompiled game code is built on the player's machine without the SDK headers, so the
// handful of runtime services the SDK's generated prefix header calls inline are exported here as
// plain C functions. Each one does what the matching macro in the SDK's fastcars_pch.h does.

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include "guest_ptr.h"
#include <rex/image_info.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/mmio_handler.h>
#include <rex/thread/mutex.h>

#include <atomic>
#include <cstdlib>
#include <string_view>

#include "../fs/layout_check.h"

// Exported from the executable's dynamic symbol table (--dynamic-list, see gen_interface.py).
#define FASTCARSFS_EXPORT __attribute__((visibility("default")))

namespace {
std::atomic<int32_t> g_global_lock_count{0};
}

extern "C" {

FASTCARSFS_EXPORT uint32_t fastcarsfs_mmio_load(uint32_t addr) {
  uint32_t value = 0;
  rex::runtime::MMIOHandler::global_handler()->CheckLoad(addr, &value);
  return value;
}

FASTCARSFS_EXPORT void fastcarsfs_mmio_store(uint32_t addr, uint32_t value) {
  rex::runtime::MMIOHandler::global_handler()->CheckStore(addr, value);
}

FASTCARSFS_EXPORT uint64_t fastcarsfs_query_timebase() {
  return rex::chrono::Clock::QueryGuestTickCount();
}

FASTCARSFS_EXPORT PPCFunc* fastcarsfs_resolve_indirect(uint32_t guest_address) {
  return rex::runtime::ResolveIndirectFunction(guest_address);
}

// The global critical region (the 360's "interrupts off" idiom). The generated code brackets each
// lwarx/stwcx. update with mfmsr (check) -> mtmsrd r13 -> enter ... mtmsrd rN -> leave, and enters
// and leaves unconditionally; the check's value only lands in a register / MSR.
//
// --fastcars_global_lock_fast (default): the check answers from this thread's own depth instead of
// taking the mutex (it used to lock + unlock just to read a counter: half the lock traffic), and
// enter spins on try_lock before blocking (the sections are one atomic update; a contended lock
// went straight to the kernel). Switch 2 profile, city driving: the main thread spent ~30% of its
// time in svcArbitrateLock, mostly here.
REXCVAR_DEFINE_BOOL(fastcars_global_lock_fast, true, "FASTCARS",
                    "Global critical region: lock-free mfmsr check (per-thread depth) and spin before blocking");
// --fastcars_global_lock_off: the region is not entered at all. Each bracketed lwarx/stwcx. pair is
// already atomic on its own (stwcx. is a compare-and-swap on the reserved value), and on the 360
// "interrupts off" only kept the current core from being preempted - the other five hardware
// threads ran on - so guest code cannot depend on it for exclusion between threads. Without it the
// game's atomics stop serialising against each other and against the HLE kernel, which takes the
// same mutex for its object table.
#if defined(__SWITCH__)
#define FASTCARS_GLOBAL_LOCK_OFF_DEFAULT true  // measured there: main thread lock waits 23% -> 14%
#else
#define FASTCARS_GLOBAL_LOCK_OFF_DEFAULT false
#endif
REXCVAR_DEFINE_BOOL(fastcars_global_lock_off, FASTCARS_GLOBAL_LOCK_OFF_DEFAULT, "FASTCARS",
                    "Global critical region: the game's mtmsrd brackets take no lock (atomics stay atomic)");
REXCVAR_DEFINE_INT32(fastcars_global_lock_spin, 256, "FASTCARS",
                     "Global critical region: try_lock attempts before blocking (with --fastcars_global_lock_fast)");

namespace {
thread_local int32_t t_global_lock_depth = 0;

inline void CpuRelax() {
  __asm__ volatile("yield");
}
}  // namespace

FASTCARSFS_EXPORT void fastcarsfs_global_lock_enter() {
  if (REXCVAR_GET(fastcars_global_lock_off)) {
    ++t_global_lock_depth;
    return;
  }
  auto& m = rex::thread::global_critical_region::mutex();
  if (REXCVAR_GET(fastcars_global_lock_fast)) {
    bool locked = false;
    for (int32_t i = REXCVAR_GET(fastcars_global_lock_spin); i > 0; --i) {
      if (m.try_lock()) {
        locked = true;
        break;
      }
      CpuRelax();
    }
    if (!locked) m.lock();
  } else {
    m.lock();
  }
  ++t_global_lock_depth;
  g_global_lock_count.fetch_add(1);
}

FASTCARSFS_EXPORT void fastcarsfs_global_lock_leave() {
  if (REXCVAR_GET(fastcars_global_lock_off)) {
    if (t_global_lock_depth > 0) --t_global_lock_depth;  // (leaves outnumber enters in the code)
    return;
  }
  --t_global_lock_depth;
  g_global_lock_count.fetch_sub(1);
  rex::thread::global_critical_region::mutex().unlock();
}

FASTCARSFS_EXPORT uint64_t fastcarsfs_global_lock_check() {
  if (REXCVAR_GET(fastcars_global_lock_fast) || REXCVAR_GET(fastcars_global_lock_off))
    return t_global_lock_depth > 0 ? 0 : 0x8000;
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  return g_global_lock_count.load() ? 0 : 0x8000;
}

FASTCARSFS_EXPORT void fastcarsfs_trap(PPCContext* ctx, uint8_t* base, uint16_t trap_type) {
  switch (trap_type) {
    case 20:
    case 26: {
      std::string_view str(reinterpret_cast<const char*>(fastcars::GuestHost(base, ctx->r3.u32)), ctx->r4.u16);
      REXCPU_DEBUG("(service trap) {}", str);
      break;
    }
    case 0:
    case 22:
      REXCPU_WARN("tw/td trap hit (type {})", trap_type);
      break;
    case 25:
      break;
    default:
      REXCPU_WARN("Unknown trap type {}", trap_type);
      break;
  }
}

[[noreturn]] FASTCARSFS_EXPORT void fastcarsfs_unimplemented(const char* what) {
  REXCPU_ERROR("{}", what);
  std::abort();
}

}  // extern "C"
