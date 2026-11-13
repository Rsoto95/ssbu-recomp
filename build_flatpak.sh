#!/usr/bin/env bash
# Build, install and start the OpenSmash Ultimate Flatpak. Run from anywhere:
#
#   ./build_flatpak.sh                 build, install, open the launcher
#   ./build_flatpak.sh --self-test     build, install, run the launcher's self-test
#   ./build_flatpak.sh --no-run        build the app and the .flatpak, do not start it
#
# Needs flatpak and flatpak-builder. The first run downloads the freedesktop SDK runtime and the LLVM20 extension from
# Flathub (about 750 MB), then builds SDL3, PyGObject and the app (a few minutes). The game itself is built later, on
# the first Install inside the app, from the player's own dump.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ID=io.github.Rsoto95.OpenSmashUltimate
MANIFEST="$REPO/packaging/flatpak/$APP_ID.yml"
BUILD="$REPO/build-flatpak"
BUNDLE="$REPO/OpenSmashUltimate.flatpak"
SDK=org.freedesktop.Sdk//25.08
LLVM=org.freedesktop.Sdk.Extension.llvm20//25.08

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

for tool in flatpak flatpak-builder; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool is missing. Install flatpak and flatpak-builder:" >&2
        echo "  Fedora:        sudo dnf install flatpak flatpak-builder" >&2
        echo "  Arch:          sudo pacman -S flatpak flatpak-builder" >&2
        echo "  Debian/Ubuntu: sudo apt install flatpak flatpak-builder" >&2
        exit 1
    fi
done

say "Flathub"
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo

if ! flatpak info --user "$SDK" >/dev/null 2>&1 || ! flatpak info --user "$LLVM" >/dev/null 2>&1; then
    say "runtime: $SDK and $LLVM (first run only)"
    flatpak install --user -y flathub "$SDK" "$LLVM"
fi

say "building $APP_ID"
rm -f "$BUNDLE"                 # the source tree is copied into the app: never include an old bundle in the new one
flatpak-builder --user --install --force-clean "$BUILD" "$MANIFEST"

say "bundle: $BUNDLE"
flatpak build-bundle "$HOME/.local/share/flatpak/repo" "$BUNDLE" "$APP_ID"

if [ "${1:-}" = "--no-run" ]; then
    say "installed; the bundle is $BUNDLE"
    exit 0
fi
say "starting $APP_ID"
exec flatpak run --user "$APP_ID" "$@"
