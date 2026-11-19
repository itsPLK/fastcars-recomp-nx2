#!/usr/bin/env python3
"""fastcars-nx2: builds the game for Nintendo Switch 2 (kaiHEN) from your own Xbox 360 discs and installs
it on the console. Needs Python 3.9+ and Docker; nothing else is installed on your computer.

  python3 fastcars-nx2.py gui                       the builder window (in your web browser)
  python3 fastcars-nx2.py build --disc1 D1.iso --disc2 D2.iso --tu tu00000009_00000000 [--commit HASH]
  python3 fastcars-nx2.py install (--sd /Volumes/SWITCH | --ftp 192.168.1.20[:1337])
  python3 fastcars-nx2.py shaders (--sd ... | --ftp ...)
  python3 fastcars-nx2.py settings (--sd ... | --ftp ...) [--fps-counter on|off] [--log error|info|debug]
                               [--fps-limit N] [--cpu-boost on|off] [--resolution PERCENT] [--taa on|off]
  python3 fastcars-nx2.py update                     is a newer version of the source available?

Everything is kept in one work folder (default: fastcars-nx2-work next to this file): the source,
the game files from your discs, the recompiled code and the build. It is yours alone: never share it,
nor the fastcars.nro it produces (they contain the publisher's copyrighted code).
"""
import argparse
import ftplib
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPO_URL = "https://github.com/itsPLK/fastcars-recomp-nx2"
IMAGE = "ghcr.io/itsplk/fastcars-recomp-nx2-builder:latest"
LOCAL_IMAGE = "fastcars-nx2-builder"
APP_DIR = "switch/fastcars"  # on the SD card
HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_WORK = HERE / "fastcars-nx2-work"
CONFIG_NAME = "fastcars-nx2.json"


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------------------------------------
# The console's SD card: a mounted volume or FTP (OunceShell: port 1337, anonymous login).

