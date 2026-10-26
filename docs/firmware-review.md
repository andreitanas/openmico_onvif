# OpenMiko and uvcd integration review (2026-10-25)

This review was done read-only on the camera (192.168.1.9, OpenMiko on Buildroot 2016.02,
kernel 3.10.14, 92 MB RAM visible to Linux) and against the OpenMiko sources (commit
00f72a6). Nothing was changed while reviewing.

Compatibility with OpenMiko is not a goal: anything below can be replaced with our own.

## How the camera boots

1. The kernel's built-in initramfs (`initramfs_skeleton/init`) mounts the SD card,
   mounts the `root` flash partition (mtd2, JFFS2), and unpacks `rootfs.tar.xz` from it
   into a 48 MB **zram** ext2 disk. It then switch_roots into that disk. The SD card stays
   mounted from the initramfs, which is why `/sdcard` shows up twice in `/proc/mounts`.
2. `/etc/inittab` runs `general_init.sh`, which:
   - runs fsck, mounts the card, and mounts the `config` flash partition (mtd3, 256 KB
     JFFS2) on `/config`;
   - copies every file under `/sdcard/config/` into `/config/` (flash) on **every boot**;
   - copies `/config/overlay/` over the RAM root.
3. `rcS` runs `/etc/init.d/S??*` in order. Ours come from the overlay on the card:
   `S60camera` (modified), `S90uvcd` and `S99probe`.

What runs today:

| Process | Notes |
|---|---|
| uvcd | about 17.8 MB RSS, 23 threads. Uses about 83 % CPU, most of it the detector at nice 19 on otherwise idle time. |
| wpa_supplicant | runs in `-d` debug mode, logging to a file (see bug 2) |
| udhcpc, ntpd (busybox), dropbear, syslogd/klogd, crond | standard services |
| micropython `/app/app.py` | OpenMiko's API on port 8081 |
| autonight | polls `/proc/jz/isp/isp_info` and calls `nightmode.sh` |
| reset_button.sh | a bash loop that polls a GPIO every second, forever |

Listening ports: 22 (dropbear) and 8081 (micropython). lighttpd and mjpg_streamer are
configured but not installed or not enabled.

## Problems found (most important first)

### 1. The root filesystem takes a quarter of the RAM
The zram root holds 34.6 MB of files, compressed to 21.5 MB. With overhead that is
**23.2 MB of RAM**, against about 40 MB free/cached for everything else.

Most of it is software we no longer use:

| Item | Size |
|---|---|
| ffmpeg (`libav*`, about 3.5 MB) | 3.5 MB |
| live555 and v4l2rtspserver | 1.1 MB+ |
| videocapture and libstdc++ | 2 MB |
| micropython and its libraries | 1.3 MB |
| sqlite | 1.2 MB |
| curl | 0.4 MB |
| freetype, libjpeg, libasound | 1.9 MB |
| lighttpd | 0.5 MB |
| kernel modules for other hardware | part of the 4.9 MB in /lib/modules |

uvcd is static and needs none of these. What we actually need: busybox, dropbear,
wpa_supplicant (libnl, libssl, libcrypto), and the 8189fs, tx-isp, sinfo, sensor and
v4l2 modules.

The kernel has no squashfs or overlayfs (`/proc/filesystems`), so the root cannot simply
be mounted from flash without a new kernel.

Two ways to get the RAM back:
- **Runtime, no flashing:** delete the unused files from the RAM root early in boot,
  then zero-fill the free space. zram stores all-zero pages for free
  (`/sys/block/zram0/zero_pages` exists), so the memory is returned. Expected gain is
  roughly 10 MB.
- **Flash a smaller `rootfs.tar.xz`** into mtd2. The initramfs and kernel stay as they
  are. This can be done from the running camera (mount mtd2 as JFFS2 and replace the
  tarball). Recovery from a bad tarball is `demo.bin` plus holding the setup button, which
  needs physical access.

### 2. Logs fill RAM until the camera runs out of memory
- `S35wifi` starts wpa_supplicant with `-d -f /var/log/wpa_supplicant.log` whenever
  `ENABLE_LOGGING=1`, which our card sets. `/var/log` points to `/tmp`, which is RAM.
  The file grew to 80 KB in 49 minutes, about 2.4 MB a day. autonight.log grows about
  0.3 MB a day.
- Nothing ever rotates them. The crontab line `. openmiko.conf && … logrotate` uses a
  relative path, so `.` searches `$PATH`, does not find the file, and fails every 15
  minutes (checked on the camera). Even if it ran, rotating wpa_supplicant's open file
  would not free anything.
