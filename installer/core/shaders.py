"""Shaders: the console saves the game's shaders it meets (data/shader_cache); they are translated
here (XenosRecomp, the uniform-buffer rewrite, dxc) and the SPIR-V goes back to data/spirv.

The translator's fingerprint (our XenosRecomp patch and pin, the rewrite, the script) is kept with
the translated shaders (WORK/out/spirv.translator) and in install.json. When an update changes the
translator, the saved microcode (WORK/shader_cache, or the console's) is translated again, so the
player never needs another first start."""
import hashlib
import pathlib
import re
import shutil

from . import build
from .install import copy_tree, _local_files, read_installed
from .lock import work_lock
from .paths import APP_DIR, REPO_ROOT

TRANSLATOR_FILES = ("patches/xenosrecomp-fastcars.patch", "switch2/tools/ubo_constants.py",
                    "switch2/builder/translate_shaders.sh")


def translator_id(root=REPO_ROOT):
    """A short fingerprint of everything that decides the translated shaders' bytes."""
    root = pathlib.Path(root)
    h = hashlib.sha256()
    for rel in TRANSLATOR_FILES:
        p = root / rel
        h.update(rel.encode() + b"\0" + (p.read_bytes() if p.is_file() else b"") + b"\0")
    build_sh = root / "switch2" / "builder" / "build.sh"
    if build_sh.is_file():  # the pinned XenosRecomp commit
        pins = re.findall(r"XenosRecomp\" checkout -q (\w+)", build_sh.read_text())
        h.update(" ".join(pins).encode())
    return h.hexdigest()[:12]


def _source_root(work):
    src = pathlib.Path(work) / "src"
    return src if (src / "switch2" / "builder" / "build.sh").is_file() else REPO_ROOT


def _marker(work):
    return pathlib.Path(work) / "out" / "spirv.translator"


def local_translator(work):
    m = _marker(work)
    return m.read_text().strip() if m.is_file() else None


def _translate_all(work, reporter):
    """Translates WORK/shader_cache into a fresh WORK/out/spirv and records the translator."""
    spirv = pathlib.Path(work) / "out" / "spirv"
    shutil.rmtree(spirv, ignore_errors=True)
    build.translate_shaders(work, reporter)
    _marker(work).write_text(translator_id(_source_root(work)) + "\n")


def _fetch_cache(work, target, reporter, base=0.0, span=0.3):
    inbox = pathlib.Path(work) / "shader_cache"
    inbox.mkdir(parents=True, exist_ok=True)
    names = [n for n in target.listdir(f"{APP_DIR}/data/shader_cache") if n.endswith(".bin")]
    new = [n for n in names if not (inbox / n).exists()]
    for i, n in enumerate(new):
        target.get(f"{APP_DIR}/data/shader_cache/{n}", inbox / n)
        reporter.progress(base + span * (i + 1) / max(len(new), 1))
    return len(names)


def ensure_current(work, target, reporter):
    """Before an install: makes sure the shaders that will be on the console come from this
    version's translator. Returns True when they were translated again."""
    work = pathlib.Path(work)
    current = translator_id(_source_root(work))
    spirv = work / "out" / "spirv"
    have_local = spirv.is_dir() and any(spirv.glob("*.spv"))
    local = local_translator(work)
    if have_local and local is None:
        # Translated before fingerprints existed: by the same translator as far as we know.
        _marker(work).write_text(current + "\n")
        return False
    if have_local and local == current:
        return False
    installed = read_installed(target) or {}
    on_console = any(n.endswith(".spv") for n in target.listdir(f"{APP_DIR}/data/spirv"))
    if not have_local and (not on_console or installed.get("translator") in (None, current)):
        return False  # nothing translated yet (first start pending), or the console's is current
    cached = list((work / "shader_cache").glob("*.bin"))
    reporter.stage("This version translates shaders differently: translating them again")
    if not cached:
        _fetch_cache(work, target, reporter)
    _translate_all(work, reporter)
    return True


def fetch_and_translate(work, target, reporter):
    """Fetches new shader_cache entries, translates them and uploads the SPIR-V. Returns the number
    of shaders saved on the console."""
    work = pathlib.Path(work).resolve()
    with work_lock(work):
        reporter.stage("Fetching the shaders the console saved")
        n = _fetch_cache(work, target, reporter)
        reporter.log(f"{n} shaders saved on the console")
        reporter.stage("Translating the shaders")
        current = translator_id(_source_root(work))
        if local_translator(work) not in (None, current):
            shutil.rmtree(work / "out" / "spirv", ignore_errors=True)  # translated by another version
        build.translate_shaders(work, reporter)
        _marker(work).write_text(current + "\n")
        reporter.stage("Copying the translated shaders to the console")
        spirv = work / "out" / "spirv"
        total = sum(p.stat().st_size for p in _local_files(spirv))
        copy_tree(target, spirv, f"{APP_DIR}/data/spirv", reporter, 0, total)
        _record_translator(target, current)
        reporter.progress(1.0)
        return n


def _record_translator(target, translator):
    """Notes in install.json which translator made the console's shaders."""
    import json
    from .install import STATE
    state = read_installed(target)
    if state is None:
        return
    state["translator"] = translator
    target.write_text(STATE, json.dumps(state, indent=2) + "\n")
