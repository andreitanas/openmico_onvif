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
| Sensor JX-F22/JX-F23 | ours, `jxf2x` (patch 0011; register tables from Ingenic's GPL sensor drivers) | builds |
| ISP | ours, `t20-isp` (patch 0012), from [`t20-isp-hardware.md`](t20-isp-hardware.md) | first version builds: NV12, no 3A or tuning |
| H.264 encoder (VPU) | ours, to write | **missing**: not started |
| Audio | ALSA, to write | **missing**: not started |
| uvcd | unchanged, closed libimp | runs without video on the new kernel; needs our libimp over V4L2 (below) |

So the new kernel is a bring-up kernel: shell on the serial console, flash, SD card,
GPIOs and LEDs, WiFi and SSH, no camera. The image scripts keep OpenMiko's kernel
as the default (`image/build.sh`, `image/flash.sh` are unchanged in behaviour).

## Where this stopped

Work paused here, on branch `kernel-modern`, with the owner to test on hardware:

- Built and checked here (clean rebuild from `kernel/build.sh`): the kernel,
  the device tree, the WiFi module, `jxf2x`, `t20-isp`, `capture/v4l2grab`.
- Written, not run: `image/build-modern.sh` and `image/flash-modern.sh` (they
  need OpenMiko's `deploy/demo.bin`, which is not in this environment).
- Not started: ISP statistics and AE/AWB in the driver, ISP tuning tables, the
  DS2 stream, mirror and flip, the H.264 and JPEG encoders, our libimp, audio.
- Next, in order: hardware test steps 1-5 (boot, flash, SD, WiFi), then step 6
  (the camera with `v4l2grab`); what comes back decides the ISP work. The
  encoder is the largest open piece and the riskiest: the VPU's registers are
  documented (Ingenic's GPL `jzm_vpu.h`), the encoding algorithm is not. Start
  with JPEG, the smaller block.
- Top first-boot risks: see "First-boot risks" in the [Hardware test plan](#hardware-test-plan). In short, a
  wrong clock or pin setting can leave the camera without console output and,
  since serial U-Boot is not set up, recovery is the SD card `demo.bin` only;
  if WiFi does not come up there is no SSH.

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

**Memory**. Linux keeps the low 96 MB as with `mem=96M@0x0`; 0x6000000 to
0x8000000 (today's `ispmem` and `rmem`) is a reserved pool for the camera's frame
buffers, used by the ISP driver through the DMA API.

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
| uImage | (in a 2048 KiB partition, with the initramfs) | 1,716,688 bytes of 2,097,152 (82 %) |
| Unpacked image | | 5,050,772 bytes of 5,242,880 (192 KB left) |
| Kernel code | 3877k | 3621k |
| Data | 1205k | 1112k "rwdata" (of which 924k read-only), 67k bss |
| Init, freed after boot | | 188k |
| Modules loaded for WiFi | 8189fs (cfg80211 built in) | cfg80211 504k, ipv6 555k, 8189fs (2.2 MB file, stripped), firmware_class, libsha1 |
| Root filesystem | `rootfs.tar.xz` unpacked into RAM (zram) on every boot | squashfs read from flash, page cache only |

The code and data figures are from the build before the camera drivers; the
media core added about 34 KB since (the camera drivers themselves are modules).
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

## Camera stack

**Decision (owner, 2026-10-27): mainline-style kernel drivers, clean-room code.**
The camera hardware gets V4L2 drivers in the kernel, like other SoCs' camera
pipelines in mainline (the closest model is ARM's Mali-C55, a later ISP of the
same Apical family). No code is taken from open-tx-isp (GPLv3), OpenIMP (mostly
unlicensed) or the unlicensed parts of Ingenic's SDK; they serve as
documentation, and what is used from them is recorded as hardware facts in
[`t20-isp-hardware.md`](t20-isp-hardware.md). Ingenic's GPL-2.0+ sources (the
sensor drivers, the 3.10 kernel) can be used directly. On top, uvcd gets our own
implementation of the libimp calls it makes, over V4L2, so uvcd's code stays as
it is; the closed libimp, `tools/imp-rename.py` and the `mm32`/`ftrunc32` shims
go.

| Piece | Kernel | Userspace (our libimp) | Status |
|---|---|---|---|
| Sensor JX-F22/JX-F23 | `jxf2x` (patch 0011): V4L2 subdevice, exposure, analog gain, frame length; both sensors, told apart by chip ID | `IMP_ISP_AddSensor`, `EnableSensor`; exposure and gain from our AE | builds |
| ISP | `t20-isp` (patch 0012): VIC + ISP core, NV12 out of DS1 at sensor size, one video node | FrameSource channels | first version builds |
| Auto exposure, white balance | statistics node (to do; the statistics layout is not known yet) | our AE/AWB, from statistics or from the frames | AE on frame luma in `capture/v4l2grab.c`; AWB not started |
| Image tuning (gamma, noise reduction, shading, sharpness) | parameters node (to do; the tables' formats are not known yet) | `IMP_ISP_Tuning_*` | not started; those blocks are bypassed, so the picture is linear (dark, flat) |
| Second stream | DS2 path (to do) | detector channel | not started |
| H.264 encoder | VPU at 0x13200000 (register map in Ingenic's GPL `jzm_vpu.h`); V4L2 stateful encoder (to do) | `IMP_Encoder_*` | not started: the encoding itself (motion search, mode decision, entropy coding, rate control) is in the closed libimp; a clean-room encoder for this VPU is the largest piece left |
| JPEG | the VPU's JPEG codec block (`JPGC`, documented in `jzm_vpu.h`) | snapshot channel | not started |
| Audio | ALSA (the AIC and the internal codec; mainline has related Ingenic drivers) | not used by uvcd yet | not started |

What `t20-isp` does today: VIC set up for the 10-bit parallel bus, the ISP core's
geometry and Bayer order, every block that needs a memory table bypassed (white
balance, demosaic, colour matrix and the DS1 colour space converter at their reset
values: gains 1.0, identity matrix, BT.601), the DS1 Y and UV writers in NV12 from
32 MB of reserved memory, frames completed on the DS1 output-end interrupt
(source 50, the one source number the SDK shows). No crop, scaling, mirroring,
statistics or tuning. Unknowns are listed in `t20-isp-hardware.md`.

### uvcd's libimp calls

uvcd calls 44 libimp functions (all in `src/capture.c`; `IMP_Log_Get_Option` is
uvcd's own, in `imp_compat.c`). Our implementation maps them onto the drivers:

| Calls | Over |
|---|---|
| `IMP_System_Init`, `Exit`, `Bind`, `UnBind`, `RebaseTimeStamp` | opening the media device and video nodes; binding is the fixed ISP → encoder path |
| `IMP_ISP_Open`, `Close`, `AddSensor`, `DelSensor`, `EnableSensor`, `DisableSensor`, `EnableTuning`, `DisableTuning` | the sensor subdevice and the ISP |
| `IMP_ISP_Tuning_SetISPRunningMode` | day/night: AE target and colour (with the IR cut filter, which uvcd drives itself) |
| `IMP_ISP_Tuning_SetISPHflip`, `SetISPVflip` | ISP mirror and sensor flip (to do in the drivers) |
| `IMP_ISP_Tuning_SetBrightness`, `SetContrast`, `SetSaturation`, `SetSharpness`, `SetTemperStrength`, `SetSceneMode`, `SetColorfxMode`, `SetWDRAttr` | ISP parameters (to do) |
| `IMP_FrameSource_CreateChn`, `DestroyChn`, `SetChnAttr`, `EnableChn`, `DisableChn`, `SetFrameDepth`, `GetFrame`, `ReleaseFrame` | the DS1 and DS2 video nodes |
| `IMP_Encoder_CreateGroup`, `DestroyGroup`, `CreateChn`, `DestroyChn`, `RegisterChn`, `UnRegisterChn`, `StartRecvPic`, `StopRecvPic`, `PollingStream`, `GetStream`, `ReleaseStream` | the encoder's V4L2 node, frames passed as dma-bufs from the ISP |

Order from here: test the sensor and ISP on hardware with `v4l2grab` (below),
then statistics and our AE/AWB, the DS2 stream, the encoder, our libimp in uvcd,
then audio.

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
6. **Camera** (with SSH up, since a mistake here can hang the SoC):
   `modprobe jxf2x`, then `dmesg` shows `jxf22` or `jxf23`; `modprobe t20-isp`,
   then `/dev/video0` and two `/dev/v4l-subdev*` exist. `v4l2grab
   /sdcard/f.nv12 100` streams 100 frames: it should report about 25 fps and a
   mean Y that settles near 110 as its auto exposure works. Convert the saved
   frame with `ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i f.nv12 f.png`:
   expect a dark, flat, unbalanced picture (no gamma, white balance or noise
   reduction yet). Failures to report: no frame in 2 s (interrupt routing or
   the input port), DMA alarms, a green or striped picture (writer format or
   Bayer order), a hang (register access with a clock off).
7. **Back to OpenMiko's kernel** once (`image/flash.sh` writes only the root
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
- **No camera in uvcd**: expected. uvcd starts, finds no `/proc/jz/sinfo/info`,
  logs it and keeps restarting every 5 s; WiFi, SSH and the LEDs work. The
  camera drivers are not loaded at boot (step 6 loads them by hand).

## Building

```sh
uvc/kernel/build.sh          # third_party/kernel/ (sources), build/kernel/out/
uvc/image/build-modern.sh    # build/image-modern/{uImage,root.sqfs,demo.bin}
uvc/image/flash-modern.sh 192.168.1.9
```

Host: Linux x86_64 with make, gcc, flex, bison, bc, python3, xz-utils, git, curl,
kmod (depmod) and squashfs-tools (sqfstar). A clean kernel build takes about 15
minutes on 4 cores.
