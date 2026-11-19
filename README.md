# fastcars-recomp-nx2

A static recompilation of an Xbox 360 game (Title Update 27) from the console's PowerPC executable
into native ARM64 code, running as a homebrew app on Switch 2 consoles with
[kaiHEN](https://github.com/ouncebrew/ounce-devkit).

**No game is included.** You build it yourself from your own Xbox 360 discs and title update. The
result contains the game publisher's copyrighted code and is for you only: don't share it (see
[Rules](#rules-for-forks-and-shared-builds)).

## What you need

- A Switch 2 running kaiHEN, with about 20 GB free on the SD card, and a way to copy files to it: the
  console's FTP server (OunceShell) or an SD card reader.
- Your own Xbox 360 discs of the game: Disc 1 (Install) and Disc 2 (Play), as ISO images or as
  extracted folders (for example a disc copied to the console's hard drive with Aurora: the top folder
  is Disc 1, its `Disc 2` folder is Disc 2). The builder checks them against known-good hashes and
  tells you if they don't match.
- **Title Update 27** from your console: the file `tu00000009_00000000` (on the console's hard drive
  under `Content/0000000000000000/545408A7/000B0000/`).
- A computer with [Docker](https://docs.docker.com/get-docker/), [Git](https://git-scm.com/) and
  [Python 3.9+](https://www.python.org/) (Windows, Linux or macOS; on a Mac with Apple silicon, turn on
  "Use Rosetta for x86/amd64 emulation" in Docker Desktop's settings), and about 60 GB of free disk space.

Your files are checked against known-good hashes before anything is built. Other versions (other
regions' title updates, other disc dumps) are refused.

## Building and installing

```sh
git clone https://github.com/itsPLK/fastcars-recomp-nx2
cd fastcars-recomp-nx2
python3 fastcars.py
```

(Or download the repository as a zip from GitHub and run `python3 fastcars.py` in its folder: git isn't
needed.) The installer opens in your web browser and takes you through it step by step:

1. **Version.** The newest release is preselected; tick "Show test versions" for pre-releases. If you
   pick another version, the installer downloads it and restarts itself from it.
2. **Checks.** Docker running, and a work folder with enough free space (about 60 GB the first time).
3. **Game files.** Your Disc 1, Disc 2 (ISO images or extracted folders) and Title Update 27, picked
   with your system's file dialogs. Only needed once: they're extracted and checked into the work
   folder.
4. **Console.** Over Wi-Fi ("Search my network" finds the console's FTP; OunceShell must be open and
   the game closed) or the SD card in your computer. The installer shows what's installed there.
5. **Build and copy.** The first build takes about an hour (longer on a Mac, where it runs under
   emulation); later ones are much shorter. The first copy is about 16 GB, then only what changed.
   Saves and settings are kept.
6. **First start** (only once per console). Open the homebrew menu in **full-memory mode** (hold R while
   starting any game) and start the game. A screen says there are no shaders yet: press A. The screen
   then stays **black** and you only hear the game: that's expected. When it turns a little grey (a
   message), press **B**. Wait 3-5 minutes, until you hear the game itself past the menus, then quit:
   press **HOME**, then **Close**. Over Wi-Fi the installer notices and fetches the shaders by itself
   once FTP is back; with an SD card, put it back in your computer and press "Fetch shaders now".

That's it: start the game again and play. If you ever see missing objects later, run the installer
again and fetch the shaders again at the end.

The same steps without the browser:

```sh
python3 fastcars.py build --disc1 "Disc 1.iso" --disc2 "Disc 2.iso" --tu tu00000009_00000000
python3 fastcars.py find                              # finds the console's FTP address
python3 fastcars.py install --ftp 192.168.1.20        # or --sd /path/to/the/sd/card
python3 fastcars.py shaders --ftp 192.168.1.20
```

Everything the builder makes stays in its work folder: your system's application data folder
(`python3 fastcars.py status` shows where), or a `fastcars-nx2-work` folder next to `fastcars.py` if
there is one. On the SD
card, the game lives in `sdmc:/switch/fastcars/`: `fastcars.nro`, `args.txt` (settings), `data/` (game files,
saves in `data/user`) and `logs/`.

## Updating

Run `python3 fastcars.py` and pick the newer version at the first step: the installer gets it by
itself, rebuilds (your game files are kept in the work folder; only the code is rebuilt) and updates
the console. What changed in each version: [`CHANGELOG.md`](CHANGELOG.md).

Updating never touches your saves.

**Coming from an early build with a different folder name?** Before installing, rename that app
folder in `sdmc:/switch/` to `fastcars`, delete the old `.nro` and `args.txt` inside it (the install
writes new ones; settings names have changed), and keep the rest: the game files, shaders and your
saves in `data/` are reused.

**Coming from the previous builder?** Clone this repository, then move the previous builder's work
folder next to `fastcars.py` and rename it to `fastcars-nx2-work`. The game files extracted from
your discs are reused; the recompiled code and the build are redone by themselves.

## Managing an install

Run `python3 fastcars.py` and choose **Manage an installed game** to back up your saves, collect logs
for a bug report, set the log level, fetch shaders again, or uninstall (your saves stay unless you
ask). It also shows the space the installer uses on your computer and frees it: "Tidy up" keeps
updates fast, "Delete build files" and "Delete everything" free more (the next build then takes
longer, or needs your discs again). The same from the command line: `backup`, `logs`, `uninstall`,
`storage`, `clean`.

## Playing

- **Exit:** press HOME, then Close.
- **Settings menu:** press **[-]** and **[+]** together (D-pad to move, A to change, B to close). It
  changes the resolution, anti-aliasing, upscaler (FSR 2 / FSR 1), frame generation, 1080p display,
  frame rate cap, FPS counter and CPU boost while you play, and saves them to `args.txt`.
- **Performance:** the game itself renders roughly 30–40 fps, depending on the area and the resolution
  setting; busy streets and fast driving are the heaviest. With **frame generation** on, a generated
  frame is shown between every two real ones, so the screen runs at **about 60 fps**. The game runs the
  CPU at its boost clock (1702 MHz). Handheld and docked both work.
- **Defaults (about 60 fps):** Resolution 1280x720 (full), Anti-aliasing TAA, Upscaler FSR 2 and Frame
  generation Interpolated, with 1080p display off. The 3D scene is rendered at the screen's 1280x720;
  lower resolutions are upscaled by FSR 2 (a temporal upscaler, like AMD FidelityFX Super Resolution 2).
- **For a sharper picture:** turn 1080p display on in the menu. FSR 2 then upscales straight to the
  screen's 1920x1080 and the HUD is drawn at full 1080p, at about 45 fps on screen with the same
  settings.
- **Saves** are in `sdmc:/switch/fastcars/data/user`. To back them up: run the installer, "Manage an
  installed game", Back up saves.

## Settings

In the game's settings menu ([-] + [+]), which saves them to `sdmc:/switch/fastcars/args.txt`. The log level
(for bug reports) isn't in the menu: set it in the installer ("Manage an installed game"), with
`python3 fastcars.py settings --ftp ... --log debug`, or by editing `args.txt`. Updates keep your
settings; a setting you never changed follows the new version's default.

| Setting | Default | |
|---|---|---|
| FPS counter | off | frames per second in the top-left corner |
| Log | error | `info` or `debug` write a detailed `logs/fastcars.log` (for bug reports; slightly slower) |
| Frame rate cap | 60 | `30`, or none |
| CPU boost | on | off: leave the CPU at its normal clock (slower) |
| Resolution | 1280x720 (full) | the 3D scene's size: 1280x720 (full), 1152x648, 1088x612, 960x540, 858x482 or 640x360; below full size it is upscaled (the menu shows each as a share of the screen) |
| Anti-aliasing | TAA | temporal anti-aliasing in place of the game's FXAA (`--fastcars_native_taa=false` for FXAA) |
| Upscaler | FSR 2 | how a smaller scene reaches the screen: FSR 2 (temporal, with TAA; the HUD stays sharp), FSR 1 (spatial, AMD FidelityFX Super Resolution 1) or bilinear |
| Frame generation | Interpolated | Off, Interpolated (smoothest, adds about one frame of latency) or Extrapolated (lower latency, more artefacts): a generated frame between every two real ones, about 60 fps on screen. Needs TAA (`--fastcars_native_frame_gen=1` / `2`) |
| 1080p display | off | renders to the screen's 1920x1080 instead of 1280x720: with FSR 2 the picture and HUD are upscaled to 1080p, otherwise the 720p picture is upscaled with FSR 1. Takes effect after a restart (`--fastcars_native_display_1080p=true`) |

## Known issues

- Cutscenes sometimes freeze for a moment or play without speech.
- Generated frames (frame generation) are made from motion vectors: fast edges can show slight
  artefacts, and the car's shadow can shimmer a little.

## Troubleshooting

- **Black screen, or most of the world is missing:** you skipped Fetch shaders, or the game was quit too
  early. Start it, wait 3-5 minutes until you hear the game itself, quit, Fetch shaders.
- **The game closes during loading:** make sure you started it in full-memory mode (hold R while starting
  a game, then open the homebrew menu).
- **Everything is very slow after trying other homebrew that changes clocks:** clock settings stay set
  after an app exits. Restart the console.
- **The build fails:** the builder shows the error; the full output is in the window (or the terminal).
  Check the free disk space and that Docker is running.
- **Reporting a bug:** use the bug report form in Issues. In the installer's "Manage an installed game",
  set the log level to `debug`, reproduce the problem, then "Collect logs" and attach the zip (it
  contains no game files). Please follow the rules there: don't
  name the game or its publisher, and no screenshots or videos of the game (see
  [`CONTRIBUTING.md`](CONTRIBUTING.md)).

## How it works

The builder runs in a Docker container (`switch2/builder/`): it checks and copies the game files from
your discs, recompiles the game executable to C++ with [ReXGlue](https://github.com/rexglue/rexglue-sdk),
and compiles that together with the runtime and renderer into one statically linked homebrew app
(devkitA64, libounce, OunceVK). The game's Xbox 360 shaders can't be translated on the console, so the
console saves the ones it meets and the builder translates them on your computer.

Developer notes: [`docs/HANDOFF_SWITCH2_NEXT.md`](docs/HANDOFF_SWITCH2_NEXT.md) (state, settings,
profiling) and [`docs/CLEANUP_AND_INSTALLER_PLAN.md`](docs/CLEANUP_AND_INSTALLER_PLAN.md) (releases and the installer).

## Credits

This port is built on the work of others:

- [andrewnakas](https://github.com/andrewnakas)' recompilation: the recompilation, its native
  renderer, setup and tools, which this port is based on.
- [ReXGlue](https://github.com/rexglue/rexglue-sdk): the static recompiler and runtime, derived from the
  [Xenia](https://github.com/xenia-project/xenia) Xbox 360 emulator.
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and [plume](https://github.com/renderbag/plume)
  (from the Unleashed Recompiled project): shader translation and the graphics API layer.
- [ounce-devkit](https://github.com/ouncebrew/ounce-devkit): kaiHEN, libounce (a libnx fork) and OunceVK
  (Mesa's NVK Vulkan driver for Switch 2), on top of [devkitPro](https://devkitpro.org/).

## Rules for forks and shared builds

This project only works because it never distributes the publisher's or Microsoft's work. **Never
share** any of these, in a repository, a release, a mirror or a download link:

- disc images, extracted game files, `default.xex` / `default.xexp`, `.rpf` archives, title updates
- the generated C++ code, a built `fastcars.nro`, or anything else with the game compiled in
- game shaders in any form (the console's `shader_cache`, HLSL, SPIR-V in `data/spirv`)
- your work folder, your SD card's `switch/fastcars` folder, memory dumps and GPU captures

When working on the code, never use the game's leaked source code (not even for names or comments),
and never commit anything made from the game, even temporarily: git history is permanent. Function
addresses, sizes and hook names from your own analysis are fine.

If you find game content in this repository or in a fork that links here, please open an issue
(without attaching it or naming the game).

## License

GNU GPL version 3 or later (`LICENSE`), for this project's own code. Third-party components keep their
own licenses. The GPL grants no rights to the publisher's or Microsoft's work.

All trademarks belong to their owners. This project is not affiliated with or endorsed by the game's
publisher, Microsoft or Nintendo.
