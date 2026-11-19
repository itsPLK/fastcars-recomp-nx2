"""Build the native renderer's own shader headers (SPIR-V) for the Switch 2 game build.

These are the repo's own small shaders (present blit, menu, resolve, ...; fastcars/src/native/shaders),
not game data. This runs XenosRecomp's bundled dxc (dxc-linux on x86-64 Linux, as in the build
container; dxc-macos on a Mac) with exactly the commands listed in fastcars/CMakeLists.txt (parsed from
it, so the two cannot drift) and writes the headers plus native_blit_shaders.h into OUT_DIR.

usage: python3 switch2/game/native_shaders.py OUT_DIR
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FASTCARS = os.path.join(ROOT, "fastcars")
DXC_DIR = os.path.join(ROOT, "thirdparty", "XenosRecomp", "thirdparty", "dxc-bin")


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    text = open(os.path.join(FASTCARS, "CMakeLists.txt")).read()
    hlsl = dict(re.findall(r"set\((FASTCARS_\w+_HLSL) \$\{CMAKE_CURRENT_SOURCE_DIR\}/(\S+)\)", text))
    commands = re.findall(r"COMMAND \$\{FASTCARS_DXC\}\s+(.*?)\$\{(FASTCARS_\w+_HLSL)\}", text, re.S)
    if len(commands) < 10:
        sys.exit(f"native_shaders: only {len(commands)} dxc commands found in fastcars/CMakeLists.txt")
    if sys.platform == "darwin":
        env = dict(os.environ, DYLD_LIBRARY_PATH=os.path.join(DXC_DIR, "lib", "arm64"))
        dxc = os.path.join(DXC_DIR, "bin", "arm64", "dxc-macos")
    else:
        env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(DXC_DIR, "lib", "x64"))
        dxc = os.path.join(DXC_DIR, "bin", "x64", "dxc-linux")
    for args, var in commands:
        argv = args.replace("${FASTCARS_NATIVE_SHADER_DIR}", out).split()
        subprocess.run([dxc] + argv + [os.path.join(FASTCARS, hlsl[var])], check=True, env=env)
    includes = re.findall(r'echo "#include \\"(\w+\.h)\\""', text)
    with open(os.path.join(out, "native_blit_shaders.h"), "w") as f:
        f.write("#pragma once\n" + "".join(f'#include "{h}"\n' for h in includes))
    print(f"native_shaders: {len(commands)} shaders, {len(includes)} headers in {out}")


if __name__ == "__main__":
    main()
