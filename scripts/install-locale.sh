#!/bin/sh
# Run by "meson install": copies the compiled translations next to the program.
set -e
[ -d "$MESON_BUILD_ROOT/locale" ] || exit 0
mkdir -p "$MESON_INSTALL_DESTDIR_PREFIX/$1"
cp -r "$MESON_BUILD_ROOT/locale/." "$MESON_INSTALL_DESTDIR_PREFIX/$1/"
