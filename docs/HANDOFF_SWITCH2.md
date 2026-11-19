# Switch 2 port: handoff (state on 2026-11-14, evening)

> **Historical** (the bring-up, 2026-11-13 to -15). The code still points here for design notes (guest fold, memory, display path). Paths changed in the 2026-11-19 cleanup: the patches are in `patches/`, and the bring-up projects (`switch2/probe`, `sdk-smoke`, `toolchain-test`, `display-test`, `alias_audit.sh`, `fold_bench.sh`) and the desktop launcher (`loader/`) are in the tag `pre-cleanup-2026-11-19`. Current state: `HANDOFF_SWITCH2_NEXT.md`.

Start here when resuming the Switch 2 (kaiHEN) port. The long-form plan, findings and history are in
[`SWITCH2_PORT_PLAN.md`](SWITCH2_PORT_PLAN.md) (progress log at the end); this file is "where are
we, how do I rebuild and deploy, what's next".

Same clean-room rules as upstream ([`RELEASE_CLEANROOM.md`](RELEASE_CLEANROOM.md)): nothing made
from the discs (generated C++, compiled game code, the game NRO, shaders, game files, logs with game
memory) ever goes into git or a release. The player builds their own NRO from their own discs.

## 0. The one open problem (pick up here)

**The game NRO boots deep into startup on hardware and then the process dies, with no message.**
Boot 7 (trace logging, log saved on the SD card as `sdmc:/fastcars/logs/fastcars.log`, earlier ones as
`boot1.log` … `boot6.log`) gets through: 3 GB guest arena, HID, XMA/audio threads, VFS mounts of
`sdmc:/fastcars/data`, the SDK's headless Vulkan device, XEX load + TU27 patch (0.0.27.9), 99,900
recompiled functions, module launch, the game's main thread (thread creation, XAM queries, mounting
the four Disc 2 content packages, `update.rpf`). Then the main thread resumes five threads it created
suspended (`NtResumeThread` ×5 in 6 ms); one of them logs `SetInterruptCallback(8380CC30, 4004D500)`
(the game's D3D init) and **the process dies within milliseconds**: no further kernel call is logged,
even at trace level. On macOS the same thread logs `[native-probe] vs obj …` (host
`fastcars/src/native/shaders.cpp`, first shader creations) 2 ms later, and the native renderer starts
~2.6 s later; neither happens on Switch.

**Why there is no crash report:** CPU faults never reach a homebrew process under kaiHEN (hardware
probe: a guarded null read kills the probe before `__libnx_exception_handler` runs, with no debugger
attached and `__nx_exception_ignoredebug = 1`). Most likely the retail kernel has user exception
handlers disabled system-wide (Atmosphère turns that on for Switch 1 homebrew; kaiHEN does not patch
the kernel). So any wild pointer, null call or `abort()` kills the process silently, and the SD
card shows only `sdmc:/kaihen/nro-cleanup.txt` (hbloader's record of hbmenu, not of the game).

**Already ruled out / fixed on the way** (each was a real bug; see §3 for the boot history):
GPU plugin dlopen, `vkDestroyInstance` from a null instance, SDK presenter needing a surface,
`CLOCK_MONOTONIC_RAW` (libounce has none: the guest clock was garbage), core count 0, host code
forming guest pointers as `base + address` (now `fastcars::GuestHost`, `fastcars/src/guest_ptr.h`). Guest
memory itself cannot fault (the whole 3 GB arena is committed and every guest address folds into it).

**Next step: a crash locator ("flight recorder"), not yet written.** Plan:
1. In the SDK (`rexglue-sdk/src/core/threading_posix.cpp`, Switch 2 branch), keep a registry of every
   thread's Horizon handle + name: register in `CaptureHorizonThread()` (called from
   `ThreadStartRoutine` and for the main thread), name from `set_name` (Switch 2 stores `name_`),
   unregister when the thread finishes.
2. A sampler thread, on only when a cvar (e.g. `switch2_sampler_ms`, 0 = off) is set: every N ms
   (2–10) for each registered thread except itself: `svcSetThreadActivity(h, ThreadActivity_Paused)`,
   `svcGetThreadContext3(&ctx, h)` (both SVCs are in the policy: 0x32, 0x33), resume; record pc, lr,
   sp and a short frame-pointer walk (bounded to the thread's stack; check with `svcQueryMemory`).
3. Write each round to `logs/sampler.txt` (fopen once, write, `fflush` + `fsync`): the last round
   before the process dies shows where every thread was. Log the runtime address of one anchor
   function once, so `pc - (anchor_runtime - anchor_elf)` can be fed to
   `llvm-symbolizer --obj=switch2/game/build/fastcars_switch2` (the unstripped ELF next to the NRO).
   A host pc in `sub_XXXXXXXX` names the guest function; in SDK/host code it names the culprit.
4. Consider `-fno-omit-frame-pointer` for the SDK, host and game code in the Switch build so the walk
   works (needs a full rebuild, ~5 min).
5. Cheap extra: `--wrap=abort` / a `std::set_terminate` handler / `--wrap=__cxa_throw` in
   `switch2/support` that log (and `fsync`) before dying, in case it is an exception or an abort.

Suspects once the location is known (none confirmed): something in the five newly resumed guest
threads' first host calls (hooks in `fastcars/src/native/d3d_hooks.cpp`, `shaders.cpp` `OnCreate`,
`capture.cpp`, `backend.cpp`); the SDK's Vulkan command processor reacting to the first PM4 (NVK, a
second VkDevice next to plume's later one); a C++ exception or `abort()` (libstdc++ unwinding with
clang-built code is untested on hardware); stack use in fibers (`fiber_posix.cpp` asm path, heap
stacks).

