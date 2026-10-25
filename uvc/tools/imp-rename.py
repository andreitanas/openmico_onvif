#!/usr/bin/env python3
"""Point libimp.a's calls to mmap/ftruncate at our 32-bit-offset shims.

libimp was built against uClibc with a 32-bit off_t; musl's mmap and
ftruncate take a 64-bit one. Overriding those symbols for the whole binary
breaks musl's own callers (sem_open, which libimp uses), so instead the
undefined references inside libimp are renamed in place (same or shorter
names fit in the existing string tables) to symbols imp_compat.c defines.

    imp-rename.py IN.a OUT.a
"""
import struct
import sys

RENAME = {b'mmap': b'mm32', b'ftruncate': b'ftrunc32'}


def patch_elf(obj, member):
    if obj[:4] != b'\x7fELF' or obj[4] != 1 or obj[5] != 1:
        return obj, 0
    obj = bytearray(obj)
    shoff, = struct.unpack_from('<I', obj, 32)
    shentsize, shnum = struct.unpack_from('<HH', obj, 46)
    secs = [struct.unpack_from('<IIIIIIIIII', obj, shoff + i * shentsize) for i in range(shnum)]
    n = 0
    for sec in secs:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[sec[6]]
        base = strtab[4]
        syms = [struct.unpack_from('<IIIBBH', obj, o) for o in range(sec[4], sec[4] + sec[5], 16)]

        def name_at(off):
            return bytes(obj[base + off:obj.index(b'\0', base + off)])

        for name, value, size, info, other, shndx in syms:
            new = RENAME.get(name_at(name))
            if new is None:
                continue
            if shndx != 0:
                sys.exit(f'{member}: defines {name_at(name).decode()}; refusing to rename')
            old = name_at(name)
            # The bytes must belong to this name alone (string tables may
            # share suffixes between names).
            for o in (s[0] for s in syms):
                if o != name and o < name < o + len(name_at(o)):
                    sys.exit(f'{member}: {old.decode()} shares its string; cannot rename')
            obj[base + name:base + name + len(old)] = new + b'\0' * (len(old) - len(new))
            n += 1
    return bytes(obj), n


def main(src, dst):
    data = open(src, 'rb').read()
    if data[:8] != b'!<arch>\n':
        sys.exit(f'{src}: not an ar archive')
    out, pos, total = bytearray(data[:8]), 8, 0
    while pos < len(data):
        hdr = data[pos:pos + 60]
        size = int(hdr[48:58])
        body = data[pos + 60:pos + 60 + size]
        name = hdr[:16].decode().strip()
        # "/" and "//" are the symbol index and long-name table; members
        # with long names are "/<offset>".
        if name not in ('/', '//', '/SYM64/'):
            body, n = patch_elf(body, name)
            total += n
        out += hdr + body
        pos += 60 + size
        if size % 2:
            out += data[pos:pos + 1]
            pos += 1
    open(dst, 'wb').write(out)
    print(f'{dst}: renamed {total} references')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
