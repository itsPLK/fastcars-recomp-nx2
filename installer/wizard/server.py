"""The wizard's local server: the page (static/), a small JSON API over installer.core, jobs that
run in the background (build, install, shaders, waiting for the first start) and native file
pickers (tkinter, on the main thread as macOS requires; without tkinter the page asks for a path).
A version switch downloads that version's source and restarts the installer from it on the same
port; the page reconnects by itself."""
import json
import mimetypes
import os
import pathlib
import queue
import sys
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from ..core import build, config, console, install, releases, settings, shaders, source, system
from ..core.paths import APP_DIR, REPO_ROOT
from ..core.report import InstallError
from . import pickers

STATIC = pathlib.Path(__file__).resolve().parent / "static"
MAIN = queue.Queue()  # work for the main thread: ("pick", args, reply queue) or ("exec", argv)


# ---------------------------------------------------------------------------------------------
# Jobs

class Job:
    """One background action. The page polls it: stage, progress, new log lines, the result."""

    def __init__(self, action):
        self.action = action
        self.lines = []
        self.stage_name = ""
        self.fraction = 0.0
        self.running = True
        self.error = None
        self.result = None
        self.lock = threading.Lock()

    # Reporter interface
    def log(self, msg):
        with self.lock:
            self.lines.extend(str(msg).splitlines() or [""])

    def stage(self, name):
        with self.lock:
            self.stage_name = name
            self.lines.append(f"== {name}")

    def progress(self, f):
        self.fraction = max(0.0, min(float(f), 1.0))

    def warn(self, msg):
        self.log(f"warning: {msg}")

    def snapshot(self, since):
        with self.lock:
            return {"action": self.action, "running": self.running, "stage": self.stage_name,
                    "progress": self.fraction, "lines": self.lines[since:], "next": len(self.lines),
                    "error": self.error, "result": self.result}


class Jobs:
    def __init__(self):
        self.current = None
        self.lock = threading.Lock()

    def start(self, action, fn):
        with self.lock:
            if self.current and self.current.running:
                raise InstallError("Another step is still running.")
            job = Job(action)
            self.current = job

        def run():
            try:
                job.result = fn(job)
            except InstallError as e:
                job.error = str(e)
            except OSError as e:
                import errno
                if e.errno == errno.ENOSPC:
                    job.error = ("A disk is full (this computer's work folder or the SD card). Free some "
                                 "space and try again: finished files are kept.")
                else:
                    job.error = f"{e.strerror or e} ({getattr(e, 'filename', '') or ''})"
            except Exception as e:  # a bug: show it instead of hanging the page
                job.error = f"Unexpected error: {e!r}"
            finally:
                job.running = False
        threading.Thread(target=run, daemon=True).start()
        return job


JOBS = Jobs()


# ---------------------------------------------------------------------------------------------
# Actions

def _target(body):
    if body.get("mode") == "sd":
        return console.open_target(sd=body.get("sd"))
    return console.open_target(ftp=body.get("ftp"), ftp_user=body.get("ftp_user", ""),
                               ftp_password=body.get("ftp_password", ""))


def inspect_console(body):
    """What is on the console: the installed version, shaders, saves."""
    target = _target(body)
    try:
        installed = install.read_installed(target)
        spirv = [n for n in target.listdir(f"{APP_DIR}/data/spirv") if n.endswith(".spv")]
        cache = [n for n in target.listdir(f"{APP_DIR}/data/shader_cache") if n.endswith(".bin")]
        has_app = bool(target.listdir(APP_DIR))
        args = target.read_text(settings.ARGS)
        return {"ok": True, "describe": target.describe(),
                "installed": installed.get("version") if installed else None,
                "has_app": has_app, "has_saves": bool(target.listdir(f"{APP_DIR}/data/user")),
                "shaders": len(spirv), "shader_cache": len(cache),
                "settings": settings.read_settings(args) if args else None,
                "free": target.free_bytes()}
    finally:
        target.close()


