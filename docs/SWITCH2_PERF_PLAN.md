# Switch 2: deeper performance work (branch `perf`, 2026-11-16)

Starting point (`docs/HANDOFF_SWITCH2_NEXT.md` section 0): ~28 fps median in city driving, the GPU
(~28 ms a frame) and the CPU side (game main thread ~25 ms, render thread ~21 ms) about balanced,
so a saving on one side alone barely showed. This branch goes after the CPU cost of the
recompiled code itself, then makes the GPU levers testable on top. Everything was checked on the
Mac (M4) as far as a Mac can check it; **nothing here has run on the console yet**.

## What changed

### 1. Registers as C++ locals (biggest change, codegen)

The generated code kept every PowerPC register in the `PPCContext` struct (`ctx.r31`, `ctx.cr6`,
...). A byte store (`u8*`) may alias anything, so clang reloaded registers from memory after every
guest store; hot loops were chains of loads and stores. ReXGlue has options to make registers
function locals; the game never enabled them. `fastcars/codegen_fast.toml` (included from the manifest)
turns on `skip_lr`, `skip_msr`, `ctr/xer/cr_as_local`, `non_argument_as_local`,
`non_volatile_as_local`.

What makes that safe for this binary:
- **Functions that don't follow the calling convention** read a register their caller left: code
  split off another function and reached by a branch, SEH funclets. `tools/codegen/register_livein.py`
  runs a dataflow over codegen output made without the options and lists the 71 functions that
  read a GPR/FPR/CR/CTR before writing it (`fastcars/codegen_share_registers.toml`, `share_registers`).
  Vector "live-ins" were all merges of garbage lanes (vrlimi128 into an unwritten register), the
  stack probe's r12 is harmless (it only touches pages), and "callee leaves a value in a volatile
  register" reads are values MSVC's whole-program optimisation keeps across a call (locals keep
  that right; marking callees by heuristic caught pow/fmod-like CRT leaves with hundreds of callers).
- **SDK codegen** (`rexglue-sdk` branch `perf`, in the patch): a `share_registers` function keeps
  every register in ctx and restores the non-volatiles it touches on every return (the elided
  `__restgprlr` would have); every call form into one (bl, tail branch, conditional tail,
  jump-table case) from a localizing function hands over all localized registers and takes them
  back. Manifest `[functions]` entries now merge across files. Default output is byte-identical.
- **Not `reserved_as_local`**: the game passes the lwarx reservation between functions
  (`sub_83829CC0` is an ldarx helper, `sub_83829CD8` the matching stdcx.); as a local the CAS never
  succeeds and threads spin forever (found on the Mac: rendering stopped ~40 s in).

### 2. Memory access classes (codegen + `fastcars/fs/fastcars_pch.h`)

