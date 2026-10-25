#!/usr/bin/env python3
"""Build the camera's root filesystem tarball from OpenMiko's.

OpenMiko's kernel unpacks rootfs.tar.xz from the root flash partition into a
zram disk on every boot, so everything in it costs RAM. This keeps busybox,
dropbear, wpa_supplicant, the camera and WiFi drivers and the
shared libraries those programs link, replaces the init scripts with ours
(image/rootfs/) and drops the rest (ffmpeg, RTSP, micropython, lighttpd,
bash, ...). uvcd is not in here: it lives next to the tarball in flash.

The tarball is filtered member by member, never unpacked, so file modes,
owners and names survive the host filesystem.

    build-rootfs.py BASE.tar.xz OVERLAY_DIR OUT.tar.xz
"""
import io
import lzma
import os
import posixpath
import struct
import sys
import tarfile

# Programs kept besides busybox (and its applet links).
KEEP_PROGRAMS = {
    'bin/busybox',
    'usr/sbin/dropbear',
    'usr/sbin/wpa_supplicant',
}
# Kernel modules kept (camera, WiFi; audio for the microphone later).
KEEP_MODULES = {
    'lib/modules/3.10.14/kernel/drivers/media/platform/tx-isp/tx-isp.ko',
    'lib/modules/3.10.14/kernel/drivers/media/platform/sensors/sensor_info/sinfo.ko',
    'lib/modules/3.10.14/kernel/drivers/media/platform/sensors/jxf22/sensor_jxf22.ko',
    'lib/modules/3.10.14/kernel/drivers/net/wireless/rtl818x/rtl8189FS/8189fs.ko',
    'driver/sensor_jxf23.ko',
    'driver/audio.ko',
}
# Whole trees that go.
DROP_TREES = (
    'app', 'var/www', 'var/spool', 'usr/libexec', 'usr/share/alsa', 'usr/share/ffmpeg',
    'usr/share/fonts', 'usr/share/pixmaps', 'usr/share/applications', 'usr/lib/micropython',
    'usr/lib/lighttpd', 'usr/lib/mjpg-streamer', 'usr/lib/tmpfiles.d', 'etc/ssl', 'etc/lighttpd', 'etc/logrotate.d', 'etc/init.d',
)
DROP_FILES = {
    'etc/openmiko.conf', 'etc/videocapture_settings.json',
    'etc/videocapture_settings_1_encoder.json', 'etc/v4l2rtspserver.conf.example',
    'etc/logrotate.conf', 'etc/sudoers', 'usr/share/timezones.tsv', 'etc/ntp.conf',
}
BIN_DIRS = ('bin', 'sbin', 'usr/bin', 'usr/sbin')
LIB_DIRS = ('lib', 'usr/lib')


def norm(name):
    return posixpath.normpath(name.lstrip('./')) if name not in ('.', './') else '.'


