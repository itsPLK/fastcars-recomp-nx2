"""The browser page (no GUI toolkit needed): each action runs fastcars.py as a child process and
streams its output to the page. The step-by-step wizard replaces it (milestone 2 of the plan)."""
import json
import pathlib
import subprocess
import sys
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .core import build, source
from .core.paths import REPO_ROOT

CONFIG_NAME = "page.json"

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

<p id="ver" class="muted">…</p>

<section><h2>1. Build</h2>
<label>Disc 1 (Install): ISO image or extracted folder</label><input id="disc1" placeholder="/path/to/Disc 1 (Install).iso">
<label>Disc 2 (Play): ISO image or extracted folder</label><input id="disc2" placeholder="/path/to/Disc 2 (Play).iso">
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
const $=id=>document.getElementById(id);const F=['disc1','disc2','tu','mode','ftp','sd','ftp_user','ftp_password'];
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


def child_args(body, work):
    """The child command line for a page action."""
    base = [sys.executable, str(REPO_ROOT / "fastcars.py"), "--work", str(work)]
    if body.get("mode") == "sd":
        target = ["--sd", body.get("sd", "")]
    else:
        target = ["--ftp", body.get("ftp", "")]
        if body.get("ftp_user"):
            target += ["--ftp-user", body["ftp_user"], "--ftp-password", body.get("ftp_password", "")]
    a = body["action"]
    if a == "build":
        cmd = base + ["build"]
        for k in ("disc1", "disc2", "tu"):
            if body.get(k):
                cmd += [f"--{k}", body[k]]
        return cmd
    if a in ("install", "shaders"):
        return base + [a] + target
    raise ValueError(a)


def serve(args):
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
                text = f"Version {source.version()}. Work folder: {work}"
                if build.data_installed(work):
                    text += " (your game files are extracted: the discs aren't needed again)"
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
                p = subprocess.Popen(child_args(body, work), stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1, errors="replace")
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
    print(f"The builder is at {url} (Ctrl+C here to stop it)", flush=True)
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
