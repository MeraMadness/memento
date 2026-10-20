#!/bin/sh
# Builds the app and runs it from the build folder (the tools are linked in next to it).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$HERE/build" ] || meson setup "$HERE/build" "$HERE"
ninja -C "$HERE/build"
ln -sf "$HERE/tools/bin/hdl_dump" "$HERE/build/hdl_dump"
ln -sf "$HERE/tools/bin/pfsshell" "$HERE/build/pfsshell"
exec "$HERE/build/ps2gm" "$@"
