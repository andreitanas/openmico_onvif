# Kernel and Ingenic SDK: options (2026-10-25)

Notes from a discussion, nothing decided or changed. The image currently keeps
OpenMiko's kernel and Ingenic's closed camera stack; this is what replacing them
would involve.

## What the image uses today

- Kernel: OpenMiko's prebuilt Linux 3.10.14 (built 2023-03-24, gcc 4.7.4, Buildroot
  2016.02), the first 2 MiB of `deploy/demo.bin`. `image/demo-bin.py` copies it
  unchanged, and `image/flash.sh` only writes the root partition, so the camera's
  kernel partition still holds OpenMiko's.
- The kernel's built-in initramfs is OpenMiko's too: it unpacks `rootfs.tar.xz` into
  RAM and zeroes the root partition from block 274 up on every boot.
- Kernel modules: OpenMiko's prebuilt `.ko` files, copied by `image/build-rootfs.py`.
- libimp and libalog: Ingenic's closed binaries, from OpenMiko's `ingenic_videocap`
  (`third_party/ingenic`, fetched by `fetch-deps.sh`).

## A newer kernel

Not with a working camera.

- Mainline Linux supports some older Ingenic SoCs (JZ47xx, X1000, X2000) but not the
  T-series camera chips, and has no driver for their ISP.
- libimp talks to the ISP driver (`tx-isp.ko`) through a private interface that only
  exists for Ingenic's 3.10.14 tree. Thingino, the active open firmware for these
  cameras, uses 3.10.14 for T10/T20/T21/T23/T30/T31 and 4.4.94 only for T40/T41.

What is realistic is building Ingenic's 3.10.14 ourselves (from Thingino's tree, with a
newer toolchain) together with its modules. That would give us:

- no OpenMiko initramfs: a normal root filesystem on flash, without the block-274 rule
  or the RAM copy;
- a kernel configured for what we use, with more free RAM;
- a path to the open ISP driver and libimp (below).

Costs: it changes the kernel partition, so it needs a new install path (`flash.sh`
writes only the root partition) or the SD-card `demo.bin`, and every board driver
(sensor, WiFi, GPIOs, SD card) has to be tested again.

Not worth it only for a newer version number, since it stays 3.10. Worth it if the
initramfs gets in the way or if we want to drop the closed drivers.

## Rebuilding the Ingenic parts

| Piece | Today | Can we build it? |
|---|---|---|
| Kernel 3.10.14 | OpenMiko's binary | Yes: Ingenic released the source; Thingino maintains a tree. |
| `8189fs.ko` (WiFi) | OpenMiko's binary | Yes: Realtek's out-of-tree driver is open. |
| `sensor_jxf22.ko`, `sinfo.ko` | OpenMiko's binaries | Probably: mostly I²C register tables; Thingino builds sensor drivers from source (not checked). |
| `audio.ko` | OpenMiko's binary | Probably, as above. |
| `tx-isp.ko` (ISP driver) | Closed Ingenic binary | Only via open-tx-isp, a rewrite of the same interface. |
| `libimp.a` (linked into uvcd) | Closed Ingenic binary | Only via openimp, a rewrite of the same API. |
| `libalog` | Closed Ingenic binary | openimp replaces it. |
| Sensor tuning file | Ingenic calibration data | No, it's data; kept if the open stack reads the same format. |

Constraints:

- Prebuilt modules only load into the exact kernel they were built for. A kernel of our
  own with a different configuration probably won't load OpenMiko's `tx-isp.ko`, so it
  likely means building the ISP driver too (Thingino's SDK or open-tx-isp).
- libimp and `tx-isp.ko` must match each other. Today both come from OpenMiko. A closed
  libimp with open-tx-isp works only as far as open-tx-isp matches the vendor interface.

Risks:

- Picture quality: auto exposure, white balance and noise reduction live in the closed
  libimp and tx-isp. The open versions are new (RC1 tagged 2026-10-11). Their wikis list
  the T20 as release quality and say it streams H.264; nothing on colour or low light.
  Self-reported, not tried.
- uvcd's use of libimp (ISP tuning, encoder, frame sources, the mmap shim in
  `tools/imp-rename.py`) has to be checked call by call against openimp.
- Recovery: a kernel that doesn't boot means the SD-card `demo.bin`. Booting a test
  kernel from RAM needs U-Boot over serial, which isn't set up.

## Possible order

1. Thingino's T20 kernel with the closed drivers built for it, running our current root
   filesystem. Drops OpenMiko's initramfs; the camera stack stays closed.
2. open-tx-isp and openimp, compared side by side with the closed stack (daylight,
   night, motion).
3. Keep the closed stack as a build option until the open one looks at least as good.

Step 1 has the clearest benefit. Step 2 is a research project, worth it only if being
fully open matters.

## Sources

- Thingino Tech Info: https://github-wiki-see.page/m/themactep/thingino-firmware/wiki/Tech-Info
- open-tx-isp: https://github.com/Lu-Fi/open-tx-isp/wiki
- openimp: https://github.com/Lu-Fi/openimp/wiki
- MIPS: Convert Ingenic to a generic board (LKML, 2020):
  https://lkml.iu.edu/hypermail/linux/kernel/2008.0/01686.html
