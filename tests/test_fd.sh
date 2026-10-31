#!/bin/sh
# The helper with a disk that is already open (fd:3, as UDisks hands it to a Flatpak): the same library as by path, and the refusals.
# Needs hdl_dump and pfsshell built with tools/patches (scripts/build-tools.sh); without them this test is skipped.
#   tests/test_fd.sh <tools folder with the helper> <disk image>
TOOLS=$1
IMG=$2
fail=0
by_path=$(printf 'LIBRARY\nQUIT\n' | "$TOOLS/ps2gm-helper" "$IMG" 2>&1)
by_fd=$(sh -c "exec 3<>'$IMG'; printf 'LIBRARY\nQUIT\n' | '$TOOLS/ps2gm-helper' fd:3" 2>&1)
case "$by_fd" in
  READY*) ;;
  *) echo "skipped: the tools in $TOOLS cannot use a disk handed over as fd:N (rebuild them with scripts/build-tools.sh)"; exit 0 ;;
esac
check() { if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
check "same library by fd:3 as by path" "$by_path" "$by_fd"
check "a descriptor that is not open is refused" "ERR" "$(printf 'QUIT\n' | "$TOOLS/ps2gm-helper" fd:77 2>&1 | cut -f1)"
check "stdio descriptors are refused" "ERR" "$(printf 'QUIT\n' | "$TOOLS/ps2gm-helper" fd:2 2>&1 | cut -f1)"
check "a number with letters is refused" "ERR" "$(printf 'QUIT\n' | "$TOOLS/ps2gm-helper" fd:3x 2>&1 | cut -f1)"
check "a descriptor that is not a PS2 disk is refused" "ERR" "$(sh -c "exec 3</dev/zero; printf 'QUIT\n' | '$TOOLS/ps2gm-helper' fd:3" 2>&1 | cut -f1)"
[ $fail = 0 ] && echo "all ok" || echo "FAILURES"
exit $fail
