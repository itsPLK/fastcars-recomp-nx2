"""The command line: the same steps as the wizard, for headless use and development.

  fastcars.py build --disc1 D1 --disc2 D2 --tu TU     (the discs only for the first build)
  fastcars.py install (--ftp HOST[:PORT] | --sd PATH) [--reset-settings]
  fastcars.py shaders (--ftp ... | --sd ...)
  fastcars.py settings (--ftp ... | --sd ...) [--fps-counter on|off] [--log ...] [...]
  fastcars.py find                                     consoles with FTP on the local network
  fastcars.py status                                   this version, the work folder
  fastcars.py gui                                      the browser page
"""
import argparse
import json
import pathlib
import subprocess
import sys

from .core import build, console, install, settings, shaders, source
from .core.paths import REPO_ROOT, default_work_dir
from .core.report import InstallError, Reporter


def _target(args):
    return console.open_target(args.sd, args.ftp, args.ftp_user or "", args.ftp_password or "")


def cmd_build(args, r):
    nro = build.build(args.work, r, args.disc1, args.disc2, args.tu)
    r.log(f"\nBuilt {install.built_version(args.work)}: {nro}\nNext: install it (install --ftp ... or --sd ...)")


def cmd_install(args, r):
    target = _target(args)
    try:
        state = install.install(args.work, target, r, args.reset_settings)
    finally:
        target.close()
    r.log(f"\nInstalled {state['version']}.")
    if not state["shaders_on_console"]:
        r.log("First start: open the homebrew menu in full-memory mode (hold R while starting a game) and "
              "start the game. Press A at the \"No shaders yet\" screen, press B when the black screen "
              "turns grey, wait 3-5 minutes until you hear the game itself, quit (HOME, Close), then "
              "run 'shaders'.")


def cmd_shaders(args, r):
    target = _target(args)
    try:
        n = shaders.fetch_and_translate(args.work, target, r)
    finally:
        target.close()
    r.log(f"Done ({n} shaders). Objects that were missing are drawn from the next start on.")


def cmd_settings(args, r):
    target = _target(args)
    try:
        current = settings.change(target, {
            "fps_counter": args.fps_counter, "log": args.log, "fps_limit": args.fps_limit,
            "cpu_boost": args.cpu_boost, "resolution": args.resolution, "taa": args.taa})
    finally:
        target.close()
    if args.machine:
        print("SETTINGS " + json.dumps(current), flush=True)
    else:
        r.log("\n".join(f"{k}: {v}" for k, v in current.items()))


def cmd_find(args, r):
    r.log("Searching the local network for consoles (FTP on port 1337)...")
    found = console.discover_consoles()
    for ip, banner in found:
        r.log(f"  {ip}:1337  {banner}")
    if not found:
        r.log("None found. Is OunceShell open on the console, and the game closed? Otherwise enter the "
              "address OunceShell shows.")
    if args.machine:
        print("CONSOLES " + json.dumps([ip for ip, _ in found]), flush=True)


def cmd_status(args, r):
    work = pathlib.Path(args.work)
    r.log(f"version:      {source.version()}")
    r.log(f"work folder:  {work}")
    r.log(f"game files:   {'extracted and checked' if build.data_installed(work) else 'not yet (the first build needs your discs)'}")
    nro = build.nro_path(work)
    r.log(f"built game:   {nro if nro.is_file() else 'not yet'}")


def cmd_update(args, r):
    if (REPO_ROOT / ".git").exists():
        r.log("Updating this folder with git (git pull --ff-only):")
        subprocess.run(["git", "-C", str(REPO_ROOT), "pull", "--ff-only"], check=False)
        r.log(f"Now at {source.version()}. Build, then install, to update the console.")
    else:
        r.log(f"This is {source.version()}. To update, download the newest release's source and run "
              "its fastcars.py: your work folder (and so your extracted game files) is reused.")


def cmd_gui(args, r):
    from . import page
    page.serve(args)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="fastcars.py",
                                 description="Build the game from your own discs and install it on Switch 2.")
    ap.add_argument("--work", default=str(default_work_dir()), help="work folder (default: %(default)s)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def target_opts(p):
        p.add_argument("--sd", help="the console's SD card, mounted on this computer")
        p.add_argument("--ftp", help="the console's FTP server, HOST[:PORT] (port 1337 by default)")
        p.add_argument("--ftp-user", help="FTP user (default: anonymous)")
        p.add_argument("--ftp-password", help="FTP password")

    p = sub.add_parser("build", help="build the game (the discs only for the first build)")
    p.add_argument("--disc1", help="Disc 1 (Install): ISO image or extracted folder")
    p.add_argument("--disc2", help="Disc 2 (Play): ISO image or extracted folder")
    p.add_argument("--tu", help="Title Update 27: the file tu00000009_00000000 or a folder")
    p.set_defaults(fn=cmd_build)

    p = sub.add_parser("install", help="copy the build and the game files to the console")
    target_opts(p)
    p.add_argument("--reset-settings", action="store_true", help="replace args.txt with the default")
    p.set_defaults(fn=cmd_install)

    p = sub.add_parser("shaders", help="translate the shaders the console saved and copy them back")
    target_opts(p)
    p.set_defaults(fn=cmd_shaders)

    p = sub.add_parser("settings", help="show or change the game's settings on the console")
    target_opts(p)
    p.add_argument("--fps-counter", choices=["on", "off"])
    p.add_argument("--log", choices=["error", "info", "debug"])
    p.add_argument("--fps-limit", type=int)
    p.add_argument("--cpu-boost", choices=["on", "off"])
    p.add_argument("--resolution", type=int, metavar="PERCENT", help="render resolution, 50-100")
    p.add_argument("--taa", choices=["on", "off"], help="temporal anti-aliasing and upscaling")
    p.add_argument("--machine", action="store_true", help=argparse.SUPPRESS)
    p.set_defaults(fn=cmd_settings)

    p = sub.add_parser("find", help="find consoles with FTP on the local network")
    p.add_argument("--machine", action="store_true", help=argparse.SUPPRESS)
    p.set_defaults(fn=cmd_find)

    p = sub.add_parser("status", help="this version and the work folder")
    p.set_defaults(fn=cmd_status)

    p = sub.add_parser("update", help="update this folder to the newest version (git checkouts)")
    p.set_defaults(fn=cmd_update)

    p = sub.add_parser("gui", help="the browser page")
    p.add_argument("--port", type=int, default=8742)
    p.add_argument("--no-browser", action="store_true")
    p.set_defaults(fn=cmd_gui)

    args = ap.parse_args(argv)
    r = Reporter()
    try:
        args.fn(args, r)
    except InstallError as e:
        print(f"\nError: {e}", file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)
        return 130
    return 0
