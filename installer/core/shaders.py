"""Shaders: the console saves the game's shaders it meets (data/shader_cache); they are translated
here (XenosRecomp, the uniform-buffer rewrite, dxc) and the SPIR-V goes back to data/spirv."""
import pathlib

from . import build
from .install import copy_tree, _local_files
from .lock import work_lock
from .paths import APP_DIR


def fetch_and_translate(work, target, reporter):
    """Fetches new shader_cache entries, translates them and uploads the SPIR-V. Returns the number
    of shaders saved on the console. The fetched microcode stays in WORK/shader_cache, so a
    translator change can retranslate everything without a new first start."""
    work = pathlib.Path(work).resolve()
    with work_lock(work):
        inbox = work / "shader_cache"
        inbox.mkdir(parents=True, exist_ok=True)
        reporter.stage("Fetching the shaders the console saved")
        names = [n for n in target.listdir(f"{APP_DIR}/data/shader_cache") if n.endswith(".bin")]
        reporter.log(f"{len(names)} shaders saved on the console")
        new = [n for n in names if not (inbox / n).exists()]
        for i, n in enumerate(new):
            target.get(f"{APP_DIR}/data/shader_cache/{n}", inbox / n)
            reporter.progress(0.3 * (i + 1) / max(len(new), 1))
        reporter.stage("Translating the shaders")
        build.translate_shaders(work, reporter)
        reporter.stage("Copying the translated shaders to the console")
        spirv = work / "out" / "spirv"
        total = sum(p.stat().st_size for p in _local_files(spirv))
        copy_tree(target, spirv, f"{APP_DIR}/data/spirv", reporter, 0, total)
        reporter.progress(1.0)
        return len(names)
