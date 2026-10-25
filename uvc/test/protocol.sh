#!/bin/sh
# End-to-end protocol test: uvcd (host build) against tools/mock_protect.py.
set -e
cd "$(dirname "$0")/.."
T=build/test
mkdir -p $T

[ -f $T/clip.h264 ] || ./test/run.sh >/dev/null
[ -f $T/snap.jpg ] || ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360 -frames:v 1 $T/snap.jpg
if [ ! -f $T/client.pem ]; then
    # Same shape of client certificate the camera ships with.
    openssl ecparam -name prime256v1 -genkey -noout -out $T/client.key 2>/dev/null
    openssl req -x509 -new -key $T/client.key -days 3650 -subj "/CN=camera.ubnt.dev" -out $T/client.crt
    cat $T/client.key $T/client.crt > $T/client.pem
    openssl req -x509 -nodes -newkey rsa:2048 -days 3650 -subj "/CN=protect" \
        -keyout $T/server.key -out $T/server.crt 2>/dev/null
    cat $T/server.key $T/server.crt > $T/server.pem
fi

cat > $T/uvcd.conf <<EOF
NVR_HOST=127.0.0.1
ADOPTION_TOKEN=test-token-1234
CERT=$T/client.pem
NAME=test-cam
MAC=AA:BB:CC:00:11:22
IP=127.0.0.1
HI_SOURCE=$T/clip.h264
SNAPSHOT_SOURCE=$T/snap.jpg
STATE_FILE=$T/uvcd.state
CAPTURE=0
EOF
rm -f $T/uvcd.state $T/video1.flv

python3 -I tools/mock_protect.py --cert $T/server.pem --client-cert $T/client.crt --out $T &
MOCK=$!
sleep 1
./build/uvcd -c $T/uvcd.conf > $T/uvcd.log 2>&1 &
UVCD=$!
trap 'kill $UVCD 2>/dev/null' EXIT

rc=0
wait $MOCK || rc=$?
kill $UVCD 2>/dev/null || true

echo "== uvcd log"
cat $T/uvcd.log
echo "== state file"
cat $T/uvcd.state
echo "== streamed FLV"
python3 -I tools/flvdump.py --unifi -q $T/video1.flv
[ $rc = 0 ] && echo OK
exit $rc
