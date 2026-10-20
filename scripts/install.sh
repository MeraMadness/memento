#!/bin/sh
# Installs Memento for the current user (no password needed): program, icon and application menu entry.
#   scripts/install.sh [prefix]      default prefix: ~/.local   -> ~/.local/bin/memento, ~/.local/lib/memento/, ...
#   scripts/install.sh --system      for all users, in /usr/local (asks for your password with sudo); this is the safe way to
#                                    use the password window: the disk helper runs as root only from files owned by root
# Run it again after changing the code to update the installed copy. scripts/uninstall.sh removes it.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
if [ "$1" = "--system" ]; then
  PREFIX=/usr/local
  BUILD="$HERE/build-system"
  if [ -d "$BUILD/meson-private" ]; then meson configure "$BUILD" --prefix="$PREFIX" --libdir=lib --buildtype=release -Dpolkit_policy=enabled >/dev/null
  else meson setup "$BUILD" "$HERE" --prefix="$PREFIX" --libdir=lib --buildtype=release -Dpolkit_policy=enabled >/dev/null; fi
  ninja -C "$BUILD" >/dev/null
  echo "Installing in $PREFIX (needs your password)..."
  sudo meson install -C "$BUILD" --no-rebuild >/dev/null
  # polkit reads its rules only from /usr/share/polkit-1/actions, not from /usr/local/share
  sudo install -D -m 644 "$BUILD/io.github.memento.policy" /usr/share/polkit-1/actions/io.github.memento.policy
  command -v update-desktop-database >/dev/null && sudo update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
  command -v gtk-update-icon-cache >/dev/null && sudo gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
  echo "Memento is installed for all users in $PREFIX (run: $PREFIX/bin/memento, or find it in the application menu)"
  exit 0
fi
PREFIX=${1:-$HOME/.local}
BUILD="$HERE/build-install"

if [ -d "$BUILD/meson-private" ]; then
  meson configure "$BUILD" --prefix="$PREFIX" --libdir=lib --buildtype=release >/dev/null
else
  meson setup "$BUILD" "$HERE" --prefix="$PREFIX" --libdir=lib --buildtype=release >/dev/null
fi
ninja -C "$BUILD" >/dev/null
meson install -C "$BUILD" --no-rebuild >/dev/null

# let the desktop notice the new entry and icon (best effort)
update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true

echo "Memento is installed in $PREFIX"
echo "  start it from the application menu, or run: $PREFIX/bin/memento"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "  (add $PREFIX/bin to your PATH to start it by typing: memento)";; esac
