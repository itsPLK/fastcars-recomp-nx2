// fastcars - native renderer: render thread, Plume device and the shadow window.

#include "renderer.h"

#include "backend.h"
#include "deferred_list.h"
#include "capture.h"
#include "frame.h"
#include "menu.h"
#include "paths.h"
#include "shader_compiler.h"

#include <plume_render_interface.h>

#if defined(__SWITCH__)
// Switch 2: no SDL. The window is the default NWindow (one VI surface per process, ours).
#include <switch.h>
#else
#include <SDL3/SDL.h>
#endif

#include <rex/cvar.h>
#include <rex/thread.h>
#include <rex/logging.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

REXCVAR_DEFINE_STRING(fastcars_native, "off", "FASTCARS", "Native renderer: off | shadow | on");
REXCVAR_DEFINE_BOOL(fastcars_native_vsync, true, "FASTCARS", "Native renderer: wait for vertical sync when presenting");
#ifdef __SWITCH__
REXCVAR_DECLARE(bool, fastcars_native_ubo_constants);  // backend.cpp
#endif
REXCVAR_DEFINE_BOOL(fastcars_native_exact_barriers, true, "FASTCARS",
                    "Native renderer: no TOP_OF_PIPE / BOTTOM_OF_PIPE next to other barrier stages (MoltenVK waits less)");
#define FASTCARS_LOAD_CLEARS_DEFAULT false
REXCVAR_DEFINE_BOOL(fastcars_native_record_thread, false, "FASTCARS",
                    "Native renderer: a second thread turns the frame's commands into the Vulkan command buffer "
                    "while the render thread prepares the next draws (deferred_list.h)");
REXCVAR_DEFINE_BOOL(fastcars_native_load_clears, FASTCARS_LOAD_CLEARS_DEFAULT, "FASTCARS",
                    "Native renderer: a whole-target clear at the start of a render pass becomes its load op (tile "
                    "GPUs skip loading the old contents)");
REXCVAR_DEFINE_STRING(fastcars_native_ab, "", "FASTCARS",
                      "Native renderer benchmark: name=a|b flips cvar name between a and b every 60 frames, logs averages");
REXCVAR_DEFINE_BOOL(fastcars_native_flicker_catch, false, "FASTCARS",
                    "Native renderer: detect one-frame glitches (a frame unlike both neighbours), save them to "
                    "logs/flicker and log them");
REXCVAR_DEFINE_INT32(fastcars_native_flicker_min_blocks, 6, "FASTCARS",
                     "Native renderer: --fastcars_native_flicker_catch saves (up to 40) only glitches covering at "
                     "least this many 32-px blocks; smaller ones (often thin poles while driving) are only logged");
REXCVAR_DEFINE_BOOL(fastcars_native_replay, false, "FASTCARS",
                    "Native renderer: keep the last 90 presented frames; F8 writes them to logs/replay");
REXCVAR_DEFINE_INT32(fastcars_native_shot_frames, 0, "FASTCARS",
                     "Native renderer: a screenshot (logs/shots/native_NNNNNN.ppm) every N guest frames during "
                     "gameplay, 60 at most a run (0 = off; for consoles without a keyboard)");
REXCVAR_DEFINE_INT32(fastcars_native_max_queued_frames, 2, "FASTCARS",
                     "Native renderer: guest frames queued for the render thread before the game waits (1-2)");
// Switch 2: 2. The game reuses streamed mesh memory after its D3D fence wait; the emulated command
// processor runs ahead of the renderer, so without this wait a queued frame drew from the reused memory
// (one-frame huge triangles over the sky, ~2 a minute while driving; none in 13 min with it, 2026-11-19).
#ifdef __SWITCH__
#define FASTCARS_SYNC_WAIT_DEFAULT 2
#else
#define FASTCARS_SYNC_WAIT_DEFAULT 0
#endif
REXCVAR_DEFINE_INT32(fastcars_native_sync_wait, FASTCARS_SYNC_WAIT_DEFAULT, "FASTCARS",
                     "Wait for submitted frames to finish recording: 0 off, 1 at CP idle, 2 at D3D fence wait "
                     "(Switch 2 default: 2, stops the one-frame black sky flashes)");
REXCVAR_DEFINE_BOOL(fastcars_native_sync_trace, false, "FASTCARS",
                    "Log guest frame, fence value and renderer backlog at CP idle and D3D fence waits");
REXCVAR_DEFINE_BOOL(fastcars_native_display_1080p, false, "FASTCARS",
                    "Switch 2: a 1920x1080 swap chain in handheld and docked mode (read at start; the screen's own "
                    "resolution). With TAA's temporal upscaler the resolve outputs 1920x1080 and the HUD is drawn at "
                    "1080p; otherwise the 1280x720 picture is upscaled with FSR 1 (EASU + RCAS) at present");
REXCVAR_DEFINE_BOOL(fastcars_native_shot_fg, false, "FASTCARS",
                    "Native renderer: with frame generation on, screenshots are 2560x720: the generated frame on the "
                    "left, the real frame it was made from on the right");
REXCVAR_DEFINE_INT32(fastcars_native_shot_max, 60, "FASTCARS",
                     "Native renderer: the most --fastcars_native_shot_frames screenshots in a run");
REXCVAR_DEFINE_BOOL(fastcars_native_fps, false, "FASTCARS", "Native renderer: on-screen frames-per-second counter");
REXCVAR_DEFINE_BOOL(fastcars_native_hidden, false, "FASTCARS",
                    "Native renderer: keep every game window hidden (unattended test runs)");
REXCVAR_DEFINE_STRING(fastcars_native_device, "NVIDIA", "FASTCARS",
                      "Substring of the Vulkan device name the native renderer prefers");
REXCVAR_DEFINE_STRING(fastcars_native_spirv_dir, "", "FASTCARS",
                      "Directory of precompiled shaders (<vs|ps>_<hash>.spv); default derive/native/census1/spirv");

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();  // SDL3 build: window comes later
void SetVulkanPipelineCacheData(std::vector<uint8_t> data);  // plume-fastcars.patch
std::vector<uint8_t> GetVulkanPipelineCacheData(RenderDevice* device);
extern uint32_t g_statRenderPasses, g_statBarriers;  // plume-fastcars.patch
extern bool g_exactBarrierStages;
extern bool g_clearLoadOps;
extern uint32_t g_statLoadClears, g_statClearsInPass, g_statClearsPartial;
extern bool g_colorInputAttachments;
}


static void PinToBigCores() {}

namespace fastcars::native {
namespace {

#ifdef __APPLE__
// MoltenVK compiles shaders with Metal fast math by default, which assumes no inf/NaN. The
// recompiled shaders rely on IEEE results (the radar's clamp(rsqrt(0)) * 0 = 0 became NaN: only
// the map's edges were drawn). Set before any Vulkan instance exists; an explicit setting wins.
const int g_mvk_precise_math = setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);
// New pipelines arrive in bursts of hundreds when an area or cutscene loads; Metal compiles them
// with more threads this way (fewer frames shown from the hold, see fastcars_native_hold_lost_frames).
const int g_mvk_concurrent_compile = setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);
#elif defined(__SWITCH__)
// Switch 2's Mesa 26.2.2 NVK/NAK compiler miscompiles uniform register promotion (UGPR) in
// fragment shaders containing OpKill / discard, producing pixel dropouts along triangle edges and
// shared diagonals even when discard never fires at runtime. Disabling UGPR before Vulkan
// initialization fixes it cleanly; an explicit NAK_DEBUG env setting still wins.
const int g_nak_no_ugpr = setenv("NAK_DEBUG", "nougpr", 0);
#endif
#ifdef __SWITCH__
REXCVAR_DEFINE_BOOL(fastcars_native_nak_ugpr, false, "FASTCARS",
                    "Switch 2 experiment: keep NAK's UGPR promotion (triangle-edge dropouts come back) "
                    "to measure what nougpr costs the GPU; disables the Mesa disk cache, own pipeline cache");
#endif

#if defined(__APPLE__)
// Render passes declare their colour attachments as input attachments: the backend's in-pass
// resolves read them by framebuffer fetch (FetchResolve). Before any Plume resource exists.
// FASTCARS_NATIVE_FETCH_RESOLVE=0 turns them off (resolves copy in passes of their own); on Android
// they are opt-in (=1) until checked on Adreno.
const bool g_plume_input_attachments = (plume::g_colorInputAttachments = [] {
  const char* e = std::getenv("FASTCARS_NATIVE_FETCH_RESOLVE");
  // iPhone: they strobed the whole screen white (A15 / MoltenVK); off as on Android.
  return !e || std::string(e) != "0";
}());
#endif

// Frames queued for the render thread before Submit blocks the guest (like a full GPU ring). No
// frame is ever dropped: the game renders some textures once (static shadow-map caches, ...) and
// reuses them for many frames, so a dropped frame lost them for good.
constexpr size_t kMaxQueuedFrames = 2;
// Two frames in flight + one being presented. Android lets an app hold only
// (images - minImageCount + 1) images before presenting (minImageCount is 3 on Adreno), so it needs
// more images or vkAcquireNextImageKHR blocks forever with frames still in flight.
constexpr uint32_t kSwapchainBuffers = 4;  // allows 2 presents per guest frame (frame gen) without acquire stalls
// Android and Switch 2 (OunceVK) swap chains offer RGBA8 (not BGRA8).
#if defined(__SWITCH2__)
constexpr plume::RenderFormat kSwapchainFormat = plume::RenderFormat::R8G8B8A8_UNORM;
#else
constexpr plume::RenderFormat kSwapchainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
#endif

class Renderer {
 public:
  // The command processor only fakes the GPU (--fastcars_native=on): it finishes a frame long before
  // this renderer replays it, and the fences after its swap tell the game that frame's memory is
  // free, so the game streams new textures and vertex data over memory a queued frame still has
  // to read (wrong textures flashing, spiky meshes). The guest's swaps are queued in order; the
  // command processor's swap N waits until the renderer has recorded guest swap N.
  void NoteGuestSwap(uint64_t frame_index) {
    std::lock_guard lock(swap_mutex_);
    guest_swaps_.push_back(frame_index);
    last_noted_ = frame_index;
    if (guest_swaps_.size() > 64) guest_swaps_.pop_front();
  }

