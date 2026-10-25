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

# stb_image_write: JPEG encoding of detection crops.
if [ ! -d stb ]; then
    mkdir stb
    curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$STB/stb_image_write.h" -o stb/stb_image_write.h
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
