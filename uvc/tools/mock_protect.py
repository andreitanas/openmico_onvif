#!/usr/bin/env python3
"""A stand-in for UniFi Protect's camera endpoints, for testing uvcd without
an NVR. Standard library only.

- wss://:7442/camera/1.0/ws  requires the client certificate, checks the
  handshake headers and hello, then sends the requests Protect sends after
  adoption and checks every response (envelope fields ds requires,
  inResponseTo matching).
- tcp://:7550                 receives the FLV video push into <out>/video1.flv
- https://:7443/upload        receives the snapshot multipart upload

Usage: mock_protect.py --cert server.pem --client-cert client.pem --out DIR
Exits 0 when every check passed.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import socket
import ssl
import struct
import sys
import threading
import time

TOKEN = "test-token-1234"
STREAM_NAME = "Zm9vYmFy"
results = {}
errors = []


def check(cond, msg):
    if not cond:
        errors.append(msg)
    return cond


def recv_exact(conn, n):
    data = b""
    while len(data) < n:
        chunk = conn.recv(n - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return data


def ws_recv(conn):
    b0, b1 = recv_exact(conn, 2)
    n = b1 & 127
    if n == 126:
        n = struct.unpack(">H", recv_exact(conn, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", recv_exact(conn, 8))[0]
    check(b1 & 0x80, "client frame not masked")
    mask = recv_exact(conn, 4) if b1 & 0x80 else b"\0\0\0\0"
    data = bytes(c ^ mask[i & 3] for i, c in enumerate(recv_exact(conn, n)))
    return b0 & 15, data


def ws_send(conn, obj):
    data = json.dumps(obj).encode()
    hdr = bytes([0x81])
    if len(data) < 126:
        hdr += bytes([len(data)])
    elif len(data) < 65536:
        hdr += bytes([126]) + struct.pack(">H", len(data))
    else:
        hdr += bytes([127]) + struct.pack(">Q", len(data))
    conn.sendall(hdr + data)


def request(mid, fn, payload, expected=True):
    return {"from": "UniFiVideo", "to": "ubnt_avclient", "functionName": fn,
            "messageId": mid, "inResponseTo": 0, "responseExpected": expected,
            "payload": payload}


RFC3339 = re.compile(r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}\+00:00$")


def check_envelope(m):
    for key in ("from", "functionName", "inResponseTo", "messageId", "payload",
                "responseExpected", "statusCode", "timeStamp", "to"):
        check(key in m, f"{m.get('functionName')}: envelope missing {key}")
    check(RFC3339.match(m.get("timeStamp", "")), f"bad timeStamp {m.get('timeStamp')}")
    check(m.get("from") == "ubnt_avclient" and m.get("to") == "UniFiVideo", "bad from/to")


def handle_ws(conn, args):
    head = b""
    while b"\r\n\r\n" not in head:
        head += conn.recv(4096)
    text = head.decode()
    lines = text.split("\r\n")
    path = lines[0].split()[1]
    hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    check(path == f"/camera/1.0/ws?token={TOKEN}", f"path {path}")
    check(hdrs.get("camera-mac") == "AABBCC001122", f"camera-mac {hdrs.get('camera-mac')}")
    check(hdrs.get("camera-model") == "0xa598", f"camera-model {hdrs.get('camera-model')}")
    check(hdrs.get("sec-websocket-protocol") == "secure_transfer", "subprotocol")
    check(re.match(r"^[0-9a-f-]{36}$", hdrs.get("device-id", "")), "device-id")
    accept = base64.b64encode(hashlib.sha1(
        (hdrs["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
    conn.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                 b"Connection: Upgrade\r\nSec-WebSocket-Protocol: secure_transfer\r\n"
                 b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")

    op, data = ws_recv(conn)
    hello = json.loads(data)
    check_envelope(hello)
    p = hello["payload"]
    check(hello["functionName"] == "ubnt_avclient_hello", "first message is not hello")
    check(p.get("adoptionCode") == TOKEN, "adoptionCode")
    check(p.get("mac") == "AABBCC001122" and p.get("hwaddr") == "aa:bb:cc:00:11:22", "mac/hwaddr")
    check(p.get("sysid") == 0xa598 and p.get("platform") == "sav837gw", "sysid/platform")
    check("person" in p.get("features", {}).get("smartDetectTypes", []), "smartDetectTypes")
    results["hello"] = p

    ws_send(conn, request(1, "ubnt_avclient_hello", {"controllerVersion": "7.3.70"}, False))
    reqs = {
        2: request(2, "ubnt_avclient_paramAgreement", {}),
        3: request(3, "ChangeVideoSettings", {"video": {
            "video1": {"avSerializer": {"destinations": [f"tcp://127.0.0.1:{args.flv_port}"],
                                        "parameters": {"streamName": STREAM_NAME}}},
            "video2": {"avSerializer": {"destinations": ["file:///dev/null"]}},
            "video3": {"avSerializer": {"destinations": ["file:///dev/null"]}}}}),
        4: request(4, "ChangeIspSettings", {"brightness": 60}),
        5: request(5, "ChangeSmartDetectSettings", {"objectTypes": ["person"]}),
        6: request(6, "ChangeAudioEventsSettings", {"some": "thing"}),  # unknown: echo
        7: request(7, "GetRequest", {"what": "snapshot", "uri":
                   f"https://127.0.0.1:{args.upload_port}/upload",
                   "formFields": {"cameraId": "abc", "kind": "snap"}}),
        8: request(8, "ubnt_avclient_time", {}),
    }
    for r in reqs.values():
        ws_send(conn, r)
    pending = set(reqs)
    conn.settimeout(1)
    end = time.time() + args.seconds
    while time.time() < end:
        try:
            op, data = ws_recv(conn)
        except socket.timeout:
            continue
        except ConnectionError:
            break
        if op != 1:
            continue
        m = json.loads(data)
        check_envelope(m)
        irt = m.get("inResponseTo")
        if irt in pending:
            pending.discard(irt)
            results[f"resp{irt}"] = m
    check(not pending, f"no response to {[reqs[i]['functionName'] for i in sorted(pending)]}")
    r3 = results.get("resp3", {}).get("payload", {})
    v1 = r3.get("video", {}).get("video1", {})
    check(v1.get("width") == 640 and v1.get("height") == 360, f"video1 size {v1.get('width')}x{v1.get('height')}")
    check(v1.get("avSerializer", {}).get("parameters", {}).get("streamName") == STREAM_NAME,
          "video1 streamName not echoed")
    check(results.get("resp6", {}).get("payload") == {"some": "thing"}, "unknown request not echoed")
    check(results.get("resp8", {}).get("functionName") == "ubnt_avclient_paramAgreement",
          "time response name")
    conn.close()


def serve_flv(sock, out):
    conn, _ = sock.accept()
    with open(os.path.join(out, "video1.flv"), "wb") as f:
        conn.settimeout(2)
        while True:
            try:
                data = conn.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            f.write(data)
            results["flv_bytes"] = results.get("flv_bytes", 0) + len(data)


def serve_upload(sock):
    conn, _ = sock.accept()
    data = b""
    while b"\r\n\r\n" not in data:
        data += conn.recv(65536)
    head, body = data.split(b"\r\n\r\n", 1)
    length = int(re.search(rb"Content-Length: (\d+)", head).group(1))
    while len(body) < length:
        body += conn.recv(65536)
    boundary = re.search(rb"boundary=(\S+)", head).group(1)
    parts = body.split(b"--" + boundary)
    names = re.findall(rb'; name="([^"]+)"', body)
    check(names == [b"cameraId", b"kind", b"payload"], f"multipart fields {names}")
    jpeg = [p for p in parts if b'name="payload"' in p][0].split(b"\r\n\r\n", 1)[1]
    check(jpeg.startswith(b"\xff\xd8"), "upload is not a JPEG")
    results["upload_bytes"] = len(jpeg)
    conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
    conn.close()


def listen(port, ctx=None):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    return ctx.wrap_socket(s, server_side=True) if ctx else s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cert", required=True)
    ap.add_argument("--client-cert", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--flv-port", type=int, default=7550)
    ap.add_argument("--upload-port", type=int, default=7443)
    args = ap.parse_args()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert)
    ctx.verify_mode = ssl.CERT_REQUIRED  # the camera must present its cert
    ctx.load_verify_locations(args.client_cert)
    up_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    up_ctx.load_cert_chain(args.cert)

    ws_sock = listen(7442, ctx)
    flv_sock = listen(args.flv_port)
    up_sock = listen(args.upload_port, up_ctx)
    threading.Thread(target=serve_flv, args=(flv_sock, args.out), daemon=True).start()
    threading.Thread(target=serve_upload, args=(up_sock,), daemon=True).start()
    print("mock Protect listening", flush=True)

    ws_sock.settimeout(30)
    conn, _ = ws_sock.accept()
    handle_ws(conn, args)
    time.sleep(1)

    check(results.get("flv_bytes", 0) > 50000, f"FLV bytes {results.get('flv_bytes', 0)}")
    check(results.get("upload_bytes", 0) > 1000, "snapshot upload missing")
    print(f"responses: {sorted(k for k in results if k.startswith('resp'))}, "
          f"flv {results.get('flv_bytes', 0)} bytes, upload {results.get('upload_bytes', 0)} bytes")
    for e in errors:
        print("ERROR:", e)
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
