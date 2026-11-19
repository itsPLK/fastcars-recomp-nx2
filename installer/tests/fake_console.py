"""A fake console for tests: an FTP server (standard library only) serving a local folder like
OunceShell serves the SD card (anonymous, passive mode). It can drop uploads on purpose to test
the installer's retries:

    with FakeConsole(root, drop_uploads=1) as c:   # the first upload is cut off halfway
        target = FtpTarget("127.0.0.1", c.port)
"""
import os
import pathlib
import shutil
import socket
import socketserver
import threading


class _Handler(socketserver.StreamRequestHandler):
    def setup(self):
        super().setup()
        self.cwd = "/"
        self.pasv = None
        self.rnfr = None

    def finish(self):
        if self.pasv is not None:
            self.pasv.close()
        super().finish()

    def reply(self, text):
        self.wfile.write((text + "\r\n").encode())
        self.wfile.flush()

    def path(self, arg):
        rel = os.path.normpath(os.path.join(self.cwd, arg or "")).lstrip("/")
        p = (self.server.root / rel).resolve()
        if p != self.server.root and self.server.root not in p.parents:
            raise PermissionError(arg)
        return p

    def data(self):
        conn, _ = self.pasv.accept()
        self.pasv.close()
        self.pasv = None
        return conn

    def handle(self):
        self.reply("220 Hello!")
        while True:
            line = self.rfile.readline()
            if not line:
                return
            cmd, _, arg = line.decode("utf-8", "replace").rstrip("\r\n").partition(" ")
            cmd = cmd.upper()
            try:
                if not self.dispatch(cmd, arg):
                    return
            except (PermissionError, FileNotFoundError, NotADirectoryError, IsADirectoryError):
                self.reply("550 Failed.")
            except OSError:
                self.reply("451 Error.")

    def dispatch(self, cmd, arg):
        if cmd in ("USER",):
            self.reply("331 Ok.")
        elif cmd == "PASS":
            self.reply("230 Logged in.")
        elif cmd in ("TYPE", "MODE", "STRU"):
            self.reply("200 Ok.")
        elif cmd == "SYST":
            self.reply("215 UNIX")
        elif cmd == "PWD":
            self.reply(f'257 "{self.cwd}"')
        elif cmd == "CWD":
            p = self.path(arg)
            if not p.is_dir():
                raise FileNotFoundError(arg)
            self.cwd = "/" + p.relative_to(self.server.root).as_posix() if p != self.server.root else "/"
            self.reply("250 Ok.")
        elif cmd == "PASV":
            if self.pasv is not None:
                self.pasv.close()
            self.pasv = socket.socket()
            self.pasv.bind(("127.0.0.1", 0))
            self.pasv.listen(1)
            port = self.pasv.getsockname()[1]
            self.reply(f"227 Entering Passive Mode (127,0,0,1,{port >> 8},{port & 255}).")
        elif cmd == "SIZE":
            p = self.path(arg)
            if not p.is_file():
                raise FileNotFoundError(arg)
            self.reply(f"213 {p.stat().st_size}")
        elif cmd in ("NLST", "LIST"):
            p = self.path(arg)
            names = sorted(p.iterdir()) if p.is_dir() else []
            if cmd == "NLST":
                base = "/" + p.relative_to(self.server.root).as_posix() if p != self.server.root else ""
                lines = [f"{base}/{n.name}" for n in names]
            else:
                lines = [("d" if n.is_dir() else "-") + f"rw-rw-rw- 1 3DS 3DS {0 if n.is_dir() else n.stat().st_size} "
                         f"Jan  1 1970 {n.name}" for n in names]
            self.reply("150 Listing.")
            with self.data() as conn:
                conn.sendall(("\r\n".join(lines) + ("\r\n" if lines else "")).encode())
            self.reply("226 Done.")
        elif cmd == "RETR":
            p = self.path(arg)
            if not p.is_file():
                raise FileNotFoundError(arg)
            self.reply("150 Sending.")
            with self.data() as conn, open(p, "rb") as f:
                shutil.copyfileobj(f, conn.makefile("wb"))
            self.reply("226 Done.")
        elif cmd == "STOR":
            p = self.path(arg)
            self.reply("150 Receiving.")
            drop = self.server.drop_uploads > 0
            if drop:
                self.server.drop_uploads -= 1
            with self.data() as conn, open(p, "wb") as f:
                got = 0
                while True:
                    chunk = conn.recv(1 << 16)
                    if not chunk:
                        break
                    if drop and got >= 1:
                        # Wi-Fi gone: keep half the file and hang up on the control connection.
                        f.write(chunk[: len(chunk) // 2])
                        return False
                    f.write(chunk)
                    got += len(chunk)
            self.server.uploads += 1
            self.reply("226 Done.")
        elif cmd == "MKD":
            p = self.path(arg)
            if p.exists():
                raise PermissionError(arg)
            p.mkdir()
            self.reply(f'257 "{arg}" created.')
        elif cmd == "DELE":
            p = self.path(arg)
            if not p.is_file():
                raise FileNotFoundError(arg)
            p.unlink()
            self.reply("250 Deleted.")
        elif cmd == "RMD":
            self.path(arg).rmdir()
            self.reply("250 Removed.")
        elif cmd == "RNFR":
            p = self.path(arg)
            if not p.exists():
                raise FileNotFoundError(arg)
            self.rnfr = p
            self.reply("350 Ready.")
        elif cmd == "RNTO":
            if self.rnfr is None:
                raise FileNotFoundError(arg)
            dst = self.path(arg)
            if dst.exists():  # like the console: no rename over an existing file
                raise PermissionError(arg)
            self.rnfr.rename(dst)
            self.rnfr = None
            self.reply("250 Renamed.")
        elif cmd == "QUIT":
            self.reply("221 Bye.")
            return False
        else:
            self.reply("502 Not implemented.")
        return True


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class FakeConsole:
    def __init__(self, root, drop_uploads=0):
        self.server = _Server(("127.0.0.1", 0), _Handler)
        self.server.root = pathlib.Path(root).resolve()
        self.server.drop_uploads = drop_uploads
        self.server.uploads = 0
        self.port = self.server.server_address[1]

    @property
    def uploads(self):
        return self.server.uploads

    def __enter__(self):
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
