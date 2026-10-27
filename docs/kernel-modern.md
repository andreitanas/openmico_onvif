# A modern kernel for the Wyze Cam v2 (2026-10-27)

Linux 6.18 LTS for the Ingenic T20, built from source by `uvc/kernel/build.sh`,
with a root filesystem on flash instead of OpenMiko's initramfs. Nothing here has
run on a camera yet: everything below "builds" is untested on hardware. Read
[Hardware test plan](#hardware-test-plan) before flashing anything.

## Status

| Piece | Source | Status |
|---|---|---|
| Kernel, MIPS32r1, T20 machine | Linux v6.18.59 + patches 0001-0006 | builds |
| Clocks (CGU) | ours, `t20-cgu.c` (patch 0007) | builds |
| Interrupts (INTC) | mainline `irq-ingenic` (jz4780) | builds |
| Timers | mainline `ingenic-sysost` (OST) and `ingenic-timer` (TCU) | builds |
| UART console (ttyS1) | mainline `8250_ingenic` | builds |
| Pins, GPIO, LEDs, `/sys/class/gpio` | mainline `pinctrl-ingenic` + T20 (patch 0008) | builds |
| SPI NOR flash, partitions | gtxaspec's `spi-ingenic-sfc` (0005, 0009), mainline spi-nor + 32 KiB erase (0010) | builds |
| SD card (MSC0) | mainline `jz4740_mmc`, PIO (no DMA yet) | builds |
| Watchdog | mainline `jz4740_wdt` | builds |
| I2C (sensor bus) | mainline `i2c-jz4780` | builds |
| WiFi, RTL8189FS on MSC1 | jwrdegoede/rtl8189ES_linux (out of tree), cfg80211 module | builds |
| Squashfs root on mtdblock2 | `image/build-modern.sh`, `image/flash-modern.sh` | written, not run (needs OpenMiko's `demo.bin` as the base) |
| ISP (tx-isp) | open-tx-isp T20 | **missing**: port started, 272 compile errors left (below) |
| Sensor JXF22/JXF23 | ingenic-sdk `sensor-src/t20` | **missing**: same framework as the ISP |
| H.264 encoder (VPU, IPU) | vendor `soc_vpu`/`jz_nvpu`/`jz_ipu_v13` (GPL, 3.10 tree) | **missing**: not started |
| Audio | ingenic-sdk `audio/t20/oss2` | **missing**: not started |
| uvcd | unchanged, libimp | runs without video on the new kernel (S50camera skips the camera modules) |

So the new kernel is a bring-up kernel: shell on the serial console, flash, SD card,
GPIOs and LEDs, WiFi and SSH, no camera. The image scripts keep OpenMiko's kernel
as the default (`image/build.sh`, `image/flash.sh` are unchanged in behaviour).

## Sources

All pinned in `uvc/kernel/build.sh`:

- **Linux v6.18.59** (latest 6.18 LTS on 2026-10-27), from the GitHub mirror of
  the stable tree (`gregkh/linux`), checked against commit
  `1253a6aa5d104312bf0992afacc15220ae35bdf9`. kernel.org itself is not reachable from
  the build environment used here.
- **Toolchain**: gcc 13.3 and binutils 2.42 for `mipsel-linux-gnu`, Ubuntu 24.04's
  cross packages, by URL and sha256. They run on any x86_64 glibc host that has
  libmpc/libmpfr/libisl (Ubuntu and Debian do with gcc installed).
- **Patches** `uvc/kernel/patches/`, applied with `git apply`:
  - 0001-0005 from gtxaspec's `thingino-linux` branch `ingenic-7.1-rc1`
    (T31 on mainline): pinctrl and machine plumbing, the SFC (SPI flash
    controller) driver from Zhou Yanjie's series ported to spi-mem, an MMC clock
    fix. They apply to 6.18 unchanged. The T31 clock driver, Ethernet, USB and
    the T31 device tree are not taken.
  - 0006-0010 ours: T20 machine and the `T20_WYZE_CAM_V2` board option; the T20
    CGU driver; T20 pinctrl; the SFC's T20 compatible; a spi-nor option to erase
    in 32 KiB blocks.
- **Device tree** `uvc/kernel/dts/t20.dtsi`, `wyze-cam-v2.dts`, copied into the
  tree and linked into the kernel.
- **RTL8189FS**: `jwrdegoede/rtl8189ES_linux` branch `rtl8189fs`, commit
  `13bbdbc` (2026-09-11; it follows kernels up to 7.3).
- **Hardware reference**: Ingenic's 3.10.14 tree as shipped in OpenMiko
  (`openmiko/openmiko`, `kernel_sources/kernel-3.10.14.tar.xz`, with its
  `patches/kernel/`), which is the tree the camera runs today. The board is the
  vendor "bull" board (`soc-t20/chip-t20/isvp/bull/board.h`), the same name the
  camera reports as its system type. Also Thingino's `gtxaspec/thingino-linux`
  branch `T20-3.12.0`.

## What was ported, and from where

**Clocks** (`drivers/clk/ingenic/t20-cgu.c`). Register layout from the vendor
`soc-t20/common/clk/`: CPCCR dividers and muxes (same bit positions as the T31),
the CGU dividers (`ce_busy_stop` 27 gives CE/busy/stop bits 29/28/27; MSC dividers
count in 2s), the source selectors (MSC and SSI muxes are 1 bit at 31, DDR/VPU/ISP/CIM
2 bits at 30), and the gates in CLKGR0/CLKGR1. The PLLs are read only, with both
output dividers and the fractional part as the vendor `pll_get_rate` reads them;
nothing changes a PLL. Every clock is also registered under its vendor name
(`isp`, `cgu_isp`, `csi`, `cgu_vpu`, ...), so drivers ported from the vendor tree
that call `clk_get(dev, "isp")` work unchanged.

**Pins** (`pinctrl-ingenic.c`). Four ports 0x100 apart with the JZ4770-style
INT/MSK/PAT1/PAT0 registers, written directly (the vendor driver does not use
the shadow registers). The pull register is at 0x60 and setting a bit enables
the pull (vendor `gpio.c`: `PXPENS` on release, `PXPENC` "disable pull as
default"); the device tree sets no pulls, so this path is unused for now. Groups
from the vendor `platform.h`: UART0/1, I2C0/1, MSC0 (PB0-5), MSC1 (PC2-7), SFC
(PA23, 24, 27, 28; quad adds PA25/26), SSI0, CIM MCLK (PA15), DMIC, PWM0-3. GPIO
numbers stay as today (port × 32 + pin): `/sys/class/gpio` users (uvcd's IR cut
25/26, IR LEDs 49, S50camera's USB power 47) are unchanged.

**Timers**. The T20's system OST at 0x12000000 has the X1000's registers (vendor
`timer_sys_ost.c`), so mainline `ingenic-sysost` drives it; its interrupt is CPU
IP4 (vendor `irq.c` dispatches IP4 to the OST). The TCU (watchdog, PWM, a second
clock event) uses the mainline X1000 TCU code.

**Flash**. The SFC registers match the X1000's (vendor `jz_sfc.h` against the
driver), so the T20 uses the X1000 data. Single-bit SPI as OpenMiko's kernel
(no `CONFIG_SPI_QUAD` there), clock as U-Boot left it. Partitions are in the
device tree with the same layout and names as today (`boot` read-only, `kernel`,
`root`, `config`, `para`, `flag`), so `/dev/mtdblock2` is still the root partition.
Erase blocks are 32 KiB as with the vendor kernel (patch 0010); mainline would
pick the largest erase size (64 KiB), and the config partition's JFFS2 image,
made with 32 KiB blocks, would not read cleanly.

**SD card and WiFi**. Both MSC controllers with the mainline JZ4740 driver,
in PIO mode: the PDMA controller is not described yet. SD card detect is polled
(`broken-cd`); the vendor board file has PB27, not confirmed on this camera.
WiFi power is WL_REG_EN, PC16 (GPIO 80), from the vendor `board.h` and
`bcm_power_control.c` (OpenMiko's 8189fs patch calls `bcm_wlan_power_on`), as
an `mmc-pwrseq-simple` reset line.

**LEDs**. `led_blue` GPIO 39 and `led_yellow` GPIO 38, active low, from OpenMiko's
`linux-led.patch`, so `/sys/class/leds/led_blue` and `led_yellow` exist as uvcd and
S01system expect.

**Memory**. The kernel gets the low 96 MB as with `mem=96M@0x0`; 0x6000000 to
0x8000000 stays outside Linux for the ISP (`ispmem`, 8 MB) and the encoder
(`rmem`, 24 MB), which libimp maps through `/dev/mem` (`CONFIG_DEVMEM=y`).

**Vendor interfaces** (`uvc/kernel/compat/`). `t20-compat.ko` provides what
Ingenic's 3.10 arch code gives the camera drivers: `/proc/jz` with
`get_jz_proc_root`/`jz_proc_mkdir`, `jzgpio_set_func` (written to the GPIO
registers as the vendor `gpio_set_func`), `ispmem_base`/`ispmem_size`,
`t20_intc_irq()` (Linux number of an INTC line), and the tx-isp platform devices
(`tx-isp`, `tx-isp-csi`, `tx-isp-vic`, `tx-isp-core`, copied from the vendor
`platform.c`). `compat/include/` has the vendor headers those drivers include
(`mach/tx_isp.h`, `soc/base.h`, `soc/gpio.h`, `soc/irq.h`, `jz_proc.h`,
`media/v4l2-chip-ident.h`). The module builds; it is not in the image yet.

## Boot with the vendor U-Boot

- `bootcmd` is `sf probe; sf read 0x80600000 0x40000 0x280000; bootm 0x80600000`.
  `build.sh` makes a legacy uImage, LZMA, load address 0x80100000 and entry at
  `kernel_entry`, as mainline's own `uImage.lzma` target does. OpenMiko's kernel
  is booted the same way (`uImage.lzma`), so this U-Boot has LZMA; gzip would not
  fit (2.3 MB).
- The unpacked kernel must end below 0x80600000, where U-Boot put the uImage:
  5 MiB at most. `build.sh` fails above that. This is why cfg80211 and IPv6 are
  modules and the log buffer is 16 KB.
- The device tree is linked into the kernel (`CONFIG_BUILTIN_DTB`): this U-Boot
  passes argc/argv, not a DTB, so mainline's generic MIPS code takes the built-in
  one. The kernel command line comes from the device tree
  (`CONFIG_MIPS_CMDLINE_FROM_DTB`) and U-Boot's arguments (`mem=`, `ispmem=`,
  `rmem=`, `mtdparts=`) are ignored, as with OpenMiko's kernel, which has its
  command line built in with `CMDLINE_OVERRIDE`:
  `console=ttyS1,115200n8 earlycon root=/dev/mtdblock2 rootfstype=squashfs ro panic=10`.

## Sizes

| | Today (OpenMiko 3.10.14) | Linux 6.18 |
|---|---|---|
| uImage | (in a 2048 KiB partition, with the initramfs) | 1,706,816 bytes of 2,097,152 (81 %) |
| Unpacked image | | 5,016,992 bytes of 5,242,880 |
| Kernel code | 3877k | 3621k |
| Data | 1205k | 1112k "rwdata" (of which 924k read-only), 67k bss |
| Init, freed after boot | | 188k |
| Modules loaded for WiFi | 8189fs (cfg80211 built in) | cfg80211 504k, ipv6 555k, 8189fs (2.2 MB file, stripped), firmware_class, libsha1 |
| Root filesystem | `rootfs.tar.xz` unpacked into RAM (zram) on every boot | squashfs read from flash, page cache only |

Kernel figures as the kernel prints them at boot ("Memory: ... kernel code,
rwdata, rodata, init, bss"), from `System.map`. The resident kernel is a little
smaller than today's; cfg80211 and IPv6 move from built-in to modules, so with
WiFi up the total is about the same. The large saving is the root filesystem,
which no longer sits in RAM.

## Root filesystem without the initramfs

Proposal, implemented in the image scripts:

- **Root**: a read-only squashfs (xz, 128 KiB blocks) on mtdblock2, mounted by the
  kernel as `/`. It is today's root filesystem (OpenMiko's base slimmed by
  `build-rootfs.py`, our overlay, our dropbear) without the 3.10 modules, plus our
  kernel's modules under `/lib/modules/6.18.59` and uvcd with its model under
  `/rom/uvc`, where `uvcd-run` already looks. There is no `/rom` mount and no
  block-274 rule; for `demo.bin` the image must still fit the 274 blocks that file
  format carries (8.6 MiB).
- **Writable storage**: settings stay where they are, the JFFS2 config partition
  (mtdblock3) on `/config`, read by S10config as today, so an existing camera keeps
  its WiFi settings, hostname and keys. `/etc` (and `/var` if it is a directory)
  gets a writable layer in RAM (`S00rootfs`, overlayfs on tmpfs), because
  S10config installs `/config/overlay` over `/etc` and S20wifi edits
  `wpa_supplicant.conf`. The SD card stays optional (`/sdcard/config` imports, a
  development uvcd). Nothing else writes to flash.
- **Install**: `image/build-modern.sh` builds `uImage`, `root.sqfs` and a
  `demo.bin` carrying both (`demo-bin.py` takes our uImage as a fourth argument).
  `image/flash-modern.sh` writes both partitions over ssh: it saves both first,
  writes root then kernel with `flashcp`, reads both back, and writes the saved
  copies back if anything does not match. It works from OpenMiko's kernel (root
  in RAM) and from ours (it then runs from busybox copied to `/tmp` and reboots
  through sysrq at once, since it rewrote the filesystem it runs from).
- The init scripts work on both kernels: S00rootfs does nothing unless `/` is a
  squashfs, S10config skips the `/rom` mount, S20wifi loads the WiFi driver with
  `modprobe` (modules from our kernel's `modules.dep`), S50camera logs that there
  are no camera drivers and returns.

Not run: `build-modern.sh` needs OpenMiko's `demo.bin` (`deploy/demo.bin`) like
`build.sh`, which was not available here. The pieces it calls were checked
separately (`build-rootfs.py` with extra overlays, `demo-bin.py` with a uImage,
`sqfstar` from squashfs-tools 4.6).

## Camera stack: gap analysis

Today: uvcd links Ingenic's closed `libimp.a` statically; libimp talks to
`tx-isp.ko` (ioctls on `/dev/tx-isp`, `/dev/isp-m0`, frame channels
`/dev/framechan*`), to the VPU through `/dev/soc_vpu` and the IPU through
`/dev/ipu`, and maps ispmem/rmem through `/dev/mem`; `sinfo.ko` probes the sensor
and `sensor_jxf22.ko`/`sensor_jxf23.ko` drive it over I2C. All of those modules
are built for 3.10.14 only.

What exists in the open:

| Piece | Open code | Depends on 3.10 how | Work |
|---|---|---|---|
| ISP | open-tx-isp `driver/t20` (2026-10-27, "fully open" on T20 with stock libimp and with OpenIMP; 39k lines with the ingenic-sdk T20 sources it builds) | the 3.10 V4L2 subdev and media-entity core (`v4l2_subdev_core_ops.g_ctrl/s_ctrl/g_chip_ident`, `v4l2_subdev.entity`, `V4L2_MBUS_FMT_*`), `set_fs`/`f_dentry` to load the tuning file, `PDE_DATA`, vendor arch exports | port (in progress, below) |
| Sensor JXF22/JXF23, sinfo | ingenic-sdk `3.10.14/sensor-src`, open-tx-isp `tx_isp_sinfo.c` | same V4L2 subdev core, `jzgpio_set_func` for the DVP pins and MCLK | port with the ISP |
| Encoder | vendor `drivers/video/soc_vpu` (`soc_vpu.c`, `jz_nvpu.c`) and `drivers/video/jz_ipu` (`jz_ipu_v13.c`), GPL, about 5k lines; the H.264 encoder itself runs in libimp/OpenIMP on the VPU | 3.10 driver APIs (misc device, `dma_cache_*`, vendor CPM registers) | port, not started |
| libimp | OpenIMP (T20 "release": all 44 IMP functions uvcd calls are implemented and device-tested on T20, table below) | none in the library; it needs the same kernel interfaces as libimp | build for musl, link into uvcd |
| Audio | ingenic-sdk `3.10.14/audio/t20/oss2` | OSS-style 3.10 driver, vendor DMA | port, not started |

**Decision: uvcd keeps the libimp API, through OpenIMP, and the kernel provides
the vendor interfaces.** Reasons: open-tx-isp and OpenIMP are device-tested on the
T20 as a pair and against stock libimp, so the ISP port can be checked against
the closed libimp first (same ioctls) and OpenIMP swapped in afterwards; the
picture quality (AE, AWB, noise reduction, the sensor tuning file) lives in that
pair and would have to be rewritten for V4L2; the encoder on the T20 is mostly
userspace firmware driven through `/dev/soc_vpu`, for which there is no V4L2
driver anywhere; and uvcd's 44 IMP calls (below) stay as they are. A V4L2 driver
would be the cleaner long-term interface, but it is a new ISP and encoder driver,
not a port. The cost of this choice is that the drivers keep vendor-style
interfaces in a modern kernel (`t20-compat.ko`, below), outside mainline.

**ISP port, state.** `t20-compat.ko` supplies the arch interfaces and
`compat/include` the vendor headers; the kernel has V4L2 and the media
controller as modules (`videodev.ko` 290 KB, `mc.ko` 49 KB, not in the kernel
image), because the ISP registers subdevices, video nodes and controls with them
and already uses the `v4l2_ctrl` framework. With that, open-tx-isp's T20 module
compiles to 272 errors, listed by cause in `uvc/kernel/compat/README`: mostly the
renamed media bus codes and V4L2 ioctls and ops removed since 3.10 (mechanical),
then videobuf2 and dma-buf changes in the frame channels, which are the
interface libimp reads frames from and must keep their ABI exactly (the
real work), plus `set_fs` in the tuning-file loader and include-order
constants. A private copy of the whole 3.10 V4L2 core in the module was the
alternative; the ISP also uses video nodes, videobuf2, priorities and controls
from it, about 12k lines of 3.10 code, more than porting the callers. Order
after the ISP builds: load it with the closed libimp first (same ioctls, so
uvcd unchanged) to get video out, then the sensor drivers (same subdev changes),
VPU/IPU (encoder), OpenIMP in uvcd, audio.

### uvcd's libimp calls against OpenIMP (T20)

From `uvc/src` (all in `capture.c`, plus `IMP_Log_Get_Option`, which uvcd itself
provides in `imp_compat.c`). OpenIMP status from its `docs/FEATURE_MATRIX.md`
(RC2, 2026-10-25): ✅ = implemented, device-tested on T20 and audited.

| Call | Use in uvcd | OpenIMP T20 |
|---|---|---|
| `IMP_System_Init`, `IMP_System_Exit` | start, stop | ✅ |
| `IMP_System_Bind`, `IMP_System_UnBind` | frame source → encoder group | ✅ |
| `IMP_System_RebaseTimeStamp` | timestamps on the monotonic clock | ✅ |
| `IMP_ISP_Open`, `IMP_ISP_Close` | ISP session | ✅ |
| `IMP_ISP_AddSensor`, `IMP_ISP_DelSensor`, `IMP_ISP_EnableSensor`, `IMP_ISP_DisableSensor` | JXF22/JXF23 | ✅ |
| `IMP_ISP_EnableTuning`, `IMP_ISP_DisableTuning` | tuning session | ✅ |
| `IMP_ISP_Tuning_SetISPRunningMode` | day/night | ✅ |
| `IMP_ISP_Tuning_SetISPHflip`, `IMP_ISP_Tuning_SetISPVflip` | image flip | ✅ |
| `IMP_ISP_Tuning_SetBrightness`, `SetContrast`, `SetSaturation`, `SetSharpness` | image settings | ✅ |
| `IMP_ISP_Tuning_SetTemperStrength` | temporal noise reduction | ✅ |
| `IMP_ISP_Tuning_SetSceneMode`, `SetColorfxMode`, `SetWDRAttr` | defaults | ✅ |
| `IMP_FrameSource_CreateChn`, `DestroyChn`, `SetChnAttr`, `EnableChn`, `DisableChn`, `SetFrameDepth` | main and detector channels | ✅ |
| `IMP_FrameSource_GetFrame`, `ReleaseFrame` | detector frames (NV12) | ✅ |
| `IMP_Encoder_CreateGroup`, `DestroyGroup`, `CreateChn`, `DestroyChn`, `RegisterChn`, `UnRegisterChn` | H.264 and JPEG channels | ✅ |
| `IMP_Encoder_StartRecvPic`, `StopRecvPic`, `PollingStream`, `GetStream`, `ReleaseStream` | encoded stream | ✅ |
| `IMP_Log_Get_Option` | libalog hook | uvcd's own (`imp_compat.c`) |

Moving to OpenIMP also removes the two workarounds for the closed library:
`tools/imp-rename.py` and the `mm32`/`ftrunc32` shims exist because libimp was
built with uClibc's 32-bit `off_t`; OpenIMP built with zig cc for musl does not
need them. The ispmem/rmem mapping stays (OpenIMP maps rmem through `/dev/mem`
too, at the address our device tree keeps free).

## Hardware test plan

**Before anything**: an SD card with a known-good `demo.bin` (today's,
`image/build.sh`, OpenMiko's kernel) and the setup-button procedure tested on this
camera. A serial console (UART on ttyS1, 115200 8N1) is strongly recommended for
the first boots: without it a kernel that does not reach WiFi is a black box. U-Boot
over serial is not set up, so a test kernel cannot be booted from RAM; every test
is a flash write.

**Recovery**: power off, put the known-good `demo.bin` on the SD card, hold the
setup button, power on (README, "Install"). The bootloader rewrites the kernel
partition and the first 274 blocks of the root partition and the camera boots
OpenMiko's kernel again. The config partition is not touched by either path,
but see the JFFS2 risk below.

1. **Serial first boot** (`demo.bin` from `image/build-modern.sh`, or
   `image/flash-modern.sh` from a running camera). Expected on ttyS1: U-Boot
   `## Booting kernel ... LZMA ... OK`, then earlycon output from the first line
   of the kernel. Check: `Linux version 6.18.59`, `T20` as system type, the CPU line
   (`Ingenic XBurst`), the clock rates (`cat /sys/kernel/debug/clk/clk_summary` is not
   built; compare `cpu` 860 MHz with `/proc/cpuinfo` BogoMIPS), timer interrupts
   (`grep -i ost /proc/interrupts` growing), `cat /proc/mtd` (six partitions, erase
   size 00008000), squashfs mounted as `/`.
2. **Flash and config**: `mount | grep config` (JFFS2 on mtdblock3 without errors in
   `dmesg`), `ls /config/overlay/etc`. Do not write `/config` until this is clean.
3. **GPIOs and LEDs**: `echo 1 > /sys/class/leds/led_blue/brightness`, the IR cut
   filter through uvcd's day/night switch or by hand (GPIO 25/26 pulses), IR LEDs
   (GPIO 49).
4. **SD card**: insert, `dmesg` for `mmc0: new ... card`, `mount /dev/mmcblk0p1`,
   read and write a file.
5. **WiFi**: `modprobe 8189fs`, `ip link` shows wlan0 with the camera's MAC, S20wifi
   associates, DHCP, `ssh` in. Then the watchdog: uvcd arms it; `killall -STOP uvcd`
   must reboot the camera.
6. **Back to OpenMiko's kernel** once (`image/flash.sh` writes only the root
   partition and leaves our kernel in place, so use demo.bin) to prove the way back.

**First-boot risks**, most likely first:

- **No output at all**: wrong load address/format for this U-Boot, the unpacked
  kernel overlapping U-Boot's load buffer, or a fault before the console. Earlycon
  is on; U-Boot's own messages show whether it accepted the image. Recovery:
  demo.bin.
- **Hang after "Calibrating delay" or no scheduler ticks**: the OST interrupt (CPU
  IP4) or its clock. The TCU registers a second clock event, which may keep the
  system going. Serial needed to tell.
- **Wrong CPU rate or MMC clock**: the PLL readout (two output dividers and the
  fractional part as the vendor reads them) is new code; UART is unaffected (it
  runs from the 24 MHz crystal), the SD card and SDIO clocks are not.
- **No flash**: the SFC at U-Boot's clock in single-bit mode should be safe; a chip
  without a 32 KiB erase in its SFDP table falls back to 64 KiB.
- **Config partition rewritten**: if the erase size is not 32 KiB, JFFS2 may
  treat blocks as dirty and erase them: WiFi settings lost, no SSH. Keep
  `wpa_supplicant.conf` under `/sdcard/config/overlay/etc/` on the SD card; S10config
  imports it at boot.
- **WiFi not coming up**, which loses SSH: the power line (PC16) and the SDIO bus
  in PIO mode at 24 MHz are both unverified. With serial, `dmesg | grep mmc1`
  shows whether the card enumerates. Without serial the only way back is demo.bin.
- **Init**: OpenMiko's busybox and uClibc on a 6.18 kernel (system calls are
  compatible; not seen to run). `/sbin/init` is the kernel's default.
- **No camera**: expected. uvcd starts, finds no `/proc/jz/sinfo/info`, logs it and
  keeps restarting every 5 s; WiFi, SSH and the LEDs work.

## Building

```sh
uvc/kernel/build.sh          # third_party/kernel/ (sources), build/kernel/out/
uvc/image/build-modern.sh    # build/image-modern/{uImage,root.sqfs,demo.bin}
uvc/image/flash-modern.sh 192.168.1.9
```

Host: Linux x86_64 with make, gcc, flex, bison, bc, python3, xz-utils, git, curl,
kmod (depmod) and squashfs-tools (sqfstar). A clean kernel build takes about 15
minutes on 4 cores.
