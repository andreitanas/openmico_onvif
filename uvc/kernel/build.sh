#!/bin/sh
# Build the Wyze Cam v2 kernel (Linux 6.18 LTS with T20 support) and its
# modules on Linux x86_64:
#   build/kernel/out/uImage          for the kernel flash partition (2 MiB)
#   build/kernel/out/modules/        lib/modules/<version>/ for the root fs
#   build/kernel/out/wyze-cam-v2.dtb the device tree (also linked in)
#   build/kernel/out/{config,System.map}
#
# Pinned inputs, fetched once into third_party/kernel/ (gitignored):
#   Linux v6.18.59 from the GitHub mirror of the stable tree, by commit
#   gcc 13.3 and binutils 2.42 for mipsel, Ubuntu 24.04's cross packages,
#     by sha256 (they run on any x86_64 glibc host with libmpc/libisl;
#     on Ubuntu 24.04 those are installed with gcc)
#   rtl8189fs (WiFi) from jwrdegoede/rtl8189ES_linux, by commit
# Patches in kernel/patches/ are applied in order; kernel/dts/ is copied in.
# Host tools: make, gcc, flex, bison, bc, python3, xz (lzma), git, curl,
# depmod (kmod).
set -e
cd "$(dirname "$0")/.."
TOP=$PWD
DL=$TOP/third_party/kernel
OUT=$TOP/build/kernel
mkdir -p "$DL" "$OUT"

LINUX_TAG=v6.18.59
LINUX_COMMIT=1253a6aa5d104312bf0992afacc15220ae35bdf9
RTL8189FS_COMMIT=13bbdbc750bfaa37ff47dff063edcc857fad0584
POOL=http://archive.ubuntu.com/ubuntu/pool/universe
DEBS="
b/binutils-mipsen/binutils-mipsel-linux-gnu_2.42-2ubuntu1cross5_amd64.deb 678275fa6abda0b7fabbc55ff6a09654701a6c2e86794c7a0b6e773e663e49db
g/gcc-13-cross-mipsen/cpp-13-mipsel-linux-gnu_13.3.0-6ubuntu2~24.04cross1_amd64.deb ea937b6dee764884d87bc2d202f865553d9aa93894d557e4620a4cfd1770ebbb
g/gcc-13-cross-mipsen/gcc-13-mipsel-linux-gnu-base_13.3.0-6ubuntu2~24.04cross1_amd64.deb b29e9a892b88a45206f87f06565a8e661dc213cc1ea015266113fefdd852f0b7
g/gcc-13-cross-mipsen/gcc-13-mipsel-linux-gnu_13.3.0-6ubuntu2~24.04cross1_amd64.deb 01195b41970b22bcfad792d5ac4c16d26b4a590ffad39f87bbd5a7d455d3ac8b
g/gcc-13-cross-mipsen/libgcc-13-dev-mipsel-cross_13.3.0-6ubuntu2~24.04cross1_all.deb 6302f486ad716ba486be60d2321b5efda4208d0b6774e7ff07f5047e7576f223
"
# U-Boot reads the uImage to 0x80600000 and unpacks the kernel to its load
# address, 0x80100000: the unpacked kernel must end below the uImage.
MAX_RAW=$((0x500000))
MAX_UIMAGE=$((0x200000))

# Toolchain
TC=$DL/toolchain
if [ ! -x "$TC/bin/mipsel-linux-gnu-gcc" ]; then
    rm -rf "$TC"
    mkdir -p "$DL/debs" "$TC/bin"
    echo "$DEBS" | while read -r path sum; do
        [ -n "$path" ] || continue
        f=$DL/debs/$(basename "$path")
        [ -f "$f" ] || curl -fsSL "$POOL/$path" -o "$f"
        echo "$sum  $f" | sha256sum -c - >/dev/null
        dpkg-deb -x "$f" "$TC" 2>/dev/null || (cd "$TC" && ar p "$f" data.tar.zst | zstd -dc | tar x)
    done
    for t in "$TC"/usr/bin/mipsel-linux-gnu-*; do
        ln -sf "$t" "$TC/bin/$(basename "$t" | sed 's/-13$//')"
    done
fi
export PATH=$TC/bin:$PATH
export LD_LIBRARY_PATH=$TC/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# Kernel source, patched
SRC=$DL/linux
if [ ! -d "$SRC/.git" ]; then
    git -c advice.detachedHead=false clone -q --depth 1 -b $LINUX_TAG https://github.com/gregkh/linux "$SRC"
fi
[ "$(git -C "$SRC" rev-parse HEAD)" = $LINUX_COMMIT ] ||
    { echo "linux $LINUX_TAG is not commit $LINUX_COMMIT" >&2; exit 1; }
