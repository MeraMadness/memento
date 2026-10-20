#!/usr/bin/env python3
"""Makes test material for the memory card page.
   make_card.py card <path> <MB>                              a blank raw card, laid out like the ones openVMC/OPL make
   make_card.py psu <path> <save name> <data bytes> <title>   a small .psu save with an icon.sys (title in full-width Shift-JIS)"""
import struct, sys


def u16(v):
    return struct.pack("<H", v)


def u32(v):
    return struct.pack("<I", v)


def dirent(mode, length, name):
    stamp = bytes([0, 0, 0, 12, 1, 1, 0xe9, 0x07])
    e = bytearray(512)
    e[0:2] = u16(mode)
    e[4:8] = u32(length)
    e[8:16] = stamp
    e[0x18:0x20] = stamp
    e[0x40:0x40 + len(name)] = name.encode()
    return bytes(e)


def card(path, mb):
    size = mb * 1024 * 1024
    clusters = size // 1024
    fat_count = 1
    while fat_count * 256 < clusters - 9 - fat_count:
        fat_count += 1
    alloc_offset = 9 + fat_count
    alloc_end = clusters - alloc_offset - 16
    data = bytearray(size)
    sb = bytearray(1024)
    sb[0:28] = b"Sony PS2 Memory Card Format "
    sb[0x1c:0x23] = b"1.2.0.0"
    sb[0x28:0x2a] = u16(512)
    sb[0x2a] = 2
    sb[0x2c] = 0x10
    sb[0x2e] = 0x00
    sb[0x2f] = 0xff
    sb[0x30:0x34] = u32(clusters)
    sb[0x34:0x38] = u32(alloc_offset)
    sb[0x38:0x3c] = u32(alloc_end)
    sb[0x3c:0x40] = u32(0)
    sb[0x40:0x44] = u32(size // 8192 - 1)
    sb[0x44:0x48] = u32(size // 8192 - 2)
    sb[0x50:0x54] = u32(8)
    sb[0xd0:0x150] = b"\xff" * 0x80
    sb[0x150] = 2
    sb[0x151] = 0x2b
    data[0:1024] = sb
    cl = bytearray()
    for i in range(256):
        cl += u32(9 + i if i < fat_count else 0xffffffff)
    data[8 * 1024:9 * 1024] = cl
    for i in range(fat_count):
        cl = bytearray()
        for j in range(256):
            idx = i * 256 + j
            cl += u32(0xffffffff if (idx == 0 or idx >= alloc_end) else 0x7fffffff)
        data[(9 + i) * 1024:(10 + i) * 1024] = cl
    root = dirent(0x8427, 2, ".") + dirent(0xa426, 0, "..")
    data[alloc_offset * 1024:alloc_offset * 1024 + 1024] = root
    open(path, "wb").write(data)


def psu(path, name, datalen, title):
    def ent(mode, length, nm):
        b = bytearray(512)
        b[0:2] = u16(mode)
        b[4:8] = u32(length)
        b[9] = 30
        b[10] = 12
        b[11] = 1
        b[12] = 1
        b[13:15] = u16(2005)
        b[0x18:0x20] = b[8:16]
        b[0x40:0x40 + len(nm)] = nm.encode()
        return bytes(b)

    def pad(b):
        return b + b"\0" * (-len(b) % 1024)

    icon = bytearray(964)
    icon[0:4] = b"PS2D"
    sj = "".join(chr(0xff00 + ord(c) - 0x20) if c != " " else "　" for c in title).encode("shift_jis")
    icon[0xc0:0xc0 + len(sj)] = sj
    body = bytes((k * 7 + ord(name[0])) & 255 for k in range(datalen))
    out = ent(0x8427, 4, name) + ent(0x8427, 0, ".") + ent(0x8427, 0, "..")
    out += ent(0x8497, 964, "icon.sys") + pad(bytes(icon)) + ent(0x8497, datalen, "data.bin") + pad(body)
    open(path, "wb").write(out)


if __name__ == "__main__":
    a = sys.argv
    if a[1] == "card":
        card(a[2], int(a[3]))
    elif a[1] == "psu":
        psu(a[2], a[3], int(a[4]), a[5])
    else:
        sys.exit("usage: see the top of this file")
