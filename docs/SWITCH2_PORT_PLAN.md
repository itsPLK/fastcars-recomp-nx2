# Recomp → Switch 2 (kaiHEN) port plan

> **Historical** (the bring-up, 2026-11-13 to -15). The code still points here for design notes (guest fold, memory, display path). Paths changed in the 2026-11-19 cleanup: the patches are in `patches/`, and the bring-up projects (`switch2/probe`, `sdk-smoke`, `toolchain-test`, `display-test`, `alias_audit.sh`, `fold_bench.sh`) and the desktop launcher (`loader/`) are in the tag `pre-cleanup-2026-11-19`. Current state: `HANDOFF_SWITCH2_NEXT.md`.

> Resuming? Read [`HANDOFF_SWITCH2.md`](HANDOFF_SWITCH2.md) first (state, rebuild steps, next steps).

Base: the upstream recompilation by andrewnakas (ReXGlue SDK v0.10.0 + patches; native Vulkan renderer).
Target: Switch 2 homebrew NRO under kaiHEN, built with ouncebrew/ounce-devkit
(devkitA64 + libounce [libnx fork] + OunceVK [Mesa NVK, Vulkan 1.4, static libvulkan.a]).

Clean-room rules from upstream apply unchanged: no game code, generated C++, compiled game
library, shaders or disc data is ever committed or distributed. The player builds the NRO on
their own machine from their own discs + TU27 (same model as "Build for iPhone/Android").

## Why it is feasible

- The Android/iOS ports already did most of the hard porting: arm64 codegen, no on-device compiler
  (game library + SPIR-V built on the PC), fibers without ucontext, touch/pad input, memory
  budgets, tile-GPU tuning, UBO constants.
