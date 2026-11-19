# Plan: repository cleanup, a wizard installer and versioned releases

Branch `cleanup-installer` (from `main` at `f3fabe7`). Three projects that build on each other:

1. **Cleanup.** The repository keeps only Switch 2, plus macOS for debugging.
2. **Releases.** Each release is a tag. The installer is the repository itself at that tag, so it
   builds and installs exactly that version. No binary downloads (decided 2026-11-19, see "Low
   profile").
3. **Installer.** A wizard with steps and native file pickers, run from the source with Python's
   standard library. Players install only Python and Docker: no pip packages, no git (a tag's
   source zip works), no typed paths.

**Platforms:** Switch 2, plus macOS for local testing. The Mac is ARM64 like the console and runs
the same native renderer, so it is for correctness and visual A/B. The console stays the test for
performance, timing and driver behaviour (MoltenVK on the Mac, NVK on the console). Linux x86-64
remains only as the host of the builder container.

Decisions taken (2026-11-19, the owner):

- **Installer technology:** ~~a native window app, Python + pywebview, packaged with
  PyInstaller~~. Replaced the same day by a standard-library Python wizard run from the source
  (no binary releases; see "Low profile").
- **Docker:** stays required. The installer checks for it and guides the player through installing it.
- **Platforms:** the installer runs on Windows, macOS and Linux (one source, Python 3.9+).
- **Cleanup depth:** files first, then the Android, iOS and Windows code paths in `fastcars/src`.

## The constraint everything follows

**No release ever contains game code.** The NRO links the recompiled game, so a release can't ship
a `fastcars.nro`, the generated C++ or shaders (see `docs/RELEASE_CLEANROOM.md`). A release is
therefore:

- the source at a tag;
- the installer, which is the source itself: it builds exactly that version from the player's own
  discs, then installs it or updates an older install.

**Old releases should keep working, within reason.** Someone should be able to install a release
from a year ago. The owner's call (2026-11-19): assume the git repositories we depend on stay up,
and don't pack toolchains or images into releases. So:

- **No registry package, no image file in releases.** The installer builds the builder image on
  the player's computer from that release's `switch2/builder/Dockerfile`. Docker caches it, so it
  is built once (about 10–20 minutes) and again only when a release changes the Dockerfile.
- **Upstream code at pinned commits,** cloned at build time, as today: the SDK, plume, XenosRecomp
  and ounce-devkit.
- **Cheap pinning against the drift that isn't git** (see 2.2):
  - the devkitPro base image by digest, not by tag;
  - Debian packages from `snapshot.debian.org` at a fixed date;
  - OunceVK by URL plus SHA-256, as now.
- **Outside our control:** whether a year-old NRO still runs on that day's kaiHEN and firmware.

## Low profile (decided 2026-11-19)

On 2026-11-18 the publisher filed four DMCA notices on GitHub (the four notices of that date in `github/dmca`). They target ports of an unauthorized web build of the game,
and their forks, on a "facilitation" theory. They dismiss "bring your own copy" disclaimers, and
they quote the projects' own READMEs naming the game. The owner chose the middle level:

- **No game, publisher or studio name** anywhere in the repository: README, docs, code comments,
  comments in our patches, the console splash screen, the builder and installer screens, the
  homebrew menu title, release notes, changelog and commit messages. The `fastcars` identifiers stay
  (settings, folders, `fastcars.nro`, `switch/fastcars`).
- **Project name: `fastcars-recomp-nx2`** (decided); "fastcars" as the short name on screens. Written
  `<name>` below. There is no start screen (removed 2026-11-19, so screenshots never pair the title
  with the repo); the homebrew menu shows the game's title read from the
  player's own disc (`switch2/game/disc_title.py`), falling back to "fastcars".
- **A brand-new GitHub repo** (`fastcars-recomp-nx2`, not a fork), starting from one fresh commit of
  the cleaned, neutral tree. Renaming the old repo was dropped: a notice already in the works
  would still reach a renamed repo (same ID, and the old URL redirects). The old repo is
  deleted once the new one works. The full history stays local and in the backup bundle.
- **No binary releases:** no installer downloads and no published images. A release is a tag plus
  a changelog section. Players run the installer from the source.
- **No public announcements.** The owner removes the X posts that name the game next to the
  link. No request about posts elsewhere in the README or CONTRIBUTING.md (it read oddly; the
  owner, 2026-11-19): only the rules for this repo's own issues.
- **Issues: on, with rules** (decided). Never name the game, publisher or studio, and no
  screenshots or videos of the game; a maintainer asks for pictures privately when needed. A bug
  report form with required rule checkboxes and no blank issues (`.github/ISSUE_TEMPLATE/`), plus
  `CONTRIBUTING.md`. Posts that break the rules are edited or deleted.
- **Backup:** a git bundle of every branch and tag, kept outside GitHub
  (`../repo-backup-2026-11-19.bundle`, local).

---

## Part 1: Cleanup

### 1.0 Before deleting anything

- Tag the current state as `pre-cleanup-2026-11-19` and push the tag when the owner says so.
  Everything removed stays reachable there.
- Rule for each removal: `git grep` for references from the code that stays. Anything still used is
  moved, not deleted.

### 1.1 Files: what stays, moves or goes

**Stays (the Switch 2 build and the Mac debug build):**

| Path | Why |
|---|---|
| `fastcars/` (`CMakeLists.txt`, `fs/`, `src/`, the manifests, `codegen_*.toml`) | The game host and renderer, used by the Switch and Mac builds |
| `switch2/game`, `switch2/cmake`, `switch2/support`, `switch2/builder`, `switch2/tools/data_install` | The NRO build and the player builder |
| `switch2/tools/{log_fps,log_audio,sampler_profile,sampler_callers}.py`, `mac_bench.sh`, `ftp_push_data.sh`, `make_spirv_ubo.sh` | Console log and profiling tools, Mac A/B runs |
| `switch2/clock-reset` | Players' clock-reset tool |
| `tools/game_dll/` | The Mac game library and the Switch build's `gen_interface.py` |
| `tools/codegen/register_livein.py` | Codegen tuning (`codegen_share_registers.toml`) |
| `tools/native/headless_run.sh`, `input_run_around.txt`, and the platform-neutral analysis scripts (`compare_*.py`, `side_by_side.py`, `sample*.py`, `fps_from_log.py`, `shader_census.py`, `ucode_disasm.cpp`) | Mac debugging. The exact list is settled in the cleanup commit, one by one. |
| `rexglue-sdk-fastcars-linux.patch`, `rexglue-sdk-fastcars-switch2.patch`, `glslang-fastcars-switch2.patch`, `plume-fastcars.patch`, `plume-fastcars-switch2.patch`, `xenosrecomp-fastcars.patch` | Applied by the builder, in that order. `ffmpeg-fastcars.patch` stays if the linux patch still uses it (to check). |
| `docs/HANDOFF_SWITCH2_NEXT.md`, `UPSCALERS_AND_TAA.md`, `SWITCH2_PERF_PLAN.md`, `RELEASE_CLEANROOM.md`, `d3d_map.md` | The owner's notes and references |
| `README.md`, `AGENTS.md`, `CLAUDE.md`, `LICENSE`, `.github/`, `gamefiles/README.md` | |

**Moves:**

| From | To | Why |
|---|---|---|
| `loader/src/install.{cpp,h}`, `loader/src/sources.{cpp,h}` | `switch2/tools/data_install/` | The builder's disc extractor and hash check. The rest of `loader/` is the desktop launcher. |
| `tools/release/expected.txt` | `switch2/data/expected.txt` (or next to data_install) | The known-good hash list, used by data_install and, later, the installer |
| `tools/release/macho_tool.py` | `tools/mac/` | Used by `fastcars/CMakeLists.txt` on Apple |
| `tools/release/taint_scan.py` | `tools/release/` (kept, reworked) | Becomes the clean-room scan of release artifacts in CI |
| `play_mac.sh` | `tools/mac/` | Mac runs (used by `headless_run.sh`) |
| the six patches above | `patches/` | One place. `build.sh`, AGENTS.md and the handoff are updated. |
| `scripts/derive_entries.py` + `derive_*.toml`, `derive/`, `scripts/{find_*,normalize_manifest}.py` | `tools/manifest/` | Only if regenerating the manifest is still wanted. Otherwise they go. |
| `docs/BLACK_FLASHES_FIX_PLAN.md`, `SWITCH2_RELEASE_PLAN.md`, `SWITCH2_PORT_PLAN.md`, `HANDOFF_SWITCH2.md` | folded into `HANDOFF_SWITCH2_NEXT.md` and this plan, then deleted | Finished or superseded plans |

**Goes:**

- **Android and iOS:**
  - `tools/android/`, `tools/ios/`, `scripts/xiso_adb.py`;
  - `fastcars/src/android_*.cpp`, `fastcars/src/ios_setup.*`;
  - `fastcars/src/native/adreno_shader_fix.*`, `touch_controls.*` (the Switch 2 file stays if it's used);
  - `tools/game_dll/libSystem_ios.tbd`.
- **Windows and desktop Linux:**
  - the root scripts `*.cmd`, `play.ps1`, `play.sh`;
  - `rexglue-sdk-fastcars-windows.patch`, `xenosrecomp-win-before-linux-1029.patch`, `sdl3-fastcars.patch`;
  - `tools/native/*.ps1`.
- **Desktop releases:**
  - `loader/` (after the move);
  - `tools/release/` except `taint_scan.py`: `build_release_*`, `make_release*`, `README*.txt`,
    `compat/`, `scan_release*`, the `lld_libxml2_stub*` files;
  - `scripts/xiso.py`, `stfs_extract.py`, `extract_basefile.py`, unless a Mac step still needs one
    (to check).
- **Notes from earlier sessions:**
  - `NEXT_SESSION_PLAN.md`, `docs/NEXT_SESSION_*`;
  - `docs/HANDOFF_{ANDROID*,LINUX*,MAC_*,WINDOWS*}`, `ANDROID_STATUS.md`, `ARM64_NOTES_FOR_ANDROID.md`;
  - `IOS_PORT.md`, `MAC_THEN_ANDROID_PLAN.md`, `RELEASE_v0.3.0.md`, `WINDOWS_PERF_*.md`;
  - `TREE_ORBS_INVESTIGATION.md`, unless the glitch is still open on Switch 2 (to check).
- **One-off investigations:**
  - `tools/old_traces/`, `tools/scripttools/`;
  - `night_*.sh`, `hangdump.sh`, `audio_capture.*`;
  - `tools/native` audio, ocean, leaf and MST one-offs;
  - `tools/cutscene_unstick.py`, `tools/fn_match.py`, unless still used.
- **Clean-room check:** `tools/scripttools/` holds `fn2name.txt`, `pro1.natives`, `strs.py`,
  `handlers.txt` and `ptrs.txt`. Check whether any of it is game-derived or comes from outside
  sources. Delete it either way. If it is game-derived, also note it in `RELEASE_CLEANROOM.md`.
- **Ignored files:** `.DS_Store` goes into `.gitignore`.
- **Inherited tags:** the upstream tags (`v0.1.0-*` … `v0.3.0`) stay in history, but our release tags
  must not collide with them (see 2.1).

**Verification after 1.1:**

- `build-fast` builds the Switch NRO.
- A clean builder run passes: `fastcars-nx2.py build` in a fresh work folder, with `deps()` re-cloning.
- The Mac host and the game library build (`cmake --build ../fastcars-mac/perf/host`).
- `mac_bench.sh` runs 30 s.
- No game-derived file is added. `git status` stays clean after the builds.

### 1.2 Code: Android, iOS and Windows paths in `fastcars/src`

- **Scope:** about 100 `#if` blocks, mostly in `backend.cpp`, `renderer.cpp`, `write_watch.cpp`,
  `capture.cpp` and `shader_compiler.cpp`, plus `fastcars/CMakeLists.txt` (the WIN32, ANDROID and
  FASTCARS_IOS branches) and `CMakePresets.json`.
- **Keep:** `__SWITCH__` and `__APPLE__` (macOS) code, and Linux x86-64 code that the builder's
  host tools need.
- **Go:** `FASTCARS_MOBILE` logic goes, keeping its Switch 2 meaning where the two are shared.
- **Order:** one file per commit. Each commit builds Switch and Mac. Behaviour must not change on
  either.
- **One console test** after the last commit: a 10-minute drive, comparing fps and glitches with
  `main`.
- **The SDK patches are not trimmed here.** They are upstream SDK changes, a much riskier edit for
  little gain. Maybe later, as a separate project.

### 1.3 Docs after cleanup

- **README:** players only (install, play, settings, troubleshooting).
- **AGENTS.md:** updated paths.
- **New `docs/RELEASING.md`:** the release checklist (2.4).
- **`HANDOFF_SWITCH2_NEXT.md`:** keeps state and open issues.

---

## Part 2: Releases

### 2.1 Versions

- **Tags:** semantic versions on `main` only, after a console test.
- **Naming:** `v1.0.0` for the first Switch 2 release (decided). Inherited upstream tags go up to
  `v0.3.0`, so `v1.x` can't collide. `git describe --match 'v[1-9]*'` then ignores the old tags.
- **NRO version:** `v<VERSION>-<commit>` (`make_version.sh`; the `VERSION` file holds `1.0.0`, bumped
  for each release), `-dirty` with local edits. The homebrew menu shows it as the version, with the
  author "local-build": no project name or link appears next to the game's title.
- **`CHANGELOG.md`:** one section per release. The installer and the GitHub release show it.

### 2.2 The builder image and build speed

- **Now:** `fastcars-nx2.py` pulls `ghcr.io/itsplk/fastcars-recomp-nx2-builder:latest` (the old repo published `fastcars-recomp-nx2-builder`). That is a
  toolchain-only image, always the newest one, so an old release would get a newer toolchain than
  it was tested with.
- **Proposed:** no published image. The installer runs `docker build` on the release's own
  Dockerfile and tags the result `<name>-builder:<Dockerfile hash>`.
  - Releases that share a Dockerfile share the image.
  - Old images can be removed from the maintenance screen.
- **The ghcr.io package goes.** Delete it after the first release that builds its image locally;
  the current builder pulls `:latest` until then. The `builder-image.yml` workflow goes with it.
- **Pinning the Dockerfile** so a later rebuild gets the same toolchain:
  - `FROM devkitpro/devkita64@sha256:…` instead of the `:20260219` tag;
  - apt sources set to `snapshot.debian.org/archive/debian/<date>/`;
  - ounce-devkit at its commit and OunceVK by SHA-256, both already pinned.
- **Faster updates without prebuilt parts:**
  - A patch change today re-clones the upstream trees and deletes `build/host` and `build/game`
    (`deps()` in `build.sh`): a full rebuild of about an hour. Instead, update the trees in place
    (`git checkout` + re-apply the patches) and let ninja rebuild only what changed. That is the
    old release plan's "Later" item.
  - Codegen is redone only when the manifest or the recompiler changed, as now.
  - The build trees live in a named Docker volume (3.4), not in the bind-mounted work folder.
- **Measure first:** time each stage of a clean builder run on the Mac and on an x86-64 machine:
  image build, deps, host tools, data install, codegen, game code, host, link. Then decide where
  speed work pays off.
- **The source without git:** the installer downloads the tag's source archive from GitHub. Its
  version and commit are baked in, so players don't need git. `make_version.sh` takes the version
  from an environment variable when there is no `.git`.
- **Fast updates for those who want them:** clone the repository and use the CLI, as the owner
  does. Builds are incremental (seconds to minutes) and follow the current `main`. The old plan's
  `build --dev` item belongs here.
- **Later, only if builds are too slow:** prebuilt game-free parts (SDK libraries, plume,
  `fastcars_host`, host tools) as a release asset or in a published image. This was considered and
  set aside (2026-11-19). It would need a published artifact per release, and its toolchain would
  have to match the player's locally built image exactly.

### 2.3 What a release is

- **A tag on `main`** (`v1.0.0`) plus a GitHub release with neutral release notes (also kept in
  `CHANGELOG.md`), written without the game's name. **No uploaded files.**
- **Pre-releases:** semantic-version suffixes (`v1.1.0-rc1`, `v1.1.0-beta.2`) with GitHub's
  "pre-release" flag set. For trying a feature with a few testers before the full release.
- **The version** is `v<VERSION>-<commit>`: the `VERSION` file (`1.0.0`, `1.1.0-rc1`) plus the commit,
  from git in a checkout or from the `COMMIT` file in a source zip (`git archive` fills it in:
  `export-subst` in `.gitattributes`).

### 2.3a Choosing a version (decided 2026-11-19)

The installer's first step (the CLI: `versions`, `--version vX`):

- **The list:** the GitHub releases (public API, no login), newest first, with their notes as the
  changelog. Pre-releases are hidden unless "Show test versions" (`--pre-releases`) is on; it turns
  on by itself when the console already runs a pre-release. Semantic-version order: `v1.1.0-rc1`
  sorts after `v1.0.x` and before `v1.1.0`.
- **Preselected:** the newest full release. Also shown: what the console has (`install.json`) and
  what this folder is.
- **Switching, from a git clone:** fetch the tags, check the chosen tag out (detached), and restart
  the installer from it, so the installer code always belongs to the version it builds. If the
  folder has local changes, refuse with an explanation instead of touching them.
- **Switching, from a zip (no git):** download the tag's source zip from GitHub into the app-data
  folder (`.../fastcars/sources/<tag>/`) and restart from there.
- **"This folder as it is":** builds the current checkout unchanged, for development on `main` or a
  branch. Also the fallback when GitHub can't be reached.
- **One work folder for all versions:** switching never re-extracts the discs; only the code is
  rebuilt. Downgrades are allowed, with a warning.

### 2.4 Release checklist

Written down in `docs/RELEASING.md` (2026-11-19), with `CHANGELOG.md`.

### 2.5 CI for every push (new; no game files needed)

- **Lint and tests:** Python lint and unit tests for the installer core.
- **Game-free build:** builds the builder image (cached in Actions) and the game-free half of the
  NRO in it. This catches build breakage, and Dockerfile drift, without discs. It needs a CMake
  target that builds without `FASTCARS_GENERATED_DIR`: the host, SDK and plume only.
- **Installer tests** on Windows, macOS and Linux runners: the standard-library wizard starts,
  its unit tests pass, and the fake-console FTP tests pass.

---

## Part 3: The installer

### 3.1 Shape

- **The installer is the repository.** `python3 <name>.py` from a checkout or a tag's source zip.
  It builds the source it ships with (the old plan's "developer mode", for everyone), so it
  installs or updates the console to exactly its own version. No source download, no git needed.
- **Standard library only (Python 3.9+):**
  - The wizard is a local web page, as the current builder is (`http.server`).
  - Native file and folder pickers come from `tkinter.filedialog`, run on the main thread while
    the web server runs on another. tkinter ships with the python.org installers for Windows and
    macOS; on Linux it is the `python3-tk` package. If it's missing, a path field is shown instead.
  - No pip packages.
- **Install logic in a core package.** The FTP and SD targets, Docker driving, copying and settings
  live in one package used by both the wizard and a CLI. The CLI keeps headless use and
  development possible (`build/install/shaders/...`, like today).
- **Prerequisites:** Python and Docker. The installer drives `docker` through its CLI, the same as
  today.
- **Its screens never name the game.** They ask for "Disc 1 (Install), Disc 2 (Play) and the title
  update file `tu00000009_00000000`". The hash check rejects anything else.

### 3.2 The steps

**1. Welcome and checks.**
- What you need: discs, TU27, a console with kaiHEN, about 60 GB free.
- Docker installed? Running? A button opens the right Docker download page, plus Re-check.
  On Apple silicon it reminds you about the Rosetta setting.
- **Work folder:** the default is the OS's app data folder. On Windows, macOS and Linux that is
  `%LOCALAPPDATA%\<name>`, `~/Library/Application Support/<name>` and
  `~/.local/share/<name>`. A Change button picks another drive, and free space is checked live.
- **Import:** an old `fastcars-nx2-work` folder (from the current builder) is found and offered for
  import, so the 16 GB copy and the codegen aren't redone.

**2. Game files.**
- Three native file pickers, *or* "Choose a folder": the files are found by name and size.
- They are checked in the background: SHA-256 in Python against the hash list from the release
  (no Docker needed, with a progress bar).
- Clear messages: "This is the European TU, version N; TU27 is needed", "Disc 2 is a bad dump".
- Skipped when the work folder already has the checked data.

**3. Console.**
- **FTP:** "Searching your network…" scans the local /24 for port 1337 and the OunceShell
  greeting, and lists consoles found. Manual address as a fallback, plus a hint: "open OunceShell
  on the console, and close the game: FTP only answers while the game isn't running".
- **SD card:** lists removable drives that have a `switch/` folder, with a folder picker as a
  fallback.
- **What's installed:** reads `switch/fastcars/install.json` (see 3.3) and the console's free space.

**4. Summary.** One of:
- "Install v1.0.0";
- "Update v0.9.0 → v1.0.0 (saves and settings kept)";
- "Reinstall v1.0.0";
- "Downgrade v1.1.0 → v1.0.0", with a warning.

It also shows what will happen and roughly how long it takes.

**5. Build.**
- Progress per stage (data, codegen, game code, link) with time estimates, and a collapsed log.
- Builds resume: each stage is skipped when already done, as in `build.sh` today.
- The computer is kept awake during the build.

**6. Copy to the console.**
- Bytes-based progress.
- Every file's size is checked after upload, with retries. A Wi-Fi drop once left a half NRO.
- The NRO goes up as `fastcars.nro.part`, then is renamed, so an interrupted copy never leaves a broken
  game.
- `install.json` is written last.

**7. First start and shaders.** Only on a fresh install, or when there are no translated shaders.
- Pictures and text: start in full-memory mode, press A, press B at grey, wait for the game's
  sound, then HOME, Close.
- The installer then waits, needing nothing further from the player:
  - **FTP:** it polls the console. While the game runs, FTP is down. When FTP is back and
    `shader_cache/` has files, it fetches them, translates and uploads them.
  - **SD:** it waits for the card to come back.

**8. Done.**
- How to start the game, the settings menu ([-] + [+]), and where saves are.
- **"Free up space?"** The disk usage panel (3.8) with the cleanup levels and their sizes. The
  pre-selected level keeps fast updates. The player can also skip.

**Maintenance screen** (reached from the start when an install exists):
- back up saves;
- fetch shaders again;
- collect logs for a bug report (a zip of `logs/fastcars.log` and `install.json`);
- set the log level;
- uninstall: removes `switch/fastcars` except saves, unless asked;
- **disk usage and cleanup** on this computer (3.8).

### 3.3 State on the console and updates

`sdmc:/switch/fastcars/install.json` is written by the installer after a successful copy. It records:

- the version and commit;
- the date;
- a hash of the shader translator (XenosRecomp and its patch);
- the list of data files and their sizes.

Update rules:

- **Game data** (`data/game`, `data/update`): copied only when missing or a different size, as
  now. It doesn't change between releases.
- **Saves** (`data/user`): never written once present.
- **`args.txt`:** the player's file is kept. Each release can list **settings migrations**: options
  renamed or removed, or defaults whose old value the menu wrote explicitly.
  - Example: `render_scale=0.9` was the old default. Migrate it only if the player never changed
    it.
  - Rule: the installer records the args it wrote, so it can tell untouched values from the
    player's own.
- **Shaders:**
  - The installer keeps a local copy of every `shader_cache` it fetched.
  - When the translator changed between versions, it retranslates them all and uploads them, with
    no new first start needed.
  - The first start is needed only once per console.
- **Downgrade:** allowed, with a warning. Saves are the game's own and not affected.

### 3.4 Docker details that matter for players

- **Build trees in a named Docker volume.** On Windows (WSL2) and macOS, a C++ build in a
  bind-mounted host folder is slow. A named volume (`<name>-build`) holds the build trees and
  the codegen. The host work folder keeps only:
  - the game data, which the installer uploads itself;
  - the outputs (NRO, SPIR-V);
  - the shader caches.

  Measure on Windows and macOS before committing to it.
- **A lock on the work folder,** so two actions never run at once (from the old release plan's
  "Later" list; the clash that failed a build).
- **The builder image:** built from the release's Dockerfile on first use, with progress shown
  ("Preparing the build tools, once, about 10–20 minutes"). It is reused by later releases with the
  same Dockerfile.

### 3.5 Code layout

```
<name>.py              entry point: the wizard (default) or the CLI (subcommands)
installer/
  core/               no UI: version.py, files.py (hash checks), docker.py, console.py (FTP/SD
                      targets, discovery), build.py, install.py, shaders.py, settings.py (args.txt
                      + migrations), state.py, storage.py (disk usage and cleanup, label-filtered)
  ui/                 index.html, app.js, styles (the wizard), assets
  server.py           the local web server and the tkinter pickers
  tests/              unit tests (args migrations, version compare, discovery parsing) and FTP
                      tests against a local FTP server written with the standard library
```

`fastcars-nx2.py` stays as a thin wrapper around the CLI for one release, then goes.

### 3.6 Risks

- **Python on the player's machine:** Windows players install it from python.org (the installer
  page explains how, and the wizard checks the version). macOS and Linux usually have it, but
  macOS's own `python3` may lack tkinter: the path-field fallback covers it.
- **Network scan:** firewalls or guest Wi-Fi can block it. A manual address is always offered.

### 3.7 Testing the installer without the owner's console

- **Unit tests and a fake console:** a local FTP server with the OunceShell layout, including
  dropping the connection mid-upload.
- **Mac end-to-end run:** the owner's discs in the local work folder, installing to a fake SD
  folder, then comparing the tree with what the current builder produces.
- **Console runs** at the milestones only: first install, update, and shader fetch.

### 3.8 Disk space on the player's computer

The installer shows what it uses and cleans up after itself. Docker never frees space on its own:
on the owner's Mac it held 29 GB of images and 76 GB of build cache (2026-11-19).

**What the installer creates:**

| Item | Where | Size (to measure) | Needed for |
|---|---|---|---|
| Builder image(s) | Docker, one per Dockerfile version | ~3.5 GB each | every build |
| Docker build cache from building that image | Docker (BuildKit) | GBs | rebuilding the image only |
| Build trees: upstream checkouts, host tools, objects | named volume `<name>-build` | several GB | fast updates |
| Codegen: C++ made from the player's XEX | same volume | several GB | fast updates (else ~minutes of codegen plus a full compile) |
| Game data from the discs | work folder `data/` | ~16 GB | installing to a console without the discs again |
| Outputs: NRO, SPIR-V | work folder `out/` | ~100 MB + shaders | re-installing without a build |
| Fetched shader caches | work folder `shader_cache/` | tens of MB | retranslating shaders after an update with no new first start: **always kept** |
| Downloaded source archives | work folder `src/` | tens of MB | the build |

**The disk usage panel:** each item with its size and a one-line "what you lose if you delete it".
Sizes come from:
- `docker image inspect` and `docker buildx du`, filtered to our items;
- a `du` inside a throwaway container for the volume;
- the file system for the work folder.

**Cleanup levels:**

1. **Tidy** (keeps fast updates):
   - builder images for other Dockerfile versions;
   - the build cache from building our image;
   - old source archives and outputs of older versions.
2. **Keep only the game files:** also the build volume (build trees and codegen) and the current
   builder image. The next update rebuilds the image and redoes codegen and the full compile, but
   doesn't need the discs.
3. **Remove everything:** also the game data and the work folder. The next install needs the discs
   again.
   - It still keeps the shader caches (tiny, and they spare a first start) and any save backups,
     unless the player ticks "also delete these".

**Rules:**

- **Only our own things are touched.**
  - Every image, volume and build the installer creates carries the label
    `org.<name>.owned=true`.
  - Cleanup filters by that label.
  - Never `docker system prune` or anything global: the player may use Docker for other things.
- **Nothing on the console is touched from here.** That is the uninstall entry, with its own
  confirmation.
- **Locked during actions:** cleanup takes the work-folder lock (3.4), so it can't run during a
  build or copy.
- **Docker's disk image may not shrink** after a cleanup. Docker Desktop keeps its data in a
  virtual disk:
  - The Mac one releases space by itself.
  - On Windows, the WSL2 disk often doesn't until Docker Desktop's "Clean / Purge data" is used or
    the disk is compacted.
  - The panel says so after a cleanup, with the steps for that OS. Measure on Windows.

---

## Progress

- **2026-11-19: step 1 (files) done.** `89a1d5b`, about 160 files deleted, moves as in 1.1.
  - The bring-up projects were deleted too.
  - The finished release and black-flash plans were folded into the handoff.
  - `HANDOFF_SWITCH2.md` and `SWITCH2_PORT_PLAN.md` were kept as history: the SDK patch and the
    code point to them.
  - Found on the way: `ubo_constants.py` is used by the player's shader translation, so it moved to
    `switch2/tools/`.
  - The disc extractor was cut down from the desktop installer to its data install. Checked on
    the owner's discs: all files match.
- **2026-11-19: step 2 (code) done; console test passed** (build `0415dbce`, the owner: "works good"). `c79a292` to `cf0b040`.
  - `fastcars/CMakeLists.txt` builds macOS only.
  - `unifdef` removed `_WIN32`, `__ANDROID__`, `TARGET_OS_IPHONE`, `__linux__` and the x86 branches
    from `fastcars/src`, `fastcars/fs` and the disc extractor (about 1,500 lines).
  - Verified: the Switch host objects, one game object with the new prefix header, the Mac host,
    and the disc extractor on the builder's Linux host.
- **2026-11-19: full player build from a clean clone** of `89a1d5b` (build.sh in `fastcars-nx2-builder`,
  game data already extracted): passed in **30 minutes** on the owner's Apple silicon Mac
  (amd64 under Rosetta). It covered deps, host tools, codegen, game code and link. Per-stage times
  are still to measure (2.2).

- **2026-11-19: installer milestone 1 landed** (`8ba3311`): `fastcars.py`, `installer/core`, CLI.
- **2026-11-19: milestone 2 (wizard) on the `wizard` branch.** Tested on the Mac: the version list
  and switching (fake releases served locally), checks, game files, console via a fake SD card,
  summary (install vs. reinstall), build, copy with a free-space check, first start (SD), shader
  fetch, done. Not yet tested: the native file dialogs by hand, and the first start over FTP with
  the console. Version switching downloads the tag's zip into the app-data folder instead of
  checking it out in the player's folder (never touches a clone).
  - Fixed on the way: the SDK patch rewrote a generated header on every configure, so every player
    build recompiled the whole SDK (319 steps, ~3 min); now a rebuild is ~12 steps, under a minute.

- **2026-11-19: milestone 2 landed** (`8434007`). Milestone 4's release mechanics on
  `release-v1`: tests against a fake console FTP server (15 tests; a dropped upload is retried),
  `CHANGELOG.md`, `docs/RELEASING.md`, the full commit in source zips. Next: the first release
  `v1.0.0`. Milestone 3 (updates and upkeep) and the builder changes (2.2) follow in later releases.

- **2026-11-19: `v1.0.0` released** (`9413800`). **Milestone 3 on the `upkeep` branch:** translator
  fingerprint and re-translation on updates (tested on the console: 3,618 re-translated, identical),
  settings migrations (template in install.json), maintenance (backup without the discs' install data,
  logs zip, log level, uninstall), disk usage and cleanup levels, a Stop button. 23 unit tests. Not
  yet tested by hand: the automatic first-start detection over Wi-Fi.

## Order of work (each step one or more commits on this branch)

1. **Cleanup 1.0–1.1** (files). Verify the builds (Switch, builder, Mac). *The owner reviews the
   deletion list before the commit.*
2. **Cleanup 1.2** (code paths), file by file. One console test at the end.
3. **Measure** the builder's stages (2.2).
4. **Versioning** (2.1): `make_version.sh`, NACP, CHANGELOG.
5. **Builder changes** (2.2): Dockerfile pinning, deps updated in place, a game-free CMake target,
   and CI for every push (2.5).
6. **Installer core** (3.5) with the CLI, ported from `fastcars-nx2.py` with tests. The current builder
   keeps working throughout.
7. **Installer UI** (3.2), the console state and updates (3.3), and disk usage and cleanup (3.8).
8. **Release mechanics** (2.3, 2.4): the `VERSION` file for source zips, `docs/RELEASING.md`, and a
   test tag `v1.0.0-rc1` (not pushed).
9. **Console tests:** a fresh install on a clean work folder, an update from the current builder's
   install, and a shader fetch. Then the owner publishes.

Everything stays on this branch until step 9 passes, then one merge into `main` (decided).

## Decided (2026-11-19, the owner)

- **Tags:** `v1.0.0` style; the first Switch 2 release is `v1.0.0`.
- **Signing / Intel Macs:** moot since there are no binary releases (earlier: unsigned, Apple
  silicon only).
- **Manifest tooling:** kept, in `tools/manifest/`. That covers `derive/`, `derive_*.toml`,
  `scripts/{derive_entries,find_*,normalize_manifest}.py` and `fastcars_manifest.base_v9.toml` (not
  used by the build).
  - `fastcars/fastcars_manifest.toml` itself stays where it is: the build needs it.
  - It is clean-room safe: only function addresses and sizes from our own analysis.
- **Landing:** everything stays on `cleanup-installer` until all of it is done, then one merge
  into `main`, with the commits kept. **Exception (low profile):** once the neutral-text sweep is
  done, the branch may go to `main` early, so the public repo stops naming the game before the
  rename. The owner decides.
