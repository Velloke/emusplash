#!/usr/bin/env bash
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  build_in_container.sh
#
#  Runs in the container of tools/build_appimage.sh (see Dockerfile): builds EmuSPLASH without
#  Dubx from /src and makes /out/EmuSPLASH-x86_64.AppImage. The SDL library and the AppImage
#  tools are kept in /cache between builds.
#
#  The AppImage tools are AppImages themselves, there is no FUSE in a container, so they're
#  unpacked and run from their folders.
#

set -euo pipefail

JOBS="$(nproc 2> /dev/null || echo 8)"
SDL_RELEASE_TAG=release-2.32.10
SDL_SHARED_LIBRARY=libSDL2-2.0.so.0.3200.10

export HOME=/tmp
cd /cache

# The AppImage tools.
for tool in linuxdeploy appimagetool; do
    if [ ! -x "/cache/$tool/AppRun" ]; then
        echo "Downloading $tool"
        case $tool in
            linuxdeploy)
                url="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage" ;;
            appimagetool)
                url="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage" ;;
        esac
        rm -rf "/cache/$tool" "/cache/$tool.AppImage" /cache/squashfs-root
        wget -q -O "/cache/$tool.AppImage" "$url"
        chmod +x "/cache/$tool.AppImage"
        "/cache/$tool.AppImage" --appimage-extract > /dev/null
        mv /cache/squashfs-root "/cache/$tool"
        rm "/cache/$tool.AppImage"
    fi
done

# SDL with PipeWire, newer than the one of Ubuntu 22.04, it replaces that one in the AppImage.
if [ ! -f "/cache/SDL/build/$SDL_SHARED_LIBRARY" ]; then
    echo "Building the SDL library $SDL_RELEASE_TAG"
    rm -rf /cache/SDL
    git clone -q --depth 1 --branch "$SDL_RELEASE_TAG" https://github.com/libsdl-org/SDL.git \
        /cache/SDL
    cmake -DCMAKE_BUILD_TYPE=Release -S /cache/SDL -B /cache/SDL/build > /dev/null
    if ! grep -q 'PKG_PIPEWIRE_VERSION:INTERNAL=[0-9]' /cache/SDL/build/CMakeCache.txt; then
        echo "The SDL library has no PipeWire support" >&2
        exit 1
    fi
    make -C /cache/SDL/build -j"$JOBS" > /dev/null
fi

# EmuSPLASH, built in a copy so /src stays as it is.
echo "Building EmuSPLASH"
rm -rf /tmp/build
cp -a /src /tmp/build
cd /tmp/build
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DAPPIMAGE_BUILD=on \
      -DBUNDLED_CERTS=on -DDUBX=OFF . | grep -E '^-- (Building|Installation|Compiler)' || true
make -j"$JOBS"
make install DESTDIR=/tmp/build/AppDir > /dev/null

cd /tmp/build/AppDir
ln -s usr/bin/emusplash AppRun
ln -s usr/share/pixmaps/org.emusplash.frontend.png .
ln -s usr/share/applications/org.emusplash.frontend.desktop .
ln -s org.emusplash.frontend.png .DirIcon
cd usr/bin
ln -s ../share/emusplash/resources .
ln -s ../share/emusplash/themes .
cd /tmp/build

# The libraries EmuSPLASH needs go into the AppImage, the graphics driver ones (OpenGL) stay
# those of the system.
/cache/linuxdeploy/AppRun --appdir AppDir \
    -l /lib/x86_64-linux-gnu/libOpenGL.so.0 -l /lib/x86_64-linux-gnu/libGLdispatch.so.0 \
    -l /lib/x86_64-linux-gnu/libgio-2.0.so.0 > /tmp/build/linuxdeploy.log 2>&1 ||
    { tail -30 /tmp/build/linuxdeploy.log >&2; exit 1; }
cp "/cache/SDL/build/$SDL_SHARED_LIBRARY" AppDir/usr/lib/libSDL2-2.0.so.0

ARCH=x86_64 /cache/appimagetool/AppRun --no-appstream AppDir /out/EmuSPLASH-x86_64.AppImage
echo "Made /out/EmuSPLASH-x86_64.AppImage"