def needed(data):
    """DT_NEEDED entries of a 32-bit little-endian ELF, or None if not ELF."""
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        return None
    e_phoff, = struct.unpack_from('<I', data, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
    loads, dyn = [], None
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz = struct.unpack_from(
            '<IIIII', data, e_phoff + i * e_phentsize)
        if p_type == 1:
            loads.append((p_vaddr, p_offset, p_filesz))
        elif p_type == 2:
            dyn = (p_offset, p_filesz)
    if not dyn:
        return []

    def off(vaddr):
        for v, o, n in loads:
            if v <= vaddr < v + n:
                return vaddr - v + o
        raise ValueError('address outside segments')

    entries, strtab = [], None
    for i in range(dyn[0], dyn[0] + dyn[1], 8):
        tag, val = struct.unpack_from('<iI', data, i)
        if tag == 0:
            break
        if tag == 1:
            entries.append(val)
        elif tag == 5:
            strtab = off(val)
    return [data[strtab + v:data.index(b'\0', strtab + v)].decode() for v in entries]


def main(base, overlay, out):
    src = tarfile.open(base)
    members, data = {}, {}
    for m in src.getmembers():
        n = norm(m.name)
        members[n] = m
        if m.isreg():
            data[n] = src.extractfile(m).read()

    def resolve(n, depth=0):
        """Follow symlinks inside the image; None if dangling."""
        m = members.get(n)
        if m is None or depth > 16:
            return None
        if not m.issym():
            return n
        t = m.linkname
        t = norm(t) if t.startswith('/') else posixpath.normpath(
            posixpath.join(posixpath.dirname(n), t))
        return resolve(t, depth + 1)

    def dropped(n):
        if n in DROP_FILES or any(n == t or n.startswith(t + '/') for t in DROP_TREES):
            return True
        m = members[n]
        if m.isreg() and n.endswith('.ko'):
            return n not in KEEP_MODULES
        if m.isreg() and n.startswith('driver/'):
            return n not in KEEP_MODULES
        if m.isreg() and posixpath.dirname(n) in BIN_DIRS:
            return n not in KEEP_PROGRAMS
        return False

    keep = {n for n in members if not dropped(n)}

    # Shared libraries in /usr/lib: the closure of what kept programs link.
    # /lib (uClibc, ~1 MB) stays whole: the dynamic loader is reached through
    # PT_INTERP, not DT_NEEDED.
    by_soname = {}
    for n in members:
        if posixpath.dirname(n) in LIB_DIRS and '.so' in posixpath.basename(n):
            by_soname.setdefault(posixpath.basename(n), n)
    libs, todo = set(), [n for n in KEEP_PROGRAMS]
    while todo:
        n = resolve(todo.pop())
        if n is None or n in libs:
            continue
        libs.add(n)
        for so in needed(data.get(n, b'')) or []:
            if so not in by_soname:
                sys.exit(f'{n} needs {so}, which is not in the image')
            todo.append(by_soname[so])
    for n in list(keep):
        m = members[n]
        if posixpath.dirname(n) == 'usr/lib' and '.so' in posixpath.basename(n):
            if (resolve(n) or n) not in libs:
                keep.discard(n)

    # Ours replace theirs.
    ours = {}
    for root, dirs, files in os.walk(overlay):
        for f in files:
            path = os.path.join(root, f)
            ours[os.path.relpath(path, overlay)] = path
    keep -= set(ours)

    # Links whose target went (busybox applets stay: busybox is kept). Links
    # dangling in the base image point at runtime paths
    # (/etc/resolv.conf -> ../tmp/resolv.conf) and stay.
    def alive(n):
        if not members[n].issym():
            return True
        t = resolve(n)
        return t is None or t in keep

    keep = {n for n in keep if alive(n)}

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.GNU_FORMAT) as dst:
        for m in src.getmembers():
            n = norm(m.name)
            if n in keep:
                dst.addfile(m, io.BytesIO(data[n]) if m.isreg() else None)
        for n, path in sorted(ours.items()):
            dirs = posixpath.dirname(n).split('/')
            for i in range(1, len(dirs) + 1):
                d = '/'.join(dirs[:i])
                if d and d not in keep:
                    t = tarfile.TarInfo('./' + d)
                    t.type, t.mode = tarfile.DIRTYPE, 0o755
                    dst.addfile(t)
                    keep.add(d)
            t = tarfile.TarInfo('./' + n)
            st = os.stat(path)
            t.size, t.mode, t.mtime = st.st_size, st.st_mode & 0o777, int(st.st_mtime)
            with open(path, 'rb') as fp:
                dst.addfile(t, fp)
    raw = buf.getvalue()
    # CRC32 and a small dictionary: decompressed by busybox in the kernel's
    # initramfs with little RAM.
    xz = lzma.compress(raw, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32,
                       filters=[{'id': lzma.FILTER_LZMA2, 'preset': 9, 'dict_size': 4 << 20}])
    with open(out, 'wb') as fp:
        fp.write(xz)
    total = sum(members[n].size for n in keep if n in members and members[n].isreg())
    print(f'{out}: {len(keep)} entries, {total / 1048576:.1f} MiB of files, '
          f'tar {len(raw) / 1048576:.1f} MiB, xz {len(xz) / 1048576:.1f} MiB')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
