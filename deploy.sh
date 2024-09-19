#!/bin/sh

# Copy to SD card:
cat app/onvif.py | ssh root@openmiko "cat > /config/overlay/app/onvif.py"
cat app/onvif_discover.py | ssh root@openmiko "cat > /config/overlay/app/onvif_discover.py"
cat etc/init.d/S66onvif | ssh root@openmiko "cat > /config/overlay/etc/init.d/S66onvif"

# Copy to live system:
ssh root@openmiko "cp /config/overlay/app/onvif.py /app/onvif.py"
ssh root@openmiko "cp /config/overlay/app/onvif_discover.py /app/onvif_discover.py"
ssh root@openmiko "cp /config/overlay/etc/init.d/S66onvif /etc/init.d/S66onvif"
