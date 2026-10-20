#!/usr/bin/env python3
"""Fills new texts into a translation: scripts/po-fill.py <lang> <file.tsv>
The .tsv has one line per text:  English text <TAB> translation   (write \\n for a line break; for texts with a number
use  singular|plural  on both sides). Every translation already in po/<lang>.po is kept; the .po is written again in
the order of po/ps2gm.pot (run scripts/update-po.sh first). Without a .tsv it only lists what is still untranslated."""
import re, sys, os

here = os.path.dirname(os.path.abspath(__file__)) + "/.."
lang = sys.argv[1]
tsv = sys.argv[2] if len(sys.argv) > 2 else None
pot, po = here + "/po/ps2gm.pot", here + "/po/%s.po" % lang

def unesc(s):
    return re.sub(r'\\(.)', lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)

def esc(t):
    return t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")

def quoted(block, key):
    m = re.search(r'^' + key + r' ((?:".*"\n?)+)', block, re.M)
    return unesc("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))) if m else None

def forms(block):
    if quoted(block, "msgid_plural") is not None:
        return [quoted(block, r"msgstr\[0\]"), quoted(block, r"msgstr\[1\]")]
    return [quoted(block, "msgstr")]

old = {}
text = open(po, encoding="utf-8").read()
for b in text.split("\n\n")[1:]:
    if b.lstrip().startswith("#~") or re.search(r'^#,.*\bfuzzy\b', b, re.M):
        continue  # obsolete texts, and the guesses msgmerge puts in new texts ("fuzzy"): never taken as translations
    mid = quoted(b, "msgid"); f = forms(b)
    if mid is not None and f and f[0]:
        old[mid] = f
if tsv:
    for line in open(tsv, encoding="utf-8").read().split("\n"):
        if "\t" in line:
            k, v = line.split("\t", 1)
            old[k.replace("\\n", "\n").split("|")[0] if False else k.split("|")[0].replace("\\n", "\n")] = [x.replace("\\n", "\n") for x in v.split("|")]

head = text.split("\n\n")[0]
out = [head]; missing = []
for b in open(pot, encoding="utf-8").read().split("\n\n")[1:]:
    if not b.strip():
        continue
    mid = quoted(b, "msgid"); plural = quoted(b, "msgid_plural") is not None
    f = old.get(mid)
    b = re.sub(r'\nmsgstr(\[\d\])? ""', "", b).rstrip("\n")
    if not f:
        missing.append(mid); b += '\nmsgstr[0] ""\nmsgstr[1] ""' if plural else '\nmsgstr ""'
    elif plural:
        b += '\nmsgstr[0] "%s"\nmsgstr[1] "%s"' % (esc(f[0]), esc(f[1] if len(f) > 1 else f[0]))
    else:
        b += '\nmsgstr "%s"' % esc(f[0])
    out.append(b)
open(po, "w", encoding="utf-8").write("\n\n".join(out) + "\n")
print("%s: %d untranslated" % (lang, len(missing)))
for m in missing:
    print("  " + m.replace("\n", "\\n"))