  void WaitForGuestSwap() {
    FenceFlush();  // the swap is where the real GPU would have caught up at the latest
    uint64_t index;
    {
      std::lock_guard lock(swap_mutex_);
      if (guest_swaps_.empty()) return;
      index = guest_swaps_.front();
      guest_swaps_.pop_front();
    }
    auto start = std::chrono::steady_clock::now();
    bool waited = false, timed_out = false;
    {
      std::unique_lock lock(mutex_);
      if (recorded_through_ <= index) {
        waited = true;
        // Bounded: a renderer that stopped must not stall the guest's GPU for good.
        timed_out = !recorded_cv_.wait_for(lock, std::chrono::milliseconds(1000),
                                           [&] { return recorded_through_ > index || failed_; });
      }
    }
    auto& st = swap_wait_stats_;
    st.swaps++;
    if (waited) {
      st.waits++;
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      st.ms_sum += ms;
      st.ms_max = std::max(st.ms_max, ms);
    }
    if (timed_out) {
      st.timeouts++;
      size_t queued;
      uint64_t recorded;
      {
        std::lock_guard lock(mutex_);
        queued = queue_.size();
        recorded = recorded_through_;
      }
      static uint32_t logged = 0;
      if (logged++ < 20)
        REXLOG_INFO("[native] swap wait timed out: waiting for guest frame {}, recorded through {}, last noted {}, "
                    "{} queued, renderer state {} on frame {}",
                    index, recorded, last_noted_.load(), queued, render_state_.load(), render_frame_.load());
    }
    if (st.swaps == 600) {
      REXLOG_INFO("[native] command processor waited for the renderer at {} of 600 swaps (avg {:.2f} max {:.2f} ms, "
                  "{} timeouts), guest frame {}",
                  st.waits, st.waits ? st.ms_sum / st.waits : 0.0, st.ms_max, st.timeouts, index);
      st = {};
    }
  }

  void OnGuestSync(GuestSync sync, uint32_t fence) {
    const int mode = REXCVAR_GET(fastcars_native_sync_wait);
    const bool wait = mode == (sync == GuestSync::Idle ? 1 : 2);
    const bool trace = REXCVAR_GET(fastcars_native_sync_trace);
    if ((!wait && !trace) || REXCVAR_GET(fastcars_native) != "on") return;

    const uint64_t frame = GuestFrame();
    std::unique_lock lock(mutex_);
    // Snapshot at entry. The producer can queue more frames while we wait. A
    // packet popped for recording still counts until Present has read its data.
    const uint64_t target = submitted_through_;
    const uint64_t before = recorded_through_;
    bool timeout = false;
    auto start = std::chrono::steady_clock::now();
    if (wait && before < target) {
      timeout = !recorded_cv_.wait_for(lock, std::chrono::seconds(1),
                                       [&] { return recorded_through_ >= target || failed_; });
    }
    lock.unlock();
    if (trace || timeout) {
      const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count();
      REXLOG_INFO("[native] sync {} guest frame {} fence {:08X} submitted_end {} recorded_end {} "
                  "wait_us {} timeout {}", sync == GuestSync::Idle ? "idle" : "fence", frame, fence,
                  target, before, wait && before < target ? us : 0, timeout);
    }
  }

  void Submit(FramePacket&& packet) {
    if (!started_.exchange(true)) thread_ = std::thread([this] { ThreadMain(); });
    {
      std::unique_lock lock(mutex_);
      // --fastcars_native_max_queued_frames: how far the renderer may trail the game. The game reuses
      // streamed mesh memory a frame or two after its last draw; a renderer further behind read some
      // of those draws with new data (one-frame stray meshes over the sky).
      const size_t max_queued = size_t(std::clamp(REXCVAR_GET(fastcars_native_max_queued_frames), 1, int32_t(kMaxQueuedFrames)));
      space_cv_.wait(lock, [this, max_queued] { return queue_.size() < max_queued || failed_; });
      if (failed_) {  // no renderer: nothing will drain the queue
        dropped_++;
        Recycle(std::move(packet));
        return;
      }
      queue_.push_back(std::move(packet));
      submitted_through_ = queue_.back().index + 1;
    }
    cv_.notify_one();
  }

  FramePacket TakeSpare() {
    std::lock_guard lock(mutex_);
    if (spare_.empty()) return FramePacket{};
    FramePacket p = std::move(spare_.back());
    spare_.pop_back();
    return p;
  }

 private:
  void Recycle(FramePacket&& p) {  // caller holds mutex_
    if (spare_.size() >= kMaxQueuedFrames + 2) return;
    p.Clear();
    spare_.push_back(std::move(p));
  }


#if !defined(__SWITCH__)
  // Settings menu input (menu.h queues it while the menu is open): the native window's mouse and
  // the navigation keys.  (Switch 2: from HID, not written yet.)
  // Mouse (and touch-made mouse) events are in window points; the menu is laid out in swap chain
  // pixels (iPhone: 2.9 pixels a point).
  static float PixelDensity(SDL_WindowID id) {
    (void)id;
    return 1.0f;
  }

  static void MenuInput(const SDL_Event* e) {
    switch (e->type) {
      // Controller: RB + Back (View) opens or closes the menu (in either order); while it is
      // open the D-pad moves, A activates and B closes.
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        static bool rb = false, back = false;
        const bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const auto button = SDL_GamepadButton(e->gbutton.button);
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER || button == SDL_GAMEPAD_BUTTON_BACK) {
          bool& held = button == SDL_GAMEPAD_BUTTON_BACK ? back : rb;
          if (down && !held && (button == SDL_GAMEPAD_BUTTON_BACK ? rb : back)) menu::OnEscape();
          held = down;
          break;
        }
        if (!menu::IsOpen()) break;
        if (button == SDL_GAMEPAD_BUTTON_EAST) {
          if (down) menu::OnEscape();
          break;
        }
        menu::Key key;
        switch (button) {
          case SDL_GAMEPAD_BUTTON_DPAD_UP: key = menu::Key::kUp; break;
          case SDL_GAMEPAD_BUTTON_DPAD_DOWN: key = menu::Key::kDown; break;
          case SDL_GAMEPAD_BUTTON_DPAD_LEFT: key = menu::Key::kLeft; break;
          case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: key = menu::Key::kRight; break;
          case SDL_GAMEPAD_BUTTON_SOUTH: key = menu::Key::kSpace; break;
          default: return;
        }
        menu::OnKey(key, down);
        break;
      }
      case SDL_EVENT_MOUSE_MOTION: {
        const float k = PixelDensity(e->motion.windowID);
        menu::OnMouseMove(e->motion.x * k, e->motion.y * k);
        break;
      }
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        // A tap arrives as button down / up with no motion before it: move the pointer there first.
        const float k = PixelDensity(e->button.windowID);
        menu::OnMouseMove(e->button.x * k, e->button.y * k);
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        menu::OnMouseButton(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
      }
      case SDL_EVENT_MOUSE_WHEEL: menu::OnMouseWheel(e->wheel.y); break;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: {
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        switch (e->key.scancode) {
          case SDL_SCANCODE_UP: menu::OnKey(menu::Key::kUp, down); break;
          case SDL_SCANCODE_DOWN: menu::OnKey(menu::Key::kDown, down); break;
          case SDL_SCANCODE_LEFT: menu::OnKey(menu::Key::kLeft, down); break;
          case SDL_SCANCODE_RIGHT: menu::OnKey(menu::Key::kRight, down); break;
          case SDL_SCANCODE_RETURN: menu::OnKey(menu::Key::kEnter, down); break;
          case SDL_SCANCODE_SPACE: menu::OnKey(menu::Key::kSpace, down); break;
          case SDL_SCANCODE_TAB: menu::OnKey(menu::Key::kTab, down); break;
          default: break;
        }
        break;
      }
      default: break;
    }
  }
#endif

  bool Init() {
#if defined(__SWITCH__)
    // The SDK's window has no surface on Switch 2 (window_switch2.h): the screen is ours.
    window_ = nwindowGetDefault();
    if (!window_) {
      REXLOG_ERROR("[native] no default NWindow");
      return false;
    }
#else
    SDL_RunOnMainThread(
        [](void* self) {
          auto* r = static_cast<Renderer*>(self);
          // --fastcars_native=on: the emulated GPU presents nothing, so its window is hidden and
          // this one becomes the game window.
          bool on = REXCVAR_GET(fastcars_native) == "on";
          if (on) {
            int count = 0;
            if (SDL_Window** windows = SDL_GetWindows(&count)) {
              for (int i = 0; i < count; i++) SDL_HideWindow(windows[i]);
              SDL_free(windows);
            }
          }
          r->window_ = SDL_CreateWindow(on ? "fastcars" : "fastcars - native renderer (shadow)", 1280, 720,
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                            (REXCVAR_GET(fastcars_native_hidden) ? SDL_WINDOW_HIDDEN : 0));
#ifdef __APPLE__
          // Plume's Vulkan backend (MoltenVK) on macOS takes the NSWindow and a CAMetalLayer.
          if (r->window_) {
            r->metal_view_ = SDL_Metal_CreateView(r->window_);
            r->render_window_.window = SDL_GetPointerProperty(SDL_GetWindowProperties(r->window_),
                                                              SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
            r->render_window_.view = r->metal_view_ ? SDL_Metal_GetLayer(r->metal_view_) : nullptr;
          }
#endif
          // F9 (dump the next frame) and F10 (screenshot; marks audio captures) from any of the
          // process's windows, like the Win32 timer poll. The watch runs where the SDK pumps events.
          SDL_AddEventWatch(
              [](void*, SDL_Event* e) {
                if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
                  if (e->key.scancode == SDL_SCANCODE_F8) key_replay_ = true;
                  if (e->key.scancode == SDL_SCANCODE_F9 || e->key.scancode == SDL_SCANCODE_N) key_dump_ = true;
                  // M as well: no fn key needed on Mac laptops, and no pad button uses it (mnk_mode).
                  if (e->key.scancode == SDL_SCANCODE_F10 || e->key.scancode == SDL_SCANCODE_M) key_shot_ = true;
                  if (e->key.scancode == SDL_SCANCODE_F11) CycleDiagMode();
                  // Android's Back button arrives as AC_BACK: it opens the settings menu too.
                  if (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_AC_BACK) menu::OnEscape();
                }
                MenuInput(e);
                return true;
              },
              nullptr);
        },
        this, true);
    if (!window_) {
      REXLOG_ERROR("[native] SDL_CreateWindow failed: {}", SDL_GetError());
      return false;
    }
#endif
#ifdef __SWITCH__
    if (REXCVAR_GET(fastcars_native_nak_ugpr)) {
      const char* nak = std::getenv("NAK_DEBUG");
      if (nak && std::string(nak) == "nougpr") unsetenv("NAK_DEBUG");
      setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);  // its entries were compiled with nougpr
      REXLOG_INFO("[native] NAK UGPR promotion kept (experiment): NAK_DEBUG={}, Mesa disk cache off",
                  std::getenv("NAK_DEBUG") ? std::getenv("NAK_DEBUG") : "");
    }