- Result: after one to two weeks of uptime, these logs alone can push the camera into
  OOM.
- Related: `ENABLE_LOGGING=1` is meant to send logs to the card. In practice it doesn't,
  because `general_init.sh` decides where logs go before it reads the card's
  `openmiko.conf`.

### 3. uvcd starts before the clock is set
There is no RTC, so the camera boots in 1970.
- `S45ntpd` starts busybox ntpd, which syncs with time.google.com in the background.
- `S60camera` starts uvcd immediately afterwards.
- In the syslog, the clock jumped from `Jan 1 00:00:37` to `Oct 25 10:19:42` while
  S60camera was running, which is exactly when uvcd starts capturing and connects to
  Protect.
- uvcd sends `clockWall`/`wallMs` taken from this clock.

This is a likely cause of the "Protect refuses streams for about 75 s after a reboot"
issue, and possibly of the scrub-preview timelapse stalls. Not yet confirmed: the uvcd log
from that boot was rotated away.

The console does not serve NTP (nothing on UDP 123), and Protect's hello carries no time.
The fix is to wait for a sane clock before connecting.

### 4. The SD card is never checked, and logging goes to FAT
- OpenMiko has no `fsck.vfat`. `general_init.sh` tries every filesystem type, logs
  "fsck failed", and mounts anyway.
- After an unclean power-off the card mounts with errors and switches to read-only on the
  first problem (`errors=remount-ro`). This already happened once.
- The card is mounted twice and never unmounted (see boot step 1), so the FAT dirty bit
  stays set.
- uvcd writes its log to FAT continuously: about 1 MB per few hours at LOG_LEVEL=3.
  S90uvcd only rotates the log when uvcd restarts, so a long-running uvcd grows its log
  without limit.

### 5. Protect's camera settings are acknowledged but ignored
uvcd answers `ChangeIspSettings` with fixed values. Protect sends real values for:
- `brightness`, `contrast`, `saturation`, `hue`, `sharpness`, `denoise`;
- `flip`, `mirror`;
- `irLedMode` (auto/on/off), `irLedLevel`;
- `aggressiveAntiFlicker`.

None of them reach the image. Flip and mirror come from `uvcd.conf` instead.

Also:
- Night mode is decided by autonight (a separate polling process). uvcd only follows it
  through `/tmp/night_vision_enabled`, so Protect's IR setting does nothing.
- Status LED settings (`ChangeSoundLedSettings`) are ignored. S85leds sets the yellow
  LED to `timer`, so it blinks permanently, and the blue LED flickers with SD card
  activity.
- OSD (date/name overlay) settings are ignored. The overlay was dropped earlier.
- Audio is a silent AAC track. The Wyze v2 has a microphone; Protect prefers Opus.

### 6. Nothing recovers a hung camera
- `/dev/watchdog` (the T20 hardware watchdog) exists but nothing opens it.
- If WiFi or the driver wedges, or uvcd deadlocks inside libimp, the camera stays down
  until someone power-cycles it.
- S90uvcd restarts uvcd if it exits, but not if it hangs.

### 7. Leftovers and unnecessary services
- **micropython API on 0.0.0.0:8081, no authentication:**
  - anyone on the LAN can toggle the IR LED and IR-cut filter;
  - `/api/cameraimage` runs `getimage`, which needs `/dev/video4`. That device no
    longer exists, so the endpoint is broken;
  - it also blocks its event loop with `time.sleep`.
- `reset_button.sh`:
  - a bash loop forking `sleep` every second;
  - its "factory reset" deletes `/config/overlay`, which `general_init.sh` copies back
    from the card on the next boot, so the reset does nothing while a card is in.
- `S99probe` runs a debug probe 60 s after every boot and writes it to the card.
- The `S70mjpg_streamer`, `S50lighttpd` and `S65api` scripts and `videocapture_settings.json`
  are dead weight. The CAPTURE=0 path in uvcd exists only for OpenMiko's videocapture.
- `/sdcard/record/`, `/sdcard/time_lapse/` and `/sdcard/.ota/` (about 5 MB) and
  `/config/.user_config`, `.time_checkpoint` etc. were written by the **stock Wyze
  firmware** at 07:50 UTC today, just before OpenMiko was flashed. The 03:50 local
  recording was cut short by the power-off for flashing, which is the FAT damage
  `/config/fsck.log` shows. This solves the "unknown /record writer" question: nothing
  on the current firmware writes there.
- `/sdcard/bench/` (8 MB detector benchmark) and `/sdcard/var/log/uvcd-cap.log` are left
  over from development.

