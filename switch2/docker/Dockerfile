# Switch 2 build environment for the recomp port (development image).
# Base: ounce-devkit's image (devkitA64 + libounce + OunceVK + libkaihen), built from
# https://github.com/ouncebrew/ounce-devkit with `docker build -t ounce-devkit .`.
# Adds LLVM 19 (the version the upstream release bundles) for the generated game code,
# plus cmake/ninja/python for the SDK build.
#   docker build -t fastcars-switch2 switch2/docker
ARG DEVKIT_IMAGE=ounce-devkit:latest
FROM ${DEVKIT_IMAGE}

RUN apt-get update && \
    apt-get install -y --no-install-recommends wget gnupg ca-certificates lsb-release \
        cmake ninja-build python3 python3-pip python3-pil git xz-utils zstd && \
    wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc && \
    echo "deb http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-19 main" > /etc/apt/sources.list.d/llvm.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends clang-19 lld-19 llvm-19 && \
    rm -rf /var/lib/apt/lists/*

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
