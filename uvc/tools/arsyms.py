#!/usr/bin/env python3
"""List the external symbols a static archive of 32-bit little-endian ELF
objects needs (undefined in its members and not defined by any member).
Usage: arsyms.py lib.a"""
import struct
import sys


def members(data):
    i = 8
    names = b""
    while i + 60 <= len(data):
        hdr = data[i:i + 60]
        name = hdr[:16].decode().strip()
        size = int(hdr[48:58].decode().strip())
        body = data[i + 60:i + 60 + size]
        if name == "//":
            names = body
        elif name not in ("/", "/SYM64/"):
            if name.startswith("/") and name[1:].isdigit():
                off = int(name[1:])
                name = names[off:names.index(b"/", off)].decode()
            yield name.rstrip("/"), body
        i += 60 + size + (size & 1)


def symbols(elf):
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", elf, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", elf, shoff + k * shentsize) for k in range(shnum)]
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", elf, off)
            bind = info >> 4
            if bind == 0 or name_off == 0:  # local
                continue
            s = strtab[4] + name_off
            name = elf[s:elf.index(b"\0", s)].decode()
            yield name, shndx == 0


data = open(sys.argv[1], "rb").read()
defined, needed = set(), set()
for name, body in members(data):
    if body[:4] != b"\x7fELF":
        continue
    for sym, undef in symbols(body):
        (needed if undef else defined).add(sym)
for sym in sorted(needed - defined):
    print(sym)
