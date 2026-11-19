"""What the installer uses on this computer, and cleaning it up. Only its own things: the work
folder and the Docker images labelled docker.LABEL. Never a global Docker prune."""
import os
import pathlib
import shutil
import subprocess

from . import docker
from .lock import work_lock

LEVELS = ("tidy", "build", "all")

# Work folder parts: (key, folders, what it is, what deleting it costs)
PARTS = [
    ("data", ["data"], "Game files from your discs",
     "the next build needs your discs again"),
    ("build", ["build", "codegen", "src"], "Build files",
     "the next build takes about an hour again"),
    ("out", ["out"], "The built game and translated shaders",
     "the next install builds the game first"),
    ("shader_cache", ["shader_cache"], "Shaders saved from the console",
     "after a translator change, a new first start on the console is needed"),
]


def dir_size(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return total


def images():
    """[(tag, size in bytes, is the current version's)] of the installer's Docker images."""
    try:
        r = subprocess.run(["docker", "images", "--filter", f"label={docker.LABEL}",
                            "--format", "{{.Repository}}:{{.Tag}}"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    current = docker.image_tag()
    out = []
    for tag in sorted(set(r.stdout.split())):
        try:
            size = int(subprocess.run(["docker", "image", "inspect", "-f", "{{.Size}}", tag],
                                      capture_output=True, text=True, timeout=30).stdout.strip() or 0)
        except (OSError, subprocess.TimeoutExpired, ValueError):
            size = 0
        out.append((tag, size, tag == current))
    return out


def usage(work):
    work = pathlib.Path(work)
    parts = []
    for key, folders, what, cost in PARTS:
        size = sum(dir_size(work / f) for f in folders if (work / f).exists())
        parts.append({"key": key, "what": what, "cost": cost, "size": size})
    imgs = [{"tag": t, "size": s, "current": c} for t, s, c in images()]
    return {"work": str(work), "parts": parts, "images": imgs}


def plan(work, level, delete_shaders=False):
    """What a cleanup level removes: (folders, image tags)."""
    if level not in LEVELS:
        raise ValueError(level)
    work = pathlib.Path(work)
    imgs = images()
    tags = [t for t, _, current in imgs if not current]
    folders = []
    if level in ("build", "all"):
        tags += [t for t, _, current in imgs if current]
        folders += [work / f for f in ("build", "codegen", "src")]
        # The built game goes; the translated shaders stay (small, and they spare a translation).
        folders += [p for p in (work / "out").glob("*") if p.name not in ("spirv", "spirv.translator")]
    if level == "all":
        folders += [work / "data", work / "out"]
        if delete_shaders:
            folders += [work / "shader_cache"]
    return [f for f in folders if f.exists()], tags


def clean(work, level, reporter, delete_shaders=False):
    """Removes what the level covers. Returns the bytes freed (as measured before)."""
    work = pathlib.Path(work)
    with work_lock(work):
        folders, tags = plan(work, level, delete_shaders)
        freed = 0
        for f in folders:
            size = dir_size(f) if f.is_dir() else f.stat().st_size
            reporter.log(f"Removing {f} ({size / 2**30:.1f} GB)")
            if f.is_dir():
                shutil.rmtree(f, ignore_errors=True)
            else:
                f.unlink()
            freed += size
        sizes = {t: s for t, s, _ in images()}
        for t in tags:
            reporter.log(f"Removing the Docker image {t} ({sizes.get(t, 0) / 2**30:.1f} GB)")
            subprocess.run(["docker", "image", "rm", t], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            freed += sizes.get(t, 0)
        reporter.log(f"Freed about {freed / 2**30:.1f} GB.")
        reporter.log("Docker Desktop keeps its own disk image: on Windows it may only shrink after "
                     "Docker Desktop's \"Clean / Purge data\". Docker's build cache can be cleared there "
                     "too (it isn't the installer's alone).")
        return freed