`memory_classes = true` names r1-relative accesses `REX_*_STACK_*` (plain: the thread's own frame)
and loads between a label and a backward goto `REX_LOAD_LOOP_*` (always volatile: what a spin-wait
polls, stack slots included), and drops the prologue's saves of localized registers the function
has not named yet (97k stores of a value nobody reads; the epilogue reloads become dead plain loads
clang removes). Plain accesses use `may_alias` types (stfd-then-lwz float conversions, byte copies).
Compile switches (`switch2/game/game_code/CMakeLists.txt`): `FASTCARS_VOLATILE_STACK` (back to
volatile stack), `FASTCARS_PLAIN_MEMORY` (everything outside loops plain; measured: changes the code by
0.06%, **not worth testing**).

### 3. D3D probe wrappers out on Switch 2

`d3d_probe.cpp` wrapped ~2,900 D3D runtime functions (a call + a branch on every D3D call the game
makes, tens of thousands a frame, even with `--fastcars_d3d_probe` off). Left out unless the Switch
build sets `-DFASTCARS_D3D_PROBE=ON`.

### 4. FSR 1 merged (branch `fsr1`)

`--fastcars_native_render_scale=0.75` now upscales with FSR 1 (`--fastcars_native_fsr`, default on when
the scale is below 1; `--fastcars_native_fsr_sharpness`). It gave nothing while the CPU co-limited the
frame; with the CPU work above it should be the GPU lever. `args.txt` does not set it.

### 5. Builder

`switch2/builder/build.sh` codegens with `codegen_fast.toml` (`FASTCARS_FAST_CODEGEN=0` for the old
code); the codegen hash covers the new files.

## Mac results (M4, no guest fold, headless, scripted pad: `switch2/tools/mac_bench.sh`)

| game library | `__text` | prologue |
|---|---|---|
| baseline codegen | 107.4 MB | plays |
| + registers as locals | 78.9 MB (-26.5%) | plays, gets further in the same time |
| + memory classes | 74.2 MB (-31%) | plays (cutscene, mission failed, retry with HUD) |
| + `FASTCARS_PLAIN_MEMORY` | 74.17 MB | plays |

Switch NRO: 123.5 MB -> 91.6 MB. The Mac is GPU-paced (~100 fps) and its cores are far faster, so
fps does not show the gain there: total process CPU -8% at the same frame count, boot to first
gameplay frame 22.0 -> 19.9 s. The console's cores are what this is for.

Known, unrelated: the guest fold (`REX_GUEST_FOLD`, what Switch 2 uses) crashes on macOS with
today's host ~100 s in (guest read of 0xE4) in either codegen; the Mac checks run without it.

## Console results

- **build-regs** (2026-11-16, city driving, handheld, CPU 1689 / GPU 561 / EMC 2133): median
  **33.0 fps** (27.2-38.5) vs 28.1 before; 4.5 min, no hang or crash. Most windows now wait 5-12 ms
  a frame on the GPU (GPU frame 25-30 ms): GPU-bound, so FSR is the next lever. One heavy stretch
  still ~28 fps with the render thread at 24-27 ms recording and the GPU at 29-31 ms.
- **build-fast** (same drive, shorter: 11 windows): median 31.2 fps (29.0-35.2), GPU frame 28-30 ms;
  no errors. Same as build-regs within noise: both GPU-bound, so the CPU difference cannot show
  until the GPU frame drops (next: render scale 0.75).
- **build-fast + render scale 0.75** (FSR 1 on, image fine as far as the log shows; 14 windows):
  GPU frame 28-30 -> 23-27 ms (the scale covers only the 1280-wide targets), but median 30.1 fps
  (26.5-34.6): the render thread no longer waits for the GPU (wait ~1 ms) and records 19-29 ms a
  frame. The CPU side (render-thread recording + the game's main thread) is the limit again, so
  FSR buys nothing yet; left off. Next: `--switch2_sampler_ms=10` profile of build-fast.
- **Profile of build-fast** (city driving, 36 windows, median 31.3 fps; rounds 1384-6332 of
  `sampler.txt`, kept outside git in `switch2/game/build-fast/profile-1/`). Busy share of wall time:
  - game's D3D thread (XThread3F6B) 94%, but ~23% of it is a spin in `sub_832B9FE0` (calls the
    7-instruction `sub_83814160` in a loop) waiting until fewer than 3 frames are outstanding, i.e.
    waiting for frames to complete; its real work (~70%) is the draw recording (RecordDraw,
    constants, memmove).
  - game main thread 64% (22% of the time in svcWaitForAddress, 7% in lock waits);
  - native render thread 63%: recording (Backend::Execute) 65% incl, waiting for the GPU slot
    (Retire, `kFramesInFlight = 2`) ~15%, waiting for the game's next frame ~16%;
  - GPU Commands thread 69% polling (steady_clock::now 25%, mutex lock/unlock 23%): burns a core
    but 6 cores and ~3.5 busy, so not the limit.
  Reading: no stage is saturated; each waits on the next. The command processor holds guest swap
  N until the renderer has recorded frame N, so the renderer's recording time sits on the game's
  critical path. Next: `--fastcars_native_record_thread=true` (moves the Vulkan recording off the
  render thread); later candidates: 3 frames in flight, a blocking wait in the GPU Commands thread.
- **build-fast + `--fastcars_native_record_thread=true`** (19 windows): runs fine on the console (first
  time); the record thread takes 4.7-6.6 ms a frame off the render thread (record avg 18.7 ms), but
  median 29.2 fps (25.2-37.9): no gain. GPU wait for slot ~0.7 ms, the command processor almost
  never waits for the renderer (0-4 of 600 swaps), so the render thread idles ~13 ms a frame for
  the game. The game's D3D thread spends ~23% spinning on GPU completion, and completion trails
  every kick by `--gpu_skip_rendering_lag_ms` (16 on Switch 2 since `4f70050`, raised from 4 to
  stop the command processor waiting for the renderer; on Windows 4 vs 6 was +10% fps). Next: lag 8
  (record thread kept on).
- **+ lag 8** (21 windows): median 31.9 fps (23.0-35.7), vs 29.2 at lag 16; GPU frame 28-31 ms
  with 3-14 ms of GPU slot waits again, so mostly GPU-bound. The command processor now waits for
  the renderer at ~40-95% of swaps but only ~0.45 ms each. Across all runs the median sits at 29-33
  fps with the GPU frame at ~29 ms whatever changed on the CPU side: the GPU is the ceiling
  (~34 fps). Next: FSR 0.75 again, now that the render thread has the record thread's headroom.
- **+ FSR 0.75 (record thread + lag 8 + FSR)** (22 windows): median **33.1 fps** (27.1-44.9),
  but the first ~70 s at 40-45 fps were the owner standing still (GPU 21-23 ms, render thread
  recording 11-14 ms); **driving only: median ~30.6 fps** (27.1-34.7), no better than the other
  runs (whose medians may include standing too). While driving the GPU is at ~25 ms, recording
  20-25 ms and almost no GPU wait, so ~10 ms a frame idle waiting for the game: in heavy scenes the
  game's own threads (main thread, D3D thread) are the limit now. Owner: no visual issues, the
  0.75 resolution is noticeable but acceptable for now. `switch2/game/args.txt` on this branch now
  sets lag 8, the record thread and render scale 0.75. Next: sampler profile of this configuration.
- **Profile 2** (that configuration, driving the whole time; median 31.0 fps; rounds 1493-3033;
  `switch2/game/build-fast/profile-2/` with the ELF that ran): lag 8 removed the D3D thread's
  completion spin (`sub_83814160` 14.9% -> 1.4%); it now waits ~23% in NtWaitForSingleObjectEx
  instead. The game's main thread is ~70% busy and ~10% of its time is blocked in
  `svcArbitrateLock` on `ObjectTable::table_mutex_` (LookupObject from NtSetEvent,
  NtWaitForSingleObjectEx, NtReleaseSemaphore), plus 9% in guest Sleep (XThread::Delay) and 9% in
  waits. Render thread 56% busy (DrawImpl 12% self).
- **ObjectTable lookups lock-free against each other** (SDK `7e410299`): lookups count themselves
  in an atomic and only wait while a writer (add/remove/resize/reset/purge/restore, rare) empties a
  slot or moves the table; writers wait for the lookups in flight. Mac: 150 s scripted run, same
  frames as before (8100), no hang. Console build: `build-fast/fastcars.nro` (the build before it is
  `fastcars-fast.nro` on the SD card). Note: the header change alters `ObjectTable`'s size, so every
  host object must be rebuilt (the Mac host aborted with "mutex lock failed" until rebuilt).
- Build trap found on the way: the Mac dylib build and the Switch dev tree share
  `cg-fast/generated/default`, and each stages its own `fastcars_hooks.h` there (the Mac one lists the
  ~2,900 D3D probe wrappers). The Switch link then fails with undefined `sub_8383FF10` etc.: delete
  `build-fast/game_interface/staged.stamp` and the game objects compiled with the Mac list.
- **ObjectTable fix on the console** (same settings, 20 windows): median **34.5 fps** (30.7-39.5),
  vs 30.6 driving-only with the same settings before (31.0 in profile 2, sampler on); without the
  first three windows (possibly standing) 33.4. The floor rose too: no window under 30.7 (before
  25.9-27.1). Render thread 13-25 ms, GPU 22-27 ms, slot waits 1-10 ms: GPU and game now about
  even. No errors, no hang.
- **Profile 3** (ObjectTable fix, sampler on, driving; median 35.2 fps even with the sampler, vs
  31.0 in profile 2; `build-fast/profile-3/`): main thread lock waits gone (svcArbitrateLock 10% ->
  ~0). Left on the main thread: 15% waits, 9% `svcSleepThread` under `rex::thread::Sleep`, from
  `sub_83585010` (`while (obj->count) Sleep(0)`: waiting for worker jobs). Cause: `XThread::Delay`
  sends Sleep(0) of a thread with `priority_ <= ThreadPriority::kBelowNormal` to a 100 us sleep,
  but `priority_` is the guest increment and kBelowNormal is -1 on Windows, 8 on POSIX: every
  normal-priority guest thread slept 100 us per Sleep(0) instead of yielding. Fixed (SDK
  `68b27dc2`, `priority_ < 0`). Mac: 8700 frames in 150 s vs 8100, CPU 5:59 vs 6:42. Console:
  `build-fast/fastcars.nro` (previous: `fastcars-objtable.nro` on the SD card).
- **Sleep(0) fix on the console** (33 windows, ~5 min driving): median **35.8 fps** (29.9-41.5) vs
  34.5 with the ObjectTable fix alone; record 15-26 ms, GPU 23-28 ms with 4-11 ms slot waits in
  about half the windows (GPU-bound there again). No hang. Audio: 29 late frames in 28 gameplay
  windows of 10 s (max 10 of 1875 in one), 0 in the previous run: inaudible-level, watch it.
- **Render thread lock** (profile 3): 13% of the render thread in `svcArbitrateLock`, ~9% of it in
  `StreamConverter::Flush` (the two vertex converter workers claim jobs and report completions
  under the same mutex Flush takes). Now a lock-free job ring (`backend.cpp`, `917a7a2`): the render
  thread publishes batches by moving an atomic index, workers claim runs of 8 with a CAS, the mutex
  is only taken to wake a sleeping worker. Mac with `--fastcars_native_convert_threads=2`: runs, the
  prologue renders correctly. Console: `build-fast/fastcars.nro` (previous `fastcars-sleep0.nro`).
  Console (34 windows, ~5 min driving): median 35.5 fps (29.3-40.3), same as 35.8 before, but the
  render thread's recording dropped 19.9 -> 17.5 ms a frame on average and late audio frames
  29 -> 6. The frame is now mostly GPU-bound (slot waits 5-17 ms in most windows, GPU 23-29 ms).
  Next: per-pass GPU times (`--fastcars_native_pass_gpu_times=true`).
- **GPU by pass kind** (render scale 0.75, 37 gameplay reports of 300 frames, driving; median 35.5
  fps): total 25.4 ms a frame. G-buffer (1280 4 targets + depth, 7.6 passes) 5.2 ms; 1280 one
  target fmt 10 + depth (9 passes) 4.0; shadow atlas 640x2561 depth only 3.2 (not scaled: the
  render scale only covered targets 1280 wide); 160x720 MSAA + depth (6 passes) 2.4; resolve
  copies ~4.1 in all (1280x720 x7 1.0, depth 0.6, 128x1 0.6, 640x360 0.45, ...); writeback compute
  0.35; the rest < 0.9 each. (`switch2/game/build-fast/city-drive-gpupass.log`)
- **`--fastcars_native_shadow_scaled`** (`2eab912`, off by default): the shadow atlas (640 wide,
  2048+ tall, its depth and the colour target bound with it) renders at the render scale too.
  `BindTargets` now warns when a pass mixes scales (that caught the colour target on the first
  try). Mac (prologue interior, scale 0.75): runs 180 s, no warnings; outdoor shadows unverified.
  Console (37 windows; ~1.5 min standing, then driving in busier areas): no mixed-scale warning;
  the shadow pass only went 3.19 -> 2.94 ms for 44% fewer pixels (it is bound by geometry, not
  fill), GPU total 25.4 -> 24.6 ms, median 35.2 fps (driving part ~34). Not worth the shadow
  resolution: left off.

## Console test plan (one change at a time; `switch2/tools/log_fps.py`, same drive each time)

NROs built from the owner's codegen (dev trees, outside git):
- `switch2/game/build/fastcars.nro`: current `switch2` (the known ~28 fps)
- `switch2/game/build-regs/fastcars.nro`: + registers as locals + probe wrappers out
- `switch2/game/build-fast/fastcars.nro`: + memory classes (what the builder now makes)

1. **build-regs** vs baseline: expect the main thread and render thread to drop; watch for any
   hang (a missed live-in shows as a spinning thread or a crash in one place). Saving + loading.
2. **build-fast** vs build-regs.
3. On the faster one: `--fastcars_native_render_scale=0.75` (FSR on by itself). Now that the CPU should
   have room, this is where the GPU-bound heavy scenes should gain.
4. `--fastcars_native_record_thread=true` (plume/Vulkan recording on a second thread; never run on the
   console; on the Mac it works).
5. Docked, if you can: untested so far, and the handheld CPU halving (section 1.F of the handoff)
   may not apply with the docked power budget.

If something breaks only in the fast builds, the A/B is: rebuild with `FASTCARS_VOLATILE_STACK=ON`
(memory classes, stack volatile again), then build-regs, then baseline.

Rebuilding a variant: codegen into its own folder (`rexglue-sdk/out/mac-arm64/Release/rexglue` on
the Mac, manifest with `includes = ["codegen_fast.toml"]` and both TOMLs next to it), then the
dev-tree build with `-DFASTCARS_GENERATED_DIR=/fastcars-mac/<folder>/generated/default
-DFASTCARS_NATIVE_SHADER_DIR=/repo/switch2/game/build-fast/native_shaders` (FSR shader headers).

## Reflection cube without MSAA (09c4d40)

The "160x720 msaa + depth" passes are the reflection cube: six 128x128 faces at 4x MSAA, ~150
draws, 2.40 ms of GPU. `--fastcars_native_msaa_min_pitch=256` renders surfaces narrower than 256 at one
sample (alpha-to-mask through the shader's dither, as for the other 1x targets): 1.69 ms
(city-drive-msaa.log, `--fastcars_native_pass_gpu_times`), -0.7 ms; reflections looked right on the
console. The Switch 2 default (8d0aba0), with the half rate below.

`--fastcars_native_cube_half_rate` (c59b8d7): the cube's draws and resolves for every other face are
skipped, alternating each frame (~97 draws a frame), so each face updates at half rate: 1.23 ms
(city-drive-cube.log, 36.2 fps median over 62 windows), another -0.46 ms; the pass's clears and
resolves of the drawn faces remain. Together 2.40 -> 1.23 ms.

## Resolution: full by default, changeable in the game (ad9e78a)

switch2/game/args.txt no longer sets `--fastcars_native_render_scale` (the Switch build's default is
1.0: native 1280x720, no FSR). Lower it from the installer (`fastcars-nx2.py settings --resolution 85`,
or the builder page's Settings) or in the game: [-] + [+] opens the settings menu (D-pad, A, B;
the game sees no input while it is open). Resolution 100/90/85/75/67% applies at the next frame
(Backend::SetRenderScale retires every scaled target and resolve texture, made again on use; a
moment of wrong exposure and a short pipeline build follow); FSR sharpening, frame rate limit, FPS
counter are there too (the reflection options are fixed on: one sample, half rate). Console
(menu-scales.log): GPU ~28-31 ms a frame at 100%, ~23-24 ms at 67%. The menu writes args.txt in place, the
same edit as the installer, so both show the same settings. Pipelines of different scales are keyed
apart by the scale in spec bits 16-31 (half-percent steps; the shaders see the low 16 bits).

## Temporal anti-aliasing and upscaling (on by default on Switch 2)

`--fastcars_native_taa=true` (Switch 2 default; menu: "Temporal anti-aliasing / upscaling"), `native_taa.hlsl`, `TaaState`
in backend.cpp. It also provides what FSR 2/3.1 or DLSS take as inputs:

- **Jitter**: draws into the 1280-wide targets whose vertex shader reads `gWorldViewProj` (c8-c11)
  with a perspective projection get a sub-pixel offset through `g_NdcOffset` (Halton 2,3; 8 positions
  at 100%, 15 at 75%). Screen-space passes (lighting, post-processing) do not read it and stay put.
  Direction checked on the Mac (`--fastcars_native_taa_debug=4`, two consecutive dumps): positive = right
  and down.
- **Camera and object motion**: the first G-buffer draw's `gWorld` (c0-c3) and `gWorldViewProj` give
  the frame's view-projection; with the scene depth (reversed z) that is a motion vector for
  everything static. Opaque G-buffer draws are also matched across frames by mesh key and `gWorld`
  (`--fastcars_native_taa_objects`, on by default): draws whose `gWorld` moved by >= 0.15 px are replayed
  before the resolve (`PSTaaId` in `native_blit.hlsl`) into an `R8_UNORM` ID target depth-tested
  against `taa.depth`, indexing a `4x256` `RGBA32F` texture of per-object `inv(WVP_cur) * WVP_prev`
  reprojection matrices (fast-spinning wheels use translation reprojection so they do not chord
  across the hub).
- **Scene without HUD**: the game's FXAA draw (ps `040C1B12B017260A`, tonemapped scene in fetch
  slot 6, drawn into the back buffer before the HUD) is replaced by the resolve.
- **Upscaling** (render scale below 100%, `--fastcars_native_taa_upscale`): the history has the full
  size; after the resolve `Target()` hands out a second, full-size set of the 1280-wide targets, so
  the HUD and the front buffer are full size (no FSR 1 pass at the present). Scene samplers get a LOD
  bias of log2(scale) (`--fastcars_native_taa_mip_bias`; MoltenVK ignores it, NVK should not).

Mac (prologue interior, the only save there): at 100% sharper and steadier than FXAA; at 75% the
picture is full size with a sharp HUD, softer than 100%, text slightly heavy while a light
flickers. Known limits: moving objects fall back to the filtered scene (no ghost trails, less
anti-aliasing on them); thin lines can break up; the stippled glass turns into a blend.

Console, to do (one at a time, `log_fps.py`, `--fastcars_native_pass_gpu_times=true` for the cost):
1. 100% with the toggle on: picture (driving, a camera turn, a cutscene, the pause menu, the phone),
   and the frame time against FXAA. The pass is 28 taps a pixel plus the sharpening pass;
   `--fastcars_native_taa_sharpness=-1` drops that one.
2. 75% and 67% with it on: against the same scale with FSR 1.
3. If the open world never shows the toggle's effect, the FXAA draw there has another shader: the log
   line `taa: N jittered draws a frame` (every 600 frames) only appears while the hook is found.

Next, on the same inputs: FSR 2 / 3.1 (their Vulkan backend needs plume's native handles and the
shaders built with glslang) or frame generation; both want motion vectors for moving objects, which
would take matching draws between frames by their `gWorld`.

## Looked at, not done (and why)

- **Vertex byte swap in the shaders**: on Switch 2 the conversion already runs on two worker
  threads (`--fastcars_native_convert_threads=2`), off the critical path, with cores to spare; moving it
  into 3,618 vertex shaders would add GPU work (the tighter side) for no frame time.
- **Removing the EDRAM resolve copies**: in the city, 60-80 resolves a frame, ~200 render passes
  (`--fastcars_native_pass_stats`, city-drive-passdiag.log: 37.5 fps, GPU ~24 ms). Skipping every
  colour copy (`--fastcars_native_skip_resolves=1`, black picture) gave GPU ~19 ms and 43.7 fps: the
  ceiling, flattered by the broken frame. 46 of 80 are whole copies in the source's own format;
  `--fastcars_native_direct_resolves` (22cca25) draws such a target straight into its resolve texture
  the next time (copying back if anything else needs the target), but only 6-7 a frame qualify on
  the console: the blur steps sample the texture they would draw into, and the downsample chains
  reuse one target at several sizes. Removing those needs proof that a step's quad covers the
  whole texture (e.g. learned from occlusion counts). Console: GPU 24.9 vs 24.1 ms, i.e. no gain
  (city-drive-direct.log); left off. The big copies (G-buffer in tile halves, HDR 16F to 16 UNORM,
  depth to R32F) are partial or convert formats.
- **Hand-tuned replacements for the heaviest shaders**: needs per-shader GPU times from the console.
- **Native replacements for hot guest functions**: the Mac profile names little (ld64 folds the
  generated functions; the visible hot spots are host-side `RecordDraw`/hashing and a worker
  polling a tiny D3D function in a loop). Profile the fast build on the console instead
  (`--switch2_sampler_ms=10`, `switch2/tools/sampler_profile.py` with the build's ELF).
- **Shadow map at lower resolution**: the renderer's per-target scale could do it, but a pass that
  binds the 640-wide depth with a colour target of another scale would be invalid; needs an
  open-world scene to verify (the Mac install has no save past the prologue).
