# make            host build (for tests on the Mac/Linux box)
# make camera     static mipsel binary for the Wyze Cam v2 (Ingenic T20)
# make test       host tests

MBEDTLS := third_party/mbedtls
CJSON := third_party/cjson
INGENIC := third_party/ingenic
NCNN := third_party/ncnn
STB := third_party/stb

SRC := $(wildcard src/*.c) $(wildcard src/*.cpp)
DEP_SRC := $(wildcard $(MBEDTLS)/library/*.c) $(CJSON)/cJSON.c
CFLAGS_COMMON := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Os \
	-Isrc -I$(MBEDTLS)/include -I$(CJSON) -I$(STB) -ffunction-sections -fdata-sections -MMD -MP
DEP_CFLAGS := -std=gnu11 -Os -I$(MBEDTLS)/include -I$(MBEDTLS)/library -I$(CJSON) \
	-ffunction-sections -fdata-sections -w

HOST_CC ?= cc
HOST_CXX ?= c++
CAMERA_CC ?= zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2
CAMERA_CXX ?= zig c++ -target mipsel-linux-musleabihf -mcpu=mips32r2

# ncnn (object detector inference), built with CMake per target.
NCNN_OPTS := -DCMAKE_BUILD_TYPE=Release -DNCNN_VULKAN=OFF -DNCNN_OPENMP=OFF -DNCNN_THREADS=OFF \
	-DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF \
	-DNCNN_BUILD_TESTS=OFF -DNCNN_SHARED_LIB=OFF -DNCNN_SIMPLEOCV=OFF -DNCNN_SIMPLESTL=OFF \
	-DNCNN_MSA=OFF -DNCNN_MMI=OFF -DNCNN_DISABLE_EXCEPTION=ON -DNCNN_DISABLE_RTTI=ON
NCNN_HOST := build/ncnn-host/src/libncnn.a
NCNN_CAMERA := build/ncnn-camera/src/libncnn.a
CXXFLAGS_COMMON := -std=c++11 -Wall -Wextra -Os -fno-exceptions -fno-rtti -Isrc \
	-I$(NCNN)/src -ffunction-sections -fdata-sections -MMD -MP

# Host: runtime CPU dispatch (a static ISA choice can include extensions
# this machine lacks).
$(NCNN_HOST):
	cmake -S $(NCNN) -B build/ncnn-host -G Ninja $(NCNN_OPTS) -DNCNN_RUNTIME_CPU=ON >/dev/null
	ninja -C build/ncnn-host >/dev/null

$(NCNN_CAMERA):
	cmake -S $(NCNN) -B build/ncnn-camera -G Ninja -DCMAKE_TOOLCHAIN_FILE=$(CURDIR)/tools/zig-mipsel.cmake \
		$(NCNN_OPTS) -DNCNN_RUNTIME_CPU=OFF >/dev/null
	ninja -C build/ncnn-camera >/dev/null

build/host/%.o: %.cpp | $(NCNN_HOST)
	@mkdir -p $(dir $@)
	$(HOST_CXX) $(CXXFLAGS_COMMON) -Ibuild/ncnn-host/src -c $< -o $@

build/camera/%.o: %.cpp | $(NCNN_CAMERA)
	@mkdir -p $(dir $@)
	$(CAMERA_CXX) $(CXXFLAGS_COMMON) -Ibuild/ncnn-camera/src -c $< -o $@

build/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(if $(filter src/% test/%,$<),$(CFLAGS_COMMON),$(DEP_CFLAGS)) -c $< -o $@

# libimp (Ingenic's camera SDK, prebuilt for uClibc) only in camera builds.
IMP_CFLAGS := -DUVC_IMP -I$(INGENIC)/src/include/imp -I$(INGENIC)/src/include

build/camera/%.o: %.c
	@mkdir -p $(dir $@)
	$(CAMERA_CC) $(if $(filter src/%,$<),$(CFLAGS_COMMON),$(DEP_CFLAGS)) \
		$(if $(filter src/capture.c src/imp_compat.c,$<),$(IMP_CFLAGS)) -c $< -o $@

OBJ_NAMES := $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(SRC) $(DEP_SRC)))
HOST_OBJ := $(addprefix build/host/,$(OBJ_NAMES))
CAMERA_OBJ := $(addprefix build/camera/,$(OBJ_NAMES))
LIB_HOST_OBJ := $(filter-out build/host/src/main.o,$(HOST_OBJ))

all: build/uvcd

third_party/mbedtls third_party/cjson:
	./fetch-deps.sh

build/uvcd: $(HOST_OBJ) $(NCNN_HOST)
	$(HOST_CXX) -o $@ $^ -lpthread

camera: build/uvcd-mipsel

# libimp with its mmap/ftruncate calls pointed at our 32-bit-offset shims.
build/camera/libimp.a: $(INGENIC)/lib/libimp.a tools/imp-rename.py
	@mkdir -p $(dir $@)
	python3 tools/imp-rename.py $< $@

build/uvcd-mipsel: $(CAMERA_OBJ) $(NCNN_CAMERA) build/camera/libimp.a
	$(CAMERA_CXX) -static -s -Wl,--gc-sections -o $@ $^ -lpthread -lm
	@ls -l $@

build/test_flv: build/host/test/test_flv.o $(LIB_HOST_OBJ) $(NCNN_HOST)
	$(HOST_CXX) -o $@ $^ -lpthread

build/test_motion: test/test_motion.c build/host/src/motion.o
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^

build/test_smart: test/test_smart.c build/host/src/smart.o build/host/$(CJSON)/cJSON.o
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^

test: build/test_flv build/test_motion build/test_smart
	./build/test_motion
	./build/test_smart
	./test/run.sh

clean:
	rm -rf build

-include $(HOST_OBJ:.o=.d) $(CAMERA_OBJ:.o=.d)

.PHONY: all camera test clean