def wait_first_start(job, body, stop):
    """FTP: waits for the game to run (the console's FTP goes away) and to be closed again (FTP
    back), then fetches and translates the shaders it saved."""
    host, _, port = (body.get("ftp") or "").partition(":")
    port = int(port or console.FTP_PORT)
    job.stage("Waiting for the first start")
    phase = "before"
    seen_down = 0
    while not stop.is_set():
        up = console.probe_ftp(host, port, timeout=2) is not None
        if phase == "before" and not up:
            seen_down += 1
            if seen_down >= 2:
                phase = "running"
                job.stage("The game is running: wait 3-5 minutes, then quit it (HOME, Close)")
        elif phase == "running" and up:
            phase = "closed"
            break
        time.sleep(3)
    if phase != "closed":
        return {"stopped": True}
    time.sleep(3)  # let OunceShell settle
    target = _target(body)
    try:
        n = shaders.fetch_and_translate(config.work_dir(), target, job)
    finally:
        target.close()
    return {"shaders": n}


# ---------------------------------------------------------------------------------------------
# HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "fastcars"

    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)
        try:
            if url.path == "/api/state":
                return self._json(state())
            if url.path == "/api/releases":
                try:
                    rel = releases.fetch_releases()
                except InstallError as e:
                    return self._json({"error": str(e), "releases": []})
                return self._json({"releases": rel})
            if url.path == "/api/job":
                job = JOBS.current
                since = int((q.get("since") or ["0"])[0])
                return self._json(job.snapshot(since) if job else {"running": False, "action": None})
            return self._static(url.path)
        except InstallError as e:
            return self._json({"error": str(e)}, 400)

    def do_POST(self):
        url = urllib.parse.urlparse(self.path)
        try:
            body = self._body()
            if url.path == "/api/config":
                cfg = config.load()
                cfg.update({k: v for k, v in body.items() if k != "ftp_password"})
                config.save(cfg)
                return self._json(state())
            if url.path == "/api/pick":
                native = pickers.native_pick(body.get("kind", "file"), body.get("title") or "Choose",
                                             body.get("initial"))
                if native is not None:
                    return self._json(native)
                reply = queue.Queue()
                MAIN.put(("pick", body, reply))
                return self._json(reply.get())
            if url.path == "/api/consoles":
                return self._json({"consoles": [ip for ip, _ in console.discover_consoles()]})
            if url.path == "/api/sdcards":
                return self._json({"cards": system.sd_cards()})
            if url.path == "/api/console":
                try:
                    return self._json(inspect_console(body))
                except InstallError as e:
                    return self._json({"ok": False, "error": str(e)})
            if url.path == "/api/use":
                return self._json(use_version(body.get("tag")))
            if url.path == "/api/job":
                return self._json(start_job(body))
            if url.path == "/api/job/stop":
                STOP.set()
                return self._json({"ok": True})
            if url.path == "/api/settings":
                target = _target(body)
                try:
                    return self._json({"settings": settings.change(target, body.get("changes") or {})})
                finally:
                    target.close()
            return self._json({"error": "not found"}, 404)
        except InstallError as e:
            return self._json({"error": str(e)}, 400)

    def _static(self, path):
        name = "index.html" if path in ("/", "") else path.lstrip("/")
        f = (STATIC / name).resolve()
        if STATIC not in f.parents or not f.is_file():
            self.send_response(404)
            self.end_headers()
            return
        data = f.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", (mimetypes.guess_type(f.name)[0] or "text/plain") + "; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


STOP = threading.Event()


def state():
    cfg = config.load()
    work = pathlib.Path(config.work_dir())
    job = JOBS.current
    return {
        "version": source.version(),
        "base": releases.base_version(source.version()),
        "root": str(REPO_ROOT),
        "from_release": REPO_ROOT.parent == releases.sources_dir(),
        "work": str(work),
        "work_free": system.free_bytes(work),
        "data_installed": build.data_installed(work),
        "built": install.built_version(work) if build.nro_path(work).is_file() else None,
        "docker": system.docker_state(),
        "docker_url": system.docker_download_url(),
        "apple_silicon": system.apple_silicon(),
        "pickers": PICKERS_AVAILABLE,
        "config": {k: v for k, v in cfg.items() if k != "ftp_password"},
        "job": {"action": job.action, "running": job.running} if job else None,
    }


def use_version(tag):
    """Switches to a release: downloads its source (once) and restarts the installer from it."""
    if not tag or tag == "local":
        return {"restarting": False}
    if releases.base_version(source.version()) == tag and not source.version().endswith("-dirty"):
        return {"restarting": False}
    folder = releases.source_for(tag)
    port = SERVER.server_address[1]
    argv = [sys.executable, str(folder / "fastcars.py"), "--work", config.work_dir(), "gui",
            "--port", str(port), "--no-browser"]
    MAIN.put(("exec", argv))
    return {"restarting": True, "tag": tag}


