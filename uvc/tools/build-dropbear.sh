#!/bin/sh
# Build the camera's SSH server, static mipsel against musl (whose crypt()
# checks the SHA-512 password hashes Protect sends): dropbear and
# dropbearkey in one binary, build/dropbear/dropbearmulti.
set -e
cd "$(dirname "$0")/.."
SRC=$PWD/third_party/dropbear
OUT=build/dropbear
[ -d "$SRC" ] || ./fetch-deps.sh
mkdir -p $OUT
cd $OUT
cat > localoptions.h <<'H'
#define DROPBEAR_X11FWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
/* sbin too, for reboot and friends */
#define DEFAULT_PATH "/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH "/usr/sbin:/usr/bin:/sbin:/bin"
H
if [ ! -f Makefile ]; then
    CC="zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2" AR="$OLDPWD/tools/zig-ar" \
        RANLIB="$OLDPWD/tools/zig-ranlib" CFLAGS="-Os -Wno-undef" LDFLAGS="-s" \
        "$SRC/configure" --host=mipsel-linux-musl --enable-static --disable-zlib \
        --disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
        --disable-pututline --disable-pututxline --disable-harden >/dev/null
fi
make -s PROGRAMS="dropbear dropbearkey" MULTI=1 STATIC=1 >/dev/null 2>&1
ls -l dropbearmulti