### 8. Smaller items
- `general_init.sh` rewrites every overlay file into the 256 KB config flash on every
  boot (flash wear). It never deletes a file that was removed from the card, so stale
  init scripts survive in flash.
- dropbear allows password logins (no `-s`). We only use keys.
- The swapfile is 64 MB on FAT, swappiness 60, and unused (0 kB). A 1080p encoder cannot
  wait on SD-card swap anyway.
- The camera's TZ is `EST5EDT`. Only syslog uses it; uvcd and Protect use UTC.
- The repo `.gitignore`s `utils/`, so a fresh clone lacks `openmiko-gen.sh` and
  `wpa-gen.sh`, which `deploy.sh` needs.
- A uvcd crash (as opposed to SIGTERM) leaves libimp half-configured. Whether the
  restarted uvcd can then initialise the ISP has not been tested.

## What already works well
- uvcd is one static binary with no runtime dependencies on OpenMiko libraries. It owns
  libimp, handles SIGTERM cleanly (WebSocket close, then libimp teardown), and S90uvcd
  restarts it.
- The adoption state, certificate and config live on the card, so no flash writes.
- Night mode ISP switching (B/W, night scene) follows autonight correctly.

## Proposed improvements

Ordered by value and risk. Steps 1–6 only change files on the SD card and uvcd itself;
everything is reversible by editing the card. Step 7 writes the flash and needs a
decision.

1. **Stop the RAM leaks.** Our own S35wifi (wpa_supplicant without `-d`/`-f`, messages
   to syslog), no autonight log, drop the broken cron line, and keep `ENABLE_LOGGING=0`.
2. **Clock before Protect.** uvcd waits until the wall clock is sane (year ≥ 2024) before
   its first connection, and S45ntpd does a quick one-shot sync at boot. Then measure the
   post-reboot stream refusal again.
3. **uvcd log.** Rotate inside uvcd by size, default LOG_LEVEL=2, and turn off the
   current debug level.
4. **Hardware watchdog in uvcd.** Feed it while capture frames keep flowing; on clean
   shutdown, disarm it with the magic close. Optionally reboot after a long period (e.g.
   15 min) without a Protect connection, to recover from wedged WiFi.
5. **Apply Protect's settings.**
   - Image settings via `IMP_ISP_Tuning_*`.
   - Flip and mirror.
   - `irLedMode` auto/on/off: uvcd takes over day/night itself, reading exposure from
     libimp, and autonight goes away.
   - Status LED: blue solid while connected, off when Protect turns the LED off.
6. **Slim the system.**
   - Remove the micropython API, mjpg/lighttpd, S99probe and reset_button.sh. The
     setup-button handling could move into uvcd.
   - Remove the CAPTURE=0/videocapture and v4l2 mirror code from uvcd and S60camera.
   - Replace S60camera with a short `S60uvcd` (load modules, start uvcd).
   - Delete unused files from the RAM root early in boot and zero-fill, to win back about
     10 MB.
7. **Our own root image (needs your go-ahead: it writes flash).** A minimal
   `rootfs.tar.xz` (busybox, dropbear, wpa_supplicant, modules, uvcd and its model)
   flashed into mtd2, keeping the OpenMiko kernel and initramfs. Benefits:
   - the RAM win becomes permanent;
   - uvcd and config live on the camera, with the SD card optional;
   - boot no longer depends on FAT.

   A later step could rebuild the 3.10.14 kernel with squashfs, so the root is mounted
   from flash and the zram root goes away entirely (about 23 MB).
8. **Later features:** real microphone audio (libimp AI and an Opus encoder), and an OSD
   timestamp.

## Status (2026-10-25, later)

Done; the camera runs it:
- **Own image** (proposals 1, 3, 6 and 7). `uvc/image/` builds a root
  partition from OpenMiko's root filesystem, filtered to busybox, dropbear,
  wpa_supplicant and the drivers, with our init scripts.
  - uvcd and the detector are read from flash (`/rom`) and settings are in
    `/config`, so the SD card is optional.
  - Logs are size-capped in RAM. wpa_supplicant runs without its debug log,
    and there is no cron, micropython, lighttpd or bash.
  - The zram root dropped from 23 MB of RAM to 7.5 MB. With uvcd running,
    58 MB is available (was 40 MB).
  - Boot to streaming takes about 36 s.
  - `image/flash.sh` installs over ssh with verification and automatic
    restore; `build/image/demo.bin` is for setup-button installs.
- **Clock** (proposal 2): uvcd waits for NTP before connecting. Without
  NTP (no internet access), it takes the time from the console's web server
  after 30 s. After reboots, Protect now records the low-res stream within a
  minute; it used to be refused for about 75 s.
