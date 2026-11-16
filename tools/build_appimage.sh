#!/usr/bin/env bash
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  build_appimage.sh
#
#  Builds the AppImage of the public version (the last commit, without Dubx in the private
#  repository, see make_public_tree.sh) in a container with Ubuntu 22.04, so it runs on every distribution
#  that is as new: dist/EmuSPLASH-x86_64.AppImage. This folder and its build stay as they are.
#
#  Needs Podman or Docker (Podman is used when both are there). The container image is
#  emusplash-appimage, the SDL library and the AppImage tools are kept in
#  ~/.cache/emusplash-appimage between builds.
#
#  Usage, in the private repository:
#    tools/build_appimage.sh
#
#  Removing everything afterwards: tools/build_appimage.sh --clean
#

set -euo pipefail

IMAGE=emusplash-appimage
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/emusplash-appimage"
DEV_DIR="$(git rev-parse --show-toplevel)"
cd "$DEV_DIR"

if command -v podman > /dev/null; then
    ENGINE=podman
elif command -v docker > /dev/null; then
    ENGINE=docker
else
    echo "Podman or Docker is needed:" >&2
    echo "  sudo apt install podman" >&2
    exit 1
fi

if [ "${1:-}" = "--clean" ]; then
    $ENGINE rmi -f "$IMAGE" docker.io/library/ubuntu:22.04 2> /dev/null || true
    rm -rf "$CACHE"
    echo "Removed the image $IMAGE and $CACHE."
    echo "$ENGINE itself is removed with: sudo apt purge $( [ $ENGINE = podman ] && echo podman || echo docker.io ) && sudo apt autoremove"
    exit 0
fi

if [ "$(uname -m)" != "x86_64" ]; then
    echo "This builds the x86_64 AppImage, on an x86_64 computer." >&2
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "Note: the changes that aren't committed aren't in the AppImage, it's built from the"
    echo "last commit ($(git log -1 --format='%h %s'))."
    echo
fi

mkdir -p "$CACHE" "$DEV_DIR/dist"
rm -rf "$CACHE/src"
if [ -f "$DEV_DIR/tools/make_public_tree.sh" ]; then
    bash "$DEV_DIR/tools/make_public_tree.sh" "$CACHE/src"
else
    # The public repository has no Dubx to leave out.
    mkdir -p "$CACHE/src"
    git archive HEAD | tar -x -C "$CACHE/src"
fi

echo "Making the container image $IMAGE (Ubuntu 22.04, only the first time takes long)"
$ENGINE build -q -t "$IMAGE" -f "$DEV_DIR/tools/appimage/Dockerfile" "$DEV_DIR/tools/appimage" \
    > /dev/null

# Docker runs as root unless told otherwise, the files made in dist would be root's.
USER_ARGS=()
if [ $ENGINE = docker ]; then
    USER_ARGS=(--user "$(id -u):$(id -g)")
fi

rm -f "$DEV_DIR/dist/EmuSPLASH-x86_64.AppImage"
$ENGINE run --rm "${USER_ARGS[@]}" \
    -v "$CACHE/src:/src:ro" -v "$CACHE:/cache" -v "$DEV_DIR/dist:/out" \
    "$IMAGE" bash /src/tools/appimage/build_in_container.sh

rm -rf "$CACHE/src"
echo
ls -lh "$DEV_DIR/dist/EmuSPLASH-x86_64.AppImage"
echo "Run it: $DEV_DIR/dist/EmuSPLASH-x86_64.AppImage"