## 1. Where things are

| Path | What | In git? |
|---|---|---|
| this repo, branch `switch2` | the upstream recompilation + the port | yes |
| `docs/SWITCH2_PORT_PLAN.md` | plan, findings, progress log | yes |
| `switch2/docker/Dockerfile` | `fastcars-switch2` image: ounce-devkit + clang/lld 19 | yes |
| `switch2/cmake/switch2-clang.cmake` | CMake toolchain (clang for aarch64-none-elf over devkitA64 + libounce) | yes |
| `switch2/support/` | linked into every Switch 2 exe: tpidr_el0 thread pointer (clang TLS), pread/pwrite; `rex_switch2_vulkan` (localized OunceVK, see §5); `include-stubs/dlfcn.h` for volk | yes |
| `switch2/toolchain-test/`, `switch2/probe/` | toolchain smoke test, hardware probe NRO | yes |
| `switch2/sdk-smoke/`, `switch2/display-test/` | hardware tests: SDK guest memory; SDK app + plume swapchain (both pass) | yes |
| `switch2/game/` | **the game NRO** project (`CMakeLists.txt`, `game_code/`, `native_shaders.py`, `args.txt` template) | yes |
| `switch2/tools/ftp_push_data.sh` | game data → `sdmc:/fastcars/data` over FTP (resumable, FAT32 check) | yes |
| `switch2/tools/alias_audit.sh`, `fold_bench.sh` | macOS diagnostics (alias audit, fold cost) | yes |
| `rexglue-sdk-fastcars-switch2.patch` | all our SDK changes, applied **after** `rexglue-sdk-fastcars-linux.patch` | yes |
| `glslang-fastcars-switch2.patch` | one-line change in the SDK's glslang submodule | yes |
| `plume-fastcars-switch2.patch` | plume on Switch 2, applied **after** `plume-fastcars.patch` (local plume branch `fastcars-switch2`: `de0df4e` = baseline with the fastcars patch) | yes |
| `gamefiles/iso`, `gamefiles/tu` | the owner's Disc 1/2 ISOs and TU27 | **ignored** |
| `rexglue-sdk/`, `thirdparty/plume`, `thirdparty/XenosRecomp` | pinned upstream trees, patched (see §2) | ignored |
| `../fastcars-mac/release/` | unpacked v0.3.0 macOS release (FASTCARS.app; its `rexglue` codegen tool) | outside repo |
| `../fastcars-mac/home/` | installed macOS game folder (`data/` = what goes on the SD card) | outside repo |
| `../fastcars-mac/build/generated/default` | codegen output for the owner's discs (the Switch build compiles it) | outside repo |
| `../fastcars-mac/fold-exp`, `../fastcars-mac/audit-lib` | fold experiment builds, alias audit runtime | outside repo |

