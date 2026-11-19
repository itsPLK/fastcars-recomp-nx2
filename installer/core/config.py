"""The wizard's remembered choices (work folder, console, file paths), in the app-data folder,
shared by every version of the installer."""
import json

from .paths import app_data_dir, default_work_dir

FILE = "config.json"


def load():
    p = app_data_dir() / FILE
    try:
        return json.loads(p.read_text())
    except (OSError, ValueError):
        return {}


def save(cfg):
    d = app_data_dir()
    d.mkdir(parents=True, exist_ok=True)
    tmp = d / (FILE + ".part")
    tmp.write_text(json.dumps(cfg, indent=2))
    tmp.replace(d / FILE)


def work_dir():
    """The chosen work folder, else the default."""
    return load().get("work") or str(default_work_dir())
