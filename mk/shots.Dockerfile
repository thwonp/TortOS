# Host build of the launcher for --shot, headless. See mk/shots.sh.
#
# A container rather than the host's own SDL because the shots are compared
# byte for byte across months: a FreeType or SDL upgrade on the host would
# change every picture with no change to TortOS, and the comparison would
# report a layout regression that is not one. Pinned the way the toolchain is,
# so the same pixels come out of the same source.
#
# Bookworm, not the toolchain's bullseye: the shelf needs SDL_RenderGeometry
# (2.0.18), and bullseye ships 2.0.14. Nothing here runs on a device, so the
# glibc floor that holds the toolchain back does not apply.
#
# Xvfb and Mesa's software GL: the launcher asks for the opengl renderer on
# Linux, and a shot should go down the renderer the device uses, not SDL's
# software one.
#
#   docker build -f mk/shots.Dockerfile -t tortos-shots mk
FROM debian:bookworm-slim@sha256:f3034a6ec3c1205360777c4aae76234998866ad18806ae62b63a3f84ccad782b

# The snapshot must postdate the base image (2026-09-18): an older index
# pairs -dev packages with runtime libraries the base has already moved past.
ARG DEBIAN_SNAPSHOT=20260920T000000Z
RUN printf '%s\n' \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bookworm main" \
        "deb http://snapshot.debian.org/archive/debian-security/${DEBIAN_SNAPSHOT} bookworm-security main" \
        "deb http://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT} bookworm-updates main" \
        > /etc/apt/sources.list \
    && rm -f /etc/apt/sources.list.d/debian.sources

RUN apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
        make \
        pkg-config \
        libsdl2-dev \
        libsdl2-image-dev \
        libsdl2-ttf-dev \
        libgl1-mesa-dri \
        xvfb \
        xauth \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
