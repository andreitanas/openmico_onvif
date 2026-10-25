#!/bin/sh
# Calibrate on camera frames (images/calib/*.jpg) + the darknet test images
# and write int8 versions of both models.
set -e
cd "$(dirname "$0")"
Q=build-tools/tools/quantize
(ls "$PWD"/images/calib/*.jpg; echo "$PWD/images/dog.jpg"; echo "$PWD/images/person.jpg") > images/calib.txt
echo "$(wc -l < images/calib.txt) calibration images"

quantize() { # name param bin W H
    $Q/ncnn2table models/$2.param models/$3.bin images/calib.txt models/$1.table \
        'mean=[0,0,0]' 'norm=[0.003922,0.003922,0.003922]' pixel=RGB thread=8 method=kl \
        "shape=[$4,$5,3]" > /dev/null
    $Q/ncnn2int8 models/$2.param models/$3.bin models/$1-int8.param models/$1-int8.bin models/$1.table
}

# Yolo-Fastest keeps the lowered 0.30 cutoff in its int8 variant.
quantize yolo-fastest-1.1 yolo-fastest-1.1-t30 yolo-fastest-1.1 320 192
quantize FastestDet FastestDet FastestDet 352 352
ls -l models/*int8*
