# Clean room: what may be committed and released

This project works only because it never distributes the publisher's or Microsoft's work.
Rewritten for the Switch 2 port on 2026-11-19. The upstream desktop releases (Windows, Linux and
macOS, tags `v0.1.0-*` to `v0.3.0`) and their tooling are in the tag `pre-cleanup-2026-11-19`.

## The rule

Nothing game-derived is ever committed or released:

- disc images, TU or RPF files, `default.xex` / `default.xexp`, extracted assets;
- the generated C++ (codegen output) and anything compiled from it, so **no `fastcars.nro`**;
- game shaders in any form: the console's `shader_cache`, HLSL, SPIR-V;
- dumps, traces, GPU captures, logs or screenshots of the game.

The player brings their own Disc 1, Disc 2 and Title Update 27. Everything game-derived is made on
the player's computer by the builder, and stays there and on their console.

**Never use the game's leaked source,** not even for names or comments.

## What may be committed and released

- **Our code:** GPL-3.0-or-later, see `LICENSE`.
- **Our patches** to the ReXGlue SDK (BSD), plume (MIT), XenosRecomp (MIT) and glslang
  (`patches/`). The builder applies them to upstream at pinned commits.
- **Our analysis, as addresses, sizes and names we chose:**
  - the manifest (`fastcars/fastcars_manifest.toml`: function addresses and sizes);
  - the codegen partition seed (`tools/game_dll/codegen.partition.json`);
  - the hook list;
  - the manifest tooling (`tools/manifest/`).
- **The known-good hashes** (`switch2/data/expected.txt`): file sizes and SHA-256 only.
- **Release artifacts:** the source at a tag and the installers. The installers drive Docker and
  the build on the player's machine. They contain no game code and no compiled game.

## Licences in the builds players make

- **The NRO, built by the player for themselves:**
  - It links the SDK runtime, which statically includes FFmpeg (LGPL-2.1-or-later, built without
    GPL parts) and binutils' PowerPC disassembler (GPL-3). That is why this project is
    GPL-3.0-or-later.
  - The NRO is never distributed, so there is no binary to ship sources with. The README tells
    players never to share it.
- **The installers:** Python (PSF licence), pywebview (BSD) and PyInstaller's bootloader (GPL with
  an exception for bundled apps). No FFmpeg and no SDK code. Their licence notices ship inside
  them.
- **The XEX retail key** in the runtime stays (upstream decision: Xenia ships it too).

## Checking a release

- **What's in a release:** a release CI step lists every artifact's files and refuses game or
  console extensions and generated-source names.
- **`tools/release/taint_scan.py`** (from the upstream releases, to be adapted to the installers;
  see `docs/CLEANUP_AND_INSTALLER_PLAN.md`):
  - It compares content-defined 32-byte windows of a release against retail-derived files: the
    decrypted executable, the TU, dumped shaders and the generated C++.
  - Windows also found in control files (SDK sources, system libraries) are public constants and
    don't count.
  - Any common run of 128 bytes or more fails.
  - The retail-derived inputs come from the owner's local work folder and never leave it.

## History

- **A shader scrubbed upstream:** one translated game shader
  (`fastcars/src/native/shaders/ps_D258EBE32001470C_tiles.hlsl`) was removed from upstream's whole
  history on 2026-10-29 with git filter-repo. Check with
  `git log --all -- '*ps_D258EBE32001470C*'`, which must print nothing.
- **Upstream script tables, deleted 2026-11-19:** `tools/scripttools/` held script-native names
  (community NativeDB style, not from the game files) and address tables taken from a game script
  (`pro1.natives` and similar). Upstream kept them out of its release packages. They remain in
  history (here and upstream). History was not rewritten for them.
- **If game content is ever committed:** remove it with git filter-repo before anything is pushed.
  If it was pushed, ask GitHub Support to purge cached views of the old commits.