#endif
    interface_ = plume::CreateVulkanInterface();
    if (!interface_) {
      REXLOG_ERROR("[native] Plume Vulkan interface creation failed");
      return false;
    }
    std::string preferred;
    for (const auto& name : interface_->getDeviceNames()) {
      REXLOG_INFO("[native] Vulkan device: {}", name);
      if (preferred.empty() && name.find(REXCVAR_GET(fastcars_native_device)) != std::string::npos)
        preferred = name;
    }
    LoadPipelineCache();
    device_ = interface_->createDevice(preferred);
    if (!device_) {
      REXLOG_ERROR("[native] Plume device creation failed");
      return false;
    }
    REXLOG_INFO("[native] using {}", device_->getDescription().name);
    {
      const auto& caps = device_->getCapabilities();
      REXLOG_INFO("[native] device caps: geometry shader {}, descriptor indexing {}, scalar layout {}, "
                  "buffer device address {}, present wait {}",
                  caps.geometryShader, caps.descriptorIndexing, caps.scalarBlockLayout,
                  caps.bufferDeviceAddress, caps.presentWait);
    }
    queue_cmd_ = device_->createCommandQueue(plume::RenderCommandListType::DIRECT);
    // Init steps are logged: a cold first boot after a rebuild sometimes never got past here.
    REXLOG_INFO("[native] init: queue created, creating the swap chain");
#ifdef __SWITCH__
    // Before the swap chain registers its buffers with the window (the size is fixed from then on).
    if (REXCVAR_GET(fastcars_native_display_1080p)) {
      Result rc = nwindowSetModeDimensions(nwindowGetDefault(), 1920, 1080, 1920, 1080);
      display_upscale_ = R_SUCCEEDED(rc);
      REXLOG_INFO("[native] display 1920x1080: {}", display_upscale_ ? "on" : fmt::format("failed ({:08X})", rc));
    }
