#ifndef UVC_CONFIG_H
#define UVC_CONFIG_H

struct config {
    char nvr[64];       // Protect host
    char token[128];    // adoption token (manage-payload), only needed until adopted
    char cert[128];     // client PEM (key + certificate)
    char name[64];      // camera name shown in Protect
    char iface[16];     // network interface for MAC/IP discovery
    char mac[32];       // normalized to 12 uppercase hex digits
    char ip[16];
    // Identity of the UniFi model we present as (see PROTOCOL notes).
    char model[32];
    char sysid[16];
    char platform[32];
    char fw_version[32];
    char fw_build[64];
    char hi_source[128]; // H.264 for video1
    char lo_source[128]; // H.264 for video2/video3 (may equal hi_source)
    char snapshot[128];  // JPEG device or file
    char state_file[128];
    int fps;
    int audio;
    int allow_reboot;
    int adopted;
};

int config_load(struct config *c, const char *path);
int config_save_adopted(const struct config *c);

#endif
