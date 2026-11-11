# Flatpak

The Flatpak is the Linux app: choose your own game packages or dump, it creates the dump, builds the native game and
starts it. It contains no game code, assets, keys or firmware; the game library is compiled on your PC from your dump.
The first build takes about 1.5–2.5 hours and needs about 30 GB of free disk space (45 GB while a `.nsz` is
decompressed).

## Install and use

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user ./OpenSmashUltimate.flatpak
flatpak run --user io.github.Rsoto95.OpenSmashUltimate
```

The app needs the `org.freedesktop.Sdk//25.08` runtime (from Flathub), about 611 MB to download. The app's own bundle
adds clang and the launcher.

The window is a wizard, one decision per page, like Fedora Media Writer:

1. **Source**: package files or an existing dump folder.
2. **Game files**: base game (`.nsz`, `.nsp` or `.xci`), the 13.0.4 or 13.0.5 update and `prod.keys` — or the dump
   folder. **Next** stays disabled until the choice is valid.
3. **Extras**: optional DLC packages and a firmware folder (its own fonts).
4. **Install location**: storage folder (default
   `~/.var/app/io.github.Rsoto95.OpenSmashUltimate/data/ssbu-recomp`), compiler jobs and free space.
5. **Review**: the summary and **Install**. It can be stopped and continued.

The **Progress** page shows a spinner, "Compiling 123/1162 — 14m elapsed", a progress bar and the live compile and
script-module log; a slow file never looks like a freeze. **Stop** is always there. The **Ready** page has **Play**,
**Open log** and **Install again**. The first start prepares the shaders (about 15 minutes); later starts go straight
to the game.

Saves, the dump, the build, the shader cache and the logs are all in the storage folder. The game's output of the last
start is `last_run.log` there; F1 > Log shows the current game log.

The look is **adw-gtk3 v6.4** (LGPL-2.1): a GTK3 theme with the libadwaita colors and control shapes, following the
system dark/light setting through the desktop portal. The runtime stays `org.freedesktop.Sdk//25.08`, so the install
size stays about 2.3 GB (the theme adds 1.3 MB).

DLC: only the add-on content ids in the file names are read and written to `build-linux/config/dlc.txt`; the DLC content
itself is part of the game update. Removing a DLC from the list and pressing Install again removes it.

## Build the bundle

The quick way, from the repository root:

```sh
./build_flatpak.sh                 # build, install, open the launcher
./build_flatpak.sh --no-run        # build and write OpenSmashUltimate.flatpak, do not open it
./build_flatpak.sh --self-test     # build, install, run the launcher's self-test
```

It adds the user Flathub remote, installs the freedesktop SDK runtime and the LLVM20 extension if they are missing,
builds and installs the app, writes `OpenSmashUltimate.flatpak`, then starts the launcher.

Or step by step:

```sh
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y flathub org.freedesktop.Sdk//25.08 org.freedesktop.Sdk.Extension.llvm20//25.08
flatpak-builder --user --install --force-clean build-flatpak \
  packaging/flatpak/io.github.Rsoto95.OpenSmashUltimate.yml
flatpak build-bundle ~/.local/share/flatpak/repo OpenSmashUltimate.flatpak \
  io.github.Rsoto95.OpenSmashUltimate
```

The build runs two checks with no game data: `install_linux.py --check-tools` (clang, pkg-config, the Python packages
and the headers) and `scripts/macos/host_check.py --target linux` (compiles and links the project's host runtime).
The launcher's self-test can be run locally with `./build_flatpak.sh --self-test`. The GitHub Actions Flatpak workflow
is disabled for now (`flatpak.yml.disabled`); the local script is the supported build path.

## Known limits

- The SDK 25.08 has SDL3 3.2.30, which lacks `SDL_GAMEPAD_TYPE_GAMECUBE` (among other newer gamepad bits the port uses).
  The app builds and bundles SDL3 3.4.14, the version line the port is tested with, and links the game against it.
- `adw-gtk3` is a GTK3 theme, not libadwaita itself: the buttons, header bar, lists and entries get the libadwaita
  look, but the exact GTK4 widget behaviour (and GTK4's automatic dark mode) needs the GNOME runtime; that is not in
  this build.
- Rollback needs `userfaultfd` write protection (kernel 6.7+); the Flatpak allows it, but it is still Linux rollback and
  keeps the limits in `BUILDING_LINUX.md`.
- Linux against Linux online play is not tested yet.
- The app is pinned to the source in the bundle. A new Flatpak release is the update; pressing Install again rebuilds
  what changed. There is no in-app source updater and no Flathub submission yet.
- `--filesystem=home` is in the manifest so the app can read your packages, keys and a dump folder you choose. Remove it
  with `flatpak override --user --nofilesystem=home io.github.Rsoto95.OpenSmashUltimate` and use the app's own storage
  folder if you prefer.

No game data is in the bundle. You must own the game and dump it yourself.
