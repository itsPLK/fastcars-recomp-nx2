# Your game files (never committed)

Everything in this folder except this README is gitignored. Put your own dumps here:

- `iso/` — the Xbox 360 **Disc 1** (Install) and **Disc 2** (Play) ISOs
- `tu/` — **Title Update 27**: `tu00000009_00000000` (or a folder with `disc002/default.xexp` and
  `disc002/update.rpf`)

Setup copies and checks them against known-good SHA-256 hashes. Anything made from them
(generated C++, game libraries, shaders, the installed game data) is game content too: keep it
out of git and never share it. See "Keeping it clean" in the top-level README.
