#!/bin/sh
# Cross-checks the VCD converter against the reference tool (psx-vcd) on synthetic discs.
#   tests/test_vcd.sh <build/ps2gm-vcd> <path/to/psx-vcd> [workdir]
set -e
MINE=${1:?ps2gm-vcd}
REF=${2:?psx-vcd}
W=${3:-$(mktemp -d)}
rm -rf "$W"; mkdir -p "$W"
python3 - "$W" <<'PY'
import os, random, sys
w = sys.argv[1]
random.seed(1)
def sectors(n, tag):
    # raw 2352-byte sectors; the first one carries a SYSTEM.CNF-like string so ID detection has something to find
    data = bytearray(random.getrandbits(8) for _ in range(n * 2352))
    if tag:
        s = b"BOOT = cdrom:\\SLES_026.98;1\r\nTCB = 4\r\n"
        data[24:24 + len(s)] = s
    return bytes(data)
def msf(sec):
    return "%02d:%02d:%02d" % (sec // 4500, sec // 75 % 60, sec % 75)

# 1) one BIN, one data track
os.makedirs(w + "/one")
open(w + "/one/game.bin", "wb").write(sectors(700, True))
open(w + "/one/game.cue", "w", newline="\r\n").write(
    'FILE "game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')

# 2) several BINs: a data track and two audio tracks, each audio file starting with its pregap
os.makedirs(w + "/multi")
open(w + "/multi/g (Track 1).bin", "wb").write(sectors(900, True))
open(w + "/multi/g (Track 2).bin", "wb").write(sectors(400, False))
open(w + "/multi/g (Track 3).bin", "wb").write(sectors(300, False))
open(w + "/multi/g.cue", "w", newline="\r\n").write(
    'FILE "g (Track 1).bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
    'FILE "g (Track 2).bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
    'FILE "g (Track 3).bin" BINARY\n  TRACK 03 AUDIO\n    INDEX 01 00:00:00\n')

# 3) one BIN holding a data track and two audio tracks
os.makedirs(w + "/single")
open(w + "/single/s.bin", "wb").write(sectors(1500, True))
open(w + "/single/s.cue", "w", newline="\r\n").write(
    'FILE "s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
    '  TRACK 02 AUDIO\n    INDEX 00 %s\n    INDEX 01 %s\n'
    '  TRACK 03 AUDIO\n    INDEX 01 %s\n' % (msf(800), msf(950), msf(1250)))
PY

fail=0
for t in "one/game" "multi/g" "single/s"; do
  for gap in "" "+2" "-2"; do
    name=$(echo "$t" | tr '/' '_')$gap
    case "$gap" in "+2") rflag="--gap-plus"; mflag=2;; "-2") rflag="--gap-minus"; mflag=-2;; *) rflag=""; mflag=0;; esac
    mkdir -p "$W/ref_$name"
    "$REF" auto $rflag -o "$W/ref_$name" "$W/$t.cue" >/dev/null 2>&1 || { echo "reference failed on $t $gap"; fail=1; continue; }
    ref=$(ls "$W/ref_$name"/*.VCD "$W/ref_$name"/*.vcd 2>/dev/null | head -1)
    "$MINE" "$W/$t.cue" "$W/mine_$name.vcd" $mflag 2>/dev/null || { echo "mine failed on $t $gap"; fail=1; continue; }
    if cmp -s "$ref" "$W/mine_$name.vcd"; then echo "OK    $t $gap   ($(stat -c %s "$W/mine_$name.vcd") bytes, identical)"
    else echo "DIFF  $t $gap   ref=$(stat -c %s "$ref") mine=$(stat -c %s "$W/mine_$name.vcd")"; cmp "$ref" "$W/mine_$name.vcd" | head -1; fail=1; fi
  done
done
id=$("$MINE" --id "$W/one/game.bin"); [ "$id" = "SLES_026.98" ] && echo "OK    game id detection ($id)" || { echo "BAD   game id: $id"; fail=1; }
for pair in "SLPS_012.34:SLPS_012.34" "SCPS_100.17:SCPS_100.17" "SLPM_86000:SLPM_860.00" "PAPX_900.35:PAPX_900.35" "ABCD_123.45:ABCD_123.45"; do
  want=${pair#*:}; raw=${pair%%:*}
  python3 - "$W/jp.bin" "$raw" <<'PY'
import sys
data = bytearray(150 * 1024); s = ("BOOT = cdrom:\\%s;1\r\n" % sys.argv[2]).encode(); data[24:24 + len(s)] = s
open(sys.argv[1], "wb").write(data)
PY
  got=$("$MINE" --id "$W/jp.bin")
  [ "$got" = "$want" ] && echo "OK    game id $raw -> $got" || { echo "BAD   game id $raw -> $got (wanted $want)"; fail=1; }
done
exit $fail