The local `rexglue-sdk` checkout has branch `fastcars-switch2`: `fa5d01e0` = v0.10.0 + linux patch
(baseline), then the Switch 2 commits. The SDK patch is the diff from `fa5d01e0` excluding only the
submodules (plain vendored dirs such as `thirdparty/renderdoc`, `crypto`, `ffmpeg-overlay` are part
of it). **After any SDK or plume change, commit in the checkout, re-export, commit the patch here:**

```sh
cd rexglue-sdk && SUB=$(git config -f .gitmodules --get-regexp path | awk '{printf ":!%s ", $2}')
git diff fa5d01e0 HEAD -- . $SUB > ../rexglue-sdk-fastcars-switch2.patch
git -C thirdparty/glslang diff > ../glslang-fastcars-switch2.patch
cd ../thirdparty/plume && git diff de0df4e > ../../plume-fastcars-switch2.patch
```

## 2. Rebuilding the environment from nothing

```sh
# Upstream trees at the pins, then the patches (upstream instructions + ours)
git clone https://github.com/rexglue/rexglue-sdk rexglue-sdk && git -C rexglue-sdk checkout f5337cd
git -C rexglue-sdk submodule update --init --recursive
git -C rexglue-sdk apply ../rexglue-sdk-fastcars-linux.patch
git -C rexglue-sdk apply ../rexglue-sdk-fastcars-switch2.patch
git -C rexglue-sdk/thirdparty/glslang apply ../../../glslang-fastcars-switch2.patch
mkdir -p thirdparty
git clone --recursive https://github.com/renderbag/plume thirdparty/plume && git -C thirdparty/plume checkout 0ce184b
git -C thirdparty/plume submodule update --init --recursive && git -C thirdparty/plume apply ../../plume-fastcars.patch
git -C thirdparty/plume apply ../../plume-fastcars-switch2.patch   # Switch 2: NWindow / VK_NN_vi_surface / static OunceVK
git clone --recursive https://github.com/hedge-dev/XenosRecomp thirdparty/XenosRecomp && git -C thirdparty/XenosRecomp checkout 990d03b
git -C thirdparty/XenosRecomp submodule update --init --recursive && git -C thirdparty/XenosRecomp apply ../../xenosrecomp-fastcars.patch

# Docker images (native arm64 on Apple silicon)
git clone https://github.com/ouncebrew/ounce-devkit /tmp/ounce-devkit
docker build -t ounce-devkit /tmp/ounce-devkit          # devkit 20261114: libounce 9382b1e, OunceVK f5afcd2, kaiHEN eb81e19
docker build -t fastcars-switch2 switch2/docker              # + clang/lld 19.1.7, cmake, ninja, python3
```

### Codegen (once per set of discs; game-derived, outside the repo)

```sh
R=../fastcars-mac/release/fastcars-recomp-macos-arm64/FASTCARS.app/Contents/Resources/fastcars; B=../fastcars-mac/build
mkdir -p $B/generated/default $B/assets && cp $R/tools/game_dll/fastcars_manifest.toml $B/ &&
cp $R/tools/game_dll/codegen.partition.json $B/generated/default/ &&
cp ../fastcars-mac/home/data/game/default.xex ../fastcars-mac/home/data/game/default.xexp $B/assets/ &&
(cd $B && $R/rexglue --log-level info codegen fastcars_manifest.toml)     # ~85 s, 818 files
```

### Build commands

