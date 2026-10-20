#!/bin/sh
# Makes memento-<version>.tar.gz: the source only (no build folders, no prebuilt disk tools: packages build those themselves).
#   scripts/make-dist.sh [output folder]      default: the current folder
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n "s/^  version: '\(.*\)',/\1/p" "$HERE/meson.build" | head -1)
OUT=$(cd "${1:-.}" && pwd)
NAME="memento-$VERSION"
TMP=$(mktemp -d)
mkdir "$TMP/$NAME"
( cd "$HERE" && tar --exclude='./build*' --exclude='./tools/bin' --exclude='./tools/test' --exclude='./docs/opl-ref' \
    --exclude='./.git' --exclude='*.pyc' --exclude='__pycache__' -cf - . ) | tar -xf - -C "$TMP/$NAME"
tar -C "$TMP" -czf "$OUT/$NAME.tar.gz" "$NAME"
rm -rf "$TMP"
echo "$OUT/$NAME.tar.gz"
