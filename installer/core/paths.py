"""Where things are: the repository (the installer's own source), the work folder, the console."""
import os
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
NAME = "fastcars"
APP_DIR = "switch/fastcars"          # on the console's SD card
NRO = "fastcars.nro"
LEGACY_WORK_NAME = "fastcars-nx2-work"  # the previous builder's work folder, next to the scripts


def app_data_dir():
    """The OS's folder for application data."""
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser(r"~\AppData\Local")
        return pathlib.Path(base) / NAME
    if sys.platform == "darwin":
        return pathlib.Path.home() / "Library" / "Application Support" / NAME
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return pathlib.Path(base) / NAME


def default_work_dir():
    """The previous builder's work folder when it sits next to the scripts (its extracted game
    files are reused), else the OS's application data folder."""
    legacy = REPO_ROOT / LEGACY_WORK_NAME
    if legacy.is_dir():
        return legacy
    return app_data_dir() / "work"