```sh
# The game NRO (host + game code + SDK + plume + OunceVK): switch2/game/build/fastcars.nro (~123 MB)
python3 switch2/game/native_shaders.py switch2/game/build/native_shaders     # on the Mac (DXC), once
docker run --rm -v "$PWD:/repo" -v "$PWD/../fastcars-mac:/fastcars-mac" -w /repo fastcars-switch2 sh -c '
  cmake -S switch2/game -B switch2/game/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/repo/switch2/cmake/switch2-clang.cmake -Wno-dev &&
  cmake --build switch2/game/build -- -j10'
# (first build ~5 min; the unstripped ELF switch2/game/build/fastcars_switch2 is for symbolizing)

# Hardware tests: same pattern with -S switch2/sdk-smoke or -S switch2/display-test
# Probe + toolchain test NROs -> switch2/probe/dist
switch2/probe/build.sh            # NO_BIG=1 to skip the 429 MB variant

# SDK alone for Switch 2 (all targets): -S rexglue-sdk -B rexglue-sdk/out/build/switch2, build all
# macOS SDK runtime (desktop compile check of shared SDK files; same settings as the v0.3.0 release)
cd rexglue-sdk && R=$(cd .. && pwd) && F="-mcpu=apple-m1 -ffile-prefix-map=$R=." &&
cmake --preset mac-arm64 -B out/build/mac-audit -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  "-DCMAKE_C_FLAGS=$F" "-DCMAKE_CXX_FLAGS=$F" "-DCMAKE_OBJC_FLAGS=$F" "-DCMAKE_OBJCXX_FLAGS=$F" \
  -DREXGLUE_BUILD_TESTS=OFF -DSYSCONFDIR=/etc &&
cmake --build out/build/mac-audit --config Release --target rexruntime -j 8
```

### Deploy, run, read the log (the iteration loop)

The console runs kaiHEN; its FTP server (OunceShell) is at **`192.168.1.135:1337`** and only answers
while OunceShell is open (not while the game runs).

```sh
F=ftp://192.168.1.135:1337
curl -s "$F/" -Q "RNFR /fastcars/logs/fastcars.log" -Q "RNTO /fastcars/logs/bootN.log"   # keep the last log
curl -sS -T switch2/game/build/fastcars.nro "$F/switch/fastcars.nro"                 # only after the build succeeded
curl -sS -T switch2/game/args.txt "$F/fastcars/args.txt"                         # when args changed
# owner: hbmenu in full-memory mode (hold R while starting a game) -> the game's entry; then reopen OunceShell
curl -s -o fastcars.log "$F/fastcars/logs/fastcars.log"
```

SD layout: `sdmc:/switch/fastcars.nro`; `sdmc:/fastcars/args.txt` (the command line: the app main reads it
and makes `sdmc:/fastcars` the working directory); `sdmc:/fastcars/logs/`; `sdmc:/fastcars/data/{game,update,
user,spirv}` (~15 GB, copied once with `switch2/tools/ftp_push_data.sh 192.168.1.135:1337`; game
fixes never need it again, `spirv` only if shader translation changes; never overwrite `user/`
after the first copy: saves). `args.txt` is at `--log_level=trace` for the crash hunt; set it back
to `info` (or `debug`) once the game runs.

macOS game install (already done): `../fastcars-mac/home/data/launcher.ini` lists the Disc 1 ISO, Disc 2
ISO and TU27 paths; `FASTCARS.app/Contents/MacOS/FASTCARS --unattended --home ../fastcars-mac/home` (~10 min).

## 3. Status

**Works on hardware**
- Probe (memory, threads, timers, SD, Vulkan, ldr:ro, 429 MB NRO), toolchain test (TLS, threads).
- `switch2/sdk-smoke`: SDK guest memory (folded windows, heaps) — all 20 checks.
- `switch2/display-test`: SDK app entry/UI loop + plume swapchain on VK_NN_vi_surface, 59.9 fps.
- The game NRO up to the crash in §0.

**Built for Switch 2** (static): the whole SDK (`rexcore`, `rexfilesystem`, `rexruntime` = system +
kernel, `rexui`, `rexinput` with a native HID driver, `rexaudio` with the Nop backend, `rexgpu-xenos`
linked statically, codegen, glslang, FFmpeg via `thirdparty/ffmpeg-overlay/switch2/config.h`), plume,
the host (`fastcars/src`, FASTCARS_GAME_DLL form) and the game code.

**Boot history** (each a run of `sdmc:/switch/fastcars.nro`; logs on the SD card):

