#!/usr/bin/env python3
"""Make a demo.bin (the file the Wyze bootloader flashes from the SD card
while the setup button is held) from OpenMiko's: its kernel, our root
partition image.

demo.bin is a legacy U-Boot image (type firmware, uncompressed) of the
kernel partition (2 MiB) followed by the root partition's first 274 blocks.

    demo-bin.py OPENMIKO_DEMO.bin ROOT.jffs2 OUT.bin
"""
import struct
import sys
import time
import zlib

HDR = 64
KERNEL = 0x200000
ROOT = 274 * 32768


def main(base, root, out):
    b = open(base, 'rb').read()
    magic, = struct.unpack_from('>I', b, 0)
    if magic != 0x27051956 or len(b) != HDR + KERNEL + ROOT:
        sys.exit(f'{base}: not an OpenMiko demo.bin')
    r = open(root, 'rb').read()
    if len(r) != ROOT:
        sys.exit(f'{root}: must be {ROOT} bytes')
    data = b[HDR:HDR + KERNEL] + r
    hdr = bytearray(b[:HDR])
    struct.pack_into('>I', hdr, 4, 0)                   # header CRC, computed last
    struct.pack_into('>I', hdr, 8, int(time.time()))    # timestamp
    struct.pack_into('>I', hdr, 12, len(data))          # size
    struct.pack_into('>I', hdr, 24, zlib.crc32(data))   # data CRC
    struct.pack_into('>I', hdr, 4, zlib.crc32(hdr))
    open(out, 'wb').write(bytes(hdr) + data)
    print(f'{out}: {HDR + len(data)} bytes')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
