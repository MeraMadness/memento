#!/bin/sh
# Collects the texts marked with _() / N_() into po/ps2gm.pot and brings every po/<lang>.po up to date.
# New text -> empty entry in each .po (translate it there); removed text -> marked obsolete.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"
xgettext --from-code=UTF-8 --language=C --add-comments=TRANSLATORS \
  --keyword=_ --keyword=N_ --keyword=g_dngettext:2,3 \
  --package-name=ps2gm --msgid-bugs-address="" \
  -f po/POTFILES.in -o po/ps2gm.pot
for po in po/*.po; do
  [ -e "$po" ] || continue
  msgmerge --quiet --update --backup=none "$po" po/ps2gm.pot
done
echo "po/ps2gm.pot: $(grep -c '^msgid ' po/ps2gm.pot) texts"
