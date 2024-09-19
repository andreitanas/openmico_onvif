## Basic toy ONVIF service for OpenMiko

This project is a toy ONVIF service for OpenMiko opensource firmware for Wyze cameras using Ingenic T20 SoC.

## Prerequisites
Use OpenMiko base image: https://github.com/openmiko/openmiko/releases and follow [the instructions](https://github.com/openmiko/openmiko) to flash the firmware, then follow the instructions below to add ONVIF service.

Files need to be copied to /overlay folder on the SD card. When the camera reboots, everything in the /overlay folder will be copied to the root of file system. When mounted, the card will be accessible at /config, and it will be possible to update the overlay files remotely.

File structure:
```
/overlay
  /app
    onvif.py
    onvif_discover.py
  /etc
    wpa_supplicant.conf --> make sure to update wifi configuration
    openmiko.conf       --> refer to openmiko documentation
    passwd              --> update password (use `passwd` then copy from /etc/passwd)
    /init.d
      S66onvif          --> script to start onvif services
```

## Deploy
Run deploy.sh to copy files over ssh to the SD card as well as the live file system on the camera. Make sure to update the wifi configuration in `wpa_supplicant.conf` and adjust settings as necessary in `openmiko.conf`.

## Usage
After deploying the files, reboot the camera. The ONVIF service (`onvif.py`) will be available on port 8000, and the discovery service (`onvif_discover.py`) will make it discoverable by ONVIF-compatible clients. This was tested with [Onvifer](https://play.google.com/store/apps/details?id=net.biyee.onvifer) Android app and Ubiquiti Unifi Protect NVR.

## Notes
* The ONVIF service is not secure and should not be exposed to the Internet.
  * **Authentication is not implemented**, the camera will accept any username/password.
* The ONVIF service is very basic, only supports a subset of functions, request parsing is extremely naive and most of the responses are hardcoded, it is **not** compliant with the ONVIF standard, but it is enough to make the camera discoverable and stream video over RTSP.
