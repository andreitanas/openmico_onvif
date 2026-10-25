#!/bin/sh
# Download pinned third-party sources into third_party/ (gitignored).
set -e
cd "$(dirname "$0")"
mkdir -p third_party
cd third_party

MBEDTLS=3.6.4
CJSON=1.7.18

if [ ! -d mbedtls ]; then
    curl -fsSL "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS/mbedtls-$MBEDTLS.tar.bz2" | tar xj
    mv "mbedtls-$MBEDTLS" mbedtls
fi

if [ ! -d cjson ]; then
    curl -fsSL "https://github.com/DaveGamble/cJSON/archive/refs/tags/v$CJSON.tar.gz" | tar xz
    mv "cJSON-$CJSON" cjson
fi

# Ingenic T20 SDK headers + libimp (prebuilt, no source), as used by
# OpenMiko's videocapture.
if [ ! -d ingenic ]; then
    git clone -q https://github.com/openmiko/ingenic_videocap ingenic
    git -C ingenic checkout -q 86c1576
fi

echo "Done."
