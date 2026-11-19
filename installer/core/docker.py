"""Docker: the builder image (built locally from this version's Dockerfile, once per Dockerfile)
and running the build inside it. Everything created here carries the label LABEL, so cleanup only
ever touches the installer's own images, never the player's other Docker things."""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

from .paths import NAME, REPO_ROOT
from .report import InstallError

LABEL = f"org.{NAME}.owned=true"
BUILDER_DIR = REPO_ROOT / "switch2" / "builder"
IMAGE_REPO = f"{NAME}-builder"


def need_docker():
    if not shutil.which("docker"):
        raise InstallError("Docker is not installed. Get Docker Desktop (Windows, macOS) or Docker "
                           "Engine (Linux): https://docs.docker.com/get-docker/")
    r = subprocess.run(["docker", "info"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode:
        raise InstallError("Docker is installed but not running. Start Docker Desktop (or the "
                           "docker service) and try again.")


def image_tag():
    """One image per Dockerfile: versions with the same Dockerfile share it."""
    digest = hashlib.sha256((BUILDER_DIR / "Dockerfile").read_bytes()).hexdigest()[:12]
    return f"{IMAGE_REPO}:{digest}"


def image_exists(tag):
    return subprocess.run(["docker", "image", "inspect", tag], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def run_streamed(cmd, reporter, on_line=None):
    """Runs cmd, passing each output line to the reporter (and on_line). Raises on failure."""
    reporter.log("$ " + " ".join(str(c) for c in cmd))
    p = subprocess.Popen([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1, errors="replace")
    for line in p.stdout:
        line = line.rstrip("\n")
        if on_line and on_line(line):
            continue
        reporter.log(line)
    p.wait()
    if p.returncode != 0:
        raise InstallError(f"Command failed (exit {p.returncode}): {cmd[0]} {cmd[1] if len(cmd) > 1 else ''}")


def ensure_image(reporter):
    tag = image_tag()
    if image_exists(tag):
        return tag
    reporter.stage("Preparing the build tools (once, about 10-20 minutes)")
    run_streamed(["docker", "build", "--platform", "linux/amd64", "--label", LABEL, "-t", tag,
                  str(BUILDER_DIR)], reporter)
    return tag


def run_in_builder(image, work, args, reporter, inputs=(), on_line=None):
    """Runs switch2/builder/build.sh ARGS in the builder container, the work folder at /work."""
    cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "--label", LABEL,
           "-v", f"{pathlib.Path(work).resolve()}:/work"]
    if sys.platform.startswith("linux"):
        # Native Docker runs as root: files in the work folder would be root's. Run as the user
        # (Docker Desktop on macOS and Windows maps ownership itself).
        (pathlib.Path(work) / ".home").mkdir(parents=True, exist_ok=True)
        cmd += ["--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/work/.home"]
    for host_path, ctr in inputs:
        cmd += ["-v", f"{pathlib.Path(host_path).resolve()}:{ctr}:ro"]
    cmd += [image, "sh", "/work/src/switch2/builder/build.sh", *args]
    run_streamed(cmd, reporter, on_line)
