#!/usr/bin/env python3
"""Dump and validate FLV streams, including UniFi Protect's variant.

Plain FLV:   flvdump.py file.flv
UniFi FLV:   flvdump.py --unifi file.flv   (expects the 16-byte wall-clock
             trailer after every PreviousTagSize, header flags 0x07)

Exits non-zero on any structural error, so it doubles as a test oracle.
"""
import argparse
import struct
import sys

TYPES = {8: "audio", 9: "video", 18: "script"}


def amf_value(b, i):
    t = b[i]
    i += 1
    if t == 0:
        return struct.unpack(">d", b[i:i + 8])[0], i + 8
    if t == 1:
        return bool(b[i]), i + 1
    if t == 2:
        n = struct.unpack(">H", b[i:i + 2])[0]
        return b[i + 2:i + 2 + n].decode(), i + 2 + n
    if t in (3, 8):
        if t == 8:
            i += 4
        out = {}
        while True:
            n = struct.unpack(">H", b[i:i + 2])[0]
            if n == 0 and b[i + 2] == 9:
                return (("obj" if t == 3 else "ecma"), out), i + 3
            key = b[i + 2:i + 2 + n].decode()
            out[key], i = amf_value(b, i + 2 + n)
    raise ValueError(f"unsupported AMF type {t} at {i - 1}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--unifi", action="store_true")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()
    data = open(args.file, "rb").read()
    err = []

    if data[:3] != b"FLV":
        sys.exit("not FLV")
    flags = data[4]
    if args.unifi and flags != 7:
        err.append(f"header flags {flags:#x}, expected 0x07")
    i = struct.unpack(">I", data[5:9])[0]
    if struct.unpack(">I", data[i:i + 4])[0] != 0:
        err.append("PreviousTagSize0 not zero")
    i += 4

    counts = {}
    last_ts = {}
    last_wall = {}
    syncs = 0
    while i + 11 <= len(data):
        ttype = data[i]
        size = int.from_bytes(data[i + 1:i + 4], "big")
        ts = int.from_bytes(data[i + 4:i + 7], "big") | data[i + 7] << 24
        body = data[i + 11:i + 11 + size]
        if len(body) < size:
            break  # truncated final tag (stream cut) is fine
        prev = struct.unpack(">I", data[i + 11 + size:i + 15 + size])[0]
        if prev != size + 11:
            err.append(f"tag @{i}: PreviousTagSize {prev} != {size + 11}")
        name = TYPES.get(ttype, str(ttype))
        counts[name] = counts.get(name, 0) + 1
        desc = ""
        if ttype == 18:
            label, j = amf_value(body, 0)
            val, _ = amf_value(body, j)
            desc = f"{label} {val}"
            if label == "onClockSync":
                syncs += 1
        elif ttype == 9:
            desc = f"frame={body[0] >> 4} codec={body[0] & 15} pkt={body[1]}"
            if body[0] & 15 == 7 and body[1] == 1:
                # Validate the AVCC length-prefixed NALs.
                j = 5
                nals = []
                while j < len(body):
                    n = struct.unpack(">I", body[j:j + 4])[0]
                    if n == 0 or j + 4 + n > len(body):
                        err.append(f"tag @{i}: bad NAL length {n}")
                        break
                    nals.append(body[j + 4] & 31)
                    j += 4 + n
                desc += f" nals={nals}"
                if (body[0] >> 4 == 1) != (5 in nals):
                    err.append(f"tag @{i}: key flag does not match IDR presence")
            if ttype in last_ts and ts < last_ts[ttype]:
                err.append(f"tag @{i}: video ts went backwards")
        elif ttype == 8:
            desc = f"fmt={body[0]:#x} pkt={body[1]} data={body[2:10].hex()}"
        if ttype in (8, 9) and ttype in last_ts and ts < last_ts[ttype]:
            err.append(f"tag @{i}: {name} ts went backwards {last_ts[ttype]} -> {ts}")
        last_ts[ttype] = ts
        i += 15 + size

        if args.unifi:
            tr = data[i:i + 16]
            if len(tr) < 16:
                break
            clock = int.from_bytes(tr[1:4], "big")
            want = 90000 if ttype == 9 else 11025
            if tr[0] != 0 or tr[4:12] != bytes(8):
                err.append(f"trailer @{i}: bad padding {tr.hex()}")
            if clock != want:
                err.append(f"trailer @{i}: clock {clock}, expected {want} for {name}")
            ticks = struct.unpack(">I", tr[12:16])[0]
            wall = ticks / clock
            key = "v" if ttype == 9 else "o"
            if key in last_wall and wall + 0.001 < last_wall[key]:
                err.append(f"trailer @{i}: wall clock went backwards")
            last_wall[key] = wall
            desc += f" wall={wall:.3f}s"
            i += 16

        if not args.quiet:
            print(f"{name:6} ts={ts:6} size={size:6} {desc}")

    print(f"tags: {counts}, clock syncs: {syncs}", file=sys.stderr)
    for e in err[:20]:
        print("ERROR:", e, file=sys.stderr)
    sys.exit(1 if err else 0)


if __name__ == "__main__":
    main()