class SdTarget:
    def __init__(self, root):
        self.root = pathlib.Path(root)
        if not self.root.is_dir():
            raise SystemExit(f"SD card not found at {self.root}")

    def describe(self):
        return f"SD card at {self.root}"

    def _p(self, rel):
        return self.root / rel

    def size(self, rel):
        p = self._p(rel)
        return p.stat().st_size if p.is_file() else None

    def exists(self, rel):
        return self._p(rel).exists()

    def listdir(self, rel):
        p = self._p(rel)
        return sorted(x.name for x in p.iterdir()) if p.is_dir() else []

    def put(self, local, rel):
        dst = self._p(rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_name(dst.name + ".part")
        shutil.copyfile(local, tmp)
        os.replace(tmp, dst)

    def get(self, rel, local):
        shutil.copyfile(self._p(rel), local)

    def read_text(self, rel):
        p = self._p(rel)
        return p.read_text() if p.is_file() else None

    def write_text(self, rel, text):
        p = self._p(rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def close(self):
        pass


class FtpTarget:
    def __init__(self, host, port=1337, user="", password=""):
        self.host, self.port = host, port
        self.user, self.password = user or "anonymous", password or ""
        self.ftp = None
        self._connect()

    def describe(self):
        return f"FTP {self.host}:{self.port}"

    def _connect(self):
        try:
            self.ftp = ftplib.FTP()
            self.ftp.connect(self.host, self.port, timeout=30)
            self.ftp.login(self.user, self.password)
            self.ftp.set_pasv(True)
            self.ftp.voidcmd("TYPE I")
        except (OSError, ftplib.Error) as e:
            raise SystemExit(f"Could not connect to FTP {self.host}:{self.port} ({e}). Is the FTP app "
                             "(OunceShell) open on the console?")

    def _retry(self, fn):
        for attempt in range(3):
            try:
                return fn()
            except (OSError, EOFError, ftplib.error_temp):
                if attempt == 2:
                    raise
                time.sleep(2)
                self._connect()

    def size(self, rel):
        try:
            return self._retry(lambda: self.ftp.size("/" + rel))
        except ftplib.error_perm:
            return None

    def exists(self, rel):
        if self.size(rel) is not None:
            return True
        parent, name = rel.rsplit("/", 1) if "/" in rel else ("", rel)
        return name in self.listdir(parent)

    def listdir(self, rel):
        try:
            return sorted(n.rsplit("/", 1)[-1] for n in self._retry(lambda: self.ftp.nlst("/" + rel)))
        except ftplib.error_perm:
            return []

    def _mkdirs(self, rel_dir):
        path = ""
        for part in rel_dir.split("/"):
            if not part:
                continue
            path += "/" + part
            try:
                self.ftp.mkd(path)
            except ftplib.error_perm:
                pass

    def put(self, local, rel):
        def go():
            if "/" in rel:
                self._mkdirs(rel.rsplit("/", 1)[0])
            with open(local, "rb") as f:
                self.ftp.storbinary(f"STOR /{rel}", f, blocksize=1 << 20)
        self._retry(go)

    def get(self, rel, local):
        def go():
            with open(local, "wb") as f:
                self.ftp.retrbinary(f"RETR /{rel}", f.write, blocksize=1 << 20)
        self._retry(go)

    def read_text(self, rel):
        chunks = []
        try:
            self._retry(lambda: self.ftp.retrbinary(f"RETR /{rel}", chunks.append))
        except ftplib.error_perm:
            return None
        return b"".join(chunks).decode("utf-8", "replace")

    def write_text(self, rel, text):
        import io
        def go():
            if "/" in rel:
                self._mkdirs(rel.rsplit("/", 1)[0])
            self.ftp.storbinary(f"STOR /{rel}", io.BytesIO(text.encode()))
        self._retry(go)

    def close(self):
        try:
            self.ftp.quit()
        except Exception:
            pass


def open_target(args):
    if getattr(args, "sd", None):
        return SdTarget(args.sd)
    if getattr(args, "ftp", None):
        host, _, port = args.ftp.partition(":")
        return FtpTarget(host, int(port or 1337), args.ftp_user or "", args.ftp_password or "")
    raise SystemExit("Choose where the console's SD card is: --sd PATH or --ftp HOST[:PORT]")


# ---------------------------------------------------------------------------------------------
# Docker and the source

def run(cmd, cwd=None, check=True):
    log("$ " + " ".join(shlex.quote(str(c)) for c in cmd))
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1)
    for line in p.stdout:
        print(line, end="", flush=True)
    p.wait()
    if check and p.returncode != 0:
        raise SystemExit(f"command failed (exit {p.returncode})")
    return p.returncode


def need_docker():
    if not shutil.which("docker"):
        raise SystemExit("Docker is not installed (https://docs.docker.com/get-docker/).")
    if subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        raise SystemExit("Docker is installed but not running. Start Docker Desktop (or the docker service).")


def ensure_image(work):
    """The builder image: pulled from the project's registry (a quick check when it is current, so
    a newer image after a toolchain change arrives by itself), else the local one, else built."""
    if run(["docker", "pull", "--platform", "linux/amd64", IMAGE], check=False) == 0:
        run(["docker", "tag", IMAGE, LOCAL_IMAGE])
        return LOCAL_IMAGE
    if subprocess.run(["docker", "image", "inspect", LOCAL_IMAGE], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode == 0:
        log("Using the builder image already on this computer (the registry could not be reached)")
        return LOCAL_IMAGE
    log("Building the builder image from the source (once, several minutes)")
    run(["docker", "build", "--platform", "linux/amd64", "-t", LOCAL_IMAGE, str(work / "src" / "switch2" / "builder")])
    return LOCAL_IMAGE


def git(*args, cwd=None, check=True):
    return subprocess.run(["git", *args], cwd=cwd, check=check, capture_output=True, text=True).stdout.strip()


def latest_commit(repo=REPO_URL):
    out = git("ls-remote", repo, "HEAD")
    return out.split()[0] if out else None


def fetch_source(work, commit=None, repo=REPO_URL):
    src = work / "src"
    if not (src / ".git").is_dir():
        log(f"Downloading the source from {repo}")
        run(["git", "clone", repo, str(src)])
    else:
        # A work folder from an earlier builder may still point at another repository.
        run(["git", "-C", str(src), "remote", "set-url", "origin", repo])
        run(["git", "-C", str(src), "fetch", "--quiet", "--prune", "origin"])
    target = commit or "origin/HEAD"
    run(["git", "-C", str(src), "checkout", "--quiet", "--detach", target])
    log(f"Source at {git('-C', str(src), 'rev-parse', '--short=8', 'HEAD')}")


def docker_run(work, image, args, inputs=()):
    cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{work}:/work"]
    if sys.platform.startswith("linux"):
        # Native Docker runs as root: files in the work folder would be root's. Run as the user
        # (Docker Desktop on macOS and Windows maps ownership itself).
        (pathlib.Path(work) / ".home").mkdir(parents=True, exist_ok=True)
        cmd += ["--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/work/.home"]
    for host_path, ctr in inputs:
        cmd += ["-v", f"{host_path}:{ctr}:ro"]
    cmd += [image, "sh", "/work/src/switch2/builder/build.sh", *args]
    run(cmd)


def cmd_build(args):
    if not shutil.which("git"):
        raise SystemExit("git is not installed.")
    need_docker()
    work = pathlib.Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    fetch_source(work, args.commit, args.repo)
    image = ensure_image(work)
    inputs, ctr_args = [], []
    if not (work / "data" / "data_installed.txt").exists():
        for name in ("disc1", "disc2", "tu"):
            p = getattr(args, name)
            if not p:
                raise SystemExit("The first build needs --disc1, --disc2 and --tu (your ISOs and the TU27 file).")
            p = pathlib.Path(p).resolve()
            if not p.exists():
                raise SystemExit(f"Not found: {p}")
            inputs.append((p, f"/input/{name}"))
            ctr_args.append(f"/input/{name}")
    docker_run(work, image, ["build", *ctr_args], inputs)
    log(f"\nBuilt: {work / 'out' / APP_DIR / 'fastcars.nro'}\nNext: install it (fastcars-nx2.py install --sd ... or --ftp ...)")


# ---------------------------------------------------------------------------------------------
# Install, shaders, settings

def copy_tree(target, local_root, remote_root, skip_existing_dir=False):
    """Copies files that are missing or differ in size. Returns (copied, skipped)."""
    local_root = pathlib.Path(local_root)
    if skip_existing_dir and target.listdir(remote_root):
        log(f"  {remote_root} is already on the console: kept as it is")
        return 0, 0
    files = sorted(p for p in local_root.rglob("*") if p.is_file())
    total = sum(p.stat().st_size for p in files)
    done = copied = skipped = 0
    for p in files:
        rel = f"{remote_root}/{p.relative_to(local_root).as_posix()}"
        size = p.stat().st_size
        if target.size(rel) == size:
            skipped += 1
        else:
            log(f"  {rel} ({size >> 20} MB)")
            target.put(p, rel)
            copied += 1
        done += size
        print(f"PROGRESS {int(done * 100 / max(total, 1))}", flush=True)
    return copied, skipped


def cmd_install(args):
    work = pathlib.Path(args.work).resolve()
    out = work / "out" / APP_DIR
    if not (out / "fastcars.nro").exists():
        raise SystemExit("Nothing built yet: run build first.")
    target = open_target(args)
    log(f"Installing to {target.describe()}")
    try:
        if not target.exists(f"{APP_DIR}/args.txt") or args.reset_settings:
            target.put(out / "args.txt", f"{APP_DIR}/args.txt")
        log("Game files (about 16 GB the first time; files already there are skipped):")
        for part in ("game", "update"):
            copy_tree(target, work / "data" / part, f"{APP_DIR}/data/{part}")
        # Saves and settings live in data/user: copied only when the console has none yet.
        copy_tree(target, work / "data" / "user", f"{APP_DIR}/data/user", skip_existing_dir=True)
        spirv = work / "out" / "spirv"
        if spirv.is_dir():
            copy_tree(target, spirv, f"{APP_DIR}/data/spirv")
        log("fastcars.nro")
        target.put(out / "fastcars.nro", f"{APP_DIR}/fastcars.nro")
    finally:
        target.close()
    log("\nInstalled. Start the game once from the homebrew menu (full memory: hold R while starting a game),"
        "\nwait 3-5 minutes (press B when the black screen turns grey), quit (HOME, Close), then run 'shaders'.")


def cmd_shaders(args):
    need_docker()
    work = pathlib.Path(args.work).resolve()
    if not (work / "src" / ".git").is_dir():
        raise SystemExit("Run build first.")
    target = open_target(args)
    inbox = work / "shader_cache"
    inbox.mkdir(parents=True, exist_ok=True)
    try:
        names = [n for n in target.listdir(f"{APP_DIR}/data/shader_cache") if n.endswith(".bin")]
        log(f"{len(names)} shaders saved on the console")
        for n in names:
            if not (inbox / n).exists():
                target.get(f"{APP_DIR}/data/shader_cache/{n}", inbox / n)
        image = ensure_image(work)
        docker_run(work, image, ["shaders", "/work/shader_cache", "/work/out/spirv"])
        log("Copying the translated shaders to the console:")
        copy_tree(target, work / "out" / "spirv", f"{APP_DIR}/data/spirv")
    finally:
        target.close()
    log("Done. Objects that were missing are drawn from the next start on.")


SETTINGS = {
    # key: (option, values -> args.txt value or None to remove)
    "fps_counter": ("--fastcars_native_fps", {"on": "true", "off": None}),
    "log": ("--log_level", {"error": "error", "info": "info", "debug": "debug"}),
    "fps_limit": ("--fastcars_fps_limit", None),
    "cpu_boost": ("--switch2_clocks", {"on": None, "off": "keep"}),
    # render resolution in percent: below 100 the 3D scene renders smaller and is upscaled (TAA or FSR 1)
    "resolution": ("--fastcars_native_render_scale", None),
    "taa": ("--fastcars_native_taa", {"on": None, "off": "false"}),
}


def resolution_value(percent):
    """args.txt value for a render resolution in percent (always written: the default is 100)."""
    p = int(percent)
    if not 50 <= p <= 100:
        raise SystemExit("--resolution: a percentage from 50 to 100")
    return f"{p / 100:g}"


def read_settings(text):
    found = {}
    for line in (text or "").splitlines():
        line = line.split("#", 1)[0]
        for word in line.split():
            if word.startswith("--") and "=" in word:
                k, v = word.split("=", 1)
                found[k] = v
    return {
        "fps_counter": "on" if found.get("--fastcars_native_fps") == "true" else "off",
        "log": found.get("--log_level", "error"),
        "fps_limit": found.get("--fastcars_fps_limit", "60"),
        "cpu_boost": "off" if found.get("--switch2_clocks") == "keep" else "on",
        "resolution": str(round(float(found.get("--fastcars_native_render_scale", "1")) * 100)),
        "taa": "off" if found.get("--fastcars_native_taa") == "false" else "on",
    }


def apply_setting(text, option, value):
    """Sets (or removes, value None) option in args.txt, keeping everything else."""
    out, done = [], False
    for line in text.splitlines():
        code, sep, comment = line.partition("#")
        words = code.split()
        if any(w.startswith(option + "=") for w in words):
            words = [w for w in words if not w.startswith(option + "=")]
            if value is not None and not done:
                words.append(f"{option}={value}")
                done = True
            if not words and not comment:
                continue
            line = " ".join(words) + (("  " + sep + comment) if sep else "")
        out.append(line)
    if value is not None and not done:
        insert = next((i for i, l in enumerate(out) if l.startswith("# Options")), len(out))
        out.insert(insert, f"{option}={value}")
    return "\n".join(out) + "\n"


def cmd_settings(args):
    target = open_target(args)
    rel = f"{APP_DIR}/args.txt"
    try:
        text = target.read_text(rel)
        if text is None:
            raise SystemExit("No args.txt on the console: install first.")
        changes = {"fps_counter": args.fps_counter, "log": args.log, "fps_limit": args.fps_limit,
                   "cpu_boost": args.cpu_boost, "resolution": args.resolution, "taa": args.taa}
        for key, value in changes.items():
            if value is None:
                continue
            option, mapping = SETTINGS[key]
            if key == "resolution":
                text = apply_setting(text, option, resolution_value(value))
                continue
            text = apply_setting(text, option, mapping[value] if mapping else str(int(value)))
        if any(v is not None for v in changes.values()):
            target.write_text(rel, text)
        current = read_settings(text)
        if args.machine:
            print("SETTINGS " + json.dumps(current), flush=True)
        else:
            log("\n".join(f"{k}: {v}" for k, v in current.items()))
    finally:
        target.close()


def cmd_update(args):
    work = pathlib.Path(args.work).resolve()
    src = work / "src"
    have = git("-C", str(src), "rev-parse", "HEAD", check=False) if (src / ".git").is_dir() else ""
    latest = latest_commit(args.repo)
    log(f"built from: {have[:8] or '(nothing yet)'}\nlatest:     {latest[:8] if latest else '(offline)'}")
    if have and latest and have != latest:
        log("A newer version is available: build again (the game files are kept), then install.")


# ---------------------------------------------------------------------------------------------
# The builder window: a local web page (no GUI toolkit needed). Each action runs this script as a
# child process and streams its output to the page.

PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>fastcars builder</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{--bg:#f6f6f4;--fg:#1d1d1b;--muted:#6b6b66;--card:#fff;--line:#deded8;--accent:#2f7d32;--err:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--fg:#ececea;--muted:#9c9c96;--card:#20201f;--line:#33332f;--accent:#7cc47f;--err:#f28b82}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:860px;margin:0 auto;padding:24px 16px 64px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:0 0 10px}
p.lead{color:var(--muted);margin:0 0 20px}
section{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px;margin:0 0 14px}
label{display:block;font-size:13px;color:var(--muted);margin:8px 0 3px}
input,select{width:100%;box-sizing:border-box;padding:7px 9px;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
.row{display:flex;gap:10px;flex-wrap:wrap}.row>div{flex:1;min-width:160px}
button{margin-top:12px;padding:8px 14px;border:0;border-radius:6px;background:var(--accent);color:#fff;font:inherit;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
pre{background:var(--bg);border:1px solid var(--line);border-radius:6px;padding:10px;height:280px;overflow:auto;font-size:12px;white-space:pre-wrap;margin:0}
.bar{height:6px;background:var(--line);border-radius:3px;margin:10px 0}.bar>div{height:100%;width:0;background:var(--accent);border-radius:3px}
.muted{color:var(--muted);font-size:13px}.err{color:var(--err)}
</style></head><body><main>
<h1>fastcars for Switch 2</h1>
<p class="lead">Builds the game from your own Xbox 360 discs and title update and installs it on your console.
Keep what this makes to yourself: it contains the publisher's copyrighted code.</p>

<section><h2>Source</h2><div id="ver" class="muted">checking…</div>
<div class="row"><div><label>Version (empty = latest, or a commit hash)</label><input id="commit" placeholder="latest"></div></div></section>

<section><h2>1. Build</h2>
<label>Disc 1 (Install) ISO</label><input id="disc1" placeholder="/path/to/Disc 1 (Install).iso">
<label>Disc 2 (Play) ISO</label><input id="disc2" placeholder="/path/to/Disc 2 (Play).iso">
<label>Title Update 27 (tu00000009_00000000)</label><input id="tu" placeholder="/path/to/tu00000009_00000000">
<p class="muted">The files are checked against known-good hashes. Only needed for the first build.</p>
<button onclick="act('build')">Build</button></section>

<section><h2>Console</h2>
<div class="row"><div><label>Connection</label><select id="mode" onchange="mode_()"><option value="ftp">FTP</option><option value="sd">SD card</option></select></div>
<div id="ftpbox"><label>Console address (host:port)</label><input id="ftp" placeholder="192.168.1.20:1337"></div>
<div id="sdbox" style="display:none"><label>SD card folder</label><input id="sd" placeholder="/Volumes/SWITCH"></div></div>
<div class="row" id="credbox"><div><label>FTP user (empty = guest)</label><input id="ftp_user"></div><div><label>FTP password</label><input id="ftp_password" type="password"></div></div></section>

<section><h2>2. Install</h2><p class="muted">Copies the game (about 16 GB the first time; later only what changed). Saves are never overwritten.</p>
<button onclick="act('install')">Install</button></section>

<section><h2>3. Shaders</h2><p class="muted">After installing, start the game once: press A at the "No shaders yet" screen, press B when the black screen turns grey, wait 3-5 minutes until you hear the game itself, quit (HOME, Close),
then fetch the shaders it saved. Repeat later if you ever see missing objects.</p>
<button onclick="act('shaders')">Fetch shaders</button></section>

<section><h2>Output</h2><div class="bar"><div id="bar"></div></div><pre id="out"></pre></section>
</main><script>
const $=id=>document.getElementById(id);const F=['commit','disc1','disc2','tu','mode','ftp','sd','ftp_user','ftp_password'];
function mode_(){const s=$('mode').value=='sd';$('ftpbox').style.display=s?'none':'';$('credbox').style.display=s?'none':'';$('sdbox').style.display=s?'':'none'}
async function load(){const c=await (await fetch('/config')).json();for(const k of F)if(c[k]!==undefined&&k!='ftp_password')$(k).value=c[k];mode_();
 const v=await (await fetch('/version')).json();$('ver').textContent=v.text}
async function act(a){const body={action:a};for(const k of F)body[k]=$(k).value;document.querySelectorAll('button').forEach(b=>b.disabled=true);
 $('out').textContent='';$('bar').style.width='0';const r=await fetch('/run',{method:'POST',body:JSON.stringify(body)});const rd=r.body.getReader();const dec=new TextDecoder();let buf='';
 for(;;){const {value,done}=await rd.read();if(done)break;buf+=dec.decode(value,{stream:true});let i;while((i=buf.indexOf('\\n'))>=0){const line=buf.slice(0,i);buf=buf.slice(i+1);
  if(line.startsWith('PROGRESS ')){$('bar').style.width=line.slice(9)+'%';continue}
  $('out').textContent+=line+'\\n';$('out').scrollTop=1e9}}
 document.querySelectorAll('button').forEach(b=>b.disabled=false);load()}
load();
</script></body></html>"""


def gui_args(body, work):
    """The child command line for a page action."""
    base = [sys.executable, str(pathlib.Path(__file__).resolve()), "--work", str(work)]
    target = []
    if body.get("mode") == "sd":
        target = ["--sd", body.get("sd", "")]
    else:
        target = ["--ftp", body.get("ftp", "")]
        if body.get("ftp_user"):
            target += ["--ftp-user", body["ftp_user"], "--ftp-password", body.get("ftp_password", "")]
    a = body["action"]
    if a == "build":
        cmd = base + ["build"]
        if body.get("commit"):
            cmd += ["--commit", body["commit"]]
        for k in ("disc1", "disc2", "tu"):
            if body.get(k):
                cmd += [f"--{k}", body[k]]
        return cmd
    if a == "install":
        return base + ["install"] + target
    if a == "shaders":
        return base + ["shaders"] + target
    raise ValueError(a)


def cmd_gui(args):
    work = pathlib.Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    cfg_path = work / CONFIG_NAME
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, data):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/":
                self._send(200, "text/html; charset=utf-8", PAGE.encode())
            elif self.path == "/config":
                cfg = json.loads(cfg_path.read_text()) if cfg_path.exists() else {"mode": "ftp", "ftp": ""}
                self._send(200, "application/json", json.dumps(cfg).encode())
            elif self.path == "/version":
                src = work / "src"
                have = git("-C", str(src), "rev-parse", "--short=8", "HEAD", check=False) if (src / ".git").is_dir() else ""
                try:
                    latest = (latest_commit(args.repo) or "")[:8]
                except Exception:
                    latest = ""
                text = f"Built from {have or 'nothing yet'}; latest {latest or 'unknown (offline?)'}"
                if have and latest and have != latest:
                    text += ". A newer version is available: Build, then Install."
                self._send(200, "application/json", json.dumps({"text": text}).encode())
            else:
                self._send(404, "text/plain", b"not found")

        def do_POST(self):
            if self.path != "/run":
                return self._send(404, "text/plain", b"not found")
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
            keep = {k: v for k, v in body.items() if k not in ("action", "ftp_password")}
            cfg_path.write_text(json.dumps(keep, indent=2))
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            if not lock.acquire(blocking=False):
                self.wfile.write(b"Another action is still running.\n")
                return
            try:
                cmd = gui_args(body, work)
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
                for line in p.stdout:
                    self.wfile.write(line.encode())
                    self.wfile.flush()
                p.wait()
                self.wfile.write(("\nFinished.\n" if p.returncode == 0 else f"\nFailed (exit {p.returncode}).\n").encode())
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                lock.release()

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{args.port}/"
    log(f"The builder is at {url} (Ctrl+C here to stop it)")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


# ---------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Build the game for Switch 2 from your own discs and install it.")
    ap.add_argument("--work", default=str(DEFAULT_WORK), help="work folder (default: %(default)s)")
    ap.add_argument("--repo", default=REPO_URL, help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def target_opts(p):
        p.add_argument("--sd", help="the console's SD card, mounted on this computer")
        p.add_argument("--ftp", help="the console's FTP server, HOST[:PORT] (port 1337 by default)")
        p.add_argument("--ftp-user", help="FTP user (default: anonymous)")
        p.add_argument("--ftp-password", help="FTP password")

    p = sub.add_parser("build", help="build fastcars.nro")
    p.add_argument("--disc1"), p.add_argument("--disc2"), p.add_argument("--tu")
    p.add_argument("--commit", help="build this commit of the source (default: the latest)")
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
    p.add_argument("--resolution", type=int, metavar="PERCENT",
                   help="render resolution, 50-100 (default 100; below 100, temporally upscaled or FSR 1)")
    p.add_argument("--taa", choices=["on", "off"],
                   help="temporal anti-aliasing and upscaling (default on)")
    p.add_argument("--machine", action="store_true", help=argparse.SUPPRESS)
    p.set_defaults(fn=cmd_settings)

    p = sub.add_parser("update", help="is a newer version of the source available?")
    p.set_defaults(fn=cmd_update)

    p = sub.add_parser("gui", help="the builder window, in your web browser")
    p.add_argument("--port", type=int, default=8742)
    p.add_argument("--no-browser", action="store_true")
    p.set_defaults(fn=cmd_gui)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
