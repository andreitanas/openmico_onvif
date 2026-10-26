## Wyze Cam v2 as a native UniFi Protect camera

`uvc/` builds a firmware image for Ingenic T20 Wyze Cam v2 cameras that
makes them adopt into UniFi Protect as UniFi cameras: video, snapshots,
motion and smart detections (person, vehicle, animal), with no RTSP, ONVIF
or bridge box.

- `uvcd`, a static C daemon, drives the camera through Ingenic's libimp and
  speaks Protect's own camera protocol. It runs the object detector (ncnn,
  Yolo-Fastest) on the camera.
- The image keeps [OpenMiko](https://github.com/openmiko/openmiko)'s kernel
  and replaces the rest: a minimal root filesystem (busybox, dropbear,
  wpa_supplicant, the camera and WiFi drivers) plus uvcd, in the root flash
  partition. Settings live in the config flash partition, so the SD card is
  optional.

## Build
- `cd uvc && ./fetch-deps.sh` once, then `image/build.sh`. It produces:
  - `build/image/root.jffs2`, the root partition;
  - `build/image/demo.bin`, the same image in the bootloader's format, with
    OpenMiko's kernel.
- The base root filesystem comes from OpenMiko's `demo.bin`, expected at
  `deploy/demo.bin` (from https://github.com/openmiko/openmiko/releases).
- `./deploy.sh utils` and `./deploy.sh passwd` generate the camera's
  settings: wifi, hostname, root password and ssh key, under
  `deploy/config/overlay`.

## Install
- **New camera:**
  1. Run `uvc/make-sdcard.sh /Volumes/CARD`.
  2. Hold the setup button while powering the camera on. The bootloader
     flashes `demo.bin`.
  3. The camera imports `config/` from the card into its flash at every boot
     when the card's copy differs, then shows up in Protect as ready to
     adopt (it answers Protect's discovery). Click Adopt there.
  - Protect's discovery only reaches its own subnet. Elsewhere set `NVR_HOST`
    and `ADOPTION_TOKEN` in `config/uvc/uvcd.conf` instead (see the comments
    there).
- **Camera already running this image or OpenMiko:** `uvc/image/flash.sh
  [camera-ip]` writes `root.jffs2` over ssh. It verifies the result and puts
  the previous image back if anything doesn't match.
  - Coming from OpenMiko, first put `uvcd.conf`, `client.pem` (and
    `uvcd.state` if already adopted) in `/config/uvc/`.
- **Removing a camera** in Protect resets it: it forgets that Protect and
  shows up as ready to adopt again.
  - Remove OpenMiko's init scripts and `openmiko.conf` from
    `/config/overlay/etc/` and from the card's `config/overlay/etc/`.

## Notes
- On the camera:
  - uvcd logs to `/tmp/uvcd.log`; the system log is `/var/log/messages`.
    Both are size-capped in RAM.
  - `/sdcard/uvc/uvcd`, if present, runs instead of the flashed uvcd (for
    development).
- OpenMiko's initramfs zeroes the root partition from block 274 up on every
  boot and deletes its contents if `rootfs.tar.xz` fails to unpack. Never
  write that partition except through `image/flash.sh` or `demo.bin`.
- `docs/firmware-review.md` has the review of OpenMiko that led to this
  image.
