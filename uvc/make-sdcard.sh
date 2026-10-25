#!/bin/sh
# Prepare an SD card (FAT32, partition 1) that installs and configures one
# camera:
#   demo.bin            our image (image/build.sh); the Wyze bootloader
#                       flashes it when the setup button is held at power-on
#   config/overlay/     wifi, hostname, passwd, ssh key (../deploy, made by
#                       ../deploy.sh), copied over the root filesystem at boot
#   config/uvc/         uvcd.conf and a per-camera client certificate
# The camera imports config/ into its config flash at every boot when the
# card's copy differs; the card is optional after that.
#
# Usage: ./make-sdcard.sh /Volumes/CARD [nvr-host]
set -e
cd "$(dirname "$0")"
CARD=${1:?usage: $0 /Volumes/CARD [nvr-host]}
NVR=${2:-192.168.1.1}
DEPLOY=../deploy

[ -d "$CARD" ] || { echo "$CARD is not mounted"; exit 1; }
[ -d "$DEPLOY/config/overlay" ] || { echo "no $DEPLOY/config/overlay; run ../deploy.sh utils and passwd"; exit 1; }
image/build.sh

mkdir -p "$CARD/config/overlay" "$CARD/config/uvc"
cp -R "$DEPLOY/config/overlay/." "$CARD/config/overlay/"
# OpenMiko's settings file has no meaning for our image.
rm -f "$CARD/config/overlay/etc/openmiko.conf"
cp build/image/demo.bin "$CARD/demo.bin"

if [ ! -f "$CARD/config/uvc/uvcd.conf" ]; then
    sed "s|@NVR_HOST@|$NVR|" image/uvcd.conf.template > "$CARD/config/uvc/uvcd.conf"
fi
if [ ! -f "$CARD/config/uvc/client.pem" ]; then
    # Per-camera client certificate, the shape real UniFi cameras present.
    TMP=$(mktemp -d)
    openssl ecparam -out "$TMP/k" -name prime256v1 -genkey -noout
    openssl req -new -x509 -sha256 -days 36500 -key "$TMP/k" -out "$TMP/c" \
        -subj "/C=TW/L=Taipei/O=Ubiquiti Networks Inc./OU=devint/CN=camera.ubnt.dev/emailAddress=support@ubnt.com"
    cat "$TMP/k" "$TMP/c" > "$CARD/config/uvc/client.pem"
    rm -rf "$TMP"
fi

# macOS litters FAT volumes with metadata files; the camera imports every
# file under config/.
find "$CARD" -name '._*' -delete 2>/dev/null || true
find "$CARD" -name '.DS_Store' -delete 2>/dev/null || true
sync
echo "Card ready (set ADOPTION_TOKEN in config/uvc/uvcd.conf):"
(cd "$CARD" && find . -type f -not -path './.Spotlight*' -not -path './.fseventsd*' | sort)
