# Releasing

A release is a tag on `main` plus a GitHub release with notes and **no uploaded files** (the
installer builds everything from the player's own discs). The installer lists the releases through
GitHub's API and builds the one the player picks. Only the owner decides when to release.

**Low profile:** the version, the tag, the release title and the notes never name the game, its
publisher or its studio (see AGENTS.md).

## Versions

- `vMAJOR.MINOR.PATCH`, e.g. `v1.1.0`: a feature release; `v1.0.1`: fixes only.
- **Test versions (pre-releases):** a suffix, `v1.1.0-rc1` or `v1.1.0-beta.2`, and GitHub's
  "pre-release" flag. The installer hides them unless "Show test versions" is on (it turns on by
  itself for a console that already runs one). `v1.1.0-rc1` sorts before `v1.1.0`.
- The `VERSION` file holds the version without the `v` (`1.1.0`, `1.1.0-rc1`). Builds show
  `v<VERSION>-<commit>` in the homebrew menu; source zips get the commit through `COMMIT`
  (`export-subst` in `.gitattributes`).

## Steps

1. **Everything is on `main` and console-tested** (AGENTS.md: only tested work lands there).
2. **On a branch** `release-vX.Y.Z` from `main`:
   - set `VERSION`;
   - add the release's section at the top of `CHANGELOG.md`: what players notice, in plain words,
     with known issues;
   - commit, and land it on `main` as one commit ("Release vX.Y.Z").
3. **Test the release build:** run the installer from a clean clone (or the GitHub zip of `main`)
   once on a fresh work folder and once as an update over the console's install. The homebrew menu
   must show `vX.Y.Z-<commit>`.
4. **Tag** the commit on `main` and push **only that tag**:
   ```sh
   git tag -a vX.Y.Z -m vX.Y.Z main
   git push origin vX.Y.Z
   ```
   Never `git push --tags`: the owner's checkout still has old tags from the earlier history.
5. **GitHub release** for the tag, the notes being its CHANGELOG section, no files:
   ```sh
   gh release create vX.Y.Z --title vX.Y.Z --notes-file notes.md            # a release
   gh release create vX.Y.Z-rc1 --title vX.Y.Z-rc1 --notes-file notes.md --prerelease
   ```
6. **Check:** `python3 fastcars.py` lists the new version (test versions with the checkbox), and
   picking it from an older folder downloads it and restarts the installer from it.

## Fixing a release

Don't move a published tag (installers may have downloaded it). Release a fix as the next patch
version (`vX.Y.Z+1`). A wrong release can be deleted on GitHub (the installer then stops listing
it); the tag stays.
