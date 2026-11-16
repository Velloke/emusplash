# EmuSPLASH

EmuSPLASH is a frontend for browsing and launching games from your multi-platform collection,
built for Linux on the desktop and on a TV.

It is based on ES-DE by Leon Styhre and Northwestern Software AB, released under the MIT
license (see [LICENSE](LICENSE) and [CREDITS.md](CREDITS.md)).

## What EmuSPLASH adds

* Iconic as the default theme. The theme downloader still finds the other themes.
* Hotkeys while a game runs, read from the pads: close game (a second press ends the emulator
  at once), save and load state, fast forward and others, for RetroArch and the standalone
  emulators.
* Emulator settings and game options from the menu, without opening the emulator.
* Up to three ROM directories with the same structure, shown as one collection.
* Custom and compiled emulators kept in their own folder.
* A media center with trailers (yt-dlp) and streaming add-ons.

The changes of each version are listed in [resources/versions.txt](resources/versions.txt) and in
the application.

## Building on Linux

```
cmake .
make -j$(nproc)
sudo make install
```

The AppImage (dist/EmuSPLASH-x86_64.AppImage, built on Ubuntu 22.04 in a container so it
runs on Debian 12+, Ubuntu 22.04+, Fedora 36+ and Arch) is made with Podman or Docker:

```
tools/build_appimage.sh
```

EmuSPLASH is installed as `/usr/bin/emusplash`, with its resources and themes in
`/usr/share/emusplash`. The settings, game lists and media are kept in `~/EmuSPLASH`.
The folder `~/ES-DE` of an ES-DE installation is moved there at the first start.

## License

MIT, see [LICENSE](LICENSE). The bundled themes have their own licenses: Linear and Iconic are
MIT, Modern and Slate are CC BY-NC-SA 2.0 (see the LICENSE file of each theme).
