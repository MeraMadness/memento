#!/bin/sh
# Rebuilds the two disk tools from source into tools/bin (and a test-only hdl_dump into tools/test).
#   hdl_dump  installs/lists PS2 games              https://github.com/ps2homebrew/hdl-dump  (GPL)
#   pfsshell  reads/writes the PS2 PFS partitions   https://github.com/ps2homebrew/pfsshell  (GPL)
# The test build of hdl_dump is compiled with DEBUG so that plain files count as disks: that is what lets
# the tests run on a disk image instead of a real drive. The app itself never uses it.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$(mktemp -d)}
mkdir -p "$HERE/tools/bin" "$HERE/tools/test"
git clone --depth 1 https://github.com/ps2homebrew/hdl-dump.git "$W/hdl-dump"
( cd "$W/hdl-dump" && make RELEASE=yes IIN_OPTICAL_MMAP=no -j"$(nproc)" && cp hdl_dump "$HERE/tools/bin/hdl_dump" \
  && make clean >/dev/null && make DEBUG=yes IIN_OPTICAL_MMAP=no -j"$(nproc)" && cp hdl_dump "$HERE/tools/test/hdl_dump" )
git clone --depth 1 https://github.com/ps2homebrew/pfsshell.git "$W/pfsshell"
( cd "$W/pfsshell" && git submodule update --init --recursive --depth 1 && meson setup build && ninja -C build \
  && cp build/pfsshell "$HERE/tools/bin/pfsshell" )
echo "built into $HERE/tools"
