#!/bin/sh
# Removes what scripts/install.sh installed (reads the list meson wrote when it installed).
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$HERE/build-install"
SUDO=
if [ "$1" = "--system" ]; then BUILD="$HERE/build-system"; SUDO=sudo; fi
LOG="$BUILD/meson-logs/install-log.txt"
[ -f "$LOG" ] || { echo "Nothing to remove: no install record at $LOG"; exit 0; }
PREFIX=$(meson introspect "$BUILD" --buildoptions 2>/dev/null | python3 -c 'import sys,json; print([o["value"] for o in json.load(sys.stdin) if o["name"]=="prefix"][0])')
grep -v '^#' "$LOG" | while read -r f; do [ -n "$f" ] && $SUDO rm -f "$f"; done
# the program's own folder (also holds the translations, which are not in the list)
# the rule copied to /usr/share by install.sh --system; leave it alone if a package installed its own (it names another helper)
RULE=/usr/share/polkit-1/actions/io.github.memento.policy
if [ -n "$SUDO" ] && [ -f "$RULE" ] && grep -q "$PREFIX/lib/memento/" "$RULE"; then $SUDO rm -f "$RULE"; fi
[ -d "$PREFIX/lib/memento" ] && $SUDO rm -rf "$PREFIX/lib/memento"
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
echo "Memento is removed (your games and settings are untouched; ~/.config/ps2gm and ~/.cache/ps2gm stay)."