#endif
    swapchain_ = queue_cmd_->createSwapChain(
        plume::RenderSwapChainDesc(
#if defined(__APPLE__)
            render_window_,
#else
            window_,
#endif
            kSwapchainFormat, kSwapchainBuffers));
    menu::LoadSettings();
    vsync_ = REXCVAR_GET(fastcars_native_vsync);
    swapchain_->setVsyncEnabled(vsync_);
    REXLOG_INFO("[native] init: swap chain created, resizing");
    swapchain_->resize();
    REXLOG_INFO("[native] init: swap chain {}x{}", swapchain_->getWidth(), swapchain_->getHeight());
    for (uint32_t i = 0; i < Backend::kFramesInFlight; i++) {
      lists_[i] = queue_cmd_->createCommandList();
      present_lists_[i] = queue_cmd_->createCommandList();
      fences_[i] = device_->createCommandFence();
      acquires_[i] = device_->createCommandSemaphore();
      fg_present_lists_[i] = queue_cmd_->createCommandList();
      fg_fences_[i] = device_->createCommandFence();
      fg_acquires_[i] = device_->createCommandSemaphore();
      gpu_times_[i] = device_->createQueryPool(2);
    }
    CreateFramebuffers();
    REXLOG_INFO("[native] init: framebuffers created, backend init");
    backend_ok_ = backend_.Init(device_.get());
    // A 1920x1080 screen: TAA's temporal upscaler outputs 1920x1080 (scene at the render scale of
    // 1280x720, HUD at 1080p); without it the 720p front buffer is upscaled with FSR 1 at present.
    if (backend_ok_ && display_upscale_) backend_.SetOutputScale(1.5f);
    REXLOG_INFO("[native] init: done");
    if (!backend_ok_) REXLOG_ERROR("[native] backend init failed; showing clear color only");
    return true;
  }


  void CreateFramebuffers() {
    framebuffers_.clear();
    if (swapchain_->isEmpty()) return;  // no images (surface lost or zero size)
    for (uint32_t i = 0; i < swapchain_->getTextureCount(); i++) {
      const plume::RenderTexture* color = swapchain_->getTexture(i);
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &color;
      desc.colorAttachmentsCount = 1;
      framebuffers_.push_back(device_->createFramebuffer(desc));
    }
    while (release_.size() < swapchain_->getTextureCount())
      release_.push_back(device_->createCommandSemaphore());
    while (fg_release_.size() < swapchain_->getTextureCount())
      fg_release_.push_back(device_->createCommandSemaphore());
  }

  // Vulkan pipeline cache, persisted next to the exe so pipelines compiled once load in about a
  // millisecond on later runs. Saved periodically (the game exits without unwinding).
  std::filesystem::path PipelineCachePath() {
#ifdef __SWITCH__
    // (Uniform-buffer constants: other SPIR-V and pipeline layout, so a cache of their own.)
    return ExeDir() / "shader_cache" /
           (REXCVAR_GET(fastcars_native_nak_ugpr)        ? "vk_pipelines_ugpr.bin"
            : REXCVAR_GET(fastcars_native_ubo_constants) ? "vk_pipelines_ubo.bin"
                                                     : "vk_pipelines_nougpr.bin");
#else
    return ExeDir() / "shader_cache" / "vk_pipelines.bin";
#endif
  }

  void LoadPipelineCache() {
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(PipelineCachePath().string().c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      data.resize(size_t(std::max(0L, std::ftell(f))));
      std::fseek(f, 0, SEEK_SET);
      if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
      std::fclose(f);
    }
    REXLOG_INFO("[native] pipeline cache: {} KB loaded", data.size() / 1024);
    plume::SetVulkanPipelineCacheData(std::move(data));
  }

  void MaybeSavePipelineCache(uint32_t new_pipelines) {
    pipelines_since_save_ += new_pipelines;
    auto now = std::chrono::steady_clock::now();
    if (!pipelines_since_save_ || now - last_cache_save_ < std::chrono::seconds(20)) return;
    last_cache_save_ = now;
    pipelines_since_save_ = 0;
    auto get_start = std::chrono::steady_clock::now();
    std::vector<uint8_t> data = plume::GetVulkanPipelineCacheData(device_.get());
    double get_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - get_start).count();
    if (data.empty()) return;
    // The file is written by a background thread: ~33 MB on the busy E: disk took ~2 s, a freeze
    // every 20 s while the renderer (and so the guest, through the swap wait) waited for it.
    if (cache_writing_.exchange(true)) return;  // the previous save is still being written
    std::thread([this, data = std::move(data), get_ms] {
      auto start = std::chrono::steady_clock::now();
      std::error_code ec;
      std::filesystem::create_directories(PipelineCachePath().parent_path(), ec);
      auto tmp = PipelineCachePath();
      tmp += ".tmp";
      if (FILE* f = std::fopen(tmp.string().c_str(), "wb")) {
        bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        if (ok) std::filesystem::rename(tmp, PipelineCachePath(), ec);
      }
      REXLOG_INFO("[native] pipeline cache saved: {} KB (get {:.0f} ms, write {:.0f} ms)", data.size() / 1024, get_ms,
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      cache_writing_ = false;
    }).detach();
  }
  std::atomic<bool> cache_writing_{false};

  // Waits for the slot's previous frame and recycles what it left behind.
  void Retire(uint32_t slot) {
    if (fg_in_flight_[slot]) {
      queue_cmd_->waitForCommandFence(fg_fences_[slot].get());
      fg_in_flight_[slot] = false;
    }
    if (!in_flight_[slot]) return;
    queue_cmd_->waitForCommandFence(fences_[slot].get());
    in_flight_[slot] = false;
    if (gpu_times_[slot] && gpu_timed_[slot]) {
      // GPU time of the slot's frame (first to last command of its main list) and the GPU idle
      // gap since the previous frame ended.
      gpu_times_[slot]->queryResults();
      const uint64_t* t = gpu_times_[slot]->getResults();
      if (t[1] > t[0] && t[1] - t[0] < 1000000000ull) {
        timing_.gpu_frame_sum += (t[1] - t[0]) / 1e6;
        if (last_gpu_end_ && t[0] > last_gpu_end_ && t[0] - last_gpu_end_ < 1000000000ull)
          timing_.gpu_gap_sum += (t[0] - last_gpu_end_) / 1e6;
        timing_.gpu_frames++;
        last_gpu_end_ = t[1];
        if (ab_slot_phase_[slot] >= 0) ab_[ab_slot_phase_[slot]].gpu += (t[1] - t[0]) / 1e6, ab_[ab_slot_phase_[slot]].gpu_n++;
      }
    }
    if (backend_ok_) backend_.FrameComplete(slot);
    FlickerAnalyze(slot);
  }

  // --fastcars_native_flicker_catch: every presented frame is copied to a per-slot readback buffer;
  // once its fence passed, a 160x90 luminance grid is compared with the frames before and after.
  // Frame n-1 is a one-frame glitch where it differs strongly from both neighbours while n-2 and n
  // agree (a triangle or plane that exists for one frame; camera motion does not revert). Frames
  // n-2, n-1 and n go to logs/flicker as PPM.
  void FlickerCapture(uint32_t slot, uint64_t index, uint32_t front_descriptor, uint32_t w, uint32_t h) {
    if (!flick_tex_) {
      flick_tex_ = device_->createTexture(
          plume::RenderTextureDesc::ColorTarget(kShotW, kShotH, plume::RenderFormat::R8G8B8A8_UNORM));
      const plume::RenderTexture* color = flick_tex_.get();
      plume::RenderFramebufferDesc fb;
      fb.colorAttachments = &color;
      fb.colorAttachmentsCount = 1;
      flick_fb_ = device_->createFramebuffer(fb);
      for (auto& rb : flick_rb_)
        rb = device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(kShotW) * kShotH * 4));
    }
    list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(flick_tex_.get(), plume::RenderTextureLayout::COLOR_WRITE));
    list_->setFramebuffer(flick_fb_.get());
    list_->setViewports(plume::RenderViewport(0, 0, float(kShotW), float(kShotH)));
    list_->setScissors(plume::RenderRect(0, 0, int32_t(kShotW), int32_t(kShotH)));
    backend_.Blit(list_, front_descriptor, plume::RenderFormat::R8G8B8A8_UNORM);
    list_->barriers(plume::RenderBarrierStage::COPY,
                    plume::RenderTextureBarrier(flick_tex_.get(), plume::RenderTextureLayout::COPY_SOURCE));
    list_->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                                 flick_rb_[slot].get(), plume::RenderFormat::R8G8B8A8_UNORM, kShotW, kShotH, 1, kShotW),
                             plume::RenderTextureCopyLocation::Subresource(flick_tex_.get()));
    list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(swap_target_, plume::RenderTextureLayout::COLOR_WRITE));
    list_->setFramebuffer(framebuffers_[swap_image_].get());
    list_->setViewports(plume::RenderViewport(0, 0, float(w), float(h)));
    list_->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
    flick_index_[slot] = index;
    flick_pending_[slot] = true;
  }

  void FlickerAnalyze(uint32_t slot) {
    if (!flick_pending_[slot]) return;
    flick_pending_[slot] = false;
    constexpr uint32_t kGw = 160, kGh = 90, kStep = kShotW / kGw;
    const uint8_t* px = static_cast<const uint8_t*>(flick_rb_[slot]->map());
    std::vector<uint8_t> grid(kGw * kGh), rgb(size_t(kShotW) * kShotH * 3);
    for (uint32_t y = 0; y < kGh; y++)
      for (uint32_t x = 0; x < kGw; x++) {
        const uint8_t* p = px + 4 * ((y * kStep + kStep / 2) * kShotW + x * kStep + kStep / 2);
        grid[y * kGw + x] = uint8_t((p[0] * 3 + p[1] * 6 + p[2]) / 10);
      }
    for (uint32_t i = 0; i < kShotW * kShotH; i++) std::memcpy(&rgb[3 * i], px + 4 * i, 3);
    flick_rb_[slot]->unmap();
    uint64_t index = flick_index_[slot];
    // History of the last two frames (grid + image), consecutive guest frames only.
    if (REXCVAR_GET(fastcars_native_flicker_catch) && flick_hist_n_ == 2 && index == flick_hist_index_[1] + 1 && flick_hist_index_[1] == flick_hist_index_[0] + 1) {
      const auto &g0 = flick_hist_grid_[0], &g1 = flick_hist_grid_[1];
      // Odd points: frame n-1 differs strongly from both neighbours, which agree. The game's dither
      // and fades scatter single odd points every frame; a glitch fills whole 4x4-point (32 px)
      // blocks. At least 6 blocks at least half odd.
      constexpr uint32_t kBw = kGw / 4, kBh = (kGh + 3) / 4;
      uint32_t block_odd[kBw * kBh] = {};
      for (uint32_t y = 0; y < kGh; y++)
        for (uint32_t x = 0; x < kGw; x++) {
          uint32_t i = y * kGw + x;
          int a = g0[i], b = g1[i], c = grid[i];
          if (std::abs(b - a) > 40 && std::abs(b - c) > 40 && std::abs(a - c) < 16) block_odd[(y / 4) * kBw + x / 4]++;
        }
      uint32_t odd = 0, x0 = kGw, y0 = kGh, x1 = 0, y1 = 0;
      for (uint32_t by = 0; by < kBh; by++)
        for (uint32_t bx = 0; bx < kBw; bx++)
          if (block_odd[by * kBw + bx] >= 8) {
            odd++;
            x0 = std::min(x0, bx * 4), y0 = std::min(y0, by * 4), x1 = std::max(x1, bx * 4 + 3), y1 = std::max(y1, by * 4 + 3);
          }
      if (odd >= 6) {
        flick_count_++;
        const bool save = odd >= uint32_t(std::max(1, REXCVAR_GET(fastcars_native_flicker_min_blocks))) && flick_saved_ < 40;
        if (save) {
          flick_saved_++;
          backend_.DumpThumbs(flick_hist_index_[0], index);  // --fastcars_native_resolve_thumbs
        }
        REXLOG_INFO("[native] flicker #{}: guest frame {} differs from both neighbours in {} of {} 32-px "
                    "blocks (box {},{}-{},{} of 160x90){}",
                    flick_count_, flick_hist_index_[1], odd, kBw * kBh, x0, y0, x1, y1,
                    save ? "; frames saved to logs/flicker" : "");
        if (save) {
          std::error_code ec;
          std::string dir = LogPath("flicker");
          std::filesystem::create_directories(dir, ec);
          auto write = [&](const std::vector<uint8_t>& img, uint64_t at, const char* tag) {
            if (FILE* f = std::fopen(fmt::format("{}/flicker_{:06}_{}.ppm", dir, at, tag).c_str(), "wb")) {
              std::fprintf(f, "P6\n%u %u\n255\n", kShotW, kShotH);
              std::fwrite(img.data(), 1, img.size(), f);
              std::fclose(f);
            }
          };
          write(flick_hist_rgb_[0], flick_hist_index_[0], "a_before");
          write(flick_hist_rgb_[1], flick_hist_index_[1], "b_glitch");
          write(rgb, index, "c_after");
          // Built in memory and written at once: a write per line (~1.5 MB of them) stalled the
          // renderer for tens of seconds on the Switch 2's SD card (the game froze).
          auto notes = [&](const std::vector<Backend::DrawNote>& list, uint64_t at, const char* tag) {
            fmt::memory_buffer out;
            uint32_t k = 0;
            for (const auto& n : list) {
              fmt::format_to(std::back_inserter(out), "{:4} vs {:016X} ps {:016X} rt {:08X} ds {:08X} vb {:08X}+{:X}/{} ib {:08X} "
                                 "start {} count {} prim {} bv {} tex {:08X} {:08X} {:08X} {:08X}{}{}{}{}\n",
                                 k++, n.vs, n.ps, n.rt0, n.depth, n.vb, n.vb_size, n.stride, n.ib, n.start, n.count,
                                 n.prim, n.base_vertex, n.tex[0], n.tex[1], n.tex[2], n.tex[3],
                                 n.copied ? " COPY" : "", n.rewritten ? " REWRITTEN" : "", n.skipped ? " SKIPPED" : "",
                                 n.uploads ? fmt::format(" UPLOADS {}", n.uploads) : "");
              fmt::format_to(std::back_inserter(out), "     vsc {:.6g}\n", fmt::join(n.vsc, " "));
            }
            if (FILE* f = std::fopen(fmt::format("{}/flicker_{:06}_{}.txt", dir, at, tag).c_str(), "wb")) {
              std::fwrite(out.data(), 1, out.size(), f);
              std::fclose(f);
            }
          };
          notes(flick_hist_notes_[0], flick_hist_index_[0], "a_before");
          notes(flick_hist_notes_[1], flick_hist_index_[1], "b_glitch");
          notes(flick_notes_[slot], index, "c_after");
        }
      }
    }
    if (REXCVAR_GET(fastcars_native_replay)) {
      if (replay_.size() >= kReplayFrames) replay_.pop_front();
      replay_.push_back({index, rgb, flick_stats_[slot]});
    }
    if (flick_hist_n_ < 2) flick_hist_n_++;
    else {
      std::swap(flick_hist_grid_[0], flick_hist_grid_[1]);
      std::swap(flick_hist_rgb_[0], flick_hist_rgb_[1]);
      std::swap(flick_hist_notes_[0], flick_hist_notes_[1]);
      flick_hist_index_[0] = flick_hist_index_[1];
    }
    flick_hist_grid_[flick_hist_n_ - 1] = std::move(grid);
    flick_hist_rgb_[flick_hist_n_ - 1] = std::move(rgb);
    flick_hist_index_[flick_hist_n_ - 1] = index;
    flick_hist_notes_[flick_hist_n_ - 1] = std::move(flick_notes_[slot]);
  }

  // F8 (--fastcars_native_replay): the last kReplayFrames presented frames, written on a worker
  // thread to logs/replay/<frame>/ as PPM with each frame's counters in frames.txt.
  void WriteReplay(uint64_t now) {
    if (!REXCVAR_GET(fastcars_native_replay)) {
      REXLOG_INFO("[native] F8: replay is off (--fastcars_native_replay=true)");
      return;
    }
    std::string dir = fmt::format("{}/{:06}", LogPath("replay"), now);
    REXLOG_INFO("[native] F8: writing the last {} frames to {}", replay_.size(), dir);
    std::thread([frames = std::move(replay_), dir] {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::ofstream list(dir + "/frames.txt");
      for (const auto& r : frames) {
        list << fmt::format("frame_{:06}.ppm: {}\n", r.index, r.stats);
        if (FILE* f = std::fopen(fmt::format("{}/frame_{:06}.ppm", dir, r.index).c_str(), "wb")) {
          std::fprintf(f, "P6\n%u %u\n255\n", kShotW, kShotH);
          std::fwrite(r.rgb.data(), 1, r.rgb.size(), f);
          std::fclose(f);
        }
      }
    }).detach();
    replay_.clear();
  }

  // Queue submissions and presents on a thread of their own: MoltenVK encodes the whole Metal
  // command buffer inside vkQueueSubmit (~3.5 ms per 1,000 draws on the M1), which then overlaps
  // the recording of the next frame instead of following it. Jobs run in order; the swap chain's
  // acquire (render thread) and present (here) are serialized by present_mutex_.
  class Submitter {
   public:
    void Push(std::function<void()> job) {
      std::lock_guard lock(mutex_);
      if (!started_) {
        started_ = true;
        std::thread([this] { Run(); }).detach();
      }
      jobs_.push_back(std::move(job));
      cv_.notify_all();
    }
    void Drain() {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return jobs_.empty() && !busy_; });
    }

    std::atomic<uint64_t> busy_us{0};  // time spent in jobs (MoltenVK encode + present), for the stats line

   private:
    void Run() {
#ifdef __SWITCH__
      rex::thread::RegisterSwitch2SamplerThread("Native submit");
#endif
      PinToBigCores();
#if defined(__APPLE__)
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // keep it on the performance cores
#endif
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(mutex_);
          cv_.wait(lock, [this] { return !jobs_.empty(); });
          job = std::move(jobs_.front());
          jobs_.pop_front();
          busy_ = true;
        }
        auto t0 = std::chrono::steady_clock::now();
        job();
        busy_us += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
        {
          std::lock_guard lock(mutex_);
          busy_ = false;
        }
        cv_.notify_all();
      }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool started_ = false, busy_ = false;
  } submitter_;
  std::mutex present_mutex_;

  void RetireAll() {
    submitter_.Drain();
    for (uint32_t i = 0; i < Backend::kFramesInFlight; i++) Retire((slot_ + i) % Backend::kFramesInFlight);
  }

  // Two frames in flight: frame N+1 is recorded while the GPU runs frame N, so a frame costs
  // max(record, GPU) instead of record + GPU. Shot and dump frames are waited for at once.
