#!/usr/bin/env python3
"""Make a demo.bin (the file the Wyze bootloader flashes from the SD card
while the setup button is held) from OpenMiko's: its kernel or ours, our
root partition image.

demo.bin is a legacy U-Boot image (type firmware, uncompressed) of the
kernel partition (2 MiB) followed by the root partition's first 274 blocks.

    demo-bin.py OPENMIKO_DEMO.bin ROOT.jffs2 OUT.bin
    demo-bin.py OPENMIKO_DEMO.bin ROOT.sqfs OUT.bin UIMAGE

With UIMAGE (our kernel, image/build-modern.sh) the kernel partition gets it
instead of OpenMiko's, and a shorter root image is padded with 0xff.
"""
import struct
import sys
import time
import zlib

HDR = 64
KERNEL = 0x200000
ROOT = 274 * 32768


def main(base, root, out, uimage=None):
    b = open(base, 'rb').read()
    magic, = struct.unpack_from('>I', b, 0)
    if magic != 0x27051956 or len(b) != HDR + KERNEL + ROOT:
        sys.exit(f'{base}: not an OpenMiko demo.bin')
    r = open(root, 'rb').read()
    kernel = b[HDR:HDR + KERNEL]
    if uimage:
        kernel = open(uimage, 'rb').read()
        if len(kernel) > KERNEL or len(r) > ROOT:
            sys.exit(f'{uimage} or {root} does not fit its partition')
        kernel = kernel.ljust(KERNEL, b'\xff')
        r = r.ljust(ROOT, b'\xff')
    if len(r) != ROOT:
        sys.exit(f'{root}: must be {ROOT} bytes')
    data = kernel + r
    hdr = bytearray(b[:HDR])
    struct.pack_into('>I', hdr, 4, 0)                   # header CRC, computed last
    struct.pack_into('>I', hdr, 8, int(time.time()))    # timestamp
    struct.pack_into('>I', hdr, 12, len(data))          # size
    struct.pack_into('>I', hdr, 24, zlib.crc32(data))   # data CRC
    struct.pack_into('>I', hdr, 4, zlib.crc32(hdr))
    open(out, 'wb').write(bytes(hdr) + data)
    print(f'{out}: {HDR + len(data)} bytes')


if __name__ == '__main__':
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    main(*sys.argv[1:])
