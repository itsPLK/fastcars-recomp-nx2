"""The source the build uses: this repository itself, copied into WORK/src.

The copy is synced (changed files copied, removed files deleted), and the upstream checkouts the
build adds there (rexglue-sdk/, thirdparty/) are kept, so a rebuild only redoes what changed.
WORK/src/COMMIT records the commit, for builds without git.
"""
import filecmp
import os
import pathlib
import shutil
import subprocess

from .paths import LEGACY_WORK_NAME, REPO_ROOT

# Never copied (and never deleted from WORK/src): build products, the checkouts the build makes,
# local game files, work folders.
SKIP_TOP = {".git", "rexglue-sdk", "thirdparty", "logs", "gamefiles", LEGACY_WORK_NAME, "COMMIT"}
SKIP_ANY = {"__pycache__", ".DS_Store"}


def _git(*args):
    try:
        r = subprocess.run(["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True)
    except OSError:
        return None
    return r.stdout if r.returncode == 0 else None


def commit():
    """The commit of this source: from git (with "-dirty" for local changes), else from the
    COMMIT file a source zip carries (git archive export-subst), else "unknown"."""
    head = _git("rev-parse", "--short=8", "HEAD")
    if head:
        dirty = _git("status", "--porcelain", "--untracked-files=no")
        return head.strip() + ("-dirty" if dirty and dirty.strip() else "")
    f = REPO_ROOT / "COMMIT"
    if f.is_file():
        text = f.read_text().strip()
        if text and not text.startswith("$"):
            return text[:8]
    return "unknown"


def version():
    base = (REPO_ROOT / "VERSION").read_text().strip() if (REPO_ROOT / "VERSION").is_file() else "0.0.0"
    return f"v{base}-{commit()}"


def source_files():
    """Relative paths of the files that make up this source."""
    listed = _git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
    if listed is not None:
        files = [f for f in listed.split("\0") if f]
        return sorted(f for f in files if (REPO_ROOT / f).is_file() and _keep(f))
    files = []
    for dirpath, dirnames, filenames in os.walk(REPO_ROOT):
        rel_dir = pathlib.Path(dirpath).relative_to(REPO_ROOT)
        if rel_dir == pathlib.Path("."):
            dirnames[:] = [d for d in dirnames if d not in SKIP_TOP]
        dirnames[:] = [d for d in dirnames if d not in SKIP_ANY and not d.startswith("build")]
        for name in filenames:
            rel = (rel_dir / name).as_posix()
            if _keep(rel):
                files.append(rel)
    return sorted(files)


def _keep(rel):
    parts = rel.split("/")
    return parts[0] not in SKIP_TOP and not any(p in SKIP_ANY for p in parts)


def sync_source(work, reporter):
    """Makes WORK/src a copy of this source. Returns the commit it was synced from."""
    src = pathlib.Path(work) / "src"
    src.mkdir(parents=True, exist_ok=True)
    if (src / ".git").exists():
        # A work folder from the previous builder held a git clone: its upstream checkouts and
        # build trees stay; the clone's own metadata goes.
        shutil.rmtree(src / ".git", ignore_errors=True)
    files = source_files()
    wanted = set(files)
    copied = 0
    for rel in files:
        a, b = REPO_ROOT / rel, src / rel
        if b.is_file() and filecmp.cmp(a, b, shallow=False):
            continue
        b.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(a, b)
        copied += 1
    removed = 0
    for dirpath, dirnames, filenames in os.walk(src, topdown=True):
        rel_dir = pathlib.Path(dirpath).relative_to(src)
        if rel_dir == pathlib.Path("."):
            dirnames[:] = [d for d in dirnames if d not in SKIP_TOP]
        for name in filenames:
            rel = (rel_dir / name).as_posix()
            if _keep(rel) and rel not in wanted:
                (src / rel).unlink()
                removed += 1
    c = commit()
    (src / "COMMIT").write_text(c + "\n")
    reporter.log(f"Source {version()}: {copied} files updated, {removed} removed")
    return c