#ifdef __SWITCH__
  // [-] and [+] together open or close the settings menu; while it is open the D-pad moves, A
  // changes and B closes, and the game sees no input (fastcars_app.h; the SDK's HID driver also hides
  // the two buttons from the game while both are held, and anything held when the menu closes).
  static void PollMenuPad() {
    static PadState pad;
    static bool ready = false, combo = false;
    if (!ready) {
      padInitializeDefault(&pad);
      ready = true;
    }
    padUpdate(&pad);
    const u64 held = padGetButtons(&pad), down = padGetButtonsDown(&pad), up = padGetButtonsUp(&pad);
    const bool both = (held & HidNpadButton_Minus) && (held & HidNpadButton_Plus);
    if (both && !combo) menu::OnEscape();
    combo = both;
    if (both || !menu::IsOpen()) return;
    if (down & HidNpadButton_B) {
      menu::OnEscape();
      return;
    }
    static constexpr struct {
      u64 hid;
      menu::Key key;
    } kMap[] = {{HidNpadButton_Up, menu::Key::kUp},       {HidNpadButton_Down, menu::Key::kDown},
                {HidNpadButton_Left, menu::Key::kLeft},   {HidNpadButton_Right, menu::Key::kRight},
                {HidNpadButton_StickLUp, menu::Key::kUp}, {HidNpadButton_StickLDown, menu::Key::kDown},
                {HidNpadButton_StickLLeft, menu::Key::kLeft}, {HidNpadButton_StickLRight, menu::Key::kRight},
                {HidNpadButton_A, menu::Key::kSpace}};
    for (const auto& m : kMap) {
      if (down & m.hid) menu::OnKey(m.key, true);
      if (up & m.hid) menu::OnKey(m.key, false);
    }
  }
