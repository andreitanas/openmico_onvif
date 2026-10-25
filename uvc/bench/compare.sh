#!/bin/sh
# Detections of each model variant on each test image.
#   ./compare.sh [bench binary] [iterations]
cd "$(dirname "$0")"
BENCH=${1:-./bench-host}
ITERS=${2:-1}
M=models
[ -d models ] || M=.   # on the camera everything sits in one directory
[ -d images ] && I=images || I=.

run() { # model-param model-bin image W H inW inH threshold
    echo "-- $1 @ $6x$7, threshold $8"
    $BENCH $M/$1.param $M/$2.bin $I/$3.rgb $4 $5 $6 $7 $ITERS $8
}

for img in "dog 768 576" "person 640 424" "scene 640 360"; do
    set -- $img
    [ -f $I/$1.rgb ] || continue
    echo "== $1"
    for v in $MODELS; do
        case $v in
        fd)     run FastestDet FastestDet $1 $2 $3 352 192 0.5 ;;
        fd35)   run FastestDet FastestDet $1 $2 $3 352 192 0.35 ;;
        fdsq)   run FastestDet FastestDet $1 $2 $3 352 352 0.5 ;;
        fd8)    run FastestDet-int8 FastestDet-int8 $1 $2 $3 352 352 0.5 ;;
        yf)     run yolo-fastest-1.1-t30 yolo-fastest-1.1 $1 $2 $3 320 192 0.3 ;;
        yf8)    run yolo-fastest-1.1-int8 yolo-fastest-1.1-int8 $1 $2 $3 320 192 0.3 ;;
        esac
    done
done
