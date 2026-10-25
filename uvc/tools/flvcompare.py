#!/usr/bin/env python3
"""Check that two FLV files carry the same video: identical AVC config and
identical NAL payloads per frame, in order. The second file may be UniFi FLV
(16-byte trailers). Usage: flvcompare.py reference.flv unifi.flv"""
import struct
import sys


def video_tags(path, trailer):
    d = open(path, "rb").read()
    i = struct.unpack(">I", d[5:9])[0] + 4
    out = []
    while i + 11 <= len(d):
        size = int.from_bytes(d[i + 1:i + 4], "big")
        body = d[i + 11:i + 11 + size]
        if len(body) < size:
            break
        if d[i] == 9 and body[1] != 2:  # skip AVC end-of-sequence
            out.append(body)
        i += 15 + size + (16 if trailer else 0)
    return out


ref = video_tags(sys.argv[1], False)
got = video_tags(sys.argv[2], True)
if ref[0] != got[0]:
    sys.exit(f"AVC config differs: {ref[0].hex()} vs {got[0].hex()}")
# The replayed clip loops and the stream may join at any keyframe: align on
# the first frame, then compare in order with wrap-around.
frames, sent = ref[1:], got[1:]
if sent[0] not in frames:
    sys.exit("first streamed frame not found in reference")
start = frames.index(sent[0])
bad = [k for k, f in enumerate(sent) if f != frames[(start + k) % len(frames)]]
if len(sent) < 10 or bad:
    k = bad[0] if bad else 0
    sys.exit(f"{len(bad)}/{len(sent)} video tags differ; first at #{k}")
print(f"AVC config + {len(sent)} frames identical to ffmpeg's FLV (joined at frame {start})")