#endif

  void Present(const FramePacket& frame) {
#ifdef __SWITCH__
    PollMenuPad();
#endif
    if (vsync_ != REXCVAR_GET(fastcars_native_vsync)) {  // switched from the menu
      vsync_ = REXCVAR_GET(fastcars_native_vsync);
      submitter_.Drain();
      std::lock_guard lock(present_mutex_);
      swapchain_->setVsyncEnabled(vsync_);
    }
    if (swapchain_->needsResize()) {
      RetireAll();  // also drains the submit thread
      if (!swapchain_->resize()) {
        framebuffers_.clear();
      }
      CreateFramebuffers();
    }
    if (swapchain_->isEmpty()) return;  // minimized
    const uint32_t slot = slot_;
    auto wait_start = std::chrono::steady_clock::now();
    Retire(slot);
    auto wait_end = std::chrono::steady_clock::now();
    list_ = lists_[slot].get();
    list_->begin();
    plume::g_exactBarrierStages = REXCVAR_GET(fastcars_native_exact_barriers);
    plume::g_clearLoadOps = REXCVAR_GET(fastcars_native_load_clears);
    gpu_timed_[slot] = bool(gpu_times_[slot]);
    if (gpu_timed_[slot]) {
      list_->resetQueryPool(gpu_times_[slot].get(), 0, 2);
      list_->writeTimestamp(gpu_times_[slot].get(), 0);
    }
    if (backend_ok_) backend_.BeginFrame(slot);
    // logs/dump_request (any content): same as F9, for scripted captures without keyboard focus.
    // The control files are polled on a background thread: a metadata lookup on the game's disk
    // (a USB hard disk busy streaming the world) blocked this thread for 100-450 ms every 64 frames.
    static std::atomic<bool> control_files_pending{false};
    static const bool control_poller = [] {
      std::thread([] {
        const std::filesystem::path paths[] = {LogPath("dump_request"), LogPath("menu_toggle"),
                                               LogPath("menu_set"), LogPath("diag_mode"),
                                               LogPath("cvar_set")};
        for (;;) {
          std::error_code ec;
          for (const auto& p : paths)
            if (std::filesystem::exists(p, ec)) control_files_pending = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      }).detach();
      return true;
    }();
    (void)control_poller;
    if (frame.index % 64 == 0 && control_files_pending.exchange(false)) {
      std::error_code ec;
      auto request = std::filesystem::path(LogPath("dump_request"));
      if (std::filesystem::exists(request, ec)) {
        std::filesystem::remove(request, ec);
        key_dump_ = true;
      }
      // logs/menu_toggle (any content): same as Esc, for testing the menu without a keyboard.
      auto toggle = std::filesystem::path(LogPath("menu_toggle"));
      if (std::filesystem::exists(toggle, ec)) {
        std::filesystem::remove(toggle, ec);
        menu::OnEscape();
      }
      // logs/menu_set ("name=value"): sets a menu setting the way its control does.
      auto set = std::filesystem::path(LogPath("menu_set"));
      if (std::filesystem::exists(set, ec)) {
        std::ifstream in(set);
        std::string line;
        std::getline(in, line);
        in.close();
        std::filesystem::remove(set, ec);
        if (auto eq = line.find('='); eq != std::string::npos) menu::TestSet(line.substr(0, eq), line.substr(eq + 1));
      }
      // logs/cvar_set ("name=value" lines): sets live settings by name, for scripted A/B runs.
      auto cvars = std::filesystem::path(LogPath("cvar_set"));
      if (std::filesystem::exists(cvars, ec)) {
        std::ifstream in(cvars);
        for (std::string line; std::getline(in, line);)
          if (auto eq = line.find('='); eq != std::string::npos) {
            rex::cvar::SetFlagByName(line.substr(0, eq), line.substr(eq + 1));
            REXLOG_INFO("[native] cvar_set {}", line);
          }
        in.close();
        std::filesystem::remove(cvars, ec);
      }
      // logs/diag_mode (a mode number): sets the F11 tree diagnostic, then is removed.
      auto diag = std::filesystem::path(LogPath("diag_mode"));
      if (std::filesystem::exists(diag, ec)) {
        std::ifstream in(diag);
        uint32_t mode = 0;
        in >> mode;
        in.close();
        std::filesystem::remove(diag, ec);
        SetDiagMode(mode);
      }
    }
    if (backend_ok_ && key_dump_.exchange(false)) {
      if (REXCVAR_GET(fastcars_native) == "shadow") {
        // The emulated renderer dumps a window of frames from now on (logs/emu_dump, SDK patch;
        // its resolves are only readable with readback on, switched on around the dump). The native
        // dump is taken a few frames later so that it falls inside that window: the tools then find
        // its twin by draw constants even while the camera moves.
        if (readback_restore_.empty()) readback_restore_ = rex::cvar::GetFlagByName("readback_resolve");
        rex::cvar::SetFlagByName("readback_resolve", "full");
        // 8 frames: twins have matched at window positions 0-3; every dumped frame is slow.
        rex::cvar::SetFlagByName("emu_draw_dump_window", "8");
        rex::cvar::SetFlagByName("emu_resolve_dump_frame", "-1");
        native_dump_at_ = frame.index + 6;
        readback_restore_at_ = frame.index + 16;  // the emulated frame count runs a few ahead
      } else {
        native_dump_at_ = frame.index;
      }
    }
    if (backend_ok_ && native_dump_at_ && frame.index >= native_dump_at_) {
      native_dump_at_ = 0;
      REXLOG_INFO("[native] F9: dumping guest frame {}", frame.index);
      backend_.RequestDump();
    }
    if (!readback_restore_.empty() && frame.index >= readback_restore_at_) {
      rex::cvar::SetFlagByName("readback_resolve", readback_restore_);
      readback_restore_.clear();
    }
    uint32_t front_descriptor = 0;
    auto record_start = std::chrono::steady_clock::now();
    // FASTCARS_NATIVE_NO_BACKEND=1: present only a clear (bisects GPU hangs between the game's frame
    // and the swap chain path).
    static const bool no_backend = std::getenv("FASTCARS_NATIVE_NO_BACKEND") != nullptr;
    plume::RenderCommandList* record_list = list_;
    const bool deferred = REXCVAR_GET(fastcars_native_record_thread);
    if (deferred) {
      if (!deferred_) deferred_ = std::make_unique<DeferredCommandList>([] { PinToBigCores(); });
      deferred_->Begin(list_);
      record_list = deferred_.get();
    }
    plume::RenderTexture* front =
        backend_ok_ && !no_backend ? backend_.Execute(frame, record_list, &front_descriptor) : nullptr;
    if (deferred) timing_.replay_wait_sum += deferred_->Finish();
    // The frame's rendering goes to the GPU now, in a command buffer of its own; only the copy
    // into the window waits for a drawable. As one submission, the whole frame waited for the
    // swap chain image (MoltenVK: ~4 ms of GPU idle per frame on the M1).
    auto record_end = std::chrono::steady_clock::now();
    if (gpu_timed_[slot]) list_->writeTimestamp(gpu_times_[slot].get(), 1);
    list_->end();
    {
      const plume::RenderCommandList* main = list_;
      submitter_.Push([this, main] {
        const plume::RenderCommandList* lists = main;
        queue_cmd_->executeCommandLists(&lists, 1, nullptr, 0, nullptr, 0, nullptr);
      });
    }
    auto main_submit_end = std::chrono::steady_clock::now();
    if (fg_last_guest_.time_since_epoch().count()) {
      double dt = std::chrono::duration<double, std::milli>(main_submit_end - fg_last_guest_).count();
      if (dt > 4.0 && dt < 200.0) fg_guest_ms_ = fg_guest_ms_ * 0.85 + dt * 0.15;
    }
    fg_last_guest_ = main_submit_end;
    uint32_t w = swapchain_->getWidth(), h = swapchain_->getHeight();
    bool shot = front && WantShot(frame.index);
    int32_t fg_mode = 0;
    const bool shot_fg = shot && REXCVAR_GET(fastcars_native_shot_fg);
    const uint32_t fg_desc = (backend_ok_ && front && (!shot || shot_fg) && !backend_.Dumping())
                                 ? backend_.FrameGenDescriptor(&fg_mode)
                                 : 0;
    const uint32_t real_desc = front_descriptor;  // frame N (front_descriptor becomes the second present)
    // Frames presented over the last second, drawn in the top-left corner (--fastcars_native_fps).
    auto fps_now = std::chrono::steady_clock::now();
    fps_frames_ += fg_desc ? 2u : 1u;
    double fps_elapsed = std::chrono::duration<double>(fps_now - fps_since_).count();
    if (fps_elapsed >= 1.0) {
      fps_value_ = uint32_t(fps_frames_ / fps_elapsed + 0.5);
      fps_frames_ = 0;
      fps_since_ = fps_now;
    }
    ImDrawData* menu_data = nullptr;
    if (backend_ok_) {
      double frame_ms = fps_value_ ? 1000.0 / fps_value_ : 0.0;
      menu_data = menu::Frame(w, h, fps_value_, frame_ms);
    }
    std::chrono::steady_clock::time_point fg_pace_until{};
    if (fg_desc) {
      const uint32_t first_desc = (fg_mode == 1) ? fg_desc : front_descriptor;
      const uint32_t second_desc = (fg_mode == 1) ? front_descriptor : fg_desc;
      uint32_t fg_image = 0;
      bool fg_acquired;
      {
        std::lock_guard lock(present_mutex_);
        fg_acquired = swapchain_->acquireTexture(fg_acquires_[slot].get(), &fg_image);
      }
      if (fg_acquired) {
        plume::RenderCommandList* fg_list = fg_present_lists_[slot].get();
        fg_list->begin();
        plume::RenderTexture* fg_target = swapchain_->getTexture(fg_image);
        fg_list->barriers(plume::RenderBarrierStage::GRAPHICS,
                          plume::RenderTextureBarrier(fg_target, plume::RenderTextureLayout::COLOR_WRITE));
        fg_list->setFramebuffer(framebuffers_[fg_image].get());
        fg_list->setViewports(plume::RenderViewport(0, 0, float(w), float(h)));
        fg_list->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
        uint32_t fw = 0, fh = 0;
        backend_.FrontSize(&fw, &fh);
        const bool fsr = fw && fh && (fw < w || fh < h) &&
                         backend_.FsrBlit(fg_list, first_desc, fw, fh, w, h, kSwapchainFormat,
                                          framebuffers_[fg_image].get(), display_upscale_);
        if (!fsr) backend_.Blit(fg_list, first_desc, kSwapchainFormat);
        if (REXCVAR_GET(fastcars_native_fps))
          backend_.DrawCounter(fg_list, fps_value_, 8, 8, std::max(1u, h / 720), kSwapchainFormat);
        if (menu_data) backend_.DrawMenu(fg_list, menu_data, kSwapchainFormat);
        fg_list->barriers(plume::RenderBarrierStage::NONE,
                          plume::RenderTextureBarrier(fg_target, plume::RenderTextureLayout::PRESENT));
        fg_list->end();
        const plume::RenderCommandList* fg_lists = fg_list;
        plume::RenderCommandSemaphore* fg_wait = fg_acquires_[slot].get();
        plume::RenderCommandSemaphore* fg_signal = fg_release_[fg_image].get();
        plume::RenderCommandFence* fg_fence = fg_fences_[slot].get();
        submitter_.Push([this, fg_lists, fg_wait, fg_signal, fg_fence, fg_image] {
          plume::RenderCommandSemaphore* w_sem = fg_wait;
          plume::RenderCommandSemaphore* s_sem = fg_signal;
          const plume::RenderCommandList* l = fg_lists;
          queue_cmd_->executeCommandLists(&l, 1, &w_sem, 1, &s_sem, 1, fg_fence);
          std::lock_guard lock(present_mutex_);
          swapchain_->present(fg_image, &s_sem, 1);
        });
        fg_in_flight_[slot] = true;
        submitter_.Drain();
        if (!vsync_) {
          const double half_ms = std::clamp(fg_guest_ms_ * 0.45, 4.0, 16.0);
          fg_pace_until =
              std::chrono::steady_clock::now() + std::chrono::microseconds(int64_t(half_ms * 1000.0));
        }
        front_descriptor = second_desc;
      }
    }
    uint32_t image = 0;
    list_ = present_lists_[slot].get();
    list_->begin();
    bool acquired;
    {
      std::lock_guard lock(present_mutex_);
      acquired = swapchain_->acquireTexture(acquires_[slot].get(), &image);
    }
    if (!acquired) {
      static bool logged_acquire_fail = false;
      if (!logged_acquire_fail) {
        logged_acquire_fail = true;
        REXLOG_WARN("[native] swapchain_->acquireTexture failed (frame {})", frame.index);
      }
      list_->end();
      if (backend_ok_) backend_.EndFrame(slot);
      const plume::RenderCommandList* lists = list_;
      plume::RenderCommandFence* fence = fences_[slot].get();
      submitter_.Push([this, lists, fence] {
        const plume::RenderCommandList* l = lists;
        queue_cmd_->executeCommandLists(&l, 1, nullptr, 0, nullptr, 0, fence);
      });
      in_flight_[slot] = true;
      slot_ = (slot + 1) % Backend::kFramesInFlight;
      return;
    }
    auto acquire_end = std::chrono::steady_clock::now();
    plume::RenderTexture* target = swapchain_->getTexture(image);
    swap_target_ = target;
    swap_image_ = image;
    list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                    plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
    list_->setFramebuffer(framebuffers_[image].get());
    list_->setViewports(plume::RenderViewport(0, 0, float(w), float(h)));
    list_->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
    if (shot) {
      // Blit the front buffer (whatever its format) into an RGBA8 capture target, read it back
      // and write it as PPM once the fence passed.
      if (!shot_tex_) {
        // Twice as wide: --fastcars_native_shot_fg puts the generated frame and the real one side by side.
        shot_tex_ = device_->createTexture(
            plume::RenderTextureDesc::ColorTarget(2 * kShotW, kShotH, plume::RenderFormat::R8G8B8A8_UNORM));
        const plume::RenderTexture* color = shot_tex_.get();
        plume::RenderFramebufferDesc fb;
        fb.colorAttachments = &color;
        fb.colorAttachmentsCount = 1;
        shot_fb_ = device_->createFramebuffer(fb);
        readback_ =
            device_->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(2 * kShotW) * kShotH * 4));
      }
      list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                      plume::RenderTextureBarrier(shot_tex_.get(), plume::RenderTextureLayout::COLOR_WRITE));
      list_->setFramebuffer(shot_fb_.get());
      list_->setViewports(plume::RenderViewport(0, 0, float(kShotW), float(kShotH)));
      list_->setScissors(plume::RenderRect(0, 0, int32_t(kShotW), int32_t(kShotH)));
      shot_wide_ = shot_fg && fg_desc;
      backend_.Blit(list_, shot_wide_ ? fg_desc : front_descriptor, plume::RenderFormat::R8G8B8A8_UNORM);
      if (shot_wide_) {
        list_->setViewports(plume::RenderViewport(float(kShotW), 0, float(kShotW), float(kShotH)));
        list_->setScissors(plume::RenderRect(int32_t(kShotW), 0, int32_t(2 * kShotW), int32_t(kShotH)));
        backend_.Blit(list_, real_desc, plume::RenderFormat::R8G8B8A8_UNORM);
      }
      list_->barriers(plume::RenderBarrierStage::COPY,
                      plume::RenderTextureBarrier(shot_tex_.get(), plume::RenderTextureLayout::COPY_SOURCE));
      list_->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback_.get(),
                                                                                 plume::RenderFormat::R8G8B8A8_UNORM,
                                                                                 2 * kShotW, kShotH, 1, 2 * kShotW),
                               plume::RenderTextureCopyLocation::Subresource(shot_tex_.get()));
      list_->barriers(plume::RenderBarrierStage::GRAPHICS,
                      plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
      list_->setFramebuffer(framebuffers_[image].get());
      list_->setViewports(plume::RenderViewport(0, 0, float(w), float(h)));
      list_->setScissors(plume::RenderRect(0, 0, int32_t(w), int32_t(h)));
    }
    if (front && (REXCVAR_GET(fastcars_native_flicker_catch) || REXCVAR_GET(fastcars_native_replay))) {
      if (backend_ok_) {
        auto st = backend_.LastStats();
        flick_stats_[slot] = fmt::format("{} draws, {} skipped, {} read after a rewrite, {} new pipelines, "
                                         "{} texture uploads",
                                         st.draws, st.skipped, st.rewritten, st.pipelines_created, st.textures);
      }
      if (backend_ok_ && REXCVAR_GET(fastcars_native_flicker_catch)) flick_notes_[slot] = backend_.LastNotes();
      FlickerCapture(slot, frame.index, front_descriptor, w, h);
    }
    if (key_replay_.exchange(false)) WriteReplay(frame.index);
    if (front) {
      // A front buffer smaller than the screen (render scale < 1): FSR 1 instead of a stretch.
      uint32_t fw = 0, fh = 0;
      backend_.FrontSize(&fw, &fh);
      const bool fsr = fw && fh && (fw < w || fh < h) &&
                       backend_.FsrBlit(list_, front_descriptor, fw, fh, w, h, kSwapchainFormat,
                                        framebuffers_[image].get(), display_upscale_);
      if (!fsr) backend_.Blit(list_, front_descriptor, kSwapchainFormat);
    } else {
      list_->clearColor(0, plume::RenderColor(0.12f, 0.0f, 0.12f, 1.0f));
    }
    if (backend_ok_ && REXCVAR_GET(fastcars_native_fps))
      backend_.DrawCounter(list_, fps_value_, 8, 8, std::max(1u, h / 720), kSwapchainFormat);
    if (menu_data) backend_.DrawMenu(list_, menu_data, kSwapchainFormat);
    list_->barriers(plume::RenderBarrierStage::NONE,
                    plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
    list_->end();
    if (backend_ok_) backend_.EndFrame(slot);
    const plume::RenderCommandList* lists = list_;
    plume::RenderCommandSemaphore* wait = acquires_[slot].get();
    plume::RenderCommandSemaphore* signal = release_[image].get();
    plume::RenderCommandFence* fence = fences_[slot].get();
    submitter_.Push([this, lists, wait, signal, fence, image, fg_pace_until] {
      plume::RenderCommandSemaphore* w = wait;
      plume::RenderCommandSemaphore* sg = signal;
      const plume::RenderCommandList* l = lists;
      queue_cmd_->executeCommandLists(&l, 1, &w, 1, &sg, 1, fence);
      if (fg_pace_until.time_since_epoch().count() && fg_pace_until > std::chrono::steady_clock::now())
        std::this_thread::sleep_until(fg_pace_until);
      std::lock_guard lock(present_mutex_);
      swapchain_->present(image, &sg, 1);
    });
    auto submit_end = std::chrono::steady_clock::now();
    auto present_end = submit_end;
    in_flight_[slot] = true;
    slot_ = (slot + 1) % Backend::kFramesInFlight;
    auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    timing_.Add(ms(record_end - record_start), ms(wait_end - wait_start));
    last_record_ms_ = ms(record_end - record_start);
    ab_slot_phase_[slot] = ab_phase_;
    // Hitch log: a present more than 25 ms after the previous one (50 ms on phones, where 30 fps
    // frames are 33 ms: every frame was a "hitch" and the log grew by megabytes a minute), with what
    // this frame cost.
// Switch 2 too: at ~38 ms frames every frame was logged, and the writes reached the SD card from
// this thread (~3% of it in fsdev_write).
#if defined(__SWITCH__)
    constexpr double kHitchMs = 50.0;
#else
    constexpr double kHitchMs = 25.0;
#endif
    double interval = last_present_.time_since_epoch().count() ? ms(present_end - last_present_) : 0;
    last_present_ = present_end;
    if (backend_ok_) MaybeSavePipelineCache(backend_.LastStats().pipelines_created);
    if (interval > kHitchMs && backend_ok_) {
      auto st = backend_.LastStats();
      REXLOG_INFO("[native] hitch {:.1f} ms at guest frame {}: record {:.1f} ms, wait {:.1f} ms, {} new pipelines, "
                  "{} textures, {} draws (misses: shaders {:.1f} pipelines {:.1f} textures {:.1f} ms; resolves {:.1f} ms)",
                  interval, frame.index, ms(record_end - record_start), ms(wait_end - wait_start),
                  st.pipelines_created, st.textures, st.draws, st.shader_ms, st.pipeline_ms, st.texture_ms,
                  st.resolve_ms);
    }
    timing_.acquire_sum += ms(acquire_end - main_submit_end);
    timing_.submit_sum += ms(submit_end - record_end);
    timing_.present_sum += ms(present_end - submit_end);
    if (shot || (backend_ok_ && backend_.Dumping())) {
      // Plume's waitForCommandFence also resets the fence: wait exactly once per submit.
      queue_cmd_->waitForCommandFence(fences_[slot].get());
      in_flight_[slot] = false;
      if (backend_ok_) {
        backend_.WriteDump();
        backend_.FrameComplete(slot);
      }
      if (shot) WriteShot(frame.index, ab_slot_phase_[slot]);
    }
  }

  // FASTCARS_NATIVE_SHOT_DIR (+ FASTCARS_SHOT_SECS, default 10): periodic PPM captures of the native
  // front buffer, comparable with the emulated renderer's FASTCARS_SHOT_DIR captures.
  std::string ShotDir() {
    const char* dir = std::getenv("FASTCARS_NATIVE_SHOT_DIR");
    return dir && *dir ? dir : LogPath("shots");
  }

  bool WantShot(uint64_t index) {
    if (key_shot_.exchange(false)) {
      std::error_code ec;
      std::filesystem::create_directories(ShotDir(), ec);
      REXLOG_INFO("[native] F10: screenshot of guest frame {} to {}", index, ShotDir());
      return true;
    }
    // Gameplay only (the intro and loading screens are a handful of draws).
    if (const int32_t n = REXCVAR_GET(fastcars_native_shot_frames);
        n > 0 && index % uint64_t(n) == 0 && cvar_shots_ < uint32_t(std::max(0, REXCVAR_GET(fastcars_native_shot_max))) &&
        backend_ok_ && backend_.LastStats().draws > 500) {
      std::error_code ec;
      if (!cvar_shots_++) std::filesystem::create_directories(ShotDir(), ec);
      return true;
    }
    const char* dir = std::getenv("FASTCARS_NATIVE_SHOT_DIR");
    if (!dir || !*dir) return false;
    const char* secs = std::getenv("FASTCARS_SHOT_SECS");
    uint64_t every = uint64_t(std::max(1, secs ? std::atoi(secs) : 10)) * 30;
    return index % every == 0;
  }

  // ab_phase: the --fastcars_native_ab side this frame was drawn with (-1: none), kept in the name so
  // captures can be compared per side.
  void WriteShot(uint64_t index, int ab_phase = -1) {
    std::string path = ab_phase >= 0 ? fmt::format("{}/native_{:06}_ab{}.ppm", ShotDir(), index, ab_phase)
                                     : fmt::format("{}/native_{:06}.ppm", ShotDir(), index);
    const uint8_t* px = static_cast<const uint8_t*>(readback_->map());
    const uint32_t out_w = shot_wide_ ? 2 * kShotW : kShotW;  // the readback rows are 2 * kShotW wide
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", out_w, kShotH);
      // One write: a write per pixel took 7-23 s a capture on the Switch 2's SD card.
      std::vector<uint8_t> rgb(size_t(out_w) * kShotH * 3);
      for (uint32_t y = 0; y < kShotH; y++)
        for (uint32_t x = 0; x < out_w; x++)
          std::memcpy(&rgb[(size_t(y) * out_w + x) * 3], px + 4 * (size_t(y) * 2 * kShotW + x), 3);
      std::fwrite(rgb.data(), 1, rgb.size(), f);
      std::fclose(f);
    }
    readback_->unmap();
  }

  std::string SpirvDir() {
    std::string dir = REXCVAR_GET(fastcars_native_spirv_dir);
    if (dir.empty()) dir = DefaultSpirvDir();
    return dir;
  }

  bool HaveShader(bool pixel, uint64_t hash) {
    auto key = hash ^ (pixel ? 1ull << 63 : 0);
    if (have_shader_generation_ != CompileGeneration()) {
      have_shader_generation_ = CompileGeneration();
      std::erase_if(have_shader_, [](const auto& kv) { return !kv.second; });
    }
    auto it = have_shader_.find(key);
    if (it != have_shader_.end()) return it->second;
    // Diagnostic only: answered from memory (a lookup on the busy game disk stalled this thread).
    std::string name = fmt::format("{}_{:016X}", pixel ? "ps" : "vs", hash);
    bool ok = hash && (Backend::SpirvListed(name + ".spv") || ShaderOnDisk(name));
    have_shader_[key] = ok;
    return ok;
  }

  // Bring-up check of the recorded frame: are its shaders, buffers and targets usable?
  void Validate(const FramePacket& f) {
    if (f.index - last_validated_ < 150) return;
    last_validated_ = f.index;
    uint32_t missing_vs = 0, missing_ps = 0, no_decl = 0, bad_ib16 = 0;
    std::set<uint32_t> rts;
    uint32_t prims[16] = {};
    for (const auto& d : f.draws) {
      prims[d.primitive & 15]++;
      missing_vs += !HaveShader(false, d.vs_hash);
      if (!HaveShader(true, d.ps_hash)) {
        missing_ps++;
        if (logged_missing_.insert(d.ps_hash).second && logged_missing_.size() <= 40)
          REXLOG_INFO("[native]   no SPIR-V for ps {:016X}", d.ps_hash);
      }
      no_decl += d.element_count == 0;
      // Inline (BeginIndexedVertices) indices carry their own size; index_size is 0 for them.
      if (d.indexed && !d.index32 && !d.inline_index_size && uint64_t(d.start + d.count) * 2 > d.index_size) {
        bad_ib16++;
        if (logged_bad_ib_++ < 12)
          REXLOG_INFO("[native]   ib16 out of range: start {} count {} base {} ib {:08X} size {} prim {} vs {:016X}",
                      d.start, d.count, d.base_vertex, d.index_address, d.index_size, d.primitive, d.vs_hash);
      }
      rts.insert(d.render_targets[0]);
    }
    REXLOG_INFO("[native] frame {}: {} draws ({} vs / {} ps without SPIR-V, {} without decl, {} ib16 out of "
                "range), {} clears, {} resolves, {} render targets, front {:08X}",
                f.index, f.draws.size(), missing_vs, missing_ps, no_decl, bad_ib16, f.clears.size(),
                f.resolves.size(), rts.size(), f.front_buffer);
    std::string ph;
    for (int i = 0; i < 16; i++)
      if (prims[i]) ph += fmt::format(" {}:{}", i, prims[i]);
    REXLOG_INFO("[native]   primitives{}", ph);
    if (logged_layouts_ < 3 && !f.resolves.empty()) {
      logged_layouts_++;
      const auto& r = f.resolves.back();
      std::string w;
      for (uint32_t x : r.dest_words) w += fmt::format("{:08X} ", x);
      REXLOG_INFO("[native]   resolve flags {:08X} rect {} ({},{})-({},{}) dest {:08X}: {}", r.flags, r.has_rect,
                  r.source_rect[0], r.source_rect[1], r.source_rect[2], r.source_rect[3], r.dest_texture, w);
    }
  }

  void ThreadMain() {
#ifdef __SWITCH__
    rex::thread::RegisterSwitch2SamplerThread("Native render");
#endif
    PinToBigCores();
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // keep it on the performance cores
#endif
    if (!Init()) {
      {
        std::lock_guard lock(mutex_);
        failed_ = true;
      }
      space_cv_.notify_all();
      recorded_cv_.notify_all();
      return;
    }
    uint64_t presented = 0;
    for (;;) {
      FramePacket frame;
      {
        std::unique_lock lock(mutex_);
        auto idle_start = std::chrono::steady_clock::now();
        render_state_ = 0;  // waiting for a frame
        cv_.wait(lock, [this] { return !queue_.empty(); });
        timing_.idle_sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - idle_start).count();
        frame = std::move(queue_.front());
        queue_.pop_front();
      }
      space_cv_.notify_one();
      render_frame_ = frame.index;
      render_state_ = 1;  // recording + presenting
      ApplyFenceCopies(frame);
      Validate(frame);
      AbStep(presented);
      Present(frame);
      AbRecord(presented);
      FenceCopiesRecorded(frame.index);
      render_state_ = 2;
      {
        std::lock_guard lock(mutex_);
        recorded_through_ = frame.index + 1;  // its guest memory has been read
        Recycle(std::move(frame));
      }
      recorded_cv_.notify_all();
      if (++presented % 300 == 0) {
        REXLOG_INFO("[native] presented {} frames (guest frame {}, dropped {}): record avg {:.2f} max {:.2f} ms, "
                    "wait for slot avg {:.2f} max {:.2f} ms",
                    presented, frame.index, dropped_.load(), timing_.record_sum / timing_.n, timing_.record_max,
                    timing_.gpu_sum / timing_.n, timing_.gpu_max);
        if (deferred_)
          REXLOG_INFO("[native]   record thread: replay busy {:.2f} ms per frame, render thread waited {:.2f} ms for it",
                      deferred_->TakeBusyMs() / timing_.n, timing_.replay_wait_sum / timing_.n);
        REXLOG_INFO("[native]   avg acquire {:.2f} submit {:.2f} present {:.2f} idle {:.2f} ms; per frame {:.0f} "
                    "render passes ({:.0f} load clears; not: {:.0f} in a pass, {:.0f} partial), {:.0f} barriers; submit thread busy {:.2f} ms; GPU frame {:.2f} ms, idle before {:.2f} ms",
                    timing_.acquire_sum / timing_.n, timing_.submit_sum / timing_.n,
                    timing_.present_sum / timing_.n, timing_.idle_sum / timing_.n,
                    double(plume::g_statRenderPasses) / timing_.n, double(plume::g_statLoadClears) / timing_.n,
                    double(plume::g_statClearsInPass) / timing_.n, double(plume::g_statClearsPartial) / timing_.n,
                    double(plume::g_statBarriers) / timing_.n,
                    submitter_.busy_us.exchange(0) / 1000.0 / timing_.n,
                    timing_.gpu_frames ? timing_.gpu_frame_sum / timing_.gpu_frames : 0.0,
                    timing_.gpu_frames ? timing_.gpu_gap_sum / timing_.gpu_frames : 0.0);
        plume::g_statRenderPasses = plume::g_statBarriers = plume::g_statLoadClears = 0;
        plume::g_statClearsInPass = plume::g_statClearsPartial = 0;
        timing_ = {};
      }
    }
  }

  // --fastcars_native_ab=name=a|b: flips cvar `name` between a and b every 60 presented frames and
  // logs per-setting averages every 1200 frames (both settings see the same scenes; the first 8
  // frames after a flip are not counted).
  struct AbStats {
    double interval = 0, record = 0, gpu = 0;
    uint32_t n = 0, gpu_n = 0;
  };
  AbStats ab_[2];
  int ab_phase_ = -1;  // -1: off or warming up
  int ab_slot_phase_[Backend::kFramesInFlight] = {-1, -1};
  std::string ab_spec_, ab_name_, ab_values_[2];
  std::chrono::steady_clock::time_point ab_last_;
  uint64_t ab_frames_ = 0;
  double last_record_ms_ = 0;
  void AbStep(uint64_t presented) {
    std::string spec = REXCVAR_GET(fastcars_native_ab);
    if (spec != ab_spec_) {
      if (!ab_name_.empty()) rex::cvar::SetFlagByName(ab_name_, ab_values_[0]);
      ab_spec_ = spec;
      ab_name_.clear();
      ab_[0] = ab_[1] = {};
      ab_frames_ = 0;
      auto eq = spec.find('='), bar = spec.find('|');
      if (eq != std::string::npos && bar != std::string::npos && bar > eq) {
        ab_name_ = spec.substr(0, eq);
        ab_values_[0] = spec.substr(eq + 1, bar - eq - 1);
        ab_values_[1] = spec.substr(bar + 1);
        REXLOG_INFO("[native] A/B {}: {} | {}", ab_name_, ab_values_[0], ab_values_[1]);
      }
    }
    ab_phase_ = -1;
    if (ab_name_.empty()) return;
    uint64_t k = ab_frames_++;
    int phase = int((k / 60) & 1);
    if (k % 60 == 0) rex::cvar::SetFlagByName(ab_name_, ab_values_[phase]);
    if (k % 60 >= 8) ab_phase_ = phase;
  }
  void AbRecord(uint64_t presented) {
    auto now = std::chrono::steady_clock::now();
    if (ab_phase_ >= 0) {
      auto& a = ab_[ab_phase_];
      a.interval += std::chrono::duration<double, std::milli>(now - ab_last_).count();
      a.record += last_record_ms_;
      a.n++;
    }
    ab_last_ = now;
    if (!ab_name_.empty() && ab_frames_ % 1200 == 0 && ab_[0].n && ab_[1].n) {
      auto line = [](const AbStats& a) {
        return fmt::format("{:.1f} fps (frame {:.2f} ms, record {:.2f}, GPU {:.2f})", 1000.0 * a.n / a.interval,
                           a.interval / a.n, a.record / a.n, a.gpu_n ? a.gpu / a.gpu_n : 0.0);
      };
      REXLOG_INFO("[native] A/B {} = {}: {} | = {}: {}", ab_name_, ab_values_[0], line(ab_[0]), ab_values_[1],
                  line(ab_[1]));
      ab_[0] = ab_[1] = {};
    }
  }

  static inline std::atomic<bool> key_dump_{false}, key_shot_{false};  // F9 / F10 (window thread)
  static inline std::atomic<bool> key_replay_{false};                   // F8
  std::string readback_restore_;  // readback_resolve to restore after a shadow-mode F9 dump
  uint64_t readback_restore_at_ = 0;
  uint64_t native_dump_at_ = 0;  // guest frame to dump (F9; delayed in shadow mode)
  std::chrono::steady_clock::time_point last_present_{}, last_cache_save_{};
  uint32_t pipelines_since_save_ = 0;
  std::mutex mutex_;
  std::condition_variable cv_, space_cv_;  // frame queued / queue has room
  std::deque<FramePacket> queue_;
  std::vector<FramePacket> spare_;  // presented/dropped packets, emptied, for TakeSpare
  std::atomic<bool> started_{false}, failed_{false};
  std::atomic<uint64_t> last_noted_{0}, render_frame_{0};
  std::atomic<int> render_state_{0};
  std::mutex swap_mutex_;
  std::condition_variable recorded_cv_;
  std::deque<uint64_t> guest_swaps_;
  uint64_t submitted_through_ = 0;  // one past the most recently enqueued frame
  uint64_t recorded_through_ = 0;  // guest frames below this have been recorded
  struct SwapWaitStats {
    uint32_t swaps = 0, waits = 0, timeouts = 0;
    double ms_sum = 0, ms_max = 0;
  } swap_wait_stats_;  // command processor thread only
  std::atomic<uint64_t> dropped_{0};
  std::chrono::steady_clock::time_point fps_since_ = std::chrono::steady_clock::now();
  uint32_t fps_frames_ = 0, fps_value_ = 0;
  bool vsync_ = true;  // the swap chain's current setting (the menu changes the cvar)
  struct Timing {  // render thread, per 300 presented frames
    double record_sum = 0, record_max = 0, gpu_sum = 0, gpu_max = 0;
    double gpu_frame_sum = 0, gpu_gap_sum = 0;
    uint32_t gpu_frames = 0;
    double acquire_sum = 0, submit_sum = 0, present_sum = 0, idle_sum = 0;
    double replay_wait_sum = 0;  // --fastcars_native_record_thread: the render thread waited for the replay
    uint32_t n = 0;
    void Add(double record, double gpu) {
      record_sum += record, gpu_sum += gpu, n++;
      record_max = std::max(record_max, record), gpu_max = std::max(gpu_max, gpu);
    }
  } timing_;
  std::thread thread_;

