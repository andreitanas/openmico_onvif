#!/bin/sh
# Build mtd-utils' mkfs.jffs2 for this machine (macOS or Linux) into
# build/host-tools/. Only the zlib/rtime compressors; libmtd is stubbed (it
# is only used to probe real MTD devices).
set -e
cd "$(dirname "$0")/.."
VER=2.2.0
OUT=build/host-tools
SRC=$OUT/mtd-utils-$VER
mkdir -p $OUT
[ -d $SRC ] || curl -fsSL https://infraroot.at/pub/mtd/mtd-utils-$VER.tar.bz2 | tar -xj -C $OUT
S=$OUT/shim
mkdir -p $S/linux $S/asm $S/sys
cat > $S/features.h <<'H'
#include <machine/endian.h>
#define __BYTE_ORDER BYTE_ORDER
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#define __BIG_ENDIAN BIG_ENDIAN
H
cat > $S/byteswap.h <<'H'
#include <libkern/OSByteOrder.h>
#define bswap_16 OSSwapInt16
#define bswap_32 OSSwapInt32
#define bswap_64 OSSwapInt64
H
cat > $S/asm/types.h <<'H'
#include <stdint.h>
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64;
typedef int8_t __s8; typedef int16_t __s16; typedef int32_t __s32; typedef int64_t __s64;
H
cp $S/features.h $S/endian.h
echo '#include <asm/types.h>' > $S/linux/types.h
echo '#include <stddef.h>' > $S/linux/kernel.h
echo '#include <stddef.h>' > $S/linux/stddef.h
echo '#include <sys/types.h>' > $S/sys/sysmacros.h
cat > $S/stubs.c <<'H'
#include <stddef.h>
void *libmtd_open(void) { return NULL; }
void libmtd_close(void *l) {}
int mtd_get_dev_info2(void *l, int id, void *info) { return -1; }
H
SHIM=
[ "$(uname)" = Darwin ] && SHIM=-I$S
cc -O2 -w -Wno-error=implicit-function-declaration -D_GNU_SOURCE $SHIM -I$SRC/include \
    -I$SRC/jffsX-utils -include common.h -include xalloc.h -DWITH_ZLIB -DCONFIG_JFFS2_RTIME \
    -DVERSION="\"$VER\"" -DPROGRAM_NAME='"mkfs.jffs2"' -o $OUT/mkfs.jffs2 \
    $SRC/jffsX-utils/mkfs.jffs2.c $SRC/jffsX-utils/rbtree.c $SRC/jffsX-utils/compr.c \
    $SRC/jffsX-utils/compr_rtime.c $SRC/jffsX-utils/compr_zlib.c $SRC/lib/libcrc32.c \
    $SRC/lib/common.c $S/stubs.c -lz
$OUT/mkfs.jffs2 --version
