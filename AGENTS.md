# Working on this repository (for AI agents and new contributors)

The game (Xbox 360, Title Update 27), statically recompiled to ARM64 with ReXGlue. The current focus
is the **Switch 2 port**: a homebrew NRO for kaiHEN with a native Vulkan renderer. Read this file
first, then the docs it points to.

## Ground rules

- **Clean room.** Never commit anything game-derived: disc, TU or RPF files, the XEX, extracted
  assets, generated C++ (codegen output), compiled game code, game shaders (binary, HLSL or
  SPIR-V), dumps, logs or screenshots of the game. Never use the game's leaked source, not even
  for names. See `docs/RELEASE_CLEANROOM.md`. Build trees, `logs/` and `gamefiles/` are gitignored:
  keep it that way.
- **Low profile.** Never write the game's, publisher's or studio's name anywhere in the repo:
  code, comments, docs, commit messages, branch and tag names, PR text, issue replies. Say "the
  game". The game's title on the console comes from the player's own disc at build time
  (`switch2/game/disc_title.py`). See `CONTRIBUTING.md` and the "Low profile" section of
  `docs/CLEANUP_AND_INSTALLER_PLAN.md`.
- **Branches.** `main` (tracking `origin/main`, github.com/itsPLK/fastcars-recomp-nx2) is what the
  player builder builds, so only console-tested work goes there.
  - **Never commit directly to `main`.** Every change, however small, starts on a feature branch
    from `main` (`git checkout -b <topic> main`), with a short neutral name.
  - **Commit as often as useful on the branch:** milestones, experiments, fixes. These commits stay
    local; don't push feature branches unless the owner asks.
  - **A feature lands on `main` as one commit:** `git merge --squash <topic>` and a commit message
    that says what the feature does (not the steps it took). Keep `main`'s history one commit per
    feature or fix. Only if the owner asks for it does a branch land with its commits kept.
  - **The owner decides when it lands and when `main` is pushed.** Delete the feature branch after
    it has landed.
  - (Older docs call the main branch `switch2`.)
- **Old history stays local.** This repository started from one fresh commit on 2026-11-19. The
  owner's checkout still has the earlier history (`old-main` and other old branches, old tags, the
  remote `old`), which names the game throughout. **Never push any of it to `origin`:** no
  `git push --all` or `--tags`, and no branch that isn't based on the new `main`.
- **Commits.** Commit at each milestone on the feature branch. Never push, open PRs or merge
  without being asked. Commit messages follow the low-profile rule too.
- **Measure on the console.** Profile and log before guessing. Make one change per test build, and
  let the logs (not impressions) decide. The owner plays the test builds and reports back.
- **The owner's notes:** `docs/HANDOFF_SWITCH2_NEXT.md` (state, settings, profiling, open issues),
  `docs/UPSCALERS_AND_TAA.md` (TAA, FSR 1/2, frame generation, their diagnostics),
  `docs/SWITCH2_PERF_PLAN.md`, `docs/CLEANUP_AND_INSTALLER_PLAN.md`, and `docs/*_PLAN.md` on feature
  branches.

## Where things are

