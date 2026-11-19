"""This computer: Docker's state, free space, SD cards, keeping the computer awake."""
import os
import pathlib
import platform
import shutil
import subprocess
import sys


def docker_state():
    """"missing", "stopped" or "running"."""
    if not shutil.which("docker"):
        return "missing"
    try:
        r = subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return "stopped"
    return "running" if r.returncode == 0 else "stopped"


def docker_download_url():
    if sys.platform == "win32":
        return "https://docs.docker.com/desktop/setup/install/windows-install/"
    if sys.platform == "darwin":
        return "https://docs.docker.com/desktop/setup/install/mac-install/"
    return "https://docs.docker.com/engine/install/"


def apple_silicon():
    return sys.platform == "darwin" and platform.machine() == "arm64"


def free_bytes(path):
    """Free space where path is (or would be created)."""
    p = pathlib.Path(path)
    while not p.exists() and p.parent != p:
        p = p.parent
    try:
        return shutil.disk_usage(p).free
    except OSError:
        return None


def sd_cards():
    """Mounted volumes that look like a Switch SD card (a switch/ folder at the top)."""
    roots = []
    if sys.platform == "win32":
        import string
        roots = [pathlib.Path(f"{d}:\\") for d in string.ascii_uppercase if d not in "AC"]
    elif sys.platform == "darwin":
        roots = [p for p in pathlib.Path("/Volumes").glob("*")]
    else:
        user = os.environ.get("USER", "")
        for base in (f"/media/{user}", f"/run/media/{user}", "/media", "/mnt"):
            roots += list(pathlib.Path(base).glob("*"))
    out = []
    for r in roots:
        try:
            if (r / "switch").is_dir():
                out.append(str(r))
        except OSError:
            continue
    return out


class KeepAwake:
    """Keeps the computer from sleeping while a build or a copy runs (best effort)."""

    def __init__(self):
        self.proc = None

    def __enter__(self):
        try:
            if sys.platform == "darwin" and shutil.which("caffeinate"):
                self.proc = subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())])
            elif sys.platform == "win32":
                import ctypes
                ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
                ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
            elif shutil.which("systemd-inhibit"):
                self.proc = subprocess.Popen(["systemd-inhibit", "--what=sleep:idle", "--why=building",
                                              "sleep", "infinity"], stdout=subprocess.DEVNULL,
                                             stderr=subprocess.DEVNULL)
        except OSError:
            self.proc = None
        return self

    def __exit__(self, *exc):
        if self.proc:
            self.proc.terminate()
        if sys.platform == "win32":
            try:
                import ctypes
                ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
            except Exception:
                pass