#if defined(__SWITCH__)
  NWindow* window_ = nullptr;
#else
  SDL_Window* window_ = nullptr;
#ifdef __APPLE__
  SDL_MetalView metal_view_ = nullptr;
  plume::RenderWindow render_window_ = {};
  static inline std::atomic<bool> ios_background_{false}, ios_drained_{false};
#endif
#endif
  std::unique_ptr<plume::RenderInterface> interface_;
  std::unique_ptr<plume::RenderDevice> device_;
  std::unique_ptr<plume::RenderCommandQueue> queue_cmd_;
  std::unique_ptr<plume::RenderSwapChain> swapchain_;
  std::unique_ptr<plume::RenderCommandList> lists_[Backend::kFramesInFlight];
  std::unique_ptr<DeferredCommandList> deferred_;  // --fastcars_native_record_thread
  std::unique_ptr<plume::RenderCommandList> present_lists_[Backend::kFramesInFlight];  // window copy
  std::unique_ptr<plume::RenderCommandFence> fences_[Backend::kFramesInFlight];
  std::unique_ptr<plume::RenderCommandList> fg_present_lists_[Backend::kFramesInFlight];
  std::unique_ptr<plume::RenderCommandFence> fg_fences_[Backend::kFramesInFlight];
  std::unique_ptr<plume::RenderCommandSemaphore> fg_acquires_[Backend::kFramesInFlight];
  bool fg_in_flight_[Backend::kFramesInFlight] = {};
  uint32_t cvar_shots_ = 0;  // --fastcars_native_shot_frames captures so far
  bool shot_wide_ = false;
  bool display_upscale_ = false;  // --fastcars_native_display_1080p: the swap chain is larger than the picture   // the pending screenshot is generated | real (--fastcars_native_shot_fg)
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> fg_release_;
  std::chrono::steady_clock::time_point fg_last_guest_{};
  double fg_guest_ms_ = 33.3;
  std::unique_ptr<plume::RenderQueryPool> gpu_times_[Backend::kFramesInFlight];  // main list start / end
  bool gpu_timed_[Backend::kFramesInFlight] = {};
  uint64_t last_gpu_end_ = 0;
  std::unique_ptr<plume::RenderCommandSemaphore> acquires_[Backend::kFramesInFlight];
  bool in_flight_[Backend::kFramesInFlight] = {};
  uint32_t slot_ = 0;
  plume::RenderCommandList* list_ = nullptr;  // the slot being recorded
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release_;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers_;

  Backend backend_;
  static constexpr uint32_t kShotW = 1280, kShotH = 720;
  // Flicker catcher (--fastcars_native_flicker_catch).
  std::unique_ptr<plume::RenderTexture> flick_tex_;
  std::unique_ptr<plume::RenderFramebuffer> flick_fb_;
  std::unique_ptr<plume::RenderBuffer> flick_rb_[Backend::kFramesInFlight];
  uint64_t flick_index_[Backend::kFramesInFlight] = {};
  bool flick_pending_[Backend::kFramesInFlight] = {};
  std::vector<uint8_t> flick_hist_grid_[2], flick_hist_rgb_[2];
  uint64_t flick_hist_index_[2] = {};
  uint32_t flick_hist_n_ = 0, flick_count_ = 0, flick_saved_ = 0;
  std::string flick_stats_[Backend::kFramesInFlight];
  std::vector<Backend::DrawNote> flick_notes_[Backend::kFramesInFlight], flick_hist_notes_[2];
  struct ReplayFrame {
    uint64_t index;
    std::vector<uint8_t> rgb;
    std::string stats;
  };
  static constexpr size_t kReplayFrames = 90;
  std::deque<ReplayFrame> replay_;
  plume::RenderTexture* swap_target_ = nullptr;
  uint32_t swap_image_ = 0;
  std::unique_ptr<plume::RenderBuffer> readback_;
  std::unique_ptr<plume::RenderTexture> shot_tex_;
  std::unique_ptr<plume::RenderFramebuffer> shot_fb_;
  bool backend_ok_ = false;
  std::unordered_map<uint64_t, bool> have_shader_;
  uint64_t have_shader_generation_ = 0;
  std::set<uint64_t> logged_missing_;
  uint32_t logged_bad_ib_ = 0;
  uint64_t last_validated_ = 0;
  int logged_layouts_ = 0;
};

Renderer g_renderer;


}  // namespace

bool Active() { return REXCVAR_GET(fastcars_native) != "off"; }

void Submit(FramePacket&& frame) { g_renderer.Submit(std::move(frame)); }

void NoteGuestSwap(uint64_t frame_index) { g_renderer.NoteGuestSwap(frame_index); }

void OnGuestSync(GuestSync sync, uint32_t fence) { g_renderer.OnGuestSync(sync, fence); }

FramePacket TakeSpare() { return g_renderer.TakeSpare(); }

}  // namespace fastcars::native

// Looked up by the SDK's D3D12 and Vulkan command processors (gpu_skip_rendering) and called at
// every swap they execute; see Renderer::NoteGuestSwap.
#define FASTCARS_EXPORT __attribute__((visibility("default")))  // exported by the link options
extern "C" FASTCARS_EXPORT void rex_skip_rendering_swap(uint64_t) { fastcars::native::g_renderer.WaitForGuestSwap(); }
