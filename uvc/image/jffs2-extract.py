#!/usr/bin/env python3
# Extract one file from an uncompressed little-endian JFFS2 image (such as
# the root partition inside OpenMiko's demo.bin).
#   jffs2-extract.py IMAGE NAME OUT
import struct
import sys
import zlib

MAGIC, DIRENT, INODE = 0x1985, 0xE001, 0xE002


def crc(b):
    # JFFS2's CRC32 is zlib's without the final inversion and seeded with 0.
    return zlib.crc32(b, 0xFFFFFFFF) ^ 0xFFFFFFFF


def nodes(img):
    i = 0
    while i + 12 <= len(img):
        magic, ntype, totlen, hcrc = struct.unpack_from('<HHII', img, i)
        if magic != MAGIC or crc(img[i:i + 8]) != hcrc or totlen < 12:
            i += 4
            continue
        yield ntype, img[i:i + totlen]
        i += (totlen + 3) & ~3


def main(path, name, out):
    img = open(path, 'rb').read()
    ino, ver, data = None, -1, {}
    for ntype, n in nodes(img):
        if ntype == DIRENT:
            pino, version, child, mctime, nsize = struct.unpack_from('<IIIIB', n, 12)
            if n[40:40 + nsize].decode() == name and version > ver and child:
                ino, ver = child, version
    if ino is None:
        sys.exit(f'{name}: not found')
    size = 0
    for ntype, n in nodes(img):
        if ntype != INODE:
            continue
        (i_ino, version, mode, uid, gid, isize, atime, mtime, ctime, offset, csize, dsize,
         compr) = struct.unpack_from('<IIIHHIIIIIIIB', n, 12)
        if i_ino != ino:
            continue
        if compr not in (0, 1):
            sys.exit(f'compression type {compr} not supported')
        chunk = n[68:68 + csize] if compr == 0 else bytes(dsize)
        if version > data.get(offset, (-1,))[0]:
            data[offset] = (version, chunk)
        size = max(size, isize)
    buf = bytearray(size)
    for offset, (_, chunk) in data.items():
        buf[offset:offset + len(chunk)] = chunk[:max(0, size - offset)]
    open(out, 'wb').write(buf)


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__ or 'usage: jffs2-extract.py IMAGE NAME OUT')
    main(*sys.argv[1:])
