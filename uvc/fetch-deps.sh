#!/bin/sh
# Download pinned third-party sources into third_party/ (gitignored).
set -e
cd "$(dirname "$0")"
mkdir -p third_party
cd third_party

MBEDTLS=3.6.4
CJSON=1.7.18
NCNN=20260526
STB=003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
DROPBEAR=2026.94
VOAAC=0.1.3
VOAAC_SHA256=ae38610405f862237f6c70360c406297cc4222abf82e86722c505ba74b53a97d
# Signed by the Dropbear release key F734 7EF2 EE2E 07A2 6762 8CA9 4493 1494 F29C 6773
DROPBEAR_SHA256=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81

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

# Neural network inference for the object detector (built by the Makefile).
if [ ! -d ncnn ]; then
    curl -fsSL "https://github.com/Tencent/ncnn/archive/refs/tags/$NCNN.tar.gz" | tar xz
    mv "ncnn-$NCNN" ncnn
fi

# AAC-LC encoder for the microphone (Android's, Apache-2.0)
if [ ! -d vo-aacenc ]; then
    curl -fsSL "https://github.com/mstorsjo/vo-aacenc/archive/refs/tags/v$VOAAC.tar.gz" -o vo-aacenc.tar.gz
    echo "$VOAAC_SHA256  vo-aacenc.tar.gz" | shasum -a 256 -c - >/dev/null
    tar xzf vo-aacenc.tar.gz
    rm vo-aacenc.tar.gz
    mv "vo-aacenc-$VOAAC" vo-aacenc
fi

# stb_image_write: JPEG encoding of detection crops.
if [ ! -d stb ]; then
    mkdir stb
    curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$STB/stb_image_write.h" -o stb/stb_image_write.h
fi

# The camera's SSH server (built by tools/build-dropbear.sh): the base
# image's is from 2015 and its libcrypt has no SHA-512 crypt.
if [ ! -d dropbear ]; then
    curl -fsSL "https://matt.ucc.asn.au/dropbear/releases/dropbear-$DROPBEAR.tar.bz2" -o dropbear.tar.bz2
    echo "$DROPBEAR_SHA256  dropbear.tar.bz2" | shasum -a 256 -c - >/dev/null
    tar xjf dropbear.tar.bz2
    rm dropbear.tar.bz2
    mv "dropbear-$DROPBEAR" dropbear
fi

# Yolo-Fastest 1.1 (COCO, ncnn format). Its detection layer's built-in
# confidence cutoff is lowered from 0.55 to 0.30; uvcd applies its own
# thresholds and confirms detections over time.
if [ ! -d models ]; then
    mkdir models
    M=https://raw.githubusercontent.com/dog-qiuqiu/Yolo-Fastest/master/sample/ncnn/model
    curl -fsSL "$M/yolo-fastest-1.1.bin" -o models/yolo-fastest-1.1.bin
    curl -fsSL "$M/yolo-fastest-1.1.param" |
        sed 's/ 2=5.500000e-01 / 2=3.000000e-01 /' > models/yolo-fastest-1.1.param
fi

echo "Done."
