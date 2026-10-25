# make            host build (for tests on the Mac/Linux box)
# make camera     static mipsel binary for the Wyze Cam v2 (Ingenic T20)
# make test       host tests

MBEDTLS := third_party/mbedtls
CJSON := third_party/cjson
INGENIC := third_party/ingenic

SRC := $(wildcard src/*.c)
DEP_SRC := $(wildcard $(MBEDTLS)/library/*.c) $(CJSON)/cJSON.c
CFLAGS_COMMON := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Os \
	-Isrc -I$(MBEDTLS)/include -I$(CJSON) -ffunction-sections -fdata-sections -MMD -MP
DEP_CFLAGS := -std=gnu11 -Os -I$(MBEDTLS)/include -I$(MBEDTLS)/library -I$(CJSON) \
	-ffunction-sections -fdata-sections -w

HOST_CC ?= cc
CAMERA_CC ?= zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2

build/host/%.o: %.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(if $(filter src/%,$<),$(CFLAGS_COMMON),$(DEP_CFLAGS)) -c $< -o $@

# libimp (Ingenic's camera SDK, prebuilt for uClibc) only in camera builds.
IMP_CFLAGS := -DUVC_IMP -I$(INGENIC)/src/include/imp -I$(INGENIC)/src/include

build/camera/%.o: %.c
	@mkdir -p $(dir $@)
	$(CAMERA_CC) $(if $(filter src/%,$<),$(CFLAGS_COMMON),$(DEP_CFLAGS)) \
		$(if $(filter src/capture.c src/imp_compat.c,$<),$(IMP_CFLAGS)) -c $< -o $@

HOST_OBJ := $(patsubst %.c,build/host/%.o,$(SRC) $(DEP_SRC))
CAMERA_OBJ := $(patsubst %.c,build/camera/%.o,$(SRC) $(DEP_SRC))
LIB_HOST_OBJ := $(filter-out build/host/src/main.o,$(HOST_OBJ))

all: build/uvcd

third_party/mbedtls third_party/cjson:
	./fetch-deps.sh

build/uvcd: $(HOST_OBJ)
	$(HOST_CC) -o $@ $^ -lpthread

camera: build/uvcd-mipsel

build/uvcd-mipsel: $(CAMERA_OBJ)
	$(CAMERA_CC) -static -s -Wl,--gc-sections -o $@ $^ $(INGENIC)/lib/libimp.a -lpthread -lm
	@ls -l $@

build/test_flv: test/test_flv.c $(LIB_HOST_OBJ)
	$(HOST_CC) $(CFLAGS_COMMON) -o $@ $^ -lpthread

test: build/test_flv
	./test/run.sh

clean:
	rm -rf build

-include $(HOST_OBJ:.o=.d) $(CAMERA_OBJ:.o=.d)

.PHONY: all camera test clean
