#!/bin/sh
# Run by "meson install": copies hdl_dump and pfsshell (from $1, the folder with the built programs) into the program folder $2.
set -e
mkdir -p "$MESON_INSTALL_DESTDIR_PREFIX/$2"
for t in hdl_dump pfsshell; do
  [ -x "$1/$t" ] || { echo "install-tools: $1/$t not found (build it with scripts/build-tools.sh, or set -Dtools_dir=)" >&2; exit 1; }
  install -m 755 "$1/$t" "$MESON_INSTALL_DESTDIR_PREFIX/$2/$t"
done
