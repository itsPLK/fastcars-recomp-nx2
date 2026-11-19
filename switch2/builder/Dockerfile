# Build environment for fastcars-recomp-nx2 on Switch 2. Tools only, no game code:
#   devkitA64 (from devkitPro's image) + libounce + OunceVK + libkaihen (ouncebrew/ounce-devkit,
#   devkit 20261114), clang/lld 19, cmake, ninja, python3 + Pillow, on Debian 13 (its libstdc++ 14
#   has the C++20 chrono the SDK's host tools need).
# The player's discs, the generated code and the built NRO stay in the mounted work folder.
#   docker build --platform linux/amd64 -t fastcars-nx2-builder switch2/builder
# amd64 only: DirectXShaderCompiler (shader translation) ships for x86-64 Linux. Apple silicon
# runs it through Docker Desktop's Rosetta emulation.
FROM --platform=linux/amd64 docker.io/devkitpro/devkita64:20260219 AS devkitpro

FROM --platform=linux/amd64 docker.io/library/debian:trixie
ARG OUNCE_DEVKIT_COMMIT=f347815
ARG OUNCE_DEVKIT_VERSION=20261114

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates wget git xz-utils zstd patch make \
        cmake ninja-build python3 python3-pil glslang-tools zlib1g-dev libzstd-dev pkg-config \
        clang-19 lld-19 llvm-19 libclang-rt-19-dev g++ \
        libx11-dev libx11-xcb-dev libxcb1-dev libwayland-dev && \
    rm -rf /var/lib/apt/lists/*

COPY --from=devkitpro /opt/devkitpro /opt/devkitpro
ENV DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
RUN git clone https://github.com/ouncebrew/ounce-devkit /tmp/ounce-devkit && \
    git -C /tmp/ounce-devkit checkout ${OUNCE_DEVKIT_COMMIT} && \
    sh /tmp/ounce-devkit/install.sh ${OUNCE_DEVKIT_VERSION} && rm -rf /tmp/ounce-devkit

# Pin the Switch 2 graphics driver independently of the base devkit.
RUN wget -q -O /tmp/ouncevk.pkg.tar.zst https://github.com/ouncebrew/ouncevk/releases/download/20261118/ouncevk-20261118-1-any.pkg.tar.zst && \
    echo "fdb15b647d06f7306c7e2573bd73f13b05346fd17c16ae690489850bfe1dba29  /tmp/ouncevk.pkg.tar.zst" | sha256sum -c - && \
    tar --zstd -xf /tmp/ouncevk.pkg.tar.zst -C / opt/devkitpro/portlibs/switch2 && \
    if [ -f /opt/devkitpro/ounce-devkit-version.txt ]; then \
        sed -i 's/^ouncevk .*/ouncevk 2d6a8aec8b03/' /opt/devkitpro/ounce-devkit-version.txt; \
    fi && \
    touch /opt/devkitpro/portlibs/switch2/lib/libouncevk.a && \
    rm /tmp/ouncevk.pkg.tar.zst

ENV PATH="/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/lib/llvm-19/bin:${PATH}"
RUN git config --global --add safe.directory '*'
WORKDIR /work
