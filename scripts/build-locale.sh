#!/bin/sh
# Turns po/<lang>.po into <out>/<lang>/LC_MESSAGES/ps2gm.mo (what the program reads).
# Usage: build-locale.sh <po dir> <out dir> [stamp file to touch when done]
set -e
PO=$1; OUT=$2; STAMP=$3
for po in "$PO"/*.po; do
  [ -e "$po" ] || continue
  lang=$(basename "$po" .po)
  mkdir -p "$OUT/$lang/LC_MESSAGES"
  msgfmt --check --output-file="$OUT/$lang/LC_MESSAGES/ps2gm.mo" "$po"
done
[ -z "$STAMP" ] || touch "$STAMP"
