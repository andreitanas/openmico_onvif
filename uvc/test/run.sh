#!/bin/sh
# Host tests: FLV framing against ffmpeg's muxer and the UniFi trailer rules.
set -e
cd "$(dirname "$0")/.."
T=build/test
mkdir -p $T

if [ ! -f $T/clip.h264 ]; then
    ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -t 4 \
        -c:v libx264 -profile:v main -bf 0 -g 10 -pix_fmt yuv420p -f h264 $T/clip.h264
fi
ffmpeg -loglevel error -y -f h264 -framerate 25 -i $T/clip.h264 -c:v copy -an \
    -metadata streamName=testStream -f flv $T/ffmpeg.flv

echo "== replaying clip through source + muxer (7 s, crosses one clock sync)"
./build/test_flv $T/clip.h264 $T/uvcd.flv 7

echo "== UniFi framing"
python3 -I tools/flvdump.py --unifi -q $T/uvcd.flv

echo "== payloads vs ffmpeg"
python3 -I tools/flvcompare.py $T/ffmpeg.flv $T/uvcd.flv

echo "== ffprobe decodes it (after stripping trailers)"
python3 -I - $T/uvcd.flv $T/plain.flv <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
out = bytearray(d[:9]) + bytes(4)
out[4] = 5
i = 13
while i + 11 <= len(d):
    size = int.from_bytes(d[i + 1:i + 4], "big")
    if i + 15 + size > len(d):
        break
    out += d[i:i + 15 + size]
    i += 15 + size + 16
open(sys.argv[2], "wb").write(out)
EOF
ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=codec_name,width,height,nb_read_frames -of csv=p=0 $T/plain.flv

echo "OK"
