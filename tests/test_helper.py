#!/usr/bin/env python3
"""Drives ps2gm-helper over its stdin/stdout protocol against the throw-away disk image.
   tests/test_helper.py <helper-binary> <tools-dir> <workdir with disk.img/test*.iso>"""
import os, shutil, subprocess, sys, tempfile

helper_src, tools, w = sys.argv[1:4]
helper = os.path.join(tools, "ps2gm-helper")      # the helper looks for hdl_dump/pfsshell next to itself
shutil.copy(helper_src, helper)
disk = os.path.join(w, "disk.img")
failures = 0

def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failures += 1

class Helper:
    def __init__(self, dev):
        self.p = subprocess.Popen([helper, dev], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.first = self.p.stdout.readline().rstrip("\n")
    def run(self, *fields):
        self.p.stdin.write("\t".join(fields) + "\n"); self.p.stdin.flush()
        lines = []
        while True:
            l = self.p.stdout.readline().rstrip("\n")
            if l == "END" or l.startswith("ERR\t") or l == "":
                return lines, l
            lines.append(l)
    def close(self):
        self.run("QUIT"); self.p.wait()

# refuses anything that is not a PS2 disk
notps2 = os.path.join(w, "plain.bin"); open(notps2, "wb").write(os.urandom(4096))
p = subprocess.run([helper, notps2], capture_output=True, text=True)
check("ERR" in p.stdout and "not a PS2 hard disk" in p.stdout, "refuses a file that is not a PS2 disk")
p = subprocess.run([helper, "/etc/hostname"], capture_output=True, text=True)
check("ERR" in p.stdout, "refuses /etc/hostname")

h = Helper(disk)
check(h.first == "READY", "starts on the test disk")

lines, end = h.run("LIBRARY")
check(end == "END" and any(l.startswith("SPACE\t") for l in lines), "LIBRARY answers with disk space")
n0 = sum(1 for l in lines if l.startswith("GAME\t"))

lines, end = h.run("INSTALL_PS2", "CD", "Helper Test Game", os.path.join(w, "test2.iso"), "SLES_999.92")
check(end == "END" and any(l.startswith("PROGRESS\t") for l in lines), "installs a PS2 ISO with progress")
lines, end = h.run("LIBRARY")
check(sum(1 for l in lines if l.startswith("GAME\t")) == n0 + 1 and any("SLES_999.92\tHelper Test Game" in l for l in lines),
      "the game shows up in the library")

art = tempfile.mkdtemp()
for name in ("SLES_999.92_COV.png", "SLES_999.92_BG.png"):
    open(os.path.join(art, name), "wb").write(os.urandom(20000))
open(os.path.join(art, "SLES_999.92.cfg"), "w").write("Title=Helper Test Game\n")
lines, end = h.run("PUT", "+OPL", "ART", os.path.join(art, "SLES_999.92_COV.png"), os.path.join(art, "SLES_999.92_BG.png"))
check(end == "END", "copies art into +OPL/ART")
lines, end = h.run("PUT", "+OPL", "CFG", os.path.join(art, "SLES_999.92.cfg"))
check(end == "END", "copies the config into +OPL/CFG")
vcd = os.path.join(art, "SLES_026.98.Helper PS1 Game.VCD"); open(vcd, "wb").write(os.urandom(2 * 1024 * 1024))
lines, end = h.run("PUT", "__.POPS", "", vcd)
check(end == "END", "copies a .VCD (name with spaces) into __.POPS")
lines, end = h.run("LIBRARY")
check(any(l.startswith("ART\tSLES_999.92_COV.png") for l in lines), "library lists the art")
check(any(l.startswith("CFG\tSLES_999.92.cfg") for l in lines), "library lists the config")
check(any(l.startswith("VCD\tSLES_026.98.Helper PS1 Game.VCD") for l in lines), "library lists the PS1 image")

disc = os.path.join(art, "DISCS.TXT"); open(disc, "w").write("a.VCD\nb.VCD\n")
lines, end = h.run("PUT", "__common", "POPS/SLES_026.98.Helper PS1 Game", disc)
check(end == "END", "creates the POPS save folder with DISCS.TXT")
lines, end = h.run("LIBRARY")
check(any(l.startswith("POPSDIR\tSLES_026.98.Helper PS1 Game") for l in lines), "library lists the POPS folder")

print("-- reading pictures back")
lines, end = h.run("GET", "ART", "SLES_999.92_COV.png", "SLES_999.92_BG.png", "NO_SUCH_FILE.png")
got = {l.split("\t")[1]: l.split("\t")[2] for l in lines if l.startswith("GOT\t")}
check(end == "END" and set(got) == {"SLES_999.92_COV.png", "SLES_999.92_BG.png"}, "GET ART returns the files that exist")
same = all(open(got[n], "rb").read() == open(os.path.join(art, n), "rb").read() for n in got) if got else False
check(same, "the pictures read back are byte for byte what was written")
folder = os.path.dirname(next(iter(got.values()))) if got else ""
lines, end = h.run("GET", "ART", "SLES_999.92_COV.png")
check(end == "END" and len([l for l in lines if l.startswith("GOT\t")]) == 1 and len(os.listdir(folder)) == 1,
      "the next GET empties the temporary folder first")

print("-- programs (APPS)")
elf = os.path.join(art, "TEST.ELF")
open(elf, "wb").write(b"\x7fELF\x01\x01\x01\x00" + b"\x00" * 8 + b"\x02\x00\x08\x00" + os.urandom(3000))
fake = os.path.join(art, "FAKE.ELF"); open(fake, "wb").write(b"MZ not a ps2 program" * 50)
notes = os.path.join(art, "notes.txt"); open(notes, "w").write("hello")
lines, end = h.run("PUT", "APPS", "", elf)
check(end == "END", "adds a PS2 program to APPS")
lines, end = h.run("LIBRARY")
check(any(l.startswith("APP\tTEST.ELF\t") for l in lines), "library lists the program")
for args, what in ((("PUT", "APPS", "", fake), "a .ELF that is not a PS2 program"), (("PUT", "APPS", "", notes), "a text file"),
                   (("PUT", "APPS", "sub", elf), "a subfolder of APPS"), (("REMOVE_FILE", "APPS", "", "lang"), "removing a folder or non-program"),
                   (("RENAME_FILE", "APPS", "", "TEST.ELF", "x.txt"), "renaming to a non-.elf name"),
                   (("RENAME_FILE", "APPS", "", "notes.txt", "x.elf"), "renaming a non-program"),
                   (("RENAME_FILE", "+OPL", "ART", "a.elf", "b.elf"), "renaming outside APPS"),
                   (("RENAME_FILE", "APPS", "", "TEST.ELF", 'q"x.elf'), "a quote in the new name")):
    lines, end = h.run(*args)
    check(end.startswith("ERR"), "refused: " + what)
lines, end = h.run("RENAME_FILE", "APPS", "", "TEST.ELF", "TEST2.ELF")
check(end == "END", "renames a program")
lines, end = h.run("PUT", "APPS", "", elf)
lines, end = h.run("RENAME_FILE", "APPS", "", "TEST.ELF", "TEST2.ELF")
check(end.startswith("ERR"), "refuses to rename over an existing program")
lines, end = h.run("REMOVE_FILE", "APPS", "", "TEST.ELF")
lines, end = h.run("REMOVE_FILE", "APPS", "", "TEST2.ELF")
lines, end = h.run("LIBRARY")
check(end == "END" and not any(l.startswith("APP\tTEST") for l in lines), "programs removed again")

print("-- ZSO games go on the disk as they are (hdl_dump loses the end of a file that is not a whole number of sectors)")
import re, struct

def make_zso(iso_bytes, path, pad):
    """a ZSO of stored blocks and literal-only LZ4 blocks, index shift 0; pad: pad the file to whole sectors"""
    bs = 2048; blocks = len(iso_bytes) // bs; pos = 24 + (blocks + 1) * 4; idx = []; parts = []
    for i in range(blocks):
        chunk = iso_bytes[i * bs:(i + 1) * bs]
        if i % 2 == 0:  # an LZ4 block with only literals: valid, and a few bytes longer than the data itself
            n = len(chunk) - 15; tok = bytes([0xF0]); ext = b""
            while n >= 255: ext += b"\xff"; n -= 255
            ext += bytes([n]); c = tok + ext + chunk
            if len(c) >= bs: c = None
        else:
            c = None
        if c is None:
            idx.append(pos | 0x80000000); parts.append(chunk); pos += len(chunk)
        else:
            idx.append(pos); parts.append(c); pos += len(c)
    idx.append(pos)
    data = b"ZISO" + struct.pack("<IQIBB2x", 24, len(iso_bytes), bs, 1, 0) + b"".join(struct.pack("<I", x) for x in idx) + b"".join(parts)
    end = len(data)
    if pad and len(data) % 2048: data += b"\0" * (2048 - len(data) % 2048)
    open(path, "wb").write(data)
    return end, len(data)

def read_back(game_id):
    toc = subprocess.run([os.path.join(tools, "hdl_dump"), "toc", disk], capture_output=True, text=True).stdout
    m = re.search(r"0x1337\s+([0-9A-Fa-f]+)[.:*]*\s+\d+\s+\d+MB\s+PP\.%s" % game_id.replace("_", "-").replace(".", ""), toc)
    if not m: return None
    with open(disk, "rb") as f:
        f.seek(int(m.group(1), 16) * 512); blob = f.read(16 << 20)
    off = next((i for i in range(0, len(blob), 512) if blob[i:i + 4] == b"ZISO"), None)
    return None if off is None else blob[off:]

iso = open(os.path.join(w, "test2.iso"), "rb").read()
iso = iso[: len(iso) // 2048 * 2048] + os.urandom(2 * 2048)   # random sectors at the end: lost data cannot look like zeros
for pad, gid, what in ((True, "SLES_999.77", "a padded ZSO is stored completely"), (False, "SLES_999.78", "an unpadded ZSO loses its tail")):
    zpath = os.path.join(art, "zsotest_%s.zso" % ("pad" if pad else "raw"))
    exact, filelen = make_zso(iso, zpath, pad)
    lines, end = h.run("INSTALL_PS2", "CD", "ZSO Test %s" % ("A" if pad else "B"), zpath, gid)
    check(end == "END", "installs a .zso as it is (%s)" % ("padded" if pad else "not padded"))
    on_disk = read_back(gid)
    if on_disk is None:
        check(False, "the ZSO header is on the disk (%s)" % gid)
    else:
        complete = on_disk[:exact] == open(zpath, "rb").read()[:exact]
        check(complete == pad, what)
    lines, end = h.run("REMOVE_PS2", gid, "ZSO Test %s" % ("A" if pad else "B"))
    check(end == "END", "removes the ZSO test game")

print("-- an .iso with a .zso of the same name beside it: hdl_dump would quietly install the .zso instead")
sib = tempfile.mkdtemp()
shutil.copy(os.path.join(w, "test2.iso"), os.path.join(sib, "game.iso"))
make_zso(iso, os.path.join(sib, "game.zso"), False)
lines, end = h.run("INSTALL_PS2", "CD", "Sibling Test", os.path.join(sib, "game.iso"), "SLES_999.79")
check(end == "END", "installs the .iso")
check(read_back("SLES_999.79") is None, "the .iso itself was installed, not the .zso next to it")
lines, end = h.run("REMOVE_PS2", "SLES_999.79", "Sibling Test")
check(end == "END", "removes the sibling test game")

print("-- things that must be refused")
for args in (("GET",), ("GET", "ART"), ("GET", "ART", "../../etc/passwd"), ("GET", "ART", "sub/x.png"),
             ("GET", "ART", "SLES_999.92.cfg"), ("GET", "ART", 'a"b.png'), ("GET", "__system", "x.png"),
             ("GET", "CHT", "SLES_999.92.png"), ("GET", "THM", "x.cht")):
    lines, end = h.run(*args)
    check(end.startswith("ERR"), "refused: " + " ".join(args))

for args in (("PUT", "+OPL", "ART/../..", vcd), ("PUT", "+OPL", "", vcd), ("PUT", "__common", "POPS/a/b", vcd),
             ("PUT", "__system", "", vcd), ("PUT", "__.POPS", "sub", vcd), ("REMOVE_FILE", "__mbr", "", "x"),
             ("PUT", "+OPL", "ART", "/nonexistent/file.png"), ("FORMAT",), ("REMOVE_PS2", "SLES_999.92", "Wrong Title")):
    lines, end = h.run(*args)
    check(end.startswith("ERR"), "refused: " + " ".join(args[:3]))

lines, end = h.run("REMOVE_FILE", "+OPL", "ART", "SLES_999.92_BG.png")
check(end == "END", "removes one art file")
lines, end = h.run("REMOVE_FILE", "__.POPS", "", "SLES_026.98.Helper PS1 Game.VCD")
check(end == "END", "removes the PS1 image")
lines, end = h.run("REMOVE_PS2", "SLES_999.92", "Helper Test Game")
check(end == "END", "removes the PS2 game")
lines, end = h.run("LIBRARY")
check(sum(1 for l in lines if l.startswith("GAME\t")) == n0 and not any("BG.png" in l for l in lines)
      and not any(l.startswith("VCD\t") for l in lines), "library is back to the start")
h.close()
check(not os.path.exists(folder), "the temporary folder is removed when the helper quits")
print("\nall ok" if not failures else "\n%d FAILURES" % failures)
sys.exit(1 if failures else 0)
