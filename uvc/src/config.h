#ifndef UVC_CONFIG_H
#define UVC_CONFIG_H

struct config {
    char nvr[64];       // Protect host
    int nvr_port;       // its camera port (wss)
    char token[128];    // adoption token (manage-payload, or sent by Protect's adopt request)
    char password_hash[128]; // crypt(3) hash of the password Protect set (UpdateUsernamePassword)
    char cert[128];     // client PEM (key + certificate)
    char name[64];      // camera name shown in Protect
    char iface[16];     // network interface for MAC/IP discovery
    char mac[32];       // normalized to 12 uppercase hex digits
    char ip[16];
    char device_id[40]; // stable UUID from the MAC
    // Identity of the UniFi model we present as (see PROTOCOL notes).
    char model[32];
    char sysid[16];
    char platform[32];
    char fw_version[32];
    char fw_build[64];
    char fw_updated[32]; // version taken from Protect's firmware update (state file), or ""
    char hi_source[128]; // H.264 for video1
    char lo_source[128]; // H.264 for video2/video3 (may equal hi_source)
    char snapshot[128];  // JPEG device or file
    char state_file[128];
    char log_file[128]; // empty: stderr
    // CAPTURE=1 (default): uvcd drives the sensor and encoders, through
    // our kernel's V4L2 drivers or libimp (capture.h). CAPTURE=0 reads H.264
    // from HI_SOURCE/LO_SOURCE (files or v4l2 devices), for tests off the
    // camera.
    int capture;
    int hi_kbps, lo_kbps, hi_gop, lo_gop;
    int lo_width, lo_height;
    int flip_v, flip_h;
    // Day/night switching points in the ISP's exposure figure (daynight.h)
    int night_exposure, day_exposure;
    char detect_model[128]; // object detector, path without .param/.bin
    int protect;         // Protect is configured (else only capture runs)
    int fps;
    int audio;
    int allow_reboot;
    int adopted;
    int watchdog; // hardware watchdog timeout, s (0: off)
};

int config_load(struct config *c, const char *path);
// The state file records adoption (ADOPTED) and the Protect host and token
// it came with; it overrides uvcd.conf.
int config_save_state(const struct config *c);
// Protect's adopt request (manage.h): a new host and token, not adopted yet.
void config_manage(struct config *c, const char *host, int port, const char *token);
// Protect removed the camera: forget it and wait to be adopted again.
void config_reset(struct config *c);
// Retry MAC/IP discovery (at boot wlan0 may not have an address before the
// camera starts). Returns c->protect.
int config_refresh(struct config *c);

#endif
