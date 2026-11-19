// fastcars - frame-rate limit (--fastcars_fps_limit).
//
// The game paces itself in sub_827B5878: while less than N/60 s (N = the int at 0x8391D9D4,
// 2 on the 360) passed since the last frame (timebase), it sleeps. N <= 0 skips the wait.
// --fastcars_fps_limit=30 (default) leaves the game's limiter alone; any other value sets N = 0
// and, unless 0 (uncapped), paces frames here to that rate instead. The game itself is not
// the only limit: with the emulated GPU's vblank at 60 Hz (--vsync=true) other waits still
// tie it to the display, so uncapped play also wants --vsync=false.

#include <rex/cvar.h>
#include "guest_ptr.h"
#include <rex/hook.h>

#include <chrono>
#include <cstdint>
#include <thread>


#if defined(__SWITCH__)
#define FASTCARS_FPS_LIMIT_DEFAULT 60  // the console reaches ~28-35: no cap below that
#else
#define FASTCARS_FPS_LIMIT_DEFAULT 30
#endif
REXCVAR_DEFINE_INT32(fastcars_fps_limit, FASTCARS_FPS_LIMIT_DEFAULT, "FASTCARS",
                     "Frame-rate limit: 30 = the game's own limiter, 0 = uncapped, other = that rate");
REXCVAR_DEFINE_BOOL(fastcars_audio_unsync, true, "FASTCARS",
                    "Above 30 fps: stop the audio update waiting for the next 16 ms audio frame "
                    "(otherwise the game cannot exceed 62.5 fps)");
REXCVAR_DEFINE_BOOL(fastcars_no_power_throttling, true, "FASTCARS",
                    "Windows: opt out of power throttling (EcoQoS / efficiency cores, ignored timer "
                    "resolution) while the window is hidden, minimized or in the background");

namespace {

constexpr uint32_t kFrameIntervalVblanks = 0x8391D9D4;  // N: frame time in 1/60 s units
// The audio update (sub_831D8A68, run by the thread in sub_82488170 that the main loop waits on
// every frame) polls up to 1 s for the audio engine's next frame (768 samples at 48 kHz = 16 ms);
// the main loop therefore never exceeds 62.5 fps. When that poll times out the game sets this
// byte and stops waiting from then on: its own no-audio-sync fallback, set here instead.
constexpr uint32_t kAudioSyncTimedOut = 0x83D0D4CD;

void Store32(uint8_t* base, uint32_t addr, int32_t value) {
  uint8_t* p = fastcars::GuestHost(base, addr);
  p[0] = uint8_t(uint32_t(value) >> 24), p[1] = uint8_t(value >> 16), p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void ApplyPowerThrottling() {}

}  // namespace

REX_EXTERN(__imp__sub_827B5878);
REX_HOOK_RAW(sub_827B5878) {
  ApplyPowerThrottling();
  int32_t limit = REXCVAR_GET(fastcars_fps_limit);
  if (limit == 30) {
    __imp__sub_827B5878(ctx, base);
    return;
  }
  Store32(base, kFrameIntervalVblanks, 0);
  if (REXCVAR_GET(fastcars_audio_unsync)) *fastcars::GuestHost(base, kAudioSyncTimedOut) = 1;
  __imp__sub_827B5878(ctx, base);
  if (limit <= 0) return;
  using clock = std::chrono::steady_clock;
  static clock::time_point next = clock::now();
  auto period = std::chrono::nanoseconds(1000000000 / limit);
  auto now = clock::now();
  if (next > now) std::this_thread::sleep_until(next);
  next = std::max(next + period, now);  // no catch-up after a long frame
}