- Switch 2 CPU is Cortex-A78C (ARMv8.2+, LSE): same codegen as the Android build
  (`-mcpu=cortex-a78c` from libounce's switch2_cpu.mk).
- GPU is NVIDIA Ampere via NVK: Vulkan 1.4, shaderInt64, geometry shaders, linear float filtering,
  real vkWaitForFences. Most Adreno workarounds (lower_int64.py, CPU rect lists, SNORM substitutes)
  should be unnecessary. Desktop SPIR-V path (Linux/Windows) is the right starting point.
- 12 GB RAM; game footprint on iPhone was ~1.5-1.7 GB.

## Hard problems (ranked by risk)

1. **Guest memory aliasing.** The SDK (Xenia-style) maps one shared-memory object at several
   virtual addresses (physical mirrors 0xA0000000/0xC0000000/0xE0000000 etc.) with
   memfd/shm + mmap(MAP_FIXED). Horizon has no mmap. Candidates to probe on hardware:
   a. `svcCreateSharedMemory` + `svcMapSharedMemory` the same object at several addresses in our own
      process (needs the svc in the game's policy, and the kernel to allow multiple maps).
   b. `svcCreateTransferMemory` / code memory tricks.
   c. RO-assisted aliasing: kaiHEN's RO already holds the code-mapping svcs and the game's process
      handle (that's how the JIT "persistent alias" works). A small RO/plugin command that maps the
      same physical pages at N addresses in the caller would solve it generically.
   d. Fallback: no aliasing; patch the codegen's address translation so physical-mirror ranges fold
      onto one base (costly, touches every load/store helper).
   → **First milestone is a probe NRO that answers this.**
2. **No signals / no resumable faults.** libounce's exception handler ends in svcBreak (not
   resumable). SDK uses SIGSEGV for MMIO/watch callbacks and fault recovery. Need: either a
   resumable exception return path (custom exception entry that restores the context from the
   dump), or disable everything that relies on faults (write watch is already off on Android/iOS;
   check MMIO paths the native renderer avoids under gpu_skip_rendering).
3. **Monolithic static link.** No dlopen: game code + runtime + renderer + OunceVK + SDL2 linked
   into one ELF → NRO (several hundred MB). Check NRO loader size limits / hbloader heap; RO maps NROs.
   FASTCARS_GAME_DLL split is dropped; codegen output compiled straight into the NRO.
4. **Compiler.** Generated code + fs/fastcars_pch.h assume clang (bundled LLVM 19). devkitA64 is GCC.
   Plan: clang `--target=aarch64-none-elf` with devkitA64's newlib/libounce sysroot and
   `-mtp=soft -fPIE`, linked with devkitA64 ld (or lld + switch.specs equivalents). Keep
   fastcars_register.cpp at -O0.
5. **SDK platform layer (REX_PLATFORM_SWITCH2).** threading (newlib pthreads + svc affinity/priority,
   8 MB stacks), timers/chrono, filesystem (sdmc:/ paths, case-insensitive like Xbox),
   fibers (Android's non-ucontext path), audio (SDL2 Switch audio or audren directly),
   input (SDL2 Switch / libnx HID → XInput mapping), window (nwindowGetDefault +
   VK_NN_vi_surface instead of SDL3 window; SDL3 has no Switch backend in-tree — use SDL2
   portlib or bypass SDL entirely for video).
6. **Renderer/plume on NVK.** Plume's Vulkan backend with a VK_NN_vi_surface swapchain, no loader
   (static vk* + vkGetInstanceProcAddr for volk). Emulated SDK Vulkan renderer must stay
   minimal/skipped (gpu_skip_rendering + small shared memory patch).
7. **Performance.** Handheld GPU clocks are low; start at the phone settings (render scale 0.75,
   30 fps cap, UBO constants) and tune. CPU recording thread was the phone bottleneck.

## Findings from existing kaiHEN homebrew (hblist survey, 2026-11-14)

Sources: xStarbound-Switch2 (large C++ engine, Vulkan/OunceVK, threads, networking), retroarch-s2
(Vulkan, 3 JIT cores incl. Flycast), sonic3air-s2 / sm64ex (SDL2 + GLES), moonlight-nx2 (Rust,
HW video decode), akira-ounce (crash reporter), DKC2Recomp-NX (SNES static recomp).

Confirmed on hardware by others:
- **Resources:** ~9,152 MiB available and 6 CPU cores in *full-memory application mode* (start a
  real game holding R; applet mode is not enough). Process core mask from svcGetInfo(CoreMask).
- **Vulkan:** "Switch 2 GPU (NVK model 0x808)", reported API 1.3.0 on SDK 20261113. 1080p
  swapchains work. **One VI surface per process** (RetroArch keeps it across driver restarts).
  No validation layers on console; stderr only shows NVK's non-conformance notice.
  Desktop GL via Zink hard-crashes (irrelevant: we use Vulkan).
- **No usable write/page faults.** Flycast: "Horizon gives no write faults" – SMC and VRAM
  tracking done in software; mem_region_lock() is a no-op. libnx exception handlers are used only
  for crash reports (akira), never resumed. → Every fault-based path in the SDK must be off
  (write watch already off on phones; check MMIO/fault recovery in the SDK).
- **No aliased memory mappings in any homebrew.** libounce has no virtmemReserve (Flycast shims it
  with virtmemFindAslr); Flycast folds its RAM mirrors in software (RAM_MASK), not with views.
- **`mrs cntvct_el0` faults.** Use clock_gettime(CLOCK_MONOTONIC) / armGetSystemTick; tick is
  31.25 MHz. Audit the SDK (mftb, chrono, profilers, tracy) for direct counter reads.
- **JIT works** via kaiHEN RO (publish 64-byte lines; only the publishing thread's I-cache is
  invalidated). Not needed for static recomp, but available.
- **LLVM objects link fine:** moonlight-nx2 builds its runtime with Rust/LLVM and links with
  devkitA64 gcc. Supports the plan to compile generated code with clang.
- **Filesystem quirks (xStarbound):** no P_tmpdir (use sdmc: paths), rename() fails with EEXIST
  if the target exists, newlib positional I/O was emulated with seek+lock (serializes reads;
  use fsFileRead with offset directly for the SDK's pread paths).
- **Default homebrew pthread stack is small**: set stack sizes explicitly (Android wrapper gives 8 MB).
- **Audio:** SDL2 audio works (sonic3air, xStarbound); RetroArch also has an audren driver.
- Largest single NRO seen is tens of MB (RetroArch ships one NRO per core); a several-hundred-MB
  NRO is still untested → keep in the M1 probe.

Consequence for risk #1 (aliasing): first test **on desktop** whether the game's CPU code ever reaches
the same physical page through two windows (0xA/0xC/0xE physical, 0x8/0x9 image). If each
physical allocation is only touched through the window it was allocated in, back the windows
separately (no aliasing) and let host code (kernel, renderer: GetPhysicalAddress paths) translate
explicitly. Verify by running the Mac/Linux build with separate, non-aliased backing and diffing
behaviour. Fallback: a per-access LUT (`lut[ea>>28] + (ea & mask)`) in the codegen's
load/store helpers. Sparse commit of the guest space via svcMapPhysicalMemory is still to probe.

## Workspace (set up 2026-11-14)

- This repo = the upstream recompilation (andrewnakas), branch `switch2`. Pinned trees beside it (gitignored), patches
  applied: `rexglue-sdk/` @ f5337cd + rexglue-sdk-fastcars-linux.patch, `thirdparty/plume` @ 0ce184b +
  plume-fastcars.patch, `thirdparty/XenosRecomp` @ 990d03b + xenosrecomp-fastcars.patch.
- Docker: `ounce-devkit` (from ouncebrew/ounce-devkit, devkit 20261114: libounce 9382b1e, OunceVK
  f5afcd2, kaiHEN eb81e19; devkitA64 GCC 15.2, native arm64 on Apple silicon) and `fastcars-switch2`
  (`switch2/docker/Dockerfile`: + clang/lld 19.1.7, cmake, ninja, python).
- `switch2/probe/`: the hardware probe (README there). Builds clean; the 429 MB variant builds too.

Found while building: **libounce's newlib has no `pread()`** (link error) → positional reads via
`fsFileRead` (takes an offset; probe measures it from 4 threads on one handle).

## SDK platform layer: work list (REX_PLATFORM_SWITCH2)

The SDK's OS code is 14 `src/core/*_posix.cpp` files (~3,800 lines) plus a few in ui/, system/,
kernel/. libounce gives newlib + pthreads + clock_gettime, so most POSIX files build; the
replacements are:

| file | uses | Switch 2 replacement |
|---|---|---|
| memory_posix.cpp | mmap/mprotect/shm_open/memfd, /proc | virtmem reservation + svcMapPhysicalMemory or heap; no protection changes (no faults); views per aliasing result |
| mapped_memory_posix.cpp | mmap of files | read file into memory (or fsFileRead on demand) |
| exception_handler_posix.cpp, seh_posix.cpp | sigaction, ucontext | __libnx_exception_handler: crash report only, or guarded longjmp recovery (probe tests it) |
| threading_posix.cpp | realtime signals for suspend / user callback (APC) / terminate, timer_create, sched_setaffinity | svcSetThreadActivity (pause/resume), APC queue drained at alertable waits, own timer thread, svcSetThreadCoreMask, 8 MB stacks |
| fiber_posix.cpp | ucontext | the Android patch's non-ucontext fibers |
| filesystem_posix.cpp | pread, /proc/self/exe | fsFileRead with offsets; fixed sdmc:/ root |
| dynlib_posix.cpp | dlopen | stub (everything is linked statically) |
| dbg_posix.cpp | /proc, SIGTRAP | stub / svcBreak |
| clock_posix.cpp, atomic, socket, system | - | probably build as is (check any direct counter reads: cntvct faults) |
| ui (window_sdl, vulkan_instance, surface) | SDL3 window, Vulkan loader | VK_NN_vi_surface on nwindowGetDefault, static vk* (no loader), one surface shared with plume |
| audio (sdl_audio_driver), input (sdl_input_driver) | SDL3 | SDL2 portlib or audren / HID directly |

## Progress log

**2026-11-14 – M2 started: rexcore compiles for Switch 2.**
- `switch2/cmake/switch2-clang.cmake`: clang 19 for aarch64-none-elf over devkitA64's newlib +
  libstdc++ 15 and libounce; links with devkitA64 g++ + switch.specs; `-D_GNU_SOURCE` (newlib
  hides POSIX in strict C++23 otherwise). Smoke test `switch2/toolchain-test` (C++23, TLS,
  std::thread, std::format) builds to an NRO.
- `switch2/support` (link into every Switch 2 executable): thread pointer copied into tpidr_el0
  per thread (clang TLS; GCC uses __aarch64_read_tp → *(tpidrro_el0+0x1F8), same layout) via
  `--wrap=__libnx_init_thread` / `--wrap=threadCreate`; pread/pwrite fallback.
- SDK (`rexglue-sdk-fastcars-switch2.patch` on top of the linux patch; `glslang-fastcars-switch2.patch` for
  the glslang submodule): REX_PLATFORM_SWITCH2 / platform `switch2-arm64`; no SDL3, Tracy, codegen
  tool or install rules; POSIX files with Switch 2 branches; new `memory_switch2.cpp` (virtmem
  reservations, svcMapPhysicalMemory commits, no-op Protect, alias groups via shared memory —
  pending the probe), `mapped_memory_switch2.cpp` (read into RAM), `dynlib_switch2.cpp` (built-in
  library registry, Vulkan via static vkGetInstanceProcAddr), `exception_handler_switch2.cpp` +
  `exception_resume_switch2.S` + `seh_switch2.cpp` (__libnx_exception_handler → registered
  handlers → SEH throw trampoline → crash report; resume loses x16 only), threading on Horizon
  (svcSetThreadActivity suspend, svcSetThreadCoreMask, svcSetThreadPriority, no signals; APCs
  drained at alertable waits), native FsFile handles with positional reads.
- Next: `Memory::MapViewsSwitch2` in xmemory.cpp (alias groups), then rexfilesystem / rexruntime /
  kernel, then native window (VK_NN_vi_surface), HID input and audren audio backends, FFmpeg.

**2026-11-14 – macOS reference install** in `../fastcars-mac/home` (outside the repo: game-derived).
Discs + TU27 verified; codegen matched the reference build (811 files).

**2026-11-14 – Alias audit result: the physical windows must alias.**
`REX_ALIAS_AUDIT=1` (desktop diagnostic in the SDK patch: `alias_audit.cpp`, hooks in
memory_posix Protect/commit, xmemory's access-violation callback, file reads; logs the faulting
module and thread) on the macOS install, one short play session plus headless runs:
- ~20,000 16 KB pages first-touched (C ~13.6k, A ~0.9k, E ~0.85k, 7F 0) and ~455 pages reached
  through a second window, all by **recompiled game code** (`fastcars_game.dylib`), on guest threads:
  reads through 0xC0000000 (16 MB pages) of memory written through 0xA0000000 (64 KB pages) and
  0xE0000000 (4 KB pages). One function accounts for most (game+0xc391f4).
- So "separate memory per window" is out. Options, in order:
  1. Real aliasing on Horizon (probe: shared memory mapped twice; svcMapProcessMemory).
  2. A kaiHEN plugin (sysmodule rights) creates the shared memory and passes the handle, if the
     game policy lacks svcCreateSharedMemory (memory pool limits to check).
  3. Codegen folding: generated loads/stores map 0xA/0xC/0xE onto one physical block
     (`lut[ea >> 29] + (ea & mask)`); always works, costs a few instructions per access.
- The first play session glitched and crashed (guest null read at 0x1C after pressing A in a
  menu); that audit build opened watched pages read-write and so hid texture writes from the
  runtime. Fixed (requested protections are applied on first touch); later runs were clean.
  Whether the crash also happens without the audit is not checked yet.

**2026-11-14 – Probe results on hardware (HOS 20.0.0, kaiHEN eb81e19, full-memory app mode).**
Four probe runs (logs on the SD card as `sdmc:/fastcars-probe/probe-run*.log`, not in git). Fixes
made along the way: privileged SVCs are gated with `envIsSyscallHinted` (an SVC outside the mask
kills the process without reaching any handler: run 1 died on `svcGetSystemInfo`), the console is
closed around the Vulkan step, stderr goes to `sdmc:/fastcars-probe/stderr.log`, and the
exceptions step runs last.
- **Syscall mask** (same as the game's policy): 0x01–0x29, 0x2C/0x2D, 0x32–0x36, 0x80. Not
  permitted: 0x50 CreateSharedMemory, 0x51/0x52 Map/UnmapTransferMemory, 0x6F GetSystemInfo,
  0x73–0x78 (process memory, process code memory). Permitted but unhelpful: 0x13 MapSharedMemory
  (no way to create the object), 0x15 CreateTransferMemory (no way to map it).
- **No aliasing.** shared/transfer/process/code memory: not permitted. `svcMapMemory` works but
  moves the pages (source inaccessible while mapped), as on Switch 1. kaiHEN's JIT API is no help
  either: on Switch 2 its RW view is a staging buffer published by copy. Plugins run under the
  same reused signed game policy, so presumably the same mask (not verified on hardware).
  → **Codegen folding (option 3 of the alias audit) is the memory design.**
- **No on-demand commit.** `svcMapPhysicalMemory` → 0xfa01 (InvalidState) in the ASLR span and
  in the alias region: the process has no system resource (`SystemResourceSizeTotal 0`). Guest
  memory therefore comes from the heap: one malloc of **8704 MB** works (total 9132 MB, all of
  it already counted as used by the heap at start), 5952 MB in 64 MB blocks.
- **No CPU exceptions.** `DebuggerAttached 0`, `__nx_exception_ignoredebug = 1`, hbloader forwards
  to the NRO, yet a guarded null read kills the process before `__libnx_exception_handler` runs
  (no "handler entered" line). Everything fault-based must be off: write watches, MMIO through
  faults, SEH-by-exception, guard pages. `svcBreak`-style crash reporting only.
- **Timers:** cntfrq 31.25 MHz; `cntpct_el0` and `clock_gettime` both ~24 ns per read; sleeps
  accurate to µs.
- **Threads:** core mask 0x3f, pinning works on all 6 cores at equal speed, 8 MB stacks, 128+
  simultaneous threads.
- **SD:** ~80 MB/s read (stdio, fsFileRead and 4 threads on one handle alike), ~10 MB/s write;
  case-insensitive paths; rename over an existing file fails (EEXIST: delete first).
- **ldr:ro LoadNro works**: a second NRO's text/rodata/data map and run, so the game code can be a
  separate NRO loaded by a small host.
- **Vulkan** (OunceVK, needs the default NWindow free: the text console blocks it): "Switch 2 GPU
  (NVK model 0x808)", API 1.4, geometry/tessellation shaders, shaderInt64/Float64, BC1–5,
  D24S8/D32F_S8, descriptor indexing (1M sampled images), bufferDeviceAddress, dynamic
  rendering, sync2, timeline semaphores, transform feedback, extended dynamic state 1–3,
  graphics pipeline library, robustness2, custom border color, 105 extensions; one 9050 MB
  heap (types: device-local, host-visible coherent, host-visible cached);
  `vkCreateDevice` with every reported feature succeeds. RGBA8_SRGB has no storage-image use.
- **Large NRO loads**: the 429 MB big400 variant starts from hbmenu, its 400 MB of rodata is all
  mapped and readable; every other step matches the small build. The NRO comes out of the heap
  (largest malloc 8448 MB instead of 8704 MB). So a monolithic NRO (runtime + game code) works;
  the separate ldr:ro module stays an option, not a requirement.

**2026-11-14 – Guest fold: design, desktop check, SDK side.**
- Generated code reaches guest memory only through the prefix-header macros (≈1.8 M sites, no
  raw `base +` anywhere), all via `REX_PHYS_HOST_OFFSET`. Under `REX_GUEST_FOLD` that becomes
  `fold[addr >> 24]` (256 × u64, constexpr): physical memory lives once at base + 0xA0000000
  (the A window is identity), C → −0x20000000, E → −0x40000000 + 0x1000, 0x7F → +0x21000000,
  0x9 → −0x10000000 (onto 0x8). Host block = 0xC0001000 bytes (3 GB) instead of 4.5 GB.
  Checked page by page against the SDK's `map_info` (0 mismatches).
- Desktop check (macOS install, codegen re-run into `../fastcars-mac/build`, game dylibs built with
  `tools/game_dll/build_game_dll.py -D REX_GUEST_FOLD` into `../fastcars-mac/fold-exp`, runner
  `switch2/tools/fold_bench.sh`): every view still exists there, so the fold is a pure
  translation change. It runs (headless, 150 s × 2 alternating rounds), with **no measurable
  cost**: CPU 7:44 / 7:46 (base) vs 7:52 / 7:43 (fold), same frames presented and record times.
  The folded `__text` is **10% smaller** (100.5 vs 111.2 MB): the macOS release already pays a
  compare + select per access for the E shift; the fold is shift + L1 table load + add.
  Workload caveat: headless intro/menu near the frame pacer, not gameplay.
- SDK (Switch 2): `memory_switch2.cpp` rewritten for the heap-only model (one 64 KB-aligned arena,
  zeroed once; commit/decommit inside it are no-ops, matching desktop where BaseHeap never
  returns pages to the host; other blocks from aligned_alloc; Protect no-op; no file mappings,
  no alias groups). `Memory::Initialize` takes the arena; the C/E/0x9 heaps get a shifted
  `membase` so `TranslateRelative` lands on the one copy while their host page arithmetic is
  unchanged; `TranslateVirtual` and `GuestPtr` use `detail::GuestFoldOffset` (same table as the
  header). Raw `base + addr` in `ppc/stack.h` (KPCR, stack pushes) and `xfile.h` now use
  `GuestPtr` (also fixes the missing E shift there on Windows/macOS).
- Known gaps: `HostToGuestVirtual` returns A-window addresses for any physical pointer (the
  original window is not recoverable); plain (non-MM) guest accesses to GPU registers in 0x7F
  read physical memory instead of faulting into the MMIO handler (codegen's MM macros cover the
  accesses it identified); `xmemory.cpp`'s Switch branch is not compiled yet (rexruntime is not
  configured for Switch 2).

**2026-11-14 – Whole SDK builds for Switch 2; first SDK code on hardware passes.**
- Static `rexcore`, `rexfilesystem`, `rexruntime` (system + kernel), `rexui`, `rexinput`, `rexaudio`,
  `rexgpu-xenos`, codegen, glslang, FFmpeg (libavutil/libavcodec via an include-path `config.h`
  for Horizon). Fixes: timegm, socket headers without netinet/ip.h, SDL input compiled out,
  renderdoc/sha256 platform checks, SPDLOG_NO_TZ_OFFSET, vulkan.hpp DynamicLoader off, weak
  symbols for the title's `rex_skip_rendering_*` hooks, no-op thread naming. Physical-memory
  access callbacks are refused on Switch 2 (they need faults); the renderer's write watch is off
  there (hashing instead).
- `switch2/sdk-smoke` (1.4 MB NRO, static rexruntime) on hardware: **all 20 checks ok**.
  `Memory::Initialize` builds and zeroes the 3 GB arena in 190 ms; E/A/C/0x7F/raw and 0x8/0x9
  reach the same bytes through TranslateVirtual, GuestPtr and TranslatePhysical; E is physical
  + 0x1000; guest heaps and SystemHeapAlloc (virtual and physical) work; spdlog writes to the SD
  card. (Its "largest malloc" lines do not touch memory, so they only show address space; the
  probe's touched figure, 8.4 GB before the arena, is the real budget.)

**2026-11-14 – Display path works on hardware.** `switch2/display-test` (11.5 MB NRO): SDK app
entry and UI loop (`windowed_app_main_switch2`, `Switch2WindowedAppContext` on appletMainLoop), an
SDK window without a surface, plume through volk on the localized static OunceVK
(`rex_switch2_vulkan`): "Switch 2 GPU (NVK model 0x808)", swapchain 1280x720 × 3 on
VK_NN_vi_surface, first frame 60 ms after start, **600 frames in 10.02 s (59.9 fps, vsync)**, clean
exit through the close path. Link fixes found on the way: volk's vk* variables vs OunceVK's vk*
functions (localized copy of libouncevk.a), two VMA versions (SDK's renamed rexsdk_*), fibers (asm
path), a duplicate memory helper.

**2026-11-14 – The game NRO: builds, boots deep into startup, then dies (open).**
- `switch2/game`: host (`fastcars/src` in its FASTCARS_GAME_DLL form, Switch 2 branches: NWindow window,
  no SDL, no child processes for the shader compiler, `SafeReadMemory` via `svcQueryMemory`,
  `GuestHost` for guest pointers), `fastcars_hooks.h` from the host objects (`gen_interface.py
  --switch2`), the owner's codegen output compiled freestanding with `fastcars_pch.h` + `REX_GUEST_FOLD`,
  one static link (~123 MB NRO). SDK additions for it: native HID input, Nop audio, statically linked
  GPU plugin, headless SDK GPU, system-counter clock, core count from the core mask, VMA renamed.
- Game data copied to `sdmc:/fastcars/data` over FTP (~15 GB at 7–8 MB/s; `switch2/tools/ftp_push_data.sh`).
- Seven boots (table in the handoff): fixed in turn the plugin load, `vkDestroyInstance`, the
  presentation surface, the clock (libounce has no `CLOCK_MONOTONIC_RAW`: the guest clock was
  uninitialized memory, so the game hung at its first timed wait), and host `base + address`
  pointers. Boot 7 (trace log) reaches the game's D3D init (`SetInterruptCallback`) right after the
  main thread resumes five suspended threads, then the process dies with nothing logged. Faults never
  reach homebrew under kaiHEN (presumably user exception handlers are off in the retail kernel), so
  the next step is a sampling crash locator (`svcSetThreadActivity` + `svcGetThreadContext3`, both in
  the policy); design in the handoff §0.

## Milestones

- **M0 – Toolchain.** ounce-devkit installed (Docker image on macOS); build examples/vulkan-clear
  and run it under kaiHEN. Build a clang-for-aarch64-none-elf hello NRO linked against libounce.
- **M1 – Probes NRO** (no game code): memory aliasing options (1a/1b/1c), max NRO size / heap,
  exception resume, thread count/stack/affinity limits, VK device features/extensions dump
  (shaderInt64, geometryShader, descriptor indexing, bufferDeviceAddress, dynamic rendering).
- **M2 – SDK port.** REX_PLATFORM_SWITCH2 in rexglue-sdk (new patch file), runtime builds as a
  static lib for aarch64-none-elf.
- **M3 – Boot to logs.** Game linked, data loaded from sdmc:/fastcars/{game,update,userdata},
  renderer off; reaches the main loop with audio.
- **M4 – Native renderer.** plume + OunceVK swapchain, SPIR-V from the PC's shader_cache
  (desktop path, not Adreno-lowered).
- **M5 – Packaging.** Launcher option "Build for Switch 2" in loader/install.cpp, producing
  `switch2/fastcars.nro` + `spirv/` on the player's PC; player copies discs/TU/data to SD.
- **M6 – Perf** (docked vs handheld presets).

## Hardware/test requirements

- Switch 2 with kaiHEN, SD card with ~20 GB free.
- Own Xbox 360 Disc 1 + Disc 2 + TU27 of the game (for M3+ only).
- nxlink/FTP (OunceShell in the kaiHEN release has an FTP server) for fast iteration.
