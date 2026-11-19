# Contributing

Thanks for helping. Two kinds of rules keep this project online.

## Keep a low profile

Everywhere in this project (issues, pull requests, commit messages, code, comments and docs):

- **Never name the game, its publisher or its studio.** Write "the game".
- **No screenshots or videos of the game.** Describe what you see in words. If a picture is really
  needed to fix something, a maintainer will ask for it privately.

Posts that break these rules are edited or deleted.

## Clean room

See [`docs/RELEASE_CLEANROOM.md`](docs/RELEASE_CLEANROOM.md). In short:

- **Never commit anything made from the game,** not even temporarily: disc, title update or `.rpf`
  files, `default.xex` / `default.xexp`, extracted assets, the generated C++, a built `fastcars.nro`,
  shaders in any form (`shader_cache`, HLSL, SPIR-V), dumps, logs or captures of the game. Git
  history is permanent.
- **Never use the game's leaked source code,** not even for names or comments.
- **Your own analysis is fine:** function addresses, sizes and hook names you worked out yourself.

## Bug reports

Use the bug report form (Issues → New issue). Attach the log (`sdmc:/switch/fastcars/logs/fastcars.log`,
written with `--log_level=debug`): it contains no game files and tells us the most.

## Code

Development notes are in [`AGENTS.md`](AGENTS.md) (building, testing on the Mac and the console)
and the docs it points to.
