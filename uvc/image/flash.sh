#!/bin/sh
# Write build/image/root.jffs2 (image/build.sh) to a camera's root flash
# partition over ssh, verify it, and reboot into it.
#
#   image/flash.sh [camera-ip] [--no-reboot]
#
# The current partition is saved to build/image/mtd2-<ip>-<time>.bin first.
# If anything does not verify, that copy is written back before the camera
# could boot from a bad image (OpenMiko's initramfs deletes the partition's
# contents when it cannot unpack rootfs.tar.xz; recovery is then demo.bin on
# an SD card and the setup button).
set -e
cd "$(dirname "$0")/.."
CAM=${1:-192.168.1.9}
REBOOT=1
[ "$2" = "--no-reboot" ] && REBOOT=0
IMG=build/image/root.jffs2
BLOCKS=274
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o HostKeyAlgorithms=+ssh-rsa -o LogLevel=ERROR root@$CAM"
cam() { $SSH "export PATH=/usr/sbin:/sbin:/usr/bin:/bin; $*"; }
md5() { md5sum "$@" 2>/dev/null || md5 -r "$@"; }

[ -f $IMG ] || { echo "no $IMG; run image/build.sh" >&2; exit 1; }
[ "$(wc -c < $IMG)" -eq $((BLOCKS * 32768)) ] || { echo "$IMG is not $BLOCKS blocks" >&2; exit 1; }
cam "grep -q '\"root\"' /proc/mtd" || { echo "$CAM has no root MTD partition" >&2; exit 1; }

SAVE=build/image/mtd2-$CAM-$(date +%Y%m%d-%H%M%S).bin
echo "saving the current root partition to $SAVE"
cam "cat /dev/mtdblock2" > "$SAVE"
[ "$(cam "md5sum < /dev/mtdblock2" | cut -d' ' -f1)" = "$(md5 < "$SAVE" | cut -d' ' -f1)" ] ||
    { echo "backup does not match the camera's partition" >&2; exit 1; }
# The block OpenMiko's initramfs checks must stay a bare cleanmarker.
PROTECTED=$(cam "dd if=/dev/mtdblock2 bs=32768 skip=$BLOCKS count=1 2>/dev/null | md5sum" | cut -d' ' -f1)

write() { # image file on this machine
    cam "rm -f /tmp/root.jffs2; cat > /tmp/root.jffs2" < "$1"
    [ "$(cam "md5sum < /tmp/root.jffs2" | cut -d' ' -f1)" = "$(md5 < "$1" | cut -d' ' -f1)" ] || return 1
    cam "flashcp /tmp/root.jffs2 /dev/mtd2; r=\$?; rm -f /tmp/root.jffs2; exit \$r"
}

verify() {
    want=$(head -c $((BLOCKS * 32768)) "$1" | md5 | cut -d' ' -f1)
    got=$(cam "dd if=/dev/mtdblock2 bs=32768 count=$BLOCKS 2>/dev/null | md5sum" | cut -d' ' -f1)
    [ "$want" = "$got" ] || { echo "partition does not match the image" >&2; return 1; }
    got=$(cam "dd if=/dev/mtdblock2 bs=32768 skip=$BLOCKS count=1 2>/dev/null | md5sum" | cut -d' ' -f1)
    [ "$got" = "$PROTECTED" ] || { echo "block $BLOCKS changed" >&2; return 1; }
}

restore() {
    echo "restoring $SAVE" >&2
    cam "cat > /tmp/root.jffs2; flashcp /tmp/root.jffs2 /dev/mtd2; rm -f /tmp/root.jffs2" < "$SAVE"
    [ "$(cam "md5sum < /dev/mtdblock2" | cut -d' ' -f1)" = "$(md5 < "$SAVE" | cut -d' ' -f1)" ] &&
        echo "restored; the camera still boots its previous image" >&2 ||
        echo "RESTORE FAILED: do not reboot $CAM; rewrite $SAVE by hand" >&2
    exit 1
}

# Our image mounts the partition at /rom and may run uvcd from it. A stale
# mount would also make the read-back check below read cached nodes, so stop
# uvcd (and init's respawn of it; the edit lives in RAM) and unmount.
cam "if grep -q ' /rom ' /proc/mounts; then
        sed -i 's|^::respawn:/usr/bin/uvcd-run|#&|' /etc/inittab; kill -HUP 1
        killall uvcd-run uvcd 2>/dev/null; sleep 3; umount /rom
    fi"

echo "writing $IMG (about a minute)"
write $IMG || restore
verify $IMG || restore

# The files as the camera reads them, against the build.
cam "umount /tmp/mtd2 2>/dev/null; mkdir -p /tmp/mtd2 && mount -t jffs2 -o ro /dev/mtdblock2 /tmp/mtd2 &&
    cd /tmp/mtd2 && find . -type f | sort | xargs md5sum; cd /; umount /tmp/mtd2" > build/image/flashed.md5
if ! diff build/image/payload.md5 build/image/flashed.md5; then
    echo "files read back from flash differ from the build" >&2
    restore
fi
echo "flashed and verified"

if [ $REBOOT = 1 ]; then
    echo "rebooting $CAM"
    cam "sync; (sleep 1; reboot) > /dev/null 2>&1 &"
else
    cam "if grep -q '^#::respawn:/usr/bin/uvcd-run' /etc/inittab; then
            mount -t jffs2 -o ro /dev/mtdblock2 /rom
            sed -i 's|^#::respawn|::respawn|' /etc/inittab; kill -HUP 1
        fi"
fi
