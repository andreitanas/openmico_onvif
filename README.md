## Wyze Cam v2 as a native UniFi Protect camera

`uvc/` holds uvcd, a small static C daemon for [OpenMiko](https://github.com/openmiko/openmiko)
firmware on Ingenic T20 Wyze cameras. It drives the camera through Ingenic's
libimp (replacing OpenMiko's videocapture) and speaks UniFi Protect's own
camera protocol, so the camera is adopted into Protect as a UniFi camera:
video, snapshots and settings without RTSP, ONVIF or a bridge box.

## Prerequisites
Flash the OpenMiko base image (https://github.com/openmiko/openmiko/releases)
following [its instructions](https://github.com/openmiko/openmiko).

## Build and deploy
- `./deploy.sh utils` and `./deploy.sh passwd` generate the camera's OpenMiko
  overlay (`deploy/config/overlay`: wifi, `openmiko.conf`, root password, ssh
  key). The camera copies everything under `config/` on the SD card to its
  flash and root filesystem on every boot.
- `cd uvc && ./fetch-deps.sh && make camera` builds `build/uvcd-mipsel`.
- `uvc/make-sdcard.sh /Volumes/CARD [protect-host]` writes a card: the overlay,
  uvcd, its config and a per-camera client certificate.
- Set `ADOPTION_TOKEN` in `uvc/uvcd.conf` on the card (see the comments there),
  boot the camera, and adopt it in Protect.

## Notes
- The RTSP server is off (`ENABLE_RTSP=0` in `openmiko.conf`); its v4l2loopback
  buffers alone take tens of MB of the camera's 92 MB. To serve RTSP again, set
  `ENABLE_RTSP=1` and the `*_MIRROR` devices in `uvcd.conf`. Protect's own
  RTSPS re-share (camera settings → Advanced) is an alternative, not yet tried
  with these cameras.
- uvcd logs to `/sdcard/var/log/uvcd.log` on the camera.
