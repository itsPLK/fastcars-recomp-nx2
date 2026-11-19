"""Looking after an install: backing up saves, collecting logs for a bug report, the log level,
uninstalling."""
import datetime
import io
import pathlib
import zipfile

from . import settings
from .install import STATE
from .paths import APP_DIR
from .report import InstallError

USER = f"{APP_DIR}/data/user"
# Disc 1's install data (content type 00000002, ~8 GB): re-created from the discs, not a save.
INSTALL_DATA = "0000000000000000/545408A7/00000002"


def _walk(target, rel, skip=()):
    """[(relative path, size)] of the files below rel, skipping the given subfolders."""
    out = []
    for name, is_dir, size in target.entries(rel):
        child = f"{rel}/{name}"
        sub = child[len(USER) + 1:] if child.startswith(USER + "/") else child
        if any(sub == s or sub.startswith(s + "/") for s in skip):
            continue
        if is_dir:
            out += _walk(target, child, skip)
        else:
            out.append((child, size))
    return out


def stamp():
    return datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")


def backup_saves(target, dest_dir, reporter):
    """Copies the saves (data/user without the discs' install data) into DEST/saves-<date>/.
    Returns that folder."""
    files = _walk(target, USER, skip=(INSTALL_DATA,))
    if not files:
        raise InstallError("There are no saves on the console yet.")
    dest = pathlib.Path(dest_dir).expanduser() / f"saves-{stamp()}"
    total = sum(s for _, s in files) or 1
    done = 0
    reporter.stage("Backing up your saves")
    for rel, size in files:
        local = dest / rel[len(USER) + 1:]
        local.parent.mkdir(parents=True, exist_ok=True)
        target.get(rel, local)
        done += size
        reporter.progress(done / total)
    reporter.log(f"{len(files)} files ({total / 2**20:.1f} MB) backed up to {dest}")
    return dest


def collect_logs(target, dest_dir, reporter):
    """A zip for a bug report: the logs, install.json and args.txt. Returns its path."""
    # This app's own logs only (fastcars.log, its rotations, the crash notes).
    names = [f"{APP_DIR}/logs/{n}" for n in target.listdir(f"{APP_DIR}/logs")
             if (n.startswith("fastcars") and n.endswith(".log")) or n == "crash_hooks.txt"]
    names += [STATE, settings.ARGS]
    dest = pathlib.Path(dest_dir).expanduser()
    dest.mkdir(parents=True, exist_ok=True)
    out = dest / f"fastcars-logs-{stamp()}.zip"
    reporter.stage("Collecting the logs")
    added = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in names:
            buf = io.BytesIO()
            try:
                target.get_into(rel, buf)
            except Exception:
                continue
            z.writestr(rel[len(APP_DIR) + 1:], buf.getvalue())
            added += 1
    if not added:
        out.unlink()
        raise InstallError("No logs on the console yet.")
    reporter.log(f"{added} files in {out}")
    return out


def set_log_level(target, level):
    return settings.change(target, {"log": level})


def uninstall(target, reporter, delete_saves=False):
    """Removes the app folder from the console. Saves (data/user) stay unless delete_saves."""
    reporter.stage("Removing the game from the console")
    for name, is_dir, _ in target.entries(APP_DIR):
        rel = f"{APP_DIR}/{name}"
        if name == "data" and not delete_saves:
            for sub, sub_dir, _ in target.entries(rel):
                if sub == "user":
                    continue
                if sub_dir:
                    target.remove_tree(f"{rel}/{sub}")
                else:
                    target.delete(f"{rel}/{sub}")
            continue
        if is_dir:
            target.remove_tree(rel)
        else:
            target.delete(rel)
    if delete_saves:
        target.remove_tree(APP_DIR)
    reporter.log("Removed." + ("" if delete_saves else f" Your saves are still in {USER}."))
