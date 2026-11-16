#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  Dockerfile
#
#  The system the AppImage is built on: Ubuntu 22.04 (glibc 2.35), so the AppImage runs on
#  Debian 12+, Ubuntu 22.04+, Fedora 36+, Arch and the others that are as new. Used by
#  tools/build_appimage.sh with Docker or Podman.
#

FROM docker.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-12 gcc-12 cmake git pkg-config gettext wget file ca-certificates \
        python3 desktop-file-utils squashfs-tools \
        libharfbuzz-dev libicu-dev libsdl2-dev \
        libavcodec-dev libavfilter-dev libavformat-dev libavutil-dev libswresample-dev \
        libswscale-dev libfreeimage-dev libfreetype6-dev libgit2-dev libcurl4-openssl-dev \
        libpugixml-dev libasound2-dev libbluetooth-dev libgl1-mesa-dev libgles2-mesa-dev \
        libpoppler-cpp-dev libmpv-dev libarchive-dev \
        libpipewire-0.3-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev \
        libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxkbcommon-dev libwayland-dev \
        wayland-protocols libdecor-0-dev libudev-dev libdbus-1-dev libibus-1.0-dev \
        libdrm-dev libgbm-dev libegl1-mesa-dev libsamplerate0-dev \
    && rm -rf /var/lib/apt/lists/*

# GCC 12 (Ubuntu 22.04 has 11 by default), its C++ library is the one of Ubuntu 22.04 too.
ENV CC=gcc-12 CXX=g++-12
