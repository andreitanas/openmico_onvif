#!/bin/sh
# Populate an OpenMiko sdcard (FAT32, partition 1) for one camera:
#   - the existing overlay from ../deploy/config/overlay (wifi, passwd, ssh key,
#     ONVIF hack), with openmiko.conf pointed at partition 1 + a swap file
#   - uvcd, its config and a per-camera client certificate under /uvc
#   - S90uvcd (starts uvcd) and S99probe (writes /probe.txt on boot)
#   - demo.bin (OpenMiko image; only flashed when the setup button is held)
#
# Usage: ./make-sdcard.sh /Volumes/WYZECAM [nvr-host]
set -e
cd "$(dirname "$0")"
CARD=${1:?usage: $0 /Volumes/CARD [nvr-host]}
NVR=${2:-192.168.1.1}
DEPLOY=../deploy

[ -d "$CARD" ] || { echo "$CARD is not mounted"; exit 1; }
[ -f build/uvcd-mipsel ] || make camera

mkdir -p "$CARD/config/overlay" "$CARD/uvc"
cp -R "$DEPLOY/config/overlay/." "$CARD/config/overlay/"
cp ../app/onvif.py ../app/onvif_discover.py "$CARD/config/overlay/app/"
cp ../etc/init.d/S66onvif "$CARD/config/overlay/etc/init.d/"
# S60camera starts uvcd in place of videocapture (VIDEO_CAPTURE=uvcd below).
cp sdcard/S60camera sdcard/S90uvcd sdcard/S99probe "$CARD/config/overlay/etc/init.d/"
# Only used with VIDEO_CAPTURE=videocapture (uvcd.conf CAPTURE=0): adds a
# 640x360 H.264 encoder on /dev/video5 for Protect's video2/video3.
cp sdcard/videocapture_settings.json "$CARD/config/overlay/etc/"

# Single FAT32 partition: sdcard is p1, swap is a file on it. Log to the card
# while bringing the camera up.
{
    sed -e 's|^SD_PARTITION=.*|SD_PARTITION=/dev/mmcblk0p1|' \
        -e 's|^SWAPFILE=.*|SWAPFILE=/sdcard/swapfile|' \
        -e 's|^ENABLE_LOGGING=.*|ENABLE_LOGGING=1|' \
        -e '/^VIDEO_CAPTURE=/d' \
        "$DEPLOY/config/overlay/etc/openmiko.conf"
    echo 'VIDEO_CAPTURE=uvcd'
} > "$CARD/config/overlay/etc/openmiko.conf"

cp build/uvcd-mipsel "$CARD/uvc/uvcd"
if [ ! -f "$CARD/uvc/uvcd.conf" ]; then
    sed "s|@NVR_HOST@|$NVR|" sdcard/uvcd.conf.template > "$CARD/uvc/uvcd.conf"
fi
if [ ! -f "$CARD/uvc/client.pem" ]; then
    # Per-camera client certificate, the shape real UniFi cameras present.
    TMP=$(mktemp -d)
    openssl ecparam -out "$TMP/k" -name prime256v1 -genkey -noout
    openssl req -new -x509 -sha256 -days 36500 -key "$TMP/k" -out "$TMP/c" \
        -subj "/C=TW/L=Taipei/O=Ubiquiti Networks Inc./OU=devint/CN=camera.ubnt.dev/emailAddress=support@ubnt.com"
    cat "$TMP/k" "$TMP/c" > "$CARD/uvc/client.pem"
    rm -rf "$TMP"
fi

[ -f "$CARD/demo.bin" ] || cp "$DEPLOY/demo.bin" "$CARD/demo.bin"

# macOS litters FAT volumes with metadata files; keep the overlay clean since
# OpenMiko copies every file under config/ into flash.
find "$CARD" -name '._*' -delete 2>/dev/null || true
find "$CARD" -name '.DS_Store' -delete 2>/dev/null || true
sync
echo "Card ready:"
(cd "$CARD" && find . -type f -not -path './.Spotlight*' -not -path './.fseventsd*' | sort)
