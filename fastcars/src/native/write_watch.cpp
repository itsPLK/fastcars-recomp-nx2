// fastcars - guest physical page write tracking (see write_watch.h).

#include "write_watch.h"

#include <rex/cvar.h>
#include <rex/kernel.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>


// Off on Android: the SDK's page-protection watch (its global lock on every fault) halved the fps
// on an S23 FE in a same-run A/B (9 vs 19 fps, game frame 110 vs 55 ms); hashing is cheaper there.
// Off on Switch 2 too: no userfaultfd and no page faults at all (the SDK's watch cannot fire there).
#if defined(__SWITCH__)
#define FASTCARS_WRITE_WATCH_DEFAULT false
#else
#define FASTCARS_WRITE_WATCH_DEFAULT true
#endif
REXCVAR_DEFINE_BOOL(fastcars_native_write_watch, FASTCARS_WRITE_WATCH_DEFAULT, "FASTCARS",
                    "Native renderer: let the kernel track guest writes to cached textures and vertex/index "
                    "buffers instead of hashing them every frame");

namespace fastcars::native::write_watch {
namespace {

constexpr uint32_t kPageShift = 12;
constexpr uint32_t kPhysicalSize = 0x20000000;  // 512 MB
constexpr uint32_t kPages = kPhysicalSize >> kPageShift;

std::unique_ptr<std::atomic<uint64_t>[]> g_page_epoch;  // harvest that saw the page written
// Pages a cached copy was made from: only these are kept write-protected (the game writes ~4,000
// other pages a frame; protecting them all cost its update thread ~1 ms of faults a frame).
std::unique_ptr<uint64_t[]> g_interest;
bool Interesting(uint32_t p) { return (g_interest[p >> 6] >> (p & 63)) & 1; }
std::atomic<uint64_t> g_epoch{1};
std::once_flag g_init;
bool g_ready = false;
uint64_t g_harvests = 0, g_found = 0;
std::atomic<uint64_t> g_pages{0};
double g_ms = 0;

void Mark(uint32_t physical, uint64_t bytes, uint64_t epoch) {
  if (physical >= kPhysicalSize) return;
  uint32_t first = physical >> kPageShift;
  uint32_t last = uint32_t(std::min<uint64_t>((uint64_t(physical) + std::max<uint64_t>(bytes, 1) - 1) >> kPageShift,
                                              kPages - 1));
  for (uint32_t p = first; p <= last; p++) g_page_epoch[p].store(epoch, std::memory_order_release);
}

void Init() {}

}  // namespace

bool Enabled() {
  if (!REXCVAR_GET(fastcars_native_write_watch)) return false;
  std::call_once(g_init, Init);
  return g_ready;
}



void Harvest() {
  if (!Enabled()) return;
}

uint64_t Watch(uint32_t physical, uint32_t size) {
  if (!g_ready || !size) return 0;
  physical &= kPhysicalSize - 1;
  if (uint64_t(physical) + size > kPhysicalSize) return 0;
  return g_epoch.load(std::memory_order_acquire);
}

bool Pending(uint32_t physical, uint32_t size) {
  return false;
}

bool Clean(uint32_t physical, uint32_t size, uint64_t epoch) {
  if (!g_ready || !epoch || !size) return false;
  physical &= kPhysicalSize - 1;
  if (uint64_t(physical) + size > kPhysicalSize) return false;
  uint32_t first = physical >> kPageShift, last = (physical + size - 1) >> kPageShift;
  for (uint32_t p = first; p <= last; p++)
    if (g_page_epoch[p].load(std::memory_order_acquire) > epoch) return false;
  return true;
}

void Touch(uint32_t physical, uint32_t size) {
  if (!g_ready || !size) return;
  Mark(physical & (kPhysicalSize - 1), size, g_epoch.fetch_add(1, std::memory_order_acq_rel) + 1);
}

Stats TakeStats() {
  Stats s{g_harvests, g_pages.exchange(0), g_ms};
  g_harvests = 0;
  g_ms = 0;
  return s;
}

}  // namespace fastcars::native::write_watch
