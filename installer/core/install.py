"""Copying the build and the game files to the console, and what is installed there."""
import datetime
import json
import pathlib

from . import settings, source
from .lock import work_lock
from .paths import APP_DIR, NRO
from .report import InstallError

STATE = f"{APP_DIR}/install.json"


def _local_files(root):
    root = pathlib.Path(root)
    return sorted(p for p in root.rglob("*") if p.is_file() and not p.name.endswith(".part"))


def copy_tree(target, local_root, remote_root, reporter, done_bytes, total_bytes):
    """Copies the files that are missing on the console or differ in size. Returns (bytes counted,
    files copied)."""
    copied = 0
    for p in _local_files(local_root):
        rel = f"{remote_root}/{p.relative_to(local_root).as_posix()}"
        size = p.stat().st_size
        if target.size(rel) != size:
            reporter.log(f"  {rel} ({size / 2**20:.0f} MB)")
            target.put(p, rel)
            copied += 1
        done_bytes += size
        reporter.progress(done_bytes / max(total_bytes, 1))
    return done_bytes, copied


def read_installed(target):
    """install.json from the console, or None."""
    text = target.read_text(STATE)
    if not text:
        return None
    try:
        return json.loads(text)
    except ValueError:
        return None


def install(work, target, reporter, reset_settings=False):
    """Copies the game files, the translated shaders and the NRO; saves are never overwritten.
    install.json is written last, so it only exists for a complete install."""
    work = pathlib.Path(work).resolve()
    out = work / "out" / APP_DIR
    if not (out / NRO).is_file():
        raise InstallError("Nothing built yet: build first.")
    with work_lock(work):
        from . import shaders  # (imports install)
        shaders.ensure_current(work, target, reporter)
        reporter.stage(f"Copying to the console ({target.describe()})")
        previous = read_installed(target) or {}
        parts = [(work / "data" / "game", f"{APP_DIR}/data/game"),
                 (work / "data" / "update", f"{APP_DIR}/data/update")]
        spirv = work / "out" / "spirv"
        if spirv.is_dir():
            parts.append((spirv, f"{APP_DIR}/data/spirv"))
        # Saves live in data/user: only copied when the console has none yet.
        user_on_console = bool(target.listdir(f"{APP_DIR}/data/user"))
        if not user_on_console:
            parts.append((work / "data" / "user", f"{APP_DIR}/data/user"))
        else:
            reporter.log("  saves (data/user) are already on the console: kept as they are")
        files = [p for local, _ in parts for p in _local_files(local)]
        total = sum(p.stat().st_size for p in files) + (out / NRO).stat().st_size
        _check_space(target, parts, out, reporter)
        done = 0
        reporter.log("Game files (about 16 GB the first time; files already there are skipped):")
        for local, remote in parts:
            done, _ = copy_tree(target, local, remote, reporter, done, total)
        template = (out / "args.txt").read_text(encoding="utf-8")
        current = target.read_text(f"{APP_DIR}/args.txt")
        if reset_settings or current is None:
            reporter.log("  args.txt (default settings)")
            target.put(out / "args.txt", f"{APP_DIR}/args.txt")
        else:
            migrated = settings.migrate(current, previous.get("args_template"), template)
            if migrated != current:
                reporter.log("  args.txt: settings you never changed follow this version's defaults")
                target.write_text(f"{APP_DIR}/args.txt", migrated)
        reporter.log(f"  {NRO}")
        target.put(out / NRO, f"{APP_DIR}/{NRO}")
        reporter.progress(1.0)
        state = {
            "version": built_version(work),
            "installed": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        }
        state["args_template"] = template  # to tell the player's own settings from defaults later
        translator = shaders.local_translator(work) if spirv.is_dir() else previous.get("translator")
        if translator:
            state["translator"] = translator
        target.write_text(STATE, json.dumps(state, indent=2) + "\n")
        # Not part of install.json: whether the console has translated shaders (else a first start
        # and a shader fetch are next).
        return dict(state, shaders_on_console=bool(
            [n for n in target.listdir(f"{APP_DIR}/data/spirv") if n.endswith(".spv")]))


def _check_space(target, parts, out, reporter):
    """Fails early, with a clear message, when the SD card can't hold what is still missing."""
    free = target.free_bytes()
    if free is None:  # FTP doesn't report it
        return
    missing = (out / NRO).stat().st_size
    for local, remote in parts:
        for p in _local_files(local):
            rel = f"{remote}/{p.relative_to(local).as_posix()}"
            if target.size(rel) != p.stat().st_size:
                missing += p.stat().st_size
    if missing + (256 << 20) > free:
        raise InstallError(f"Not enough free space on the SD card: {missing / 2**30:.1f} GB still to copy, "
                           f"{free / 2**30:.1f} GB free.")
    reporter.log(f"  {missing / 2**30:.1f} GB to copy, {free / 2**30:.1f} GB free on the SD card")


def built_version(work):
    """The version of the NRO in the work folder (out/BUILT, written when a build finishes), or
    None without a build."""
    f = pathlib.Path(work) / "out" / "BUILT"
    if f.is_file():
        return f.read_text().strip()
    c, v = pathlib.Path(work) / "src" / "COMMIT", pathlib.Path(work) / "src" / "VERSION"
    if c.is_file() and v.is_file():  # builds from before out/BUILT existed
        return f"v{v.read_text().strip()}-{c.read_text().strip()}"
    return None
