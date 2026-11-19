"""The console's SD card: a mounted volume, or FTP (OunceShell: port 1337, anonymous login).

Uploads go to "<name>.part", are checked by size, and are then renamed into place, so a dropped
Wi-Fi connection never leaves a half-written file (a half NRO once crashed at start-up).
"""
import concurrent.futures
import ftplib
import io
import ipaddress
import os
import pathlib
import shutil
import socket
import time

from .report import InstallError

FTP_PORT = 1337
RETRIES = 5


class SdTarget:
    def __init__(self, root):
        self.root = pathlib.Path(root)
        if not self.root.is_dir():
            raise InstallError(f"SD card not found at {self.root}")

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
        if tmp.stat().st_size != os.path.getsize(local):
            raise InstallError(f"Copy of {rel} is incomplete (is the SD card full?)")
        os.replace(tmp, dst)

    def get(self, rel, local):
        shutil.copyfile(self._p(rel), local)

    def read_text(self, rel):
        p = self._p(rel)
        return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else None

    def write_text(self, rel, text):
        p = self._p(rel)
        p.parent.mkdir(parents=True, exist_ok=True)
        tmp = p.with_name(p.name + ".part")
        tmp.write_text(text, encoding="utf-8")
        os.replace(tmp, p)

    def delete(self, rel):
        p = self._p(rel)
        if p.is_file():
            p.unlink()

    def free_bytes(self):
        return shutil.disk_usage(self.root).free

    def close(self):
        pass


class FtpTarget:
    def __init__(self, host, port=FTP_PORT, user="", password=""):
        self.host, self.port = host, int(port)
        self.user, self.password = user or "anonymous", password or ""
        self.ftp = None
        self._connect()

    def describe(self):
        return f"FTP {self.host}:{self.port}"

    def _connect(self):
        if self.ftp is not None:  # a dropped connection: let go of it before reconnecting
            try:
                self.ftp.close()
            except Exception:
                pass
        try:
            self.ftp = ftplib.FTP()
            self.ftp.connect(self.host, self.port, timeout=30)
            self.ftp.login(self.user, self.password)
            self.ftp.set_pasv(True)
            self.ftp.voidcmd("TYPE I")
        except (OSError, ftplib.Error) as e:
            raise InstallError(f"Could not connect to FTP {self.host}:{self.port} ({e}). Is the FTP "
                               "app (OunceShell) open on the console, and the game closed? FTP only "
                               "answers while the game isn't running.")

    def _retry(self, fn):
        for attempt in range(RETRIES):
            try:
                return fn()
            except (OSError, EOFError, ftplib.error_temp):
                if attempt == RETRIES - 1:
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
        want = os.path.getsize(local)
        part = rel + ".part"
        for attempt in range(RETRIES):
            try:
                if "/" in rel:
                    self._mkdirs(rel.rsplit("/", 1)[0])
                with open(local, "rb") as f:
                    self.ftp.storbinary(f"STOR /{part}", f, blocksize=1 << 20)
                if self.size(part) == want:
                    break
            except (OSError, EOFError, ftplib.error_temp):
                time.sleep(2)
                self._connect()
        else:
            raise InstallError(f"Uploading {rel} kept failing (the Wi-Fi connection dropped?). "
                               "Try again; files already on the console are kept.")
        self._retry(lambda: self._replace(part, rel))

    def _replace(self, part, rel):
        try:
            self.ftp.delete("/" + rel)
        except ftplib.error_perm:
            pass  # not there yet
        self.ftp.rename("/" + part, "/" + rel)

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
        data = text.encode("utf-8")
        part = rel + ".part"

        def go():
            if "/" in rel:
                self._mkdirs(rel.rsplit("/", 1)[0])
            self.ftp.storbinary(f"STOR /{part}", io.BytesIO(data))
            self._replace(part, rel)
        self._retry(go)

    def delete(self, rel):
        try:
            self._retry(lambda: self.ftp.delete("/" + rel))
        except ftplib.error_perm:
            pass

    def free_bytes(self):
        return None  # OunceShell does not report it

    def close(self):
        try:
            self.ftp.quit()
        except Exception:
            pass


def open_target(sd=None, ftp=None, ftp_user="", ftp_password=""):
    if sd:
        return SdTarget(sd)
    if ftp:
        host, _, port = ftp.partition(":")
        return FtpTarget(host, int(port or FTP_PORT), ftp_user, ftp_password)
    raise InstallError("Choose where the console's SD card is: an SD card folder or the console's "
                       "FTP address.")


# ---------------------------------------------------------------------------------------------
# Finding the console

def local_networks():
    """The /24 networks of this computer's IPv4 addresses (the default route's first)."""
    addrs = []
    try:  # the address used for the default route (no packet is sent)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            addrs.append(s.getsockname()[0])
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.append(info[4][0])
    except OSError:
        pass
    nets = []
    for a in addrs:
        ip = ipaddress.ip_address(a)
        if ip.is_loopback or ip.is_link_local:
            continue
        net = ipaddress.ip_network(f"{a}/24", strict=False)
        if net not in nets:
            nets.append(net)
    return nets


def probe_ftp(host, port=FTP_PORT, timeout=0.6):
    """The FTP greeting of host:port, or None."""
    try:
        with socket.create_connection((str(host), port), timeout=timeout) as s:
            s.settimeout(1.5)
            banner = s.recv(256).decode("utf-8", "replace").strip()
            return banner if banner.startswith("220") else None
    except OSError:
        return None


def discover_consoles(port=FTP_PORT):
    """Hosts on the local /24 networks with an FTP server on the console's port: [(ip, greeting)]."""
    hosts = [h for net in local_networks() for h in net.hosts()]
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        for host, banner in zip(hosts, pool.map(lambda h: probe_ftp(h, port), hosts)):
            if banner:
                found.append((str(host), banner))
    return found
