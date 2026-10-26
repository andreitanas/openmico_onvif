#!/bin/sh
# Build the camera's root flash partition image (build/image/root.jffs2) and
# a demo.bin with it (build/image/demo.bin, OpenMiko's kernel):
#   rootfs.tar.xz    our slimmed root filesystem (build-rootfs.py), which
#                    OpenMiko's kernel unpacks into RAM at boot
#   uvc/uvcd         the camera daemon, run from flash (mounted at /rom)
#   uvc/models/      the object detector
# The tarball gets our dropbear (tools/build-dropbear.sh) next to
# image/rootfs/.
# Base: the root filesystem inside OpenMiko's demo.bin (../deploy/demo.bin).
# Install with image/flash.sh over ssh, or with demo.bin on an SD card
# (make-sdcard.sh) and the setup button held at power-on.
set -e
cd "$(dirname "$0")/.."
OUT=build/image
DEMO=${DEMO:-../deploy/demo.bin}
# OpenMiko's initramfs zeroes the root partition from block 274 (of 32 KiB)
# up on every boot, so the image must end before it.
MAX=$((274 * 32768))

make -s camera
[ -x build/host-tools/mkfs.jffs2 ] || tools/build-mkfs-jffs2.sh
mkdir -p $OUT
[ -f $OUT/openmiko-rootfs.tar.xz ] ||
    python3 image/jffs2-extract.py "$DEMO" rootfs.tar.xz $OUT/openmiko-rootfs.tar.xz

[ -x build/dropbear/dropbearmulti ] || tools/build-dropbear.sh
rm -rf $OUT/overlay
cp -R image/rootfs $OUT/overlay
mkdir -p $OUT/overlay/usr/sbin $OUT/overlay/usr/bin
cp build/dropbear/dropbearmulti $OUT/overlay/usr/sbin/dropbear
ln -s ../sbin/dropbear $OUT/overlay/usr/bin/dropbearkey

rm -rf $OUT/payload
mkdir -p $OUT/payload/uvc/models
python3 image/build-rootfs.py $OUT/openmiko-rootfs.tar.xz $OUT/overlay $OUT/payload/rootfs.tar.xz
cp build/uvcd-mipsel $OUT/payload/uvc/uvcd
chmod 755 $OUT/payload/uvc/uvcd
cp third_party/models/yolo-fastest-1.1.param third_party/models/yolo-fastest-1.1.bin \
    $OUT/payload/uvc/models/

# Uncompressed like OpenMiko's (the tarball is xz already), little-endian,
# 4 KiB pages (the camera's, not the build host's), 32 KiB erase blocks with
# cleanmarkers, owned by root.
build/host-tools/mkfs.jffs2 -l -s 4096 -e 0x8000 -m none -U -p -r $OUT/payload -o $OUT/root.jffs2
SIZE=$(wc -c < $OUT/root.jffs2)
if [ "$SIZE" -gt $MAX ]; then
    echo "root.jffs2 is $SIZE bytes, more than the $MAX that survive OpenMiko's boot" >&2
    exit 1
fi
# Padded to all 274 blocks: flashcp erases only what it writes, and JFFS2
# would merge nodes left over from the previous image into our files.
build/host-tools/mkfs.jffs2 -l -s 4096 -e 0x8000 -m none -U -p$MAX -r $OUT/payload -o $OUT/root.jffs2
(cd $OUT/payload && find . -type f | sort | xargs md5sum 2>/dev/null || find . -type f | sort | xargs md5 -r) > $OUT/payload.md5
python3 image/demo-bin.py "$DEMO" $OUT/root.jffs2 $OUT/demo.bin
echo "$OUT/root.jffs2: $SIZE bytes of data ($((SIZE / 32768)) of 274 blocks), padded to $MAX"