# Re-patched from the pristine tree whenever kernel/patches/ changes
PATCHES=$(cat "$TOP"/kernel/patches/*.patch | sha256sum | cut -d' ' -f1)
if [ "$(cat "$SRC/.git/patched" 2>/dev/null)" != "$PATCHES" ]; then
    git -C "$SRC" reset -q --hard
    git -C "$SRC" clean -qfdx
    for p in "$TOP"/kernel/patches/*.patch; do
        git -C "$SRC" apply "$p"
    done
    echo "$PATCHES" > "$SRC/.git/patched"
fi
cp "$TOP"/kernel/dts/*.dts "$TOP"/kernel/dts/*.dtsi "$SRC/arch/mips/boot/dts/ingenic/"

export ARCH=mips CROSS_COMPILE=mipsel-linux-gnu-
export KBUILD_BUILD_USER=uvc KBUILD_BUILD_HOST=build
export KBUILD_BUILD_TIMESTAMP="$(git -C "$TOP" log -1 --format=%cd --date=rfc 2>/dev/null || echo 'Thu, 01 Jan 2026 00:00:00 +0000')"
export SOURCE_DATE_EPOCH=$(date -d "$KBUILD_BUILD_TIMESTAMP" +%s)
K="make -s -C $SRC O=$OUT/obj LOCALVERSION="
JOBS=$(nproc)

# Configuration: tinyconfig plus kernel/config, every option checked
mkdir -p "$OUT/obj"
$K tinyconfig
"$SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT/obj" "$OUT/obj/.config" "$TOP/kernel/config" >/dev/null
$K olddefconfig
bad=0
while read -r l; do
    case "$l" in CONFIG_*) ;; *) continue ;; esac
    k=${l%%=*}
    if [ "${l#*=}" = n ]; then
        ! grep -q "^$k=" "$OUT/obj/.config" || { echo "kernel/config: $k is still set" >&2; bad=1; }
    else
        grep -qx "$l" "$OUT/obj/.config" || { echo "kernel/config: $l did not apply" >&2; bad=1; }
    fi
done < "$TOP/kernel/config"
[ $bad = 0 ] || exit 1

$K -j"$JOBS" vmlinux.bin modules
VER=$($K kernelrelease)

# WiFi driver (out of tree)
WIFI=$DL/rtl8189fs
if [ ! -d "$WIFI" ]; then
    git -c advice.detachedHead=false clone -q -b rtl8189fs https://github.com/jwrdegoede/rtl8189ES_linux "$WIFI"
    git -C "$WIFI" checkout -q $RTL8189FS_COMMIT
fi
# Its own make does not notice a new kernel configuration (struct layouts
# change with it): rebuilt from clean when the configuration differs
if ! cmp -s "$OUT/obj/.config" "$WIFI/.kernel-config"; then
    make -s -C "$OUT/obj" M="$WIFI" clean
    cp "$OUT/obj/.config" "$WIFI/.kernel-config"
fi
make -s -C "$OUT/obj" M="$WIFI" -j"$JOBS" \
    CONFIG_RTL8189FS=m CONFIG_MP_INCLUDED=n USER_EXTRA_CFLAGS="-Wno-error" modules

# Outputs
D=$OUT/out
rm -rf "$D"
mkdir -p "$D"
$K INSTALL_MOD_PATH="$D/modules" INSTALL_MOD_STRIP=1 modules_install
make -s -C "$OUT/obj" M="$WIFI" INSTALL_MOD_PATH="$D/modules" INSTALL_MOD_STRIP=1 modules_install
rm -f "$D/modules/lib/modules/$VER/build" "$D/modules/lib/modules/$VER/source"
cp "$OUT/obj/.config" "$D/config"
cp "$OUT/obj/System.map" "$D/System.map"
cp "$OUT/obj/arch/mips/boot/dts/ingenic/wyze-cam-v2.dtb" "$D/"

RAW=$OUT/obj/arch/mips/boot/vmlinux.bin
SIZE=$(wc -c < "$RAW")
[ "$SIZE" -le $MAX_RAW ] ||
    { echo "vmlinux.bin is $SIZE bytes, more than the $MAX_RAW below U-Boot's load buffer" >&2; exit 1; }
LOAD=$(awk '$3 == "_text" { print "0x" $1 }' "$OUT/obj/System.map")
ENTRY=$(awk '$3 == "kernel_entry" { print "0x" $1 }' "$OUT/obj/System.map")
lzma -9 < "$RAW" > "$OUT/vmlinux.bin.lzma"
python3 "$TOP/kernel/mkuimage.py" "$LOAD" "$ENTRY" "$SOURCE_DATE_EPOCH" "Linux-$VER" \
    "$OUT/vmlinux.bin.lzma" "$D/uImage"
USIZE=$(wc -c < "$D/uImage")
[ "$USIZE" -le $MAX_UIMAGE ] ||
    { echo "uImage is $USIZE bytes, more than the kernel partition" >&2; exit 1; }
echo "$D/uImage: $USIZE bytes of $MAX_UIMAGE (Linux $VER, unpacked $SIZE of $MAX_RAW, load $LOAD, entry $ENTRY)"
