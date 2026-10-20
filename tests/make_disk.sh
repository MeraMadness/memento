#!/bin/sh
# Builds the throw-away PS2 disk image and test ISOs used by tests/test_tools.c
#   tests/make_disk.sh <workdir> <pfsshell>
set -e
W=${1:?workdir}; P=${2:?pfsshell}
rm -rf "$W"; mkdir -p "$W/iso1" "$W/iso2"
truncate -s 6G "$W/disk.img"
printf 'device %s\ninitialize yes\nmkpart +OPL 128M PFS\nmkpart __.POPS 1G PFS\nmkpart APPS 128M PFS\nexit\n' "$W/disk.img" | "$P" >/dev/null 2>&1
for n in 1 2; do
  printf 'BOOT2 = cdrom0:\\SLES_999.9%s;1\r\nVER = 1.00\r\nVMODE = PAL\r\n' "$n" > "$W/iso$n/SYSTEM.CNF"
  head -c 2000000 /dev/urandom > "$W/iso$n/DATA.BIN"
  xorriso -as mkisofs -sysid PLAYSTATION -V TEST$n -o "$W/test$n.iso" "$W/iso$n" >/dev/null 2>&1
done
# PS1 test discs: disc1 has a real game ID (so its art can be downloaded), disc2 a made-up one, and a
# 1.1 GB sparse "huge" disc that cannot fit in the 1 GB test __.POPS
mkdir -p "$W/ps1"
python3 - "$W/ps1" <<'PY'
import random, sys
d = sys.argv[1]; random.seed(3)
def disc(name, id_, sectors):
    data = bytearray(random.getrandbits(8) for _ in range(sectors * 2352))
    s = ("BOOT = cdrom:\\%s;1\r\n" % id_).encode(); data[24:24 + len(s)] = s
    open(d + "/" + name + ".bin", "wb").write(data)
    open(d + "/" + name + ".cue", "w", newline="\r\n").write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % name)
disc("disc1", "SLES_026.98", 3000); disc("disc2", "SLES_999.94", 2000)
with open(d + "/huge.bin", "wb") as f: f.truncate(1100 * 1024 * 1024)
open(d + "/huge.cue", "w").write('FILE "huge.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
PY
echo "$W/disk.img"
