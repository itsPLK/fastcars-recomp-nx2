"""Native file and folder dialogs. macOS: osascript (the system's own dialog; tkinter's are
unreliable with Apple's bundled Python). Linux: zenity or kdialog when present. Otherwise (Windows,
or neither tool): tkinter, run on the main thread by server.py. Without any of these, the page asks
for a typed path."""
import pathlib
import shutil
import subprocess
import sys


def _initial(path):
    p = pathlib.Path(path) if path else pathlib.Path.home()
    while not p.exists() and p.parent != p:
        p = p.parent
    return p if p.exists() else pathlib.Path.home()


def _applescript_text(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def native_pick(kind, title, initial):
    """{"path": str|None} from a system dialog, or None when this system has no dialog tool
    (then tkinter is tried)."""
    start = _initial(initial)
    if sys.platform == "darwin" and shutil.which("osascript"):
        what = "folder" if kind == "folder" else "file"
        folder = start if start.is_dir() else start.parent
        script = (f"POSIX path of (choose {what} with prompt {_applescript_text(title)} "
                  f"default location (POSIX file {_applescript_text(folder)}))")
        r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True)
        if r.returncode != 0:  # cancelled (error -128) or no dialog possible
            return {"path": None}
        path = r.stdout.strip()
        if kind == "folder" and len(path) > 1:
            path = path.rstrip("/")
        return {"path": path or None}
    if sys.platform.startswith("linux"):
        if shutil.which("zenity"):
            cmd = ["zenity", "--file-selection", f"--title={title}", f"--filename={start}/"]
            if kind == "folder":
                cmd.append("--directory")
        elif shutil.which("kdialog"):
            cmd = ["kdialog", "--title", title,
                   "--getexistingdirectory" if kind == "folder" else "--getopenfilename", str(start)]
        else:
            return None
        r = subprocess.run(cmd, capture_output=True, text=True)
        return {"path": r.stdout.strip() or None if r.returncode == 0 else None}
    return None


def native_pick_available():
    if sys.platform == "darwin":
        return bool(shutil.which("osascript"))
    if sys.platform.startswith("linux"):
        return bool(shutil.which("zenity") or shutil.which("kdialog"))
    return False
