# Cross-toolchain for the TrimUI Brick (tg3040).
#
# Nothing in here comes from NextUI, MinUI, LoveRetro or TrimUI's SDK. A stock
# Debian cross-compiler is the whole toolchain; the platform's SDL2 libraries
# and their headers live in the sysroot (mk/fetch-sysroot.sh), not in this
# image, so the image never needs rebuilding when the device libraries change.
#
# This replaces ghcr.io/loveretro/tg5040-toolchain:latest, which built TortOS
# correctly but was someone else's image on an UNPINNED tag: a republish would
# have changed every build here with nothing failing and no record of it.
#
# Base is bullseye because of glibc, not preference: the image's glibc is the
# FLOOR of what the output binary demands, and it must stay at or below the
# device's 2.33. Bullseye's 2.31 clears that; bookworm's 2.36 would not.
#
# g++ is here for FBNeo (mk/build-fbneo.sh), the one C++ core built from
# source. Bullseye's g++-10 emits at most GLIBCXX_3.4.28, which is exactly the
# Brick's libstdc++ (6.0.28), so the core links it dynamically like the rest.
#
# Pinned by digest, never :latest. The digest is the multi-arch manifest list,
# so on an arm64 host this runs natively with no emulation.
#
#   docker build -f mk/toolchain.Dockerfile -t tortos-toolchain mk
FROM debian:bullseye-slim@sha256:f313b4bd62667092a59b3a664d7d3ab8b5e65f41675f48e81455a15dc5abe792

# apt reads from a fixed snapshot, not the live mirror. Bullseye is past end
# of life: by 2026-09-23 its security updates had left deb.debian.org's pool
# while the index still named them, so a plain apt-get install failed on 404s.
# A snapshot also gets the same packages on every build, for the same reason
# the base is pinned by digest. Valid-Until is off because a snapshot's Release
# file is past its own expiry by design; the signatures are still checked.
ARG DEBIAN_SNAPSHOT=20260901T000000Z
RUN printf '%s\n' \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye main" \
        "deb http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bullseye-security main" \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bullseye-updates main" \
        > /etc/apt/sources.list

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu \
        g++-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        binutils-aarch64-linux-gnu \
        make \
    && rm -rf /var/lib/apt/lists/*

ENV CC=aarch64-linux-gnu-gcc
WORKDIR /work
