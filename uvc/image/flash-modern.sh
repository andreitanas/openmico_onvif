#!/bin/sh
# Write our kernel and root filesystem (image/build-modern.sh) to a camera's
# kernel (mtd1) and root (mtd2) flash partitions over ssh, verify them, and
# reboot into them.
#
#   image/flash-modern.sh [camera-ip] [--no-reboot] [--no-rollback]
#
# Works from OpenMiko's kernel (root in RAM) and from ours (root is the
# squashfs on mtd2: the camera runs from busybox and uClibc copied to /tmp
# and reboots right after the write, without unmounting anything).
#
# Both partitions are saved to build/image-modern/mtd{1,2}-<ip>-<time>.bin
# first and written back if anything does not verify. They also go to the
# camera's SD card as /sdcard/rollback/image.bin (a demo.bin), which makes
# the new image's boots trials: unless `rollback keep` runs on the camera
# within 10 minutes of boot, the camera writes the saved image back and
# reboots into it (/usr/sbin/rollback). A kernel that hangs before userspace
# is recovered with demo.bin on the SD card (docs/kernel-modern.md,
# "Recovery"): keep one at hand before running this.
set -e
cd "$(dirname "$0")/.."
CAM=192.168.1.9
REBOOT=1
ROLLBACK=1
for a; do
    case "$a" in
    --no-reboot) REBOOT=0 ;;
    --no-rollback) ROLLBACK=0 ;;
    *) CAM=$a ;;
    esac
done
DEMO=${DEMO:-../deploy/demo.bin}
DIR=build/image-modern
KERNEL=$DIR/uImage
ROOT=$DIR/root.sqfs
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o HostKeyAlgorithms=+ssh-rsa -o LogLevel=ERROR root@$CAM"
cam() { $SSH "export PATH=/tmp/fl:/usr/sbin:/sbin:/usr/bin:/bin; $*"; }
md5() { md5sum "$@" 2>/dev/null || md5 -r "$@"; }

for f in $KERNEL $ROOT; do
    [ -f $f ] || { echo "no $f; run image/build-modern.sh" >&2; exit 1; }
done
[ "$(wc -c < $KERNEL)" -le $((0x200000)) ] || { echo "$KERNEL is larger than the kernel partition" >&2; exit 1; }
[ "$(wc -c < $ROOT)" -le $((0xd30000)) ] || { echo "$ROOT is larger than the root partition" >&2; exit 1; }
cam "grep -q '\"kernel\"' /proc/mtd && grep -q '\"root\"' /proc/mtd" ||
    { echo "$CAM has no kernel and root MTD partitions" >&2; exit 1; }
if [ $ROLLBACK = 1 ]; then
    [ -f "$DEMO" ] || { echo "no $DEMO (for the rollback image's header)" >&2; exit 1; }
    cam "grep -q ' /sdcard ' /proc/mounts" ||
        { echo "$CAM has no SD card mounted for the rollback image (or use --no-rollback)" >&2; exit 1; }
    [ "$(cam "df -k /sdcard | awk 'NR == 2 { print \$4 }'")" -gt 24000 ] ||
        { echo "less than 24 MB free on $CAM's SD card" >&2; exit 1; }
fi

STAMP=$CAM-$(date +%Y%m%d-%H%M%S)
for n in 1 2; do
    echo "saving mtd$n to $DIR/mtd$n-$STAMP.bin"
    cam "cat /dev/mtdblock$n" > $DIR/mtd$n-$STAMP.bin
    [ "$(cam "md5sum < /dev/mtdblock$n" | cut -d' ' -f1)" = "$(md5 < $DIR/mtd$n-$STAMP.bin | cut -d' ' -f1)" ] ||
        { echo "backup of mtd$n does not match the camera's partition" >&2; exit 1; }
done

