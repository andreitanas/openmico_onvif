#!/usr/bin/env python3
"""Wrap a compressed kernel in a legacy U-Boot image header, as the vendor
U-Boot's bootm expects (the same format as OpenMiko's uImage.lzma).

    mkuimage.py LOAD ENTRY TIMESTAMP NAME KERNEL.lzma OUT

LOAD and ENTRY are hexadecimal (0x80100000); TIMESTAMP goes in the header
so that a build is reproducible.
"""
import struct
import sys
import zlib

MAGIC = 0x27051956
OS_LINUX, ARCH_MIPS, TYPE_KERNEL, COMP_LZMA = 5, 5, 2, 3


def main(load, entry, stamp, name, src, out):
    data = open(src, 'rb').read()
    hdr = bytearray(64)
    struct.pack_into('>IIIIIII', hdr, 0, MAGIC, 0, int(stamp), len(data),
                     int(load, 16), int(entry, 16), zlib.crc32(data))
    struct.pack_into('>BBBB', hdr, 28, OS_LINUX, ARCH_MIPS, TYPE_KERNEL, COMP_LZMA)
    hdr[32:64] = name.encode()[:31].ljust(32, b'\0')
    struct.pack_into('>I', hdr, 4, zlib.crc32(hdr))
    open(out, 'wb').write(bytes(hdr) + data)


if __name__ == '__main__':
    if len(sys.argv) != 7:
        sys.exit(__doc__)
    main(*sys.argv[1:])
