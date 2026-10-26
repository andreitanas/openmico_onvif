#!/bin/sh
# Prepare an SD card (FAT32, partition 1) that installs and configures one
# camera:
#   demo.bin            our image (image/build.sh); the Wyze bootloader
#                       flashes it when the setup button is held at power-on
#   config/overlay/     wifi, passwd, ssh key (../deploy, made by
#                       ../deploy.sh), copied over the root filesystem at boot
#   config/uvc/         uvcd.conf
# The camera imports config/ into its config flash at every boot when the
# card's copy differs; the card is optional after that. It then shows up in
# Protect, ready to adopt. Nothing on the card is per camera, so one card
# sets up any number: each camera names itself after its MAC (wyze-2BE8)
# and makes its own certificate.
#
# Usage: ./make-sdcard.sh /Volumes/CARD
set -e
cd "$(dirname "$0")"
CARD=${1:?usage: $0 /Volumes/CARD}
DEPLOY=../deploy

[ -d "$CARD" ] || { echo "$CARD is not mounted"; exit 1; }
[ -d "$DEPLOY/config/overlay" ] || { echo "no $DEPLOY/config/overlay; run ../deploy.sh utils and passwd"; exit 1; }
image/build.sh

mkdir -p "$CARD/config/overlay" "$CARD/config/uvc"
cp -R "$DEPLOY/config/overlay/." "$CARD/config/overlay/"
# OpenMiko's settings file has no meaning for our image, and a hostname
# would be the same on every camera.
rm -f "$CARD/config/overlay/etc/openmiko.conf" "$CARD/config/overlay/etc/hostname"
cp build/image/demo.bin "$CARD/demo.bin"

if [ ! -f "$CARD/config/uvc/uvcd.conf" ]; then
    cp image/uvcd.conf.template "$CARD/config/uvc/uvcd.conf"
fi
# Cards made before cameras made their own certificates carry one, which
# every camera set up from the card would share.
rm -f "$CARD/config/uvc/client.pem"

# macOS litters FAT volumes with metadata files; the camera imports every
# file under config/.
find "$CARD" -name '._*' -delete 2>/dev/null || true
find "$CARD" -name '.DS_Store' -delete 2>/dev/null || true
sync
echo "Card ready:"
(cd "$CARD" && find . -type f -not -path './.Spotlight*' -not -path './.fseventsd*' | sort)