# The previous image for the camera's rollback: kernel partition and the
# root partition's first 274 blocks (all of either image's root) as a
# demo.bin, uploaded before anything is written, armed for our kernel only.
if [ $ROLLBACK = 1 ]; then
    head -c $((274 * 32768)) $DIR/mtd2-$STAMP.bin > $DIR/root-$STAMP.bin
    python3 image/demo-bin.py "$DEMO" $DIR/root-$STAMP.bin $DIR/rollback-$STAMP.bin $DIR/mtd1-$STAMP.bin
    rm $DIR/root-$STAMP.bin
    SUM=$(md5 < $DIR/rollback-$STAMP.bin | cut -d' ' -f1)
    echo "uploading the rollback image to $CAM:/sdcard/rollback/image.bin"
    cam "mkdir -p /sdcard/rollback && cat > /sdcard/rollback/image.tmp" < $DIR/rollback-$STAMP.bin
    [ "$(cam "md5sum < /sdcard/rollback/image.tmp" | cut -d' ' -f1)" = "$SUM" ] ||
        { echo "rollback image on $CAM's SD card does not match" >&2; exit 1; }
    cam "cd /sdcard/rollback && mv image.tmp image.bin && echo $SUM > image.md5 &&
        echo $(head -c 64 $KERNEL | md5 | cut -d' ' -f1) > kernel.md5 && rm -f boots && sync"
fi

# Our kernel runs from mtd2: keep the tools in RAM while it is rewritten,
# uClibc too (busybox is linked against it).
MODERN=0
cam "awk '\$2 == \"/\" && \$3 == \"squashfs\" { f = 1 } END { exit !f }' /proc/mounts" && MODERN=1
cam "mkdir -p /tmp/fl && cp /bin/busybox /tmp/fl/ && for a in sh cat dd md5sum flashcp rm sync; do ln -sf busybox /tmp/fl/\$a; done"
[ $MODERN = 0 ] ||
    cam "mkdir -p /tmp/fl/lib && cp -L /lib/ld-uClibc.so.0 /lib/libc.so.1 /tmp/fl/lib/ && mount -o bind /tmp/fl/lib /lib"

# Stop uvcd (and init's respawn of it; the edit lives in RAM); on
# OpenMiko's kernel also unmount the old root image from /rom.
cam "sed -i 's|^::respawn:/usr/bin/uvcd-run|#&|' /etc/inittab; kill -HUP 1
    killall uvcd-run uvcd 2>/dev/null; sleep 3
    if grep -q ' /rom ' /proc/mounts && [ $MODERN = 0 ]; then umount /rom; fi"

write() { # image partition
    cam "rm -f /tmp/img; cat > /tmp/img" < "$1"
    [ "$(cam "md5sum < /tmp/img" | cut -d' ' -f1)" = "$(md5 < "$1" | cut -d' ' -f1)" ] || return 1
    cam "flashcp /tmp/img /dev/mtd$2; r=\$?; rm -f /tmp/img; exit \$r"
}

verify() { # image partition
    want=$(md5 < "$1" | cut -d' ' -f1)
    got=$(cam "dd if=/dev/mtd$2 bs=4096 2>/dev/null | head -c $(wc -c < "$1") | md5sum" | cut -d' ' -f1)
    [ "$want" = "$got" ] || { echo "mtd$2 does not match $1" >&2; return 1; }
}

restore() {
    echo "restoring mtd1 and mtd2" >&2
    ok=1
    for n in 2 1; do
        write $DIR/mtd$n-$STAMP.bin $n && verify $DIR/mtd$n-$STAMP.bin $n || ok=0
    done
    [ $ok = 1 ] && echo "restored; the camera still boots its previous image" >&2 ||
        echo "RESTORE FAILED: do not reboot $CAM; rewrite $DIR/mtd*-$STAMP.bin by hand" >&2
    exit 1
}

echo "writing $ROOT"
write $ROOT 2 || restore
verify $ROOT 2 || restore
echo "writing $KERNEL"
write $KERNEL 1 || restore
verify $KERNEL 1 || restore
echo "flashed and verified"

if [ $REBOOT = 1 ] || [ $MODERN = 1 ]; then
    # From our kernel the old root is gone under the running system: reboot
    # at once, from RAM, without touching the root filesystem again.
    echo "rebooting $CAM"
    if [ $MODERN = 1 ]; then
        cam "sync; (sleep 1; echo b > /proc/sysrq-trigger) > /dev/null 2>&1 &"
    else
        cam "sync; (sleep 1; reboot) > /dev/null 2>&1 &"
    fi
else
    cam "sed -i 's|^#::respawn|::respawn|' /etc/inittab; kill -HUP 1"
fi