| Boot | Stopped at | Fix |
|---|---|---|
| 1 | GPU plugin `librexgpu-xenos.so` not found | plugin linked statically, called directly (`gpu_plugin_loader.cpp`) |
| 2 | "Failed to get Vulkan loader function pointers" | `vkDestroyInstance` trampoline in `dynlib_switch2.cpp` |
| 3 | "instance doesn't support surface types" | SDK GPU headless on Switch 2 (`rex_app.cpp`, `runtime.cpp`) |
| 4 | hung after module launch (black screen) | clock: no `CLOCK_MONOTONIC_RAW` in libounce → `armGetSystemTick`; core count from the core mask |
| 5, 6 | died right after `SetInterruptCallback` | host `base + address` → `fastcars::GuestHost` (real bug, not this crash) |
| 7 | same, trace log: dies right after five threads are resumed | **open**, see §0 |

**Not done**
- The crash in §0 (then: does the native renderer start, first frames).
- Audio: Nop backend (silent); a native audren backend is next after the crash.
- The native renderer's menu input and hotkeys on Switch 2 (SDL path compiled out in
  `renderer.cpp`), touch controls (stubbed: `touch_controls_switch2.cpp`).
- Shutdown from the HOME menu is slow and then the app crashes (exit path; hard exit after
  `TerminateTitle`).
- Vibration (HID driver accepts and ignores), 2+ players, docked resolution (window fixed at the
  default NWindow size, 1280x720).
- Launcher "Build for Switch 2" (`loader/src/install.cpp`): codegen + this build + SD layout.

## 4. Next steps (in order)

1. Crash locator and the crash (§0).
2. First frames from the native renderer (`[native] init` lines in the log), then performance.
3. Audio backend (audren), menu input from HID, vibration.
4. Exit path, then the launcher integration (§3 "Not done").

## 5. Gotchas learned

- **libounce/newlib gaps:** no `pread`/`pwrite` (support lib), strict C++ hides POSIX (toolchain sets
  `-D_GNU_SOURCE`), no signals, no `sys/mman.h`, `sys/syscall.h`, `sys/uio.h`, `netinet/ip.h`,
  `dlfcn.h`, `timegm`, `creat`, `pthread_setname_np`, `pthread_cancel`, `cpu_set_t`;
  `clock_gettime`/`clock_getres` only know `CLOCK_MONOTONIC` and `CLOCK_REALTIME` (others fail and
  leave the result unset — the SDK's asserts are compiled out in Release, so that went unnoticed);
  `std::thread::hardware_concurrency()` is 0; `<switch.h>` declares a global `Event` (include only
  `switch/kernel/svc.h` where that clashes, inside `extern "C"`).
- **No faults, no aliasing:** see §0 and the plan. Guest windows are folded (`REX_GUEST_FOLD`);
  host code must translate guest addresses with `TranslateVirtual`/`TranslatePhysical`/`GuestPtr`
  (`fastcars::GuestHost`), never `base + address`.
- **One static NRO:** two VMA versions (SDK 3.3.0, plume 3.2.1): the SDK's is renamed `rexsdk_*` by a
  generated force-include (root `CMakeLists.txt`). volk owns the `vk*` names, so OunceVK is linked as a
  localized copy whose only global entry is `rex_ounce_vkGetInstanceProcAddr`
  (`switch2/support/localize_ouncevk.cmake`). dlsym hooks became weak symbols; the GPU plugin is
  called directly; `gen_interface.py --switch2` writes `fastcars_hooks.h` from the host's objects.
- **One VI surface per process:** The game's renderer (plume) owns it; the SDK window has none and the
  SDK GPU is headless.
- **Build:** The game code must not see libstdc++ (`switch2/game/game_code` strips the toolchain's
  `-isystem .../c++/...` from its directory's flags); the container has no DXC (the renderer's own
  shaders are built on the Mac by `native_shaders.py`); POST_BUILD commands with spaces or
  parentheses need `VERBATIM`.
- **Tooling:** `grep -c` exits 1 when it counts 0, which breaks `&&` chains; never run the upload in
  parallel with the build that produces it; don't edit a shell script while `sh` is running it.
- CMake caches `CMAKE_*_FLAGS_INIT`: delete the build dir after toolchain flag changes.
- SDK files with Switch 2 code have `#if REX_PLATFORM_SWITCH2` branches; whole-file replacements are
  `*_switch2.cpp`. The SDK's Switch 2 exception/SEH files never fire (faults do not arrive).
- The support library must be an OBJECT library (its `--wrap` targets are referenced from libounce.a).
