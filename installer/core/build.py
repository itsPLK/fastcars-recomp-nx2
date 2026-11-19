"""The build: game files from the player's discs (first time), codegen, the NRO; and shader
translation. Runs switch2/builder/build.sh in the builder container on WORK."""
import pathlib
import re

from . import docker, source
from .lock import work_lock
from .paths import APP_DIR, NRO
from .report import InstallError

# build.sh's "== Stage" lines -> (stage name for the UI, overall progress at its start)
STAGES = [
    (r"Fetching the SDK", "Fetching the recompiler and libraries", 0.02),
    (r"Building the data installer", "Building the tools", 0.05),
    (r"Building the recompiler", "Building the tools", 0.08),
    (r"Building XenosRecomp", "Building the tools", 0.15),
    (r"Copying and checking the game files", "Copying and checking your game files", 0.20),
    (r"Recompiling the game executable", "Recompiling the game code", 0.35),
    (r"Building the renderer's shaders", "Building", 0.45),
    (r"Building .*\.nro", "Building the game (the longest step)", 0.50),
    (r"Done:", "Done", 1.0),
]


def data_installed(work):
    data = pathlib.Path(work) / "data"
    return (data / "data_installed.txt").is_file() and (data / "game" / "default.xex").is_file()


def nro_path(work):
    return pathlib.Path(work) / "out" / APP_DIR / NRO


def _stage_parser(reporter):
    ninja = re.compile(r"^\[(\d+)/(\d+)\]")
    state = {"start": 0.0, "end": 0.0}

    def on_line(line):
        if line.startswith("== "):
            for pattern, name, at in STAGES:
                if re.search(pattern, line[3:]):
                    reporter.stage(name)
                    state["start"] = at
                    nxt = [a for _, _, a in STAGES if a > at]
                    state["end"] = nxt[0] if nxt else 1.0
                    reporter.progress(at)
                    break
        elif line.startswith("PROGRESS "):  # data_install's copy progress
            try:
                f = int(line.split()[1]) / 100
            except ValueError:
                return True
            reporter.progress(state["start"] + f * (state["end"] - state["start"]))
            return True
        else:
            m = ninja.match(line)
            if m and int(m.group(2)) > 50:
                f = int(m.group(1)) / int(m.group(2))
                reporter.progress(state["start"] + f * (state["end"] - state["start"]))
        return False
    return on_line


def build(work, reporter, disc1=None, disc2=None, tu=None):
    """Builds the NRO from this source. The discs and title update are needed only while the work
    folder has no checked game files yet. Returns the NRO's path."""
    work = pathlib.Path(work).resolve()
    with work_lock(work):
        docker.need_docker()
        inputs, args = [], []
        if not data_installed(work):
            for name, p in (("disc1", disc1), ("disc2", disc2), ("tu", tu)):
                if not p:
                    raise InstallError("The first build needs your Disc 1, Disc 2 and Title Update 27.")
                p = pathlib.Path(p).expanduser().resolve()
                if not p.exists():
                    raise InstallError(f"Not found: {p}")
                inputs.append((p, f"/input/{name}"))
                args.append(f"/input/{name}")
        reporter.stage("Preparing the source")
        source.sync_source(work, reporter)
        image = docker.ensure_image(reporter)
        docker.run_in_builder(image, work, ["build", *args], reporter, inputs, _stage_parser(reporter))
        nro = nro_path(work)
        if not nro.is_file():
            raise InstallError("The build finished without producing the game. See the output above.")
        reporter.progress(1.0)
        return nro


def translate_shaders(work, reporter):
    """WORK/shader_cache (microcode the console saved) -> WORK/out/spirv."""
    work = pathlib.Path(work).resolve()
    docker.need_docker()
    source.sync_source(work, reporter)
    image = docker.ensure_image(reporter)
    docker.run_in_builder(image, work, ["shaders", "/work/shader_cache", "/work/out/spirv"], reporter)
