#!/bin/sh
# Runs every test against a throw-away disk image (never a real drive).
#   tests/run_tests.sh [path/to/psx-vcd]     (the reference VCD converter is optional)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
W=${TMPDIR:-/tmp}/ps2gm-test
ninja -C "$HERE/build" >/dev/null
rm -rf "${W:?}-tools"; mkdir -p "$W-tools"
ln -sf "$HERE/tools/test/hdl_dump" "$W-tools/hdl_dump"
ln -sf "$HERE/tools/bin/pfsshell" "$W-tools/pfsshell"
cp "$HERE/build/ps2gm-helper" "$W-tools/"
export PS2GM_TEST_DISK="$W/disk.img" PS2GM_TEST_TOOLS="$W-tools" PS2GM_TEST_HELPER="$W-tools/ps2gm-helper"
export PS2GM_TEST_ISO="$W/test1.iso" PS2GM_TEST_ISO2="$W/test2.iso" PS2GM_TEST_PS1="$W/ps1"
step() { echo; echo "=== $1"; shift; "$@"; }
"$HERE/tests/make_disk.sh" "$W" "$HERE/tools/bin/pfsshell" >/dev/null
step "disk tool wrappers"   "$HERE/build/test_tools"
"$HERE/tests/make_disk.sh" "$W" "$HERE/tools/bin/pfsshell" >/dev/null
step "privileged helper"    "$HERE/tests/test_helper.py" "$HERE/build/ps2gm-helper" "$W-tools" "$W"
"$HERE/tests/make_disk.sh" "$W" "$HERE/tools/bin/pfsshell" >/dev/null
step "backend and library"  "$HERE/build/test_backend"
"$HERE/tests/make_disk.sh" "$W" "$HERE/tools/bin/pfsshell" >/dev/null
step "workflows (needs network for the art)" "$HERE/build/test_ops"
if [ -x "$HERE/build/test_ziso" ]; then step "ZSO unpacking (reference files made with liblz4)" "$HERE/build/test_ziso"; else echo; echo "(ZSO tests skipped: liblz4 development files not found)"; fi
"$HERE/tests/make_disk.sh" "$W" "$HERE/tools/bin/pfsshell" >/dev/null
step "folder mode (SD card / USB drive)" "$HERE/build/test_folder"
step "PS1 fixes (CHEATS.TXT)" "$HERE/build/test_ps1fix"
"$HERE/tests/make_disk.sh" "$W" "$HERE/tools/bin/pfsshell" >/dev/null
step "memory cards (VMC)" "$HERE/build/test_vmc"
step "Redump lists (zip, DAT, hashes)" "$HERE/build/test_redump"
if [ -n "$1" ]; then step "VCD converter vs the reference tool" "$HERE/tests/test_vcd.sh" "$HERE/build/ps2gm-vcd" "$1" "$W-vcd"; fi
echo; echo "ALL TESTS PASSED"
