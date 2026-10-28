#!/bin/sh
# Build the image for our kernel (kernel/build.sh, Linux 6.18):
#   build/image-modern/uImage    for the kernel partition (mtd1)
#   build/image-modern/root.sqfs for the root partition (mtd2): read-only
#                                squashfs, mounted as / by the kernel
#   build/image-modern/demo.bin  both, for the SD card and the setup button
# The root filesystem is image/build.sh's (OpenMiko's base, slimmed, our
# overlay) without the 3.10 kernel modules, plus our kernel's modules and
# uvcd with its model under /rom/uvc, where uvcd-run looks for it. No
# initramfs: nothing in RAM but /tmp and the writable layer over /etc
# (S00rootfs). Settings stay on the config partition (mtd3, JFFS2), as
# today.
# Install with image/flash-modern.sh over ssh, or with demo.bin.
set -e
cd "$(dirname "$0")/.."
OUT=build/image-modern
DEMO=${DEMO:-../deploy/demo.bin}
KOUT=build/kernel/out
# demo.bin carries the root partition's first 274 blocks (image/demo-bin.py)
MAX=$((274 * 32768))

[ -f $KOUT/uImage ] || kernel/build.sh
make -s camera
mkdir -p build/image $OUT
[ -f build/image/openmiko-rootfs.tar.xz ] ||
    python3 image/jffs2-extract.py "$DEMO" rootfs.tar.xz build/image/openmiko-rootfs.tar.xz

[ -x build/dropbear/dropbearmulti ] || tools/build-dropbear.sh
rm -rf $OUT/overlay $OUT/payload
cp -R image/rootfs $OUT/overlay
mkdir -p $OUT/overlay/usr/sbin $OUT/overlay/usr/bin
cp build/dropbear/dropbearmulti $OUT/overlay/usr/sbin/dropbear
ln -s ../sbin/dropbear $OUT/overlay/usr/bin/dropbearkey
mkdir -p $OUT/payload/rom/uvc/models
# Mount points: the root is read-only (S10config mounts these)
mkdir -p $OUT/payload/config $OUT/payload/sdcard
cp build/uvcd-mipsel $OUT/payload/rom/uvc/uvcd
chmod 755 $OUT/payload/rom/uvc/uvcd
# Camera driver test (docs/kernel-modern.md, hardware test plan)
mkdir -p $OUT/payload/usr/bin
zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
    -o $OUT/payload/usr/bin/v4l2grab capture/v4l2grab.c
zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
    -o $OUT/payload/usr/bin/v4l2jpeg capture/v4l2jpeg.c
zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
    -o $OUT/payload/usr/bin/v4l2h264 capture/v4l2h264.c
zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s \
    -o $OUT/payload/usr/bin/audiorec capture/audiorec.c
cp third_party/models/yolo-fastest-1.1.param third_party/models/yolo-fastest-1.1.bin \
    $OUT/payload/rom/uvc/models/

python3 image/build-rootfs.py build/image/openmiko-rootfs.tar.xz $OUT/overlay $OUT/rootfs.tar \
    $KOUT/modules $OUT/payload
# Owned by root (the tar's owners), xz like the kernel's squashfs support
rm -f $OUT/root.sqfs
sqfstar -quiet -comp xz -b 131072 -no-xattrs -force-uid 0 -force-gid 0 \
    -mkfs-time 0 -all-time 0 $OUT/root.sqfs < $OUT/rootfs.tar
SIZE=$(wc -c < $OUT/root.sqfs)
if [ "$SIZE" -gt $MAX ]; then
    echo "root.sqfs is $SIZE bytes, more than the $MAX demo.bin carries" >&2
    exit 1
fi
cp $KOUT/uImage $OUT/uImage
python3 image/demo-bin.py "$DEMO" $OUT/root.sqfs $OUT/demo.bin $OUT/uImage
echo "$OUT/root.sqfs: $SIZE bytes ($(( (SIZE + 32767) / 32768 )) of 274 blocks); $OUT/uImage: $(wc -c < $OUT/uImage) bytes"
