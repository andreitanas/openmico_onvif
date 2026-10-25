#!/bin/sh
# Detector benchmark: build ncnn (host + mipsel) and the bench program.
#   ./build.sh          fetch sources/models/images and build both
set -e
cd "$(dirname "$0")"
NCNN=20260526

mkdir -p src images models
MODELS=https://raw.githubusercontent.com/dog-qiuqiu/Yolo-Fastest/master/sample/ncnn/model
for f in yolo-fastest-1.1.param yolo-fastest-1.1.bin yolo-fastest-1.1-xl.param yolo-fastest-1.1-xl.bin; do
    [ -f models/$f ] || curl -fsSL "$MODELS/$f" -o models/$f
done
FDET=https://raw.githubusercontent.com/dog-qiuqiu/FastestDet/main/example/ncnn
for f in FastestDet.param FastestDet.bin; do
    [ -f models/$f ] || curl -fsSL "$FDET/$f" -o models/$f
done
# Same model with the built-in 0.55 confidence cutoff lowered to 0.30.
sed 's/ 2=5.500000e-01 / 2=3.000000e-01 /' models/yolo-fastest-1.1.param > models/yolo-fastest-1.1-t30.param
if [ ! -d src/ncnn ]; then
    curl -fsSL "https://github.com/Tencent/ncnn/archive/refs/tags/$NCNN.tar.gz" | tar xz -C src
    mv "src/ncnn-$NCNN" src/ncnn
fi
for img in dog person; do
    [ -f images/$img.jpg ] ||
        curl -fsSL "https://raw.githubusercontent.com/pjreddie/darknet/master/data/$img.jpg" -o images/$img.jpg
    # Raw RGB so the bench needs no image decoder.
    [ -f images/$img.rgb ] || ffmpeg -loglevel error -y -i images/$img.jpg -f rawvideo -pix_fmt rgb24 images/$img.rgb
done

NCNN_OPTS="-DCMAKE_BUILD_TYPE=Release -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF -DNCNN_THREADS=OFF \
  -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF \
  -DNCNN_SHARED_LIB=OFF -DNCNN_SIMPLEOCV=OFF -DNCNN_SIMPLESTL=OFF -DNCNN_RUNTIME_CPU=OFF \
  -DNCNN_MSA=OFF -DNCNN_MMI=OFF -DNCNN_DISABLE_EXCEPTION=ON -DNCNN_DISABLE_RTTI=ON"

# Host build: correctness reference.
if [ ! -f build-host/src/libncnn.a ]; then
    # Runtime dispatch on the host: a static ISA choice can include ARM
    # extensions this machine lacks (SIGILL).
    cmake -S src/ncnn -B build-host -G Ninja $NCNN_OPTS -DNCNN_RUNTIME_CPU=ON >/dev/null
    ninja -C build-host >/dev/null
fi
c++ -std=c++11 -O2 -Ibuild-host/src -Isrc/ncnn/src bench.cpp build-host/src/libncnn.a -o bench-host

# Quantization tools (ncnn2table, ncnn2int8), host only.
if [ ! -f build-tools/tools/quantize/ncnn2int8 ]; then
    cmake -S src/ncnn -B build-tools -G Ninja $NCNN_OPTS -DNCNN_RUNTIME_CPU=ON \
        -DNCNN_BUILD_TOOLS=ON -DNCNN_SIMPLEOCV=ON -DNCNN_DISABLE_EXCEPTION=OFF -DNCNN_DISABLE_RTTI=OFF >/dev/null
    ninja -C build-tools ncnn2table ncnn2int8 >/dev/null
fi

# Camera build: static mipsel via zig, same as uvcd.
cat > build-mips.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2)
set(CMAKE_CXX_COMPILER zig c++ -target mipsel-linux-musleabihf -mcpu=mips32r2)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/zig-ar)
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/zig-ranlib)
EOF
printf '#!/bin/sh\nexec zig ar "$@"\n' > zig-ar
printf '#!/bin/sh\nexec zig ranlib "$@"\n' > zig-ranlib
chmod +x zig-ar zig-ranlib
if [ ! -f build-mips/src/libncnn.a ]; then
    cmake -S src/ncnn -B build-mips -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/build-mips.cmake" $NCNN_OPTS >/dev/null
    ninja -C build-mips >/dev/null
fi
zig c++ -target mipsel-linux-musleabihf -mcpu=mips32r2 -std=c++11 -O2 -static -s \
    -Ibuild-mips/src -Isrc/ncnn/src bench.cpp build-mips/src/libncnn.a -o bench-mipsel
ls -l bench-host bench-mipsel