- **Watchdog** (proposal 4): armed at 60 s and fed while video flows and
  Protect was reachable within 30 min. Tested:
  - a frozen uvcd reboots the camera in under a minute;
  - a killed uvcd is restarted by init and re-arms it;
  - a clean stop disarms it.
- **Protect settings** (proposal 5): ISP settings are applied, and the status
  LEDs are blue when connected and yellow when not.
- **Day/night** (`uvc/src/daynight.c`) replaces autonight and nightmode.sh.
  - Protect sends cameras `irLedMode` "auto" or "manual", `irLedLevel` (0:
    no IR LEDs) and `icrSwitchMode`/`icrCustomValue` for its custom modes;
    the UI's names (on, off, autoFilterOnly, ...) never reach the camera, so
    the old nightmode.sh only ever ran "auto". All six modes now work.
  - autonight's switch back to day needed an iridix (tone mapping) strength
    below 14, which this camera never reports in night mode (~30-50), so it
    stayed in night mode after any dark spell. With the IR LEDs on, exposure
    can't decide either: a hand over the lens, lit by the LEDs, read ~250k,
    brighter than the uncovered room (~445k). With the LEDs off it read
    0.9-1.05M within 2 s.
  - So: night when day-mode exposure stays above 1.2M for 3 s; at night,
    turn the LEDs off for 3.5 s and go back to day if exposure stays below
    800k. Probes start 10 s after the switch and back off (30 s doubling to
    10 min), and come sooner when the exposure moves. Tested with a hand
    over the lens.
- **Adoption from the Protect UI** (`uvc/src/discovery.c`, `manage.c`), no
  token to copy:
  - uvcd answers Protect's discovery probes (UDP 10001, every minute) with
    the fields a real G4 Instant sends, flagged unmanaged until adopted, and
    Protect lists it as ready to adopt.
  - Clicking Adopt makes Protect POST `https://<camera>/api/1.2/manage`
    with its hosts and a token; uvcd keeps them in `uvcd.state` and
    connects. Tested: adopted 1.4 s after the click.
  - Removing the camera in Protect sends `ResetToDefaults`; uvcd forgets
    that Protect and is adoptable again. Once adopted, uvcd only takes a new
    Protect with the password Protect set over `UpdateUsernamePassword` (a
    SHA-512 crypt hash), as Protect does when it re-adopts a camera that
    stays disconnected.
- **Firmware updates** from Protect are accepted without installing
  anything: `UpdateFirmwareRequest` names the version in its download URI
  (`/internal/update/<platform>/<product>/<type>/<version>/<mac>`); uvcd
  keeps it (`UPDATED_FW_VERSION` in `uvcd.state`), goes offline for 45 s as
  if flashing, and reconnects reporting it. Protect then records the update
  as done, so `FW_VERSION` needs no upkeep. Tested from 5.5.30 to 5.5.31.
- **Credentials from Protect** (`UpdateUsernamePassword`, at every
  connection) are not the console's device SSH password: Protect makes a
  random 20-character password per camera (`getUpdatedPassword`) and sends
  its SHA-512 crypt hash. uvcd keeps the hash only to check Protect's
  re-adopt requests, which carry the plain password. `StopService` for ssh
  (Device SSH off in the console) is ignored.
- **dropbear 2026.94** (`tools/build-dropbear.sh`, static musl) replaces
  OpenMiko's v2015.71: plain `ssh` works without `+ssh-rsa`, and root's
  PATH includes `/sbin`. The host key in `/config` is kept.
- **Leftovers**: the SD card was cleaned (stock Wyze recordings, OTA files,
  bench files, swapfile). The old OpenMiko init scripts were removed from
  the config flash.

Found on the way:
- Scrub-preview "stalls": Protect's recorder writes timelapse frames in
  batches (about 4 min for the main track, 15 min for the low-res one) and
  drops the batch in progress whenever a camera reconnects. Frequent
  reconnects while developing explained the gaps; with a stable connection
  they fill in.
- The libimp compatibility shim overrode `mmap` for the whole binary.
  musl's `sem_open` (64-bit offset) then mapped its semaphore at a garbage
  offset and crashed with SIGBUS; it had only worked by luck. libimp's own
  references are now renamed to the shim.

Not done:
- The setup button does nothing; it is no longer polled.
- dropbear still allows password logins.
- Real microphone audio.
- OSD.
- Rebuilding the kernel with squashfs, to drop the zram root entirely.
- `demo.bin` installs are untested on hardware. The format reproduces
  OpenMiko's file exactly, but nobody has pressed the button with ours yet.
