# A modern kernel for the Wyze Cam v2 (2026-10-27)

Linux 6.18 LTS for the Ingenic T20, built from source by `uvc/kernel/build.sh`,
with a root filesystem on flash instead of OpenMiko's initramfs. It boots on a
Wyze Cam v2 from flash, with WiFi, SSH and the SD card (2026-10-27, with a
serial console; see [Bring-up on hardware](#bring-up-on-hardware)). Our sensor
and ISP drivers deliver 1080p colour frames at 25 fps, the VPU drivers encode
them to H.264 and JPEG, and uvcd streams them to Protect (2026-10-28). Read
[Hardware test plan](#hardware-test-plan) before flashing anything.

## Status

| Piece | Source | Status |
|---|---|---|
| Kernel, MIPS32r1, T20 machine | Linux v6.18.59 + patches 0001-0006 | runs |
| Clocks (CGU) | ours, `t20-cgu.c` (patch 0007) | runs |
| Interrupts (INTC) | mainline `irq-ingenic` (jz4780) | runs |
| Timers | mainline `ingenic-sysost` (OST) and `ingenic-timer` (TCU) | runs: OST is the clocksource (24 MHz, BogoMIPS 858), TCU registered too |
| UART console (ttyS1) | mainline `8250_ingenic` | runs |
| Pins, GPIO, LEDs, `/sys/class/gpio` | mainline `pinctrl-ingenic` + T20 (patch 0008) | runs: IR cut filter (GPIO 25/26) and IR LEDs (GPIO 49) checked from frames; the status LEDs not checked by eye |
| SPI NOR flash, partitions | gtxaspec's `spi-ingenic-sfc` (0005, 0009), mainline spi-nor + 32 KiB erase (0010) | runs: reads, `flashcp` writes, JFFS2 config |
| SD card (MSC0) | mainline `jz4740_mmc`, DMA through the PDMA (`dma-jz4780`, built in) | runs: 16 MB/s reads (one 8 GB card read only zeros; see below) |
| Watchdog | mainline `jz4740_wdt` | runs: `/dev/watchdog`, an unfed watchdog resets the SoC |
| I2C (sensor bus) | mainline `i2c-jz4780` | runs |
| WiFi, RTL8189FS on MSC1 | jwrdegoede/rtl8189ES_linux (out of tree), cfg80211 module; SDIO transfers by DMA | runs: WPA, DHCP, SSH; with both streams going out, the kernel's share of the CPU went from 31% (PIO) to 11% (DMA) |
| Squashfs root on mtdblock2 | `image/build-modern.sh`, `image/flash-modern.sh` | runs; installed over SSH from both kernels |
| Sensor JX-F22/JX-F23 | ours, `jxf2x` (patch 0011; register tables from Ingenic's GPL sensor drivers) | runs (JX-F22): 1080p, exposure, gain, mirror and flip controls |
| ISP | ours, `t20-isp` (patch 0012), from [`t20-isp-hardware.md`](t20-isp-hardware.md) | runs: 1920x1080 NV12 at 24.6 fps in colour: black level, auto white balance, colour matrix, sRGB gamma, noise reduction, sharpening, brightness/contrast/saturation |
| JPEG encoder (VPU) | ours, `t20-vpu` (patch 0013; the VPU sequence and Huffman table from Ingenic's GPL X1000/T31 drivers) | runs: 1080p NV12 to JPEG in 10.7 ms, from a file or an ISP frame as a dma-buf |
| H.264 encoder (VPU) | ours, `t20-vpu` (patch 0013; register names from Ingenic's GPL Helix encoder, the T20's values in [`t20-isp-hardware.md`](t20-isp-hardware.md)) | runs: 1080p Main profile, CABAC, I and P frames, 16 ms a frame in the VPU, constant QP or bit rate; streams pass strict ffmpeg decoding |
| Audio | mainline `jz4740-i2s` (T20 variant) and `dma-jz4780`, our `t20-codec` (patches 0014, 0015; sequences from Ingenic's GPL OSS driver) | runs: the microphone at 8-96 kHz through `/dev/dsp` (OSS emulation) or ALSA; `audiorec` records it; uvcd high-passes and amplifies it in software; the speaker is not driven yet |
| uvcd | `src/capture_v4l2.c` on this kernel, the closed libimp on 3.10 (`capture.c` picks at start) | runs: both H.264 streams to Protect, snapshots, object detection, auto exposure, the exposure figure for day/night; `test/test_capture_v4l2.c` (on the camera, uvcd stopped) checks streams, snapshots, raw frames, flip and mirror restarts and start/stop cycles |

So the new kernel is a bring-up kernel: shell on the serial console, flash, SD card,
GPIOs and LEDs, WiFi and SSH, the watchdog, and raw camera frames that only
`v4l2grab` reads. The image scripts keep OpenMiko's kernel
as the default (`image/build.sh`, `image/flash.sh` are unchanged in behaviour).

## Where this stopped

On branch `kernel-modern`, with one camera on the new kernel and a serial
console attached:

- Runs on the camera: hardware test steps 1, 2, 4, 5, 6 and 7 (boot, flash and
  config, SD card, WiFi and SSH, the watchdog, the camera with `v4l2grab`,
  the microphone; listening in Protect still to do),
  and `flash-modern.sh` from both kernels. S50camera loads the camera drivers
  at boot.
- Open from bring-up: the 8 GB card that read only zeros; of step 3, only the
  status LEDs (the IR cut filter and IR LEDs were checked from frames on
  2026-10-28: at fixed exposure the filter out raised the mean luma from 104
  to 133 with the magenta cast of IR, the IR LEDs to 208, and the filter back
  in returned it to 110).
- Done since (2026-10-28): colour (white balance with AWB, colour matrix,
  gamma, noise reduction), the DS2 stream, mirror and flip, and the VPU's JPEG
  and H.264 encoders (`t20-vpu`, one driver, a video node each; tests
  `v4l2jpeg` and `v4l2h264`, which also feed ISP frames as dma-bufs).
- Temporal noise reduction (Temper) runs since 2026-10-28, from the camera
  memory, which is now a CMA area (below). Iridix (local tone mapping) runs
  since 2026-10-28 as Protect's WDR (below). Not started: ISP shading, which
  needs a flat-field calibration of the lens (a white target in front of
  the camera).
  **Decision (2026-10-28): Temper's frame buffer stays GEN32 (8.3 MB), not
  RAW16 (4.1 MB):** RAW16 removed only 30-75% of the noise GEN32 removes at
  full gain ([`t20-isp-hardware.md`](t20-isp-hardware.md)), night pictures
  are where that matters, and the memory is not short (about 45 MB free
  with uvcd streaming).
- uvcd streams on this kernel (2026-10-28) through `src/capture_v4l2.c`
  (below), not through a libimp of our own as first planned.
- Audio (2026-10-28): the microphone in the kernel (patches 0014, 0015) and in
  uvcd's streams (AAC-LC); the speaker is not driven yet. The first version
  was silent in Protect: the audio clock ran 1.2x fast (the CGU read the
  I2SCDR PLL select as the X1000's) and the signal was 30 dB too low (the
  codec's gain controls do nothing). Both fixed; see Audio gain below.
- Snapshots (2026-10-28) take 40-60 ms, from 86-116 ms: the JPEG buffer is
  now cached (Snapshot buffers, below).
- Not checked yet: booting without an SD card. From the scripts it should
  work, 10 s slower (S10config waits that long for the card; the rollback
  runs only with the card mounted; uvcd and its model are in flash).
- Left on the test camera (2026-10-28): uvcd runs from the SD card
  (`/sdcard/uvc/uvcd`, which `uvcd-run` prefers to the one in flash), as the
  flashed image predates the audio limiter; delete it once an image with
  the limiter is flashed. `/sdcard/nightlog.sh` appends the exposure figure,
  gain and day/night state to `/sdcard/nightlog.txt` every minute, for the
  night test below; stop it and delete both when done.
- Next: night mode in real darkness (the day/night thresholds were measured
  under the vendor's ISP; uvcd's exposure figure follows the same formula
  but has not seen a dark room yet: read `/sdcard/nightlog.txt` after a
  night), shading (needs a flat field), the speaker for talkback (the test
  camera's speaker is not connected).
- Needs someone at the camera or in Protect: the status LEDs by eye, the
  8 GB card, a boot without an SD card, the way back to OpenMiko with
  `demo.bin` (hardware test step 8), listening to the microphone in Protect
  (the limiter has not met a loud sound yet), judging WDR level 1 and
  whether Protect shows the WDR setting (WDR, below), and the white target
  for shading.

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
WiFi power is PB30 (GPIO 62), high for on, as an `mmc-pwrseq-simple` reset
line. This camera's U-Boot names it `wifi_enable_gpio` in its boot log. The
vendor `board.h` has WL_REG_EN on PC16 (GPIO 80), which is not this board's.

**LEDs**. `led_blue` GPIO 39 and `led_yellow` GPIO 38, active low, from OpenMiko's
`linux-led.patch`, so `/sys/class/leds/led_blue` and `led_yellow` exist as uvcd and
S01system expect.

**Memory**. 0x6000000 to 0x8000000 (today's `ispmem` and `rmem`) is a CMA
area for the camera's frame buffers (`reusable` in the device tree), used
by the ISP and VPU drivers through the DMA API. A plain reserved pool
(`no-map`, the first version) rounds every allocation up to a power of two
(4 MB for a 3 MB 1080p frame) and filled up with the streams alone; CMA
allocates to the page, so Temper's 8.3 MB frame buffer fits too (uvcd
running: 30.6 of 32 MB used), and Linux uses the free part for movable
pages. CMA costs 37 KB of kernel.

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
| uImage | (in a 2048 KiB partition, with the initramfs) | 1,771,467 bytes of 2,097,152 (84 %) |
| Unpacked image | | 5,215,064 bytes of 5,242,880 (27 KB left; 2026-10-28, with the encoders, audio, CMA) |
| Kernel code | 3877k | 3717k |
| Data | 1205k | 187k rwdata, 940k rodata, 67k bss |
| Init, freed after boot | | 204k |
| Modules loaded for WiFi | 8189fs (cfg80211 built in) | cfg80211 504k, ipv6 555k, 8189fs (2.2 MB file, stripped), firmware_class, libsha1 |
| Root filesystem | `rootfs.tar.xz` unpacked into RAM (zram) on every boot | squashfs read from flash, page cache only |
| Free at boot | | 91,620K of 131,072K with the camera memory a reserved pool; with it a CMA area, 124,868K total and 59 MB available while uvcd streams |

Kernel figures as the kernel prints them at boot on the camera ("Memory: ...
kernel code, rwdata, rodata, init, bss"), with the media core built in and the
camera drivers as modules. The resident kernel is a little
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
  its WiFi settings, hostname and keys. `/etc`, `/root` and `/var` (if it is a
  directory) get a writable layer in RAM (`S00rootfs`, overlayfs on tmpfs),
  because S10config installs `/config/overlay` over them (`/etc/dropbear`,
  `/root/.ssh/authorized_keys`) and S20wifi edits `wpa_supplicant.conf`. The SD card stays optional (`/sdcard/config` imports, a
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

With our kernel, `build-rootfs.py` drops all of `lib/modules/3.10.14` and
`driver/`, and the init scripts tell the kernels apart by `uname -r`. Empty
directories from the overlays are kept: `/config` and `/sdcard` are mount
points on a read-only root.

## Camera stack

**Decision (owner, 2026-10-27): mainline-style kernel drivers, clean-room code.**
The camera hardware gets V4L2 drivers in the kernel, like other SoCs' camera
pipelines in mainline (the closest model is ARM's Mali-C55, a later ISP of the
same Apical family). No code is taken from open-tx-isp (GPLv3), OpenIMP (mostly
unlicensed) or the unlicensed parts of Ingenic's SDK; they serve as
documentation, and what is used from them is recorded as hardware facts in
[`t20-isp-hardware.md`](t20-isp-hardware.md). Ingenic's GPL-2.0+ sources (the
sensor drivers, the 3.10 kernel) can be used directly. On top, uvcd has a
second capture backend, `src/capture_v4l2.c`, with the same interface
(`capture.h`) over these V4L2 drivers; `capture.c` uses it when the drivers
are there and libimp otherwise. A libimp of our own (the plan below) was not
needed: uvcd's interface to the camera is six functions, libimp's is 44 with
the vendor's structure layouts. The closed libimp, `tools/imp-rename.py` and
the `mm32`/`ftrunc32` shims stay only for the 3.10 kernel.

| Piece | Kernel | Userspace (our libimp) | Status |
|---|---|---|---|
| Sensor JX-F22/JX-F23 | `jxf2x` (patch 0011): V4L2 subdevice, exposure, analog gain, frame length; both sensors, told apart by chip ID | `IMP_ISP_AddSensor`, `EnableSensor`; exposure and gain from our AE | runs (JX-F22) |
| ISP | `t20-isp` (patch 0012): VIC + ISP core, NV12 out of DS1 at sensor size, one video node | FrameSource channels | runs: 25 fps |
| Auto exposure, white balance | AWB in the driver (grey world from the ISP's global R/G, B/G statistics, every frame); white balance, brightness, contrast, saturation and noise reduction as V4L2 controls | our AE from the frames | runs: AWB in `t20-isp`, AE on frame luma in uvcd (`src/capture_v4l2.c`) and `capture/v4l2grab.c` |
| Image tuning (gamma, noise reduction, shading, sharpness) | fixed in the driver: sRGB gamma, a generic colour matrix, Sinter with a flat noise profile | `IMP_ISP_Tuning_*` | gamma, colour, Sinter, sharpening, Temper, Iridix run; shading not started |
| Second stream | DS2 path: a second video node (`t20-isp-sub`, `/dev/video1`) scaled to any even size, 640x360 by default | detector channel | runs, alone or with the main stream |
| H.264 encoder | `t20-vpu` (patch 0013), node `t20-h264`: V4L2 stateful encoder, NV12 in (MMAP or dma-buf), Annex B out (SPS and PPS before every IDR frame); controls GOP size, force key frame, frame rate control with bit rate and min/max QP (or I/P QP), frame rate by `S_PARM`. The VPU does motion search, mode decision, transform and CABAC; the driver writes the headers and runs the rate control | `IMP_Encoder_*` | runs (`v4l2h264`): 1080p at the ISP's 25 fps, 16 ms a frame in the VPU; PSNR 43 dB on a synthetic pan; 1-8 Mbit/s held within 2% |
| JPEG | `t20-vpu` (patch 0013), node `t20-jpeg`: V4L2 mem2mem, NV12 in (MMAP or dma-buf), JFIF out, `V4L2_CID_JPEG_COMPRESSION_QUALITY` | snapshot channel | runs (`v4l2jpeg`), also while H.264 encodes (the jobs take turns); uvcd's snapshots take 40-60 ms (below) |
| Audio | ALSA: the AIC (mainline `jz4740-i2s`, a T20 variant), PDMA (`dma-jz4780` as the X1830's), our internal codec driver `t20-codec` (patch 0014), `simple-audio-card`; the codec is the I2S master on the AIC's 256 fs system clock; its one ADC is the right channel | uvcd's `src/audio.c`: `/dev/dsp` at 16 kHz, AAC-LC 32 kbit/s (vo-aacenc), into the streams' audio track in place of silence | microphone runs (`audiorec`, `test/test_audio.c`; uvcd streams it, gain from Protect's microphone volume); speaker (DAC, amplifier GPIO) not started |

What `t20-isp` does today: VIC set up for the 10-bit parallel bus, the ISP core's
geometry and Bayer order, every block that needs a memory table bypassed (white
balance, demosaic, colour matrix and the DS1 colour space converter at their reset
values: gains 1.0, identity matrix, BT.601), the DS1 Y and UV writers in NV12 from
32 MB of reserved memory, frames completed on the DS1 output-end interrupt
(source 50, the one source number the SDK shows). No crop, scaling, mirroring,
statistics or tuning. Unknowns are listed in `t20-isp-hardware.md`.

### Audio gain

**Decision (2026-10-28): the microphone's gain is applied in software, in
uvcd, not in the codec.** The internal codec's PGA (CACR2) and the AGC-like
registers at 0x100-0x130 change nothing on the T20 (measured,
[`t20-isp-hardware.md`](t20-isp-hardware.md)); only the +20 dB boost and the
ALC bits do, and the kernel sets both. That leaves the ADC's output about 30
dB below a usable level, with a DC offset and room rumble below 100 Hz far
above the rest. The vendor's stack does the same in libimp (`IMP_AI_SetVol`).
So `t20-codec` keeps the vendor's fixed settings (its "Mic Capture Volume"
control stays, as the vendor's register, but changes nothing measurable), and
`src/audio.c` filters and amplifies:

- a 4th-order Butterworth high-pass at 120 Hz (two biquads, float), against
  the DC offset and the rumble, which the gain would otherwise bring near
  full scale;
- then a gain linear in dB from Protect's microphone volume
  (`ChangeVideoSettings` `audio.volume`, 0-100, default 100): 0 is silence,
  100 is +30 dB;
- then a peak limiter at -3 dBFS (instant attack, 100 ms release), so a
  loud sound near the camera is turned down instead of clipping (added
  2026-10-28: a -20 dBFS burst at +30 dB came out at -3 dBFS peak with no
  clipped samples, and quiet sound after it at its level again).

At volume 100 a quiet room comes out at -46 dBFS RMS (peaks -21 dBFS), so
background sound is audible and a loud sound has about 20 dB before it
reaches the limiter. An automatic gain control in software, if needed,
would go in the same place.

### WDR

**Decision (2026-10-28): Protect's WDR setting drives the ISP's Iridix, and
level 1, Protect's default, turns it on at a quarter of its reset strength.**
`t20-isp` has a "Dynamic Range Compression" control, Iridix's strength, 0
(the default) bypassing it; uvcd sets it from Protect's `wdr` (0 off, 1 to 3
strength 32, 64, 128) and reports `hasWdr`, so that Protect can offer the
setting. Iridix lifts the shadows and auto exposure, aiming at the same
mean, takes the exposure down, which keeps highlights (a window, a
screen) from clipping; the cost is a flatter, hazier picture, already
plain at 128 ([`t20-isp-hardware.md`](t20-isp-hardware.md)). Protect sends
`wdr: 1` to this camera even without `hasWdr`; at 32 the change is small
(clipped pixels 7.8% to 6.5% in the test scene). If Protect keeps its
cached feature flags until the camera is adopted again, the slider is not
shown and level 1 stays.

### Snapshot buffers

**Decision (2026-10-28): the JPEG encoder's capture buffer is cached, with the
driver keeping it coherent, rather than a bounce buffer.** A snapshot took
86-116 ms: about 14 ms in the VPU, up to a frame's wait for the next ISP
frame, and 40-57 ms for uvcd to copy the 250-450 KB JPEG out of the
encoder's buffer, which `vb2_dma_contig` maps uncached (reads run at about
7 MB/s). The H.264 node avoids this with a vmalloc capture buffer that the
driver fills from its own buffer, read through the cached KSEG0 alias; the
same for JPEG would cost another 2 MB (the capture buffer is sized for the
raw frame). Instead `t20-vpu` lets its dma-contig capture queue take
`V4L2_MEMORY_FLAG_NON_COHERENT` (`allow_cache_hints`), and uvcd asks for it.
The JPEG codec then writes back what the CPU writes into that buffer, the
header before the job and the EOI marker after it (with that line
invalidated first): the XBurst core is one of those that get a post-DMA
invalidate (`cpu_needs_post_dma_flush`), which would otherwise drop them. The
copy now takes 2-3 ms, a snapshot 40-60 ms in `test_capture_v4l2` (which asks
just after a frame, so it waits most of one; about 35 ms on average
otherwise). Without the flag the buffer stays uncached and correct, as
`v4l2jpeg` uses it.

### uvcd's libimp calls

uvcd calls 44 libimp functions (all in `src/capture.c`; `IMP_Log_Get_Option` is
uvcd's own, in `imp_compat.c`). The first plan was to implement them over the
drivers as below; `src/capture_v4l2.c` does the same work directly instead
(one thread per ISP output: ISP buffer as a dma-buf into its H.264 context,
the encoded frame to the source; JPEG from the main output on demand; raw
low-resolution frames copied on demand; auto exposure on the low-resolution
luma through the sensor's exposure and gain; flips restart the pipeline, as
they change the Bayer order):

| Calls | Over |
|---|---|
| `IMP_System_Init`, `Exit`, `Bind`, `UnBind`, `RebaseTimeStamp` | opening the media device and video nodes; binding is the fixed ISP → encoder path |
| `IMP_ISP_Open`, `Close`, `AddSensor`, `DelSensor`, `EnableSensor`, `DisableSensor`, `EnableTuning`, `DisableTuning` | the sensor subdevice and the ISP |
| `IMP_ISP_Tuning_SetISPRunningMode` | day/night: AE target and colour (with the IR cut filter, which uvcd drives itself) |
| `IMP_ISP_Tuning_SetISPHflip`, `SetISPVflip` | ISP mirror and sensor flip (to do in the drivers) |
| `IMP_ISP_Tuning_SetBrightness`, `SetContrast`, `SetSaturation`, `SetSharpness`, `SetTemperStrength`, `SetSceneMode`, `SetColorfxMode`, `SetWDRAttr` | ISP parameters (to do) |
| `IMP_FrameSource_CreateChn`, `DestroyChn`, `SetChnAttr`, `EnableChn`, `DisableChn`, `SetFrameDepth`, `GetFrame`, `ReleaseFrame` | the DS1 and DS2 video nodes |
| `IMP_Encoder_CreateGroup`, `DestroyGroup`, `CreateChn`, `DestroyChn`, `RegisterChn`, `UnRegisterChn`, `StartRecvPic`, `StopRecvPic`, `PollingStream`, `GetStream`, `ReleaseStream` | the encoder's V4L2 node, frames passed as dma-bufs from the ISP |

Done in that order, audio included (2026-10-28).

## Bring-up on hardware

First boots on 2026-10-27, one Wyze Cam v2, serial console on ttyS1 (115200
8N1) through a USB serial adapter.

**U-Boot over serial**. The vendor U-Boot 2013.07 (`isvp_t20#` prompt) waits 1 s
before autoboot. Enter pressed repeatedly during power-on stops it. Its
environment is compiled in ("bad CRC, using default environment"), so `bootcmd`
cannot be changed, but at the prompt it has what testing needs: `loady`
(YMODEM), `sf`, `crc32`, `mw`, `bootm`.

- Kernel from RAM, nothing written: `loady 0x80600000`, send the uImage,
  `bootm 0x80600000`. The root filesystem is still the one on flash.
- Flash from U-Boot: `loady` the image to 0x80600000, compare `crc32` with
  the file's, `sf probe`, `sf update 0x80600000 <offset> <length>`, then
  `sf read` it back elsewhere and compare `crc32` again. `sf update` here
  erases only whole 64 KiB blocks and fails on a partial last block ("len
  align as 10000", "sfc erase error"). Pad the image in RAM with 0xff to a
  multiple of 0x10000 first (`mw.b`). Offsets: kernel 0x40000, root 0x240000.
- YMODEM at 115200 moves about 11 KB/s: the kernel takes 3 minutes, the root
  8 minutes.

**Board GPIOs**. This U-Boot logs the board GPIOs it sets (`misc_init_r`). They
are this camera's, where the vendor `board.h` has another board's.

| GPIO | U-Boot's name | U-Boot leaves it |
|---|---|---|
| 62 (PB30) | `wifi_enable_gpio` | 0, then 1 (WiFi power) |
| 38, 39 | `yellow_gpio`, `blue_gpio` | LEDs |
| 81, 25, 49 | `night_gpio` | 0 |
| 47 | `USB_able_gpio` | 0 |
| 43 | `TF_able_gpio` | 1 |
| 63 | `SPK_able_gpio` | 0 |
| 48 | `SD_able_gpio` | 0 |
| 46 | (SD update button) | input |

**What the first boots found**, each fixed in this branch:

- The kernel hung before the console came up. The CGU registers clocks in
  index order, and `rtc_ercs` came before its parent `exclk_div512`. The
  binding indices are swapped (`T20_CLK_EXCLK_DIV512` 34, `T20_CLK_RTC` 40),
  so the OST and its clock event come up.
- No WiFi: the power line was PC16 from the vendor `board.h`. It is PB30, as
  U-Boot shows.
- Init died with SIGILL (exit code 4): OpenMiko's userspace is hard-float, so
  `CONFIG_MIPS_FP_SUPPORT`.
- `sleep` and udhcpc failed ("clock_gettime(MONOTONIC) failed"), and uvcd-run
  restarted uvcd in a tight loop: uClibc 1.0.12 uses the 32-bit `time_t`
  system calls, so `CONFIG_COMPAT_32BIT_TIME`.
- The config partition gave "compression type 0x07 not available": the vendor
  kernel writes it with zlib, rtime and lzo, so all three JFFS2 compressors.
- `/etc/dropbear` gave EIO: overlayfs needs xattrs on its tmpfs upper layer to
  make an opaque directory over the base image's symlink, so
  `CONFIG_TMPFS_XATTR`.
- No `/config` or `/sdcard` mount points on the read-only root, and an empty
  `lib/modules/3.10.14` made the init scripts take the 3.10 paths
  (`build-rootfs.py`, `build-modern.sh`, S20wifi, S50camera).
- No SSH keys: S10config installs `/config/overlay` before S40sshd replaced
  the base image's dangling `/etc/dropbear` link, and `/root` was read-only.
  S10config now replaces the link, and S00rootfs gives `/root` a RAM layer.

**The camera** (step 6, 2026-10-28):

- The sensor did not answer on I2C ("no sensor on I2C", reading the chip ID
  gave -EIO). Its MCLK pin (PA15) was in the ISP's pin group, set up only
  when the ISP driver probes, after the sensor. The sensor node now has the
  `cim-mclk` group itself.
- Streaming started but no frame came, and every VIC and ISP register read 0:
  the ISP clock gate (CLKGR0 bit 23) was closed. The kernel has no
  `CONFIG_PM`, so `pm_runtime_resume_and_get()` succeeds without calling the
  driver's resume, which enables the clocks. `t20-isp` now enables them in
  probe, like `jxf2x` powers the sensor.
- Then: JX-F22 found, 1920x1080 NV12 at 24.6 fps, the DS1 output-end
  interrupt (source 50 on line 13) once per frame. `v4l2grab`'s auto exposure
  settles at mean Y 105 indoors (exposure 1346 lines, the maximum at 25 fps,
  analog gain code 71). The picture is right side up, nearly colourless (a sepia cast: white balance gains 1.0, no
  saturation) and noisy, with no gamma, as expected.
- The TCU failed with -ENODEV ("Unable to start CPU timers: -19"), so the
  watchdog, a TCU child, was never probed. Its interrupt domain is
  `CONFIG_INGENIC_TCU_IRQ`, on by default for Ingenic but off in tinyconfig.
  With it, `/dev/watchdog` resets the camera when not fed (tested by closing
  it without the magic character).

**U-Boot's SD update on every boot**: with a `demo.bin` on the SD card, U-Boot
runs its update check at each boot, whether or not the setup button is held.
On this camera it logs "FWGRADEUP not find", "gradeup check fail", then
"addr align as 10000 / sfc erase error", and boots normally: nothing is
erased. Keep only a demo.bin meant for recovery on the card.

**Open**:

- An 8 GB SDHC card (SMI-S) enumerated as "high speed" and read only zeros,
  from sector 0 on, without errors. A 128 GB SDXC card reads correctly in
  the same camera. Small reads at default speed (SCR, CMD6) worked on both
  cards. The difference only shows on 512-byte block reads at 48 MHz, so
  test that card at 24 MHz before suspecting the PIO code.

## Hardware test plan

**Before anything**: an SD card with a known-good `demo.bin` (today's,
`image/build.sh`, OpenMiko's kernel) and the setup-button procedure tested on this
camera. A serial console (UART on ttyS1, 115200 8N1) is strongly recommended:
without it a kernel that does not reach WiFi is a black box. With it, a test
kernel boots from RAM through U-Boot ([Bring-up on hardware](#bring-up-on-hardware)).

**Rollback** (automatic, needs the SD card in the camera): `image/flash-modern.sh`
saves the image the camera runs (kernel partition and the root partition's
first 274 blocks, as a demo.bin) to `/sdcard/rollback/image.bin` before it
writes anything. Every boot of the new kernel is then a trial: unless `rollback
keep` runs on the camera within 10 minutes of boot, or by the third boot, it
writes the saved image back and reboots into it (`/usr/sbin/rollback`,
`S15rollback`). During a trial, `/sdcard/rollback/trial.txt` gets dmesg, the
syslog, `ip addr` and `lsmod` every minute, and `/sdcard/rollback/log` says what
happened, so a kernel that boots without WiFi leaves its log on the card.
`rollback status` shows the time left, `rollback now` rolls back at once. It
cannot help a kernel that hangs before userspace or cannot read the SD card:
that still needs the recovery below.

**Recovery**: power off, put the known-good `demo.bin` on the SD card, hold the
setup button, power on (README, "Install"). The bootloader rewrites the kernel
partition and the first 274 blocks of the root partition and the camera boots
OpenMiko's kernel again. The config partition is not touched by either path,
but see the JFFS2 risk below.

1. **Serial first boot** (`image/flash-modern.sh` from a running camera, which
   arms the rollback; `demo.bin` from `image/build-modern.sh` and the setup
   button do not). Expected on ttyS1: U-Boot
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
   (GPIO 49). Without anyone at the camera: stop uvcd and grab frames with
   `v4l2grab` at a fixed exposure, gain and white balance (`EXPOSURE`, `GAIN`,
   `CTRLS="whitebalanceautomatic=0 ..."`) with the filter in, out, and out
   with the LEDs on; the mean luma and colour show each one move.
4. **SD card**: insert, `dmesg` for `mmc0: new ... card`, `mount /dev/mmcblk0p1`,
   read and write a file.
5. **WiFi**: `modprobe 8189fs`, `ip link` shows wlan0 with the camera's MAC, S20wifi
   associates, DHCP, `ssh` in. Then the watchdog: uvcd arms it; `killall -STOP uvcd`
   must reboot the camera.
6. **Camera** (with SSH up, since a mistake here can hang the SoC): S50camera
   loads `t20-isp` and `jxf2x` at boot; `dmesg` shows `jxf22` or `jxf23`, and
   `/dev/video0` and two `/dev/v4l-subdev*` exist. `v4l2grab
   /sdcard/f.nv12 100` streams 100 frames: it should report about 25 fps and a
   mean Y that settles near 110 as its auto exposure works. Convert the saved
   frame with `ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i f.nv12 f.png`:
   expect a dark, flat, unbalanced picture (no gamma, white balance or noise
   reduction yet). Failures to report: no frame in 2 s (interrupt routing or
   the input port), DMA alarms, a green or striped picture (writer format or
   Bayer order), a hang (register access with a clock off).
7. **Microphone**: `audiorec /tmp/a.wav 10 16000 2` with uvcd stopped
   (`killall uvcd`; it restarts in 5 s): 10 s of samples must take about 10
   s (a fast or slow clock shows here; it was 8.5 s with the wrong PLL), and
   a 1 kHz tone played near the camera must come out at 1 kHz in the right
   channel. Then `test_audio` (uvcd's path: high-pass, gain, AAC) and
   `ffmpeg -i /tmp/audio.aac`: a quiet room near -46 dBFS, no DC. Finally
   listen in Protect: room sound audible, voices clear, not slowed.
8. **Back to OpenMiko's kernel** once (`image/flash.sh` writes only the root
   partition and leaves our kernel in place, so use demo.bin) to prove the way back.

**Risks on another camera** (the first one boots, so these are about board
differences):

- **No output at all**: a fault before the console. Earlycon is on; U-Boot's
  own messages show whether it accepted the image. Recovery: demo.bin, or
  U-Boot over serial.
- **Another flash chip**: one without a 32 KiB erase in its SFDP table falls
  back to 64 KiB, and JFFS2 may then treat the config partition's blocks as
  dirty and erase them: WiFi settings lost, no SSH. Keep
  `wpa_supplicant.conf` under `/sdcard/config/overlay/etc/` on the SD card;
  S10config imports it at boot. `cat /proc/mtd` must show erase size 00008000.
- **WiFi not coming up**, which loses SSH: another board revision may power the
  WiFi from another GPIO. Its U-Boot log names it (`wifi_enable_gpio`). With
  serial, `dmesg | grep mmc1` shows whether the card enumerates.
- **No camera in uvcd**: expected. uvcd starts, finds no `/proc/jz/sinfo/info`,
  logs it and keeps restarting every 5 s; WiFi, SSH and the LEDs work. It
  fails before it arms the watchdog. Our camera drivers are loaded at boot
  for `v4l2grab`.

## Building

```sh
uvc/kernel/build.sh          # third_party/kernel/ (sources), build/kernel/out/
uvc/image/build-modern.sh    # build/image-modern/{uImage,root.sqfs,demo.bin}
uvc/image/flash-modern.sh 192.168.1.9
```

Host: Linux x86_64 with make, gcc, flex, bison, bc, python3, xz-utils, git, curl,
kmod (depmod) and squashfs-tools (sqfstar). A clean kernel build takes about 15
minutes on 4 cores. Only `kernel/build.sh` needs Linux: it can run in an
`ubuntu:24.04` container with those packages on any x86_64 Docker host, with
`build/kernel/out/` copied back; `build-modern.sh` runs on macOS too (zig,
`brew install squashfs`).