| Path | What |
|---|---|
| `fastcars/src/native/` | Native renderer: `backend.cpp` (draws, targets, resolves, TAA, frame gen), `renderer.cpp` (frame queue, swap chain, presents, screenshots, flicker catcher), `capture.cpp` (draw capture on the game's D3D thread, fence copies), `menu.cpp` (the in-game settings menu, [-] + [+]) |
| `fastcars/src/native/shaders/` | Our HLSL (blit, TAA/FSR 2/frame gen in `native_taa.hlsl`, FSR 1). The DXC commands are listed in `fastcars/CMakeLists.txt`. |
| `switch2/game/` | The Switch 2 NRO build: `CMakeLists.txt`, `native_shaders.py`, and `args.txt` (the template for new installs). |
| `fastcars.py`, `installer/`, `switch2/builder/` | The player's builder: `installer/core` (no UI: console FTP/SD, Docker, source sync, build, install, shaders, settings), `installer/cli.py`, the browser page; `switch2/builder/build.sh` runs in the builder container (about an hour from scratch): not for iterating. `fastcars-nx2.py` is the old name, a wrapper. |
| `switch2/tools/` | Console log tools (`log_fps.py`, `sampler_*.py`), `mac_bench.sh`, the builder's disc extractor (`data_install/`) and shader constant rewrite (`ubo_constants.py`) |
| `switch2/data/expected.txt` | Known-good sizes and SHA-256 of the player's game files (hashes only) |
| `patches/` | Our changes to the SDK, glslang, plume and XenosRecomp, applied by the builder in the order of `switch2/builder/build.sh` |
| `tools/native/` | Renderer debugging on the Mac: dump comparisons, `gen_probe.py`, `shader_census.py`, `headless_run.sh` |
| `tools/game_dll/`, `tools/mac/` | The Mac game library build (`build_game_dll.py`, `gen_interface.py`, also used by the Switch build) and Mac run helpers |
| `tools/manifest/` | How the codegen manifest (`fastcars/fastcars_manifest.toml`) was derived: only for redoing that analysis |
| `rexglue-sdk/`, `thirdparty/` | Gitignored checkouts. Changes to them go into `patches/rexglue-sdk-fastcars-switch2.patch` / `patches/plume-fastcars-switch2.patch` (see the handoff). |

Only Switch 2 and macOS (for debugging) are supported. Linux x86-64 matters only as the builder
container's host. The upstream Windows, Linux, Android and iOS code was removed on 2026-11-19
(tag `pre-cleanup-2026-11-19`).

## Fast rebuild (seconds to minutes)

These steps rebuild the console NRO from the dev tree on the owner's Mac.

```sh
# from the repo root
# 1. Only when HLSL in fastcars/src/native/shaders or the shader list in fastcars/CMakeLists.txt changed:
python3 switch2/game/native_shaders.py switch2/game/build-fast/native_shaders
# 2. Build (Docker image fastcars-switch2; ../fastcars-mac holds the codegen output, outside the repo):
docker run --rm -v "$PWD:/repo" -v "$PWD/../fastcars-mac:/fastcars-mac" -w /repo fastcars-switch2 \
  cmake --build switch2/game/build-fast -- -j10
# -> switch2/game/build-fast/fastcars.nro   (prints "fastcars.nro: version <commit>[-dirty]")
```

- **Use `build-fast`.** `switch2/game/build` is an old tree: it lacks newer shader headers and
  fails to compile.
- **Shader check without a build:**
  `thirdparty/XenosRecomp/thirdparty/dxc-bin/bin/arm64/dxc-macos` (with
  `DYLD_LIBRARY_PATH=.../dxc-bin/lib/arm64`) using the same flags as `fastcars/CMakeLists.txt`.
- **Before replacing a build** the owner may want to compare against, copy the old NRO aside
  (e.g. `build-fast/fastcars.before-<change>.nro`).
- **Setting up a tree from scratch:** see the header of `switch2/game/CMakeLists.txt` and the
  "build trap" note in `docs/SWITCH2_PERF_PLAN.md`.

## Testing on the Mac first

The renderer and the SDK runtime build for macOS too. A desktop run takes seconds to set up, needs
no console, and gives keyboard-free scripted captures, so try renderer changes (TAA, frame gen,
shaders, resolves) there before the console. The Mac is far faster on both CPU and GPU, so its
timings don't carry over: use it for correctness and visual A/B, and the console for performance
and timing bugs.

**Layout** (outside the repo, on the owner's Mac; game-derived, never commit any of it):

| Path | What |
|---|---|
| `../fastcars-mac/home/` | the macOS install: `data/game`, `data/update`, `data/user`, `data/spirv`, `shader_cache/`, `vulkan/` |
| `../fastcars-mac/perf/host/` | the host build: `fastcars_fast` and the SDK dylibs, from `fastcars/` with `FASTCARS_GAME_DLL=ON`; native shaders compiled by its CMake |
| `../fastcars-mac/perf/cg-fast/`, `perf/lib-fast/`, `perf/rel/` | codegen output and the game library (`fastcars_game.dylib`, built with `tools/game_dll/build_game_dll.py`) |

**Rebuild:**

```sh
cmake --build ../fastcars-mac/perf/host -- -j10              # host: renderer, menu, capture, shaders
# SDK change: build and install the runtime first, then the host
(cd rexglue-sdk && cmake --build out/build/mac-perf --target rexruntime -- -j10 &&
 cmake --install out/build/mac-perf --config Release)
# Game library (after a codegen or prefix-header change; ~2 min; the fold build, as on Switch 2):
python3 tools/game_dll/build_game_dll.py /usr/bin ../fastcars-mac/perf/cg-fast/generated/default \
  ../fastcars-mac/perf/host/game_interface ../fastcars-mac/perf/lib-fast -j 10 -D REX_GUEST_FOLD \
  --runtime ../fastcars-mac/perf/host/librexruntime.dylib
```

- **Codegen** (only when the manifest, `codegen_*.toml` or the recompiler changed; ~70 s): in
  `../fastcars-mac/perf/cg-fast` (its `fastcars_manifest.toml` is the repo's, with
  `includes = ["codegen_fast.toml"]`; `assets` links to the extracted `default.xex`/`.xexp`), run
  `rexglue-sdk/out/install/mac-perf/bin/rexglue --log-level warn codegen fastcars_manifest.toml`.
  The Switch dev tree and the Mac library both build from its `generated/default`.
- **A fresh host tree** (e.g. after moving folders):
  `cmake -S fastcars -B ../fastcars-mac/perf/host -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DFASTCARS_GAME_DLL=ON -DCMAKE_PREFIX_PATH=$PWD/rexglue-sdk/out/install/mac-perf
  -DFASTCARS_XR_BIN=$PWD/../fastcars-mac/home/tools/XenosRecomp`, then copy
  `librexgpu-xenos.dylib` from `rexglue-sdk/out/install/mac-perf/lib` into it (`mac_bench.sh`
  links the host's dylibs into each run).

**Run:**

- **Scripted A/B:** `switch2/tools/mac_bench.sh SECONDS NAME=path/to/fastcars_game.dylib [...]` runs
  headless with a scripted pad (`tools/native/input_run_around.txt`). Each variant gets its own
  run folder and its own copy of the saves. Options:
  - `EXTRA="--flags"` adds runtime flags;
  - `SHOTS=1` takes screenshots every 20 s;
  - `SAMPLE=N` takes a macOS `sample` profile of the last N seconds.

  Its launch line is the reference for the flags a manual run needs.
- **Live control of a running game (no keyboard focus needed):** write files into the run's log
  folder:
  - `logs/cvar_set` with `name=value` lines sets options live;
  - `logs/dump_request` (any content) dumps the next frame's resolves to `logs/native_dump/`
    (like F9);
  - `logs/diag_mode` (a mode number) sets the F11 diagnostic.
- **Playing by hand:** `tools/mac/play_mac.sh` (older desktop build layout; see its header).

**Build trap:** the Mac game library and the Switch dev tree share `cg-fast/generated/default` and
each stages its own `fastcars_hooks.h` there (`docs/SWITCH2_PERF_PLAN.md`). After a Mac build, delete
`switch2/game/build-fast/game_interface/staged.stamp` if the Switch link fails with undefined
`sub_8...` symbols, then build **twice**: the first run restages the header, the second recompiles
the objects that saw the wrong one.

## Deploying to the console (FTP)

The owner gives you the console's address. The FTP server is OunceShell, on port 1337, anonymous.
It **only answers while the game is not running**: ask the owner to quit the game and say when FTP is
up.

| Console path | What |
|---|---|
| `/switch/fastcars/fastcars.nro` | the game |
| `/switch/fastcars/args.txt` | settings; the in-game menu writes it too |
| `/switch/fastcars/logs/fastcars.log` (+ `fastcars.1.log` after 16 MB) | log |
| `/switch/fastcars/logs/shots/`, `/switch/fastcars/logs/flicker/` | captures |
| `/switch/fastcars/data/user/` | saves: never overwrite or delete them without asking |

```sh
F=ftp://<console-ip>:1337
curl -sS --connect-timeout 10 --max-time 280 -T switch2/game/build-fast/fastcars.nro "$F/switch/fastcars/fastcars.nro"
curl -sS -I "$F/switch/fastcars/fastcars.nro"     # Content-Length must equal the local file size
```

- **Always verify the size.** An upload cut off by Wi-Fi left a half NRO that crashed at start-up.
  Retry in a loop until the sizes match.
- **Big downloads (PPMs, logs) reset now and then.** Fetch one file at a time with `--max-time` and
  retries, and check sizes.
- **Deleting a file:** `curl -Q "DELE /switch/fastcars/logs/flicker/<file>" "$F/"`.
- **Renaming a file:** `-Q "RNFR <old>" -Q "RNTO <new>"`, e.g. to move a save aside. Make a local
  copy first.

## Test runs

1. **Edit `args.txt` around the test.** Download it and keep a copy of the owner's version.
   Append the test flags under a `# TEMP diagnostics: remove after the test` line, upload, and
   restore the owner's version when the test is done. Don't change the owner's own settings
   silently: say what you set.
2. **Clear old captures** (`logs/shots`, `logs/flicker`) before a capture run.
3. **Read the right session.** Ask the owner to play, then quit. The newest session in the log
   starts at the last `Vulkan instance API` line; if `fastcars.1.log` is newer than expected, the log
   rotated: `cat fastcars.1.log fastcars.log` first.
4. **Tell the owner what to look for and how long to play.** Ask for the on-screen frame rate when
   it matters (the FPS counter; with frame generation it is twice the game's).

**Logs worth knowing:**

| Line | What it means |
|---|---|
| `hitch N ms at guest frame F: record R ms, wait W ms` | `wait` = the renderer waited for the game; `record` = the renderer itself was slow |
| `presented 300 frames ...` | per-300-frame stats: record, GPU frame, idle |
| `flicker #...` | one-frame glitches (with the flicker catcher) |

`switch2/tools/log_fps.py` gives the real fps per 300-frame window.

**Diagnostic options** (all off by default; descriptions in the cvar help and the docs above):

| Option | Use |
|---|---|
| `--fastcars_native_flicker_catch` (`--fastcars_native_flicker_min_blocks=100` for large ones) | Counts one-frame glitches and saves before / glitch / after frames plus draw lists. |
| `--fastcars_native_resolve_thumbs`, `--fastcars_native_resolve_thumbs_gbuffer=N` | Which pass / block of draws breaks a glitch frame. |
| `--fastcars_native_shot_frames=N`, `--fastcars_native_shot_max`, `--fastcars_native_shot_fg` | Periodic screenshots (generated \| real side by side with frame gen). |
| `--fastcars_native_taa_debug=1`, `--fastcars_native_taa_diag`, `--fastcars_native_frame_gen_debug` | Motion vectors, object tracking, frame-gen routes. |
| `--fastcars_native_pass_gpu_times` | GPU time per kind of render pass. |
| `--switch2_sampler_ms=10` | Thread stack sampler (see the handoff, section 2b). |
| `--fastcars_native_diag_mode=3` + `--fastcars_native_diag_vs=<hash>` | Flat-shade chosen draws to see what object they are. |

**Pitfalls:**

- **The console's SD card is slow.** Write files in one `fwrite`, never per pixel or per line:
  that froze the game twice.
- **Diagnostics change timing.** A diagnostic that slows the game can hide timing bugs. The black
  sky flashes vanished under `--fastcars_native_check_draw_data` and draw snapshots, both only because
  they slowed the game. Control for that (`--fastcars_native_capture_delay_us` adds the slowdown
  alone).
- **zsh:** `rm *.ppm` with no match aborts the whole command line. Use `find . -name '*.ppm' -delete`.
- **Clocks:** raising GPU or EMC clocks in handheld halves the CPU, and clock settings persist after
  the app exits (see the handoff and `switch2/clock-reset/`).
