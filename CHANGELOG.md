# Changelog

Each release is a tag on `main`; the installer (`python3 fastcars.py`) lists them and builds the
one you choose from your own discs. Versions with a suffix (`-rc1`, `-beta.2`) are test versions.

## v1.0.0

The first release.

- **Runs on Switch 2 with kaiHEN,** handheld and docked. With the defaults (full 1280x720 resolution,
  TAA with the FSR 2 upscaler, interpolated frame generation) the screen runs at about 60 fps.
- **Step-by-step installer:** `python3 fastcars.py` opens it in your browser. It picks the version,
  checks Docker and free space, takes your discs (ISO images or extracted folders) through your
  system's file dialogs, finds the console on your network (or uses the SD card), builds, copies,
  and handles the shaders after the first start. Saves and settings are kept on every update.
- **In-game settings menu:** press [-] and [+] together for resolution, anti-aliasing, upscaler,
  frame generation, 1080p display, frame rate cap, FPS counter and CPU boost.
- **Fixed:** the one-frame black or blue sky flashes while driving fast.
- The homebrew menu shows the game's title and icon from your own disc, with this version.

Known issues: cutscenes sometimes freeze for a moment or play without speech. Frame generation can
show slight artefacts on fast edges, and the car's shadow can shimmer a little.