def start_job(body):
    action = body.get("action")
    work = config.work_dir()
    STOP.clear()
    if action == "build":
        def fn(job):
            with system.KeepAwake():
                nro = build.build(work, job, body.get("disc1"), body.get("disc2"), body.get("tu"))
            return {"nro": str(nro), "version": install.built_version(work)}
    elif action == "install":
        def fn(job):
            target = _target(body)
            try:
                with system.KeepAwake():
                    return install.install(work, target, job, bool(body.get("reset_settings")))
            finally:
                target.close()
    elif action == "shaders":
        def fn(job):
            target = _target(body)
            try:
                return {"shaders": shaders.fetch_and_translate(work, target, job)}
            finally:
                target.close()
    elif action == "wait_first_start":
        def fn(job):
            return wait_first_start(job, body, STOP)
    else:
        raise InstallError(f"Unknown step: {action}")
    JOBS.start(action, fn)
    return {"started": action}


# ---------------------------------------------------------------------------------------------
# Main thread: the server runs on a thread; this thread serves file pickers and restarts.

PICKERS_AVAILABLE = False
SERVER = None


def _pick(tk_root, body):
    from tkinter import filedialog
    kind = body.get("kind", "file")
    title = body.get("title") or "Choose"
    initial = body.get("initial") or str(pathlib.Path.home())
    if not pathlib.Path(initial).exists():
        initial = str(pathlib.Path(initial).parent) if pathlib.Path(initial).parent.exists() else str(pathlib.Path.home())
    tk_root.deiconify()
    tk_root.attributes("-topmost", True)
    tk_root.lift()
    tk_root.update()
    try:
        if kind == "folder":
            path = filedialog.askdirectory(parent=tk_root, title=title, initialdir=initial, mustexist=True)
        else:
            path = filedialog.askopenfilename(parent=tk_root, title=title, initialdir=initial)
    finally:
        tk_root.withdraw()
        tk_root.update()
    return path or None


def _on_terminate(signum, frame):
    raise KeyboardInterrupt  # runs serve()'s cleanup (stops the build containers)


def _new_tk_root():
    """A hidden Tk window for the file dialogs, or None without tkinter (or a display)."""
    try:
        import tkinter
        root = tkinter.Tk()
        root.withdraw()
        return root
    except Exception:
        return None


def serve(work, port=8742, open_browser=True):
    global PICKERS_AVAILABLE, SERVER
    if work:
        cfg = config.load()
        if cfg.get("work") != str(work):
            cfg["work"] = str(work)
            config.save(cfg)
    # Only where no system dialog tool exists is tkinter needed (Windows, bare Linux).
    import signal
    signal.signal(signal.SIGTERM, _on_terminate)
    tk_root = None if pickers.native_pick_available() else _new_tk_root()
    PICKERS_AVAILABLE = tk_root is not None or pickers.native_pick_available()

    ThreadingHTTPServer.allow_reuse_address = True
    SERVER = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    SERVER.daemon_threads = True
    threading.Thread(target=SERVER.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{port}/"
    print(f"The installer is at {url} (version {source.version()}). Keep this window open; "
          "Ctrl+C here to stop it.", flush=True)
    if open_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        while True:
            try:
                item = MAIN.get(timeout=0.1)
            except queue.Empty:
                if tk_root is not None:
                    try:
                        tk_root.update()
                    except Exception:  # its window was closed (or the app quit from the Dock)
                        tk_root = _new_tk_root()
                continue
            if item[0] == "pick":
                _, body, reply = item
                if tk_root is None:
                    reply.put({"unavailable": True})
                else:
                    try:
                        reply.put({"path": _pick(tk_root, body)})
                    except Exception as e:
                        reply.put({"unavailable": True, "error": str(e)})
                        tk_root = _new_tk_root()
            elif item[0] == "exec":
                time.sleep(0.5)  # let the reply reach the page
                SERVER.shutdown()
                SERVER.server_close()
                if tk_root is not None:
                    try:
                        tk_root.destroy()
                    except Exception:
                        pass
                argv = item[1]
                print(f"Restarting from {argv[1]}", flush=True)
                if sys.platform == "win32":  # execv does not replace the process there
                    import subprocess
                    subprocess.Popen(argv)
                    os._exit(0)
                os.execv(argv[0], argv)
    except KeyboardInterrupt:
        pass
    finally:
        from ..core import docker
        docker.stop_builds()
