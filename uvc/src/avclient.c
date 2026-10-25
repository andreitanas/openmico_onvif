#include "avclient.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "analytics.h"
#include "cJSON.h"
#include "capture.h"
#include "https.h"
#include "mbedtls/sha1.h"
#include "stream.h"
#include "util.h"
#include "ws.h"

static struct config *cfg;
static struct source *hi_src, *lo_src;
static struct ws ws;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static int connected; // guarded by send_lock
static int msg_id;    // guarded by send_lock
static int64_t start_ms;
static char device_id[40], guid[40];

static struct stream streams[3] = {
    {.channel = "video1"},
    {.channel = "video2"},
    {.channel = "video3"},
};

// --- JSON helpers -------------------------------------------------------

#define NUM(o, k, v) cJSON_AddNumberToObject(o, k, v)
#define STR(o, k, v) cJSON_AddStringToObject(o, k, v)
#define BOOL(o, k, v) cJSON_AddBoolToObject(o, k, v)
#define OBJ(o, k) cJSON_AddObjectToObject(o, k)

static cJSON *str_array(const char *const *items, int n) {
    return cJSON_CreateStringArray(items, n);
}

static void timestamp(char *out, size_t len) {
    int64_t now = wall_ms();
    time_t sec = now / 1000;
    struct tm tm;
    gmtime_r(&sec, &tm);
    size_t n = strftime(out, len, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + n, len - n, ".%03d+00:00", (int)(now % 1000));
}

// Envelope for everything we send. statusCode and an RFC 3339 timeStamp
// are required by Protect 7.x's device service (ds) to parse it at all.
// Called from the protocol thread and (events) from analytics.
static void send_msg(const char *fn, int in_response_to, cJSON *payload) {
    pthread_mutex_lock(&send_lock);
    if (!connected) {
        pthread_mutex_unlock(&send_lock);
        cJSON_Delete(payload);
        return;
    }
    char ts[40];
    timestamp(ts, sizeof(ts));
    cJSON *m = cJSON_CreateObject();
    STR(m, "from", "ubnt_avclient");
    STR(m, "functionName", fn);
    NUM(m, "inResponseTo", in_response_to);
    NUM(m, "messageId", ++msg_id);
    cJSON_AddItemToObject(m, "payload", payload ? payload : cJSON_CreateObject());
    BOOL(m, "responseExpected", 0);
    NUM(m, "statusCode", 0);
    STR(m, "timeStamp", ts);
    STR(m, "to", "UniFiVideo");
    char *text = cJSON_PrintUnformatted(m);
    log_debug("> %s", text);
    // Text frames: ds ignores binary ones.
    ws_send_text(&ws, text, strlen(text));
    free(text);
    cJSON_Delete(m);
    pthread_mutex_unlock(&send_lock);
}

void avclient_event(const char *fn, cJSON *payload) { send_msg(fn, 0, payload); }

static double uptime_s(void) { return (mono_ms() - start_ms) / 1000.0; }

// --- identity -----------------------------------------------------------

static void uuid_format(const uint8_t *b, char *out) {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
}

// RFC 4122 v5 UUID in the DNS namespace: a stable device-id per MAC.
static void uuid5(const char *name, char *out) {
    static const uint8_t ns_dns[16] = {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1,
                                       0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8};
    uint8_t in[16 + 64], hash[20];
    size_t n = strlen(name);
    if (n > 64)
        n = 64;
    memcpy(in, ns_dns, 16);
    memcpy(in + 16, name, n);
    mbedtls_sha1(in, 16 + n, hash);
    hash[6] = (hash[6] & 0x0f) | 0x50;
    hash[8] = (hash[8] & 0x3f) | 0x80;
    uuid_format(hash, out);
}

static void uuid4(char *out) {
    uint8_t b[16];
    FILE *fp = fopen("/dev/urandom", "rb");
    if (!fp || fread(b, 1, 16, fp) != 16)
        for (int i = 0; i < 16; i++)
            b[i] = rand();
    if (fp)
        fclose(fp);
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    uuid_format(b, out);
}

static cJSON *feature_flags(void) {
    static const char *const smart[] = {"person", "vehicle", "animal"};
    static const char *const def[] = {"default"};
    static const char *const enhanced[] = {"enhanced"};
    static const char *const h264[] = {"h264"};
    static const char *const aac[] = {"aac"};
    static const char *const nature[] = {"nature"};
    cJSON *f = cJSON_CreateObject();
    NUM(f, "mic", 1);
    cJSON_AddItemToObject(f, "aec", cJSON_CreateArray());
    cJSON_AddItemToObject(f, "videoMode", str_array(def, 1));
    cJSON_AddItemToObject(f, "motionDetect", str_array(enhanced, 1));
    // Advertised from day one: Protect caches capabilities at adoption.
    cJSON_AddItemToObject(f, "smartDetect", str_array(smart, 3));
    cJSON_AddItemToObject(f, "smartDetectTypes", str_array(smart, 3));
    cJSON_AddItemToObject(f, "smartDetectAudioTypes", cJSON_CreateArray());
    BOOL(f, "hasSmartDetect", 1);
    cJSON_AddItemToObject(f, "videoCodecs", str_array(h264, 1));
    cJSON_AddItemToObject(f, "audioCodecs", str_array(aac, 1));
    cJSON_AddItemToObject(f, "audioStyle", str_array(nature, 1));
    BOOL(f, "hasHdr", 0);
    BOOL(f, "hasWdr", 0);
    BOOL(f, "hasMic", 1);
    BOOL(f, "hasSpeaker", 0);
    BOOL(f, "hasInfrared", 1);
    BOOL(f, "hasMotionZones", 1);
    BOOL(f, "hasPrivacyMask", 0);
    BOOL(f, "isPtz", 0);
    return f;
}

static void hwaddr(char *out) {
    const char *m = cfg->mac;
    snprintf(out, 18, "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s", m, m + 2, m + 4, m + 6, m + 8, m + 10);
    for (char *p = out; *p; p++)
        if (*p >= 'A' && *p <= 'F')
            *p += 'a' - 'A';
}

static void send_hello(void) {
    char semver[40], hw[18];
    snprintf(semver, sizeof(semver), "v%s", cfg->fw_version);
    hwaddr(hw);
    cJSON *p = cJSON_CreateObject();
    STR(p, "adoptionCode", cfg->token);
    STR(p, "connectionHost", cfg->nvr);
    NUM(p, "connectionSecurePort", 7442);
    STR(p, "fwVersion", cfg->fw_version);
    NUM(p, "hwrev", 19);
    NUM(p, "idleTime", 191.96);
    STR(p, "ip", cfg->ip);
    STR(p, "mac", cfg->mac);
    STR(p, "model", cfg->model);
    STR(p, "name", cfg->name);
    NUM(p, "protocolVersion", 67);
    NUM(p, "rebootTimeoutSec", 30);
    STR(p, "semver", semver);
    NUM(p, "totalLoad", 0.5);
    NUM(p, "upgradeTimeoutSec", 150);
    NUM(p, "uptime", (int)uptime_s());
    cJSON_AddItemToObject(p, "features", feature_flags());
    // Protect >= 3 recognises the model by sysid/platform/firmwareBuild
    // (and the WSS headers), not by the model string.
    NUM(p, "sysid", strtol(cfg->sysid, NULL, 0));
    STR(p, "platform", cfg->platform);
    STR(p, "firmwareBuild", cfg->fw_build);
    STR(p, "hwaddr", hw);
    STR(p, "lensmodel", cfg->model);
    STR(p, "cameraName", cfg->name);
    BOOL(p, "isGen5s", 1);
    BOOL(p, "isDoorbellSeries", 0);
    send_msg("ubnt_avclient_hello", 0, p);
}

// --- settings payloads --------------------------------------------------

static cJSON *isp_settings(void) {
    static const struct {
        const char *k;
        double v;
    } nums[] = {
        {"aeTargetPercent", 50}, {"aggressiveAntiFlicker", 0}, {"brightness", 50},
        {"contrast", 50}, {"criticalTmpOfProtect", 40}, {"dZoomCenterX", 50},
        {"dZoomCenterY", 50}, {"dZoomScale", 0}, {"dZoomStreamId", 4},
        {"darkAreaCompensateLevel", 0}, {"denoise", 50}, {"enable3dnr", 1},
        {"enableExternalIr", 0}, {"enableMicroTmpProtect", 1}, {"enablePauseMotion", 0},
        {"flip", 0}, {"focusPosition", 0}, {"forceFilterIrSwitchEvents", 0}, {"hue", 50},
        {"icrLightSensorNightThd", 0}, {"icrSensitivity", 0}, {"irLedLevel", 215},
        {"irOnStsBrightness", 0}, {"irOnStsContrast", 0}, {"irOnStsDenoise", 0},
        {"irOnStsHue", 0}, {"irOnStsSaturation", 0}, {"irOnStsSharpness", 0},
        {"irOnStsWdr", 0}, {"irOnValBrightness", 50}, {"irOnValContrast", 50},
        {"irOnValDenoise", 50}, {"irOnValHue", 50}, {"irOnValSaturation", 50},
        {"irOnValSharpness", 50}, {"irOnValWdr", 1}, {"lensDistortionCorrection", 1},
        {"mirror", 0}, {"queryIrLedStatus", 0}, {"saturation", 50}, {"sharpness", 50},
        {"touchFocusX", 1001}, {"touchFocusY", 1001}, {"wdr", 1}, {"zoomPosition", 0},
    };
    cJSON *p = cJSON_CreateObject();
    STR(p, "aeMode", "auto");
    STR(p, "focusMode", "ztrig");
    STR(p, "irLedMode", "auto");
    cJSON_AddNullToObject(p, "masks");
    for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++)
        NUM(p, nums[i].k, nums[i].v);
    return p;
}

static cJSON *osd_settings(void) {
    cJSON *p = cJSON_CreateObject();
    for (int i = 1; i <= 4; i++) {
        char k[4];
        snprintf(k, sizeof(k), "_%d", i);
        cJSON *o = OBJ(p, k);
        NUM(o, "enableDate", 1);
        NUM(o, "enableLogo", 1);
        NUM(o, "enableReportdStatsLevel", 0);
        NUM(o, "enableStreamerStatsLevel", 0);
        STR(o, "tag", cfg->name);
    }
    NUM(p, "enableOverlay", 1);
    NUM(p, "logoScale", 50);
    NUM(p, "overlayColorId", 0);
    NUM(p, "textScale", 50);
    NUM(p, "useCustomLogo", 0);
    return p;
}

static cJSON *network_status(void) {
    cJSON *p = cJSON_CreateObject();
    NUM(p, "connectionState", 2);
    STR(p, "connectionStateDescription", "CONNECTED");
    STR(p, "defaultInterface", "eth0");
    NUM(p, "dhcpLeasetime", 86400);
    STR(p, "dnsServer", "");
    STR(p, "gateway", "");
    STR(p, "ipAddress", cfg->ip);
    NUM(p, "linkDuplex", 1);
    NUM(p, "linkSpeedMbps", 100);
    STR(p, "mode", "dhcp");
    STR(p, "networkMask", "255.255.255.0");
    return p;
}

static cJSON *sound_led_settings(void) {
    cJSON *p = cJSON_CreateObject();
    NUM(p, "ledFaceAlwaysOnWhenManaged", 1);
    NUM(p, "ledFaceEnabled", 1);
    NUM(p, "speakerEnabled", 0);
    NUM(p, "speakerVolume", 100);
    NUM(p, "systemSoundsEnabled", 0);
    NUM(p, "userLedBlinkPeriodMs", 0);
    STR(p, "userLedColorFg", "blue");
    NUM(p, "userLedOnNoff", 1);
    return p;
}

static void add_fps_values(cJSON *o) {
    static const int fps[] = {1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 30};
    cJSON_AddItemToObject(o, "validFpsValues", cJSON_CreateIntArray(fps, 17));
}

static cJSON *video_channel(int idx, const char *desc, double cbr, double vbr_max,
                            double range_max, int stream_id) {
    struct stream *s = &streams[idx];
    struct source *src = idx == 0 ? hi_src : lo_src;
    cJSON *v = cJSON_CreateObject();
    NUM(v, "M", 1);
    NUM(v, "N", 30);
    cJSON *ser = OBJ(v, "avSerializer");
    cJSON *dst = cJSON_AddArrayToObject(ser, "destinations");
    if (s->running) {
        char url[96];
        snprintf(url, sizeof(url), "tcp://%s:%d", s->host, s->port);
        cJSON_AddItemToArray(dst, cJSON_CreateString(url));
        cJSON *par = OBJ(ser, "parameters");
        cJSON_AddNullToObject(par, "audioId");
        STR(par, "streamName", s->name);
        cJSON_AddNullToObject(par, "suppressAudio");
        cJSON_AddNullToObject(par, "suppressVideo");
        cJSON_AddNullToObject(par, "videoId");
    } else {
        cJSON_AddItemToArray(dst, cJSON_CreateString("file:///dev/null"));
        cJSON_AddNullToObject(ser, "parameters");
    }
    STR(ser, "type", "extendedFlv");
    NUM(v, "bitRateCbrAvg", cbr);
    NUM(v, "bitRateVbrMax", vbr_max);
    NUM(v, "bitRateVbrMin", 48000);
    STR(v, "description", desc);
    BOOL(v, "enabled", 1);
    NUM(v, "fps", cfg->fps);
    NUM(v, "gopModel", 0);
    NUM(v, "height", src->height);
    BOOL(v, "horizontalFlip", 0);
    BOOL(v, "isCbr", 0);
    NUM(v, "maxFps", 30);
    NUM(v, "minClientAdaptiveBitRate", 0);
    NUM(v, "minMotionAdaptiveBitRate", 0);
    NUM(v, "nMultiplier", 6);
    STR(v, "name", s->channel);
    NUM(v, "sourceId", idx);
    NUM(v, "streamId", stream_id);
    NUM(v, "streamOrdinal", idx);
    STR(v, "type", "h264");
    NUM(v, "validBitrateRangeMax", range_max);
    NUM(v, "validBitrateRangeMin", 32000);
    add_fps_values(v);
    BOOL(v, "verticalFlip", 0);
    NUM(v, "width", src->width);
    return v;
}

static cJSON *video_settings(void) {
    cJSON *p = cJSON_CreateObject();
    cJSON *a = OBJ(p, "audio");
    NUM(a, "bitRate", 32000);
    NUM(a, "channels", 1);
    STR(a, "description", "audio track");
    BOOL(a, "enableTemporalNoiseShaping", 0);
    BOOL(a, "enabled", 1);
    NUM(a, "mode", 0);
    NUM(a, "quality", 0);
    NUM(a, "sampleRate", 16000);
    STR(a, "type", "aac");
    NUM(a, "volume", 100);
    STR(p, "firmwarePath", "/lib/firmware/");

    cJSON *v = OBJ(p, "video");
    BOOL(v, "enableHrd", 0);
    NUM(v, "hdrMode", 0);
    BOOL(v, "lowDelay", 0);
    STR(v, "videoMode", "default");
    STR(v, "videoCodec", "h264");

    cJSON *mj = OBJ(v, "mjpg");
    cJSON *ser = OBJ(mj, "avSerializer");
    static const char *const snaps[] = {"file:///tmp/snap.jpeg", "file:///tmp/snap_av.jpg"};
    cJSON_AddItemToObject(ser, "destinations", str_array(snaps, 2));
    cJSON *par = OBJ(ser, "parameters");
    NUM(par, "audioId", 1000);
    BOOL(par, "enableTimestampsOverlapAvoidance", 0);
    BOOL(par, "suppressAudio", 1);
    BOOL(par, "suppressVideo", 0);
    NUM(par, "videoId", 1001);
    STR(ser, "type", "mjpg");
    NUM(mj, "bitRateCbrAvg", 500000);
    NUM(mj, "bitRateVbrMax", 500000);
    cJSON_AddNullToObject(mj, "bitRateVbrMin");
    STR(mj, "description", "JPEG pictures");
    BOOL(mj, "enabled", 1);
    NUM(mj, "fps", 5);
    NUM(mj, "height", hi_src->height);
    BOOL(mj, "isCbr", 0);
    NUM(mj, "maxFps", 5);
    NUM(mj, "minClientAdaptiveBitRate", 0);
    NUM(mj, "minMotionAdaptiveBitRate", 0);
    cJSON_AddNullToObject(mj, "nMultiplier");
    STR(mj, "name", "mjpg");
    NUM(mj, "quality", 80);
    NUM(mj, "sourceId", 3);
    NUM(mj, "streamId", 8);
    NUM(mj, "streamOrdinal", 3);
    STR(mj, "type", "mjpg");
    NUM(mj, "validBitrateRangeMax", 6000000);
    NUM(mj, "validBitrateRangeMin", 32000);
    NUM(mj, "width", hi_src->width);

    cJSON_AddItemToObject(v, "video1",
                          video_channel(0, "Hi quality video track", 2000000, 4000000, 6000000, 1));
    cJSON_AddItemToObject(v, "video2",
                          video_channel(1, "Medium quality video track", 500000, 1200000, 1500000, 2));
    cJSON_AddItemToObject(v, "video3",
                          video_channel(2, "Low quality video track", 300000, 200000, 750000, 4));
    NUM(v, "vinFps", cfg->fps);
    return p;
}

// --- request handlers ---------------------------------------------------

static void change_video_settings(cJSON *payload) {
    cJSON *video = cJSON_GetObjectItem(payload, "video");
    for (int i = 0; i < 3 && video; i++) {
        cJSON *ch = cJSON_GetObjectItem(video, streams[i].channel);
        cJSON *ser = cJSON_GetObjectItem(ch, "avSerializer");
        cJSON *dsts = cJSON_GetObjectItem(ser, "destinations");
        cJSON *d0 = cJSON_GetArrayItem(dsts, 0);
        if (!cJSON_IsString(d0))
            continue;
        if (strstr(d0->valuestring, "/dev/null")) {
            stream_stop(&streams[i]);
            continue;
        }
        cJSON *name = cJSON_GetObjectItem(cJSON_GetObjectItem(ser, "parameters"), "streamName");
        char host[64];
        int port;
        if (!cJSON_IsString(name) || sscanf(d0->valuestring, "tcp://%63[^:]:%d", host, &port) != 2)
            continue;
        if (stream_matches(&streams[i], name->valuestring, host, port))
            continue;
        stream_start(&streams[i], name->valuestring, host, port);
        if (!cfg->adopted) {
            // Protect only asks a managed camera for video: adoption worked.
            cfg->adopted = 1;
            config_save_adopted(cfg);
            log_info("adopted by %s", cfg->nvr);
        }
    }
}

struct upload {
    char uri[512];
    char what[64];
    char filename[64];
    int nfields;
    struct form_field fields[16];
    char *strings; // backing store for fields
};

static void *upload_main(void *arg) {
    struct upload *u = arg;
    uint8_t *data;
    size_t len;
    const char *name = "screen.jpg";
    // A motion event's snapshot/heatmap, else a fresh picture.
    int rc = analytics_file(u->what, u->filename, &data, &len, &name);
    if (rc < 0)
        rc = cfg->capture ? capture_jpeg(&data, &len) : read_jpeg(cfg->snapshot, &data, &len);
    if (rc == 0) {
        int status = https_post_file(u->uri, u->fields, u->nfields, name, data, len);
        log_debug("uploaded %s as %s (%zu bytes): HTTP %d", u->what, name, len, status);
        free(data);
    } else {
        log_warn("no snapshot available from %s", cfg->snapshot);
    }
    free(u->strings);
    free(u);
    return NULL;
}

// GetRequest: Protect wants a JPEG uploaded to the URI it names (live
// thumbnails, event snapshots). Done off the protocol thread.
static void get_request(cJSON *payload) {
    cJSON *uri = cJSON_GetObjectItem(payload, "uri");
    cJSON *what = cJSON_GetObjectItem(payload, "what");
    if (!cJSON_IsString(uri))
        return;
    struct upload *u = calloc(1, sizeof(*u));
    snprintf(u->uri, sizeof(u->uri), "%s", uri->valuestring);
    snprintf(u->what, sizeof(u->what), "%s", cJSON_IsString(what) ? what->valuestring : "?");
    const char *filename = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "filename"));
    snprintf(u->filename, sizeof(u->filename), "%s", filename ? filename : "");
    cJSON *fields = cJSON_GetObjectItem(payload, "formFields");
    size_t total = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, fields) if (cJSON_IsString(f)) total +=
        strlen(f->string) + strlen(f->valuestring) + 2;
    u->strings = malloc(total + 1);
    char *p = u->strings;
    cJSON_ArrayForEach(f, fields) {
        if (!cJSON_IsString(f) || u->nfields == 16)
            continue;
        u->fields[u->nfields].name = strcpy(p, f->string);
        p += strlen(p) + 1;
        u->fields[u->nfields].value = strcpy(p, f->valuestring);
        p += strlen(p) + 1;
        u->nfields++;
    }
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, upload_main, u)) {
        free(u->strings);
        free(u);
    }
    pthread_attr_destroy(&attr);
}

// Returns 1 when the connection should be dropped and re-established.
static int handle(const char *text) {
    cJSON *m = cJSON_Parse(text);
    if (!m)
        return 0;
    const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(m, "functionName"));
    int id = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "messageId"));
    int expected = cJSON_IsTrue(cJSON_GetObjectItem(m, "responseExpected"));
    cJSON *payload = cJSON_GetObjectItem(m, "payload");
    int reconnect = 0;
    if (!fn)
        goto out;
    log_debug("< %s", text);
    log_info("Protect: %s", fn);

    // Fire-and-forget messages we act on even without a response.
    static const char *const unsolicited[] = {"GetRequest", "ChangeVideoSettings",
                                              "UpdateFirmwareRequest", "Reboot",
                                              "ubnt_avclient_hello"};
    int known = expected;
    for (size_t i = 0; i < sizeof(unsolicited) / sizeof(*unsolicited); i++)
        known |= !strcmp(fn, unsolicited[i]);
    if (!known)
        goto out;

    cJSON *res = NULL;
    const char *res_fn = fn;
    int respond = expected;
    if (!strcmp(fn, "ubnt_avclient_hello")) {
        respond = 0;
    } else if (!strcmp(fn, "ubnt_avclient_time")) {
        res_fn = "ubnt_avclient_paramAgreement";
        res = cJSON_CreateObject();
        NUM(res, "monotonicMs", uptime_s() * 1000);
        NUM(res, "wallMs", (double)wall_ms());
        OBJ(res, "features");
        respond = 1;
    } else if (!strcmp(fn, "ubnt_avclient_paramAgreement")) {
        res = cJSON_CreateObject();
        STR(res, "authToken", cfg->token);
        cJSON_AddItemToObject(res, "features", feature_flags());
        respond = 1;
    } else if (!strcmp(fn, "ChangeVideoSettings")) {
        change_video_settings(payload);
        res = video_settings();
        respond = 1;
    } else if (!strcmp(fn, "ResetIspSettings") || !strcmp(fn, "ChangeIspSettings")) {
        res = isp_settings();
    } else if (!strcmp(fn, "ChangeDeviceSettings")) {
        res = cJSON_CreateObject();
        STR(res, "name", cfg->name);
        STR(res, "timezone", "UTC");
    } else if (!strcmp(fn, "ChangeOsdSettings")) {
        res = osd_settings();
    } else if (!strcmp(fn, "NetworkStatus")) {
        res = network_status();
    } else if (!strcmp(fn, "ChangeSoundLedSettings")) {
        res = sound_led_settings();
    } else if (!strcmp(fn, "GetRequest")) {
        get_request(payload);
        res_fn = "GetRequest";
        res = cJSON_CreateObject();
    } else if (!strcmp(fn, "UpdateFirmwareRequest")) {
        log_warn("Protect wants a firmware update; set FW_VERSION/FW_BUILD to the newest "
                 "version Protect knows for platform %s", cfg->platform);
        respond = 0;
    } else if (!strcmp(fn, "Reboot")) {
        if (cfg->allow_reboot) {
            log_info("reboot requested by Protect");
            pthread_mutex_lock(&send_lock);
            connected = 0;
            ws_close(&ws);
            pthread_mutex_unlock(&send_lock);
            sync();
            execl("/sbin/reboot", "reboot", (char *)NULL);
        }
        reconnect = 1;
        respond = 0;
    } else if (!strcmp(fn, "ChangeSmartMotionSettings")) {
        analytics_settings(payload);
        res = cJSON_CreateObject();
    } else if (!strcmp(fn, "ChangeSmartDetectSettings")) {
        res = cJSON_CreateObject(); // zones are stored once smart detection exists
    }
    if (respond) {
        // Protect 7.x stalls ~5 s on every unanswered request, so anything
        // not handled above is acknowledged by echoing its payload.
        if (!res)
            res = payload ? cJSON_Duplicate(payload, 1) : cJSON_CreateObject();
        send_msg(res_fn, id, res);
    } else {
        cJSON_Delete(res);
    }
out:
    cJSON_Delete(m);
    return reconnect;
}

// --- connection loop ----------------------------------------------------

static void stop_streams(void) {
    for (int i = 0; i < 3; i++)
        stream_stop(&streams[i]);
}

static void session(void) {
    char path[256], headers[512];
    if (cfg->adopted)
        snprintf(path, sizeof(path), "/camera/1.0/ws");
    else
        snprintf(path, sizeof(path), "/camera/1.0/ws?token=%s", cfg->token);
    // Protect looks the camera up by camera-mac in exactly the 12-hex form
    // and recognises the model by the hex sysid in camera-model.
    snprintf(headers, sizeof(headers),
             "camera-mac: %s\r\ncamera-model: 0x%04lx\r\ncamera-ip: %s\r\n"
             "camera-firmware: %s\r\ndevice-id: %s\r\nx-guid: %s\r\n",
             cfg->mac, strtol(cfg->sysid, NULL, 0), cfg->ip, cfg->fw_version, device_id, guid);

    log_info("connecting to wss://%s:7442 (%s)", cfg->nvr, cfg->adopted ? "adopted" : "adopting");
    if (ws_connect(&ws, cfg->nvr, 7442, path, headers) < 0)
        return;
    log_info("connected");
    pthread_mutex_lock(&send_lock);
    connected = 1;
    pthread_mutex_unlock(&send_lock);
    send_hello();
    for (;;) {
        const char *text;
        size_t len;
        int r = ws_recv(&ws, &text, &len, 1000);
        if (r < 0)
            break;
        if (r > 0 && handle(text))
            break;
    }
    pthread_mutex_lock(&send_lock);
    connected = 0;
    ws_close(&ws);
    pthread_mutex_unlock(&send_lock);
    stop_streams();
}

void avclient_run(struct config *c, struct source *hi, struct source *lo) {
    cfg = c;
    hi_src = hi;
    lo_src = lo;
    start_ms = mono_ms();
    char name[64];
    snprintf(name, sizeof(name), "uvcd-%s", cfg->mac);
    uuid5(name, device_id);
    uuid4(guid);
    for (int i = 0; i < 3; i++) {
        streams[i].src = i == 0 ? hi : lo;
        streams[i].fps = cfg->fps;
        streams[i].audio = cfg->audio;
    }

    int backoff = 1;
    for (;;) {
        int64_t t0 = mono_ms();
        session();
        if (mono_ms() - t0 > 60000)
            backoff = 1;
        log_info("reconnecting in %d s", backoff);
        sleep_ms(backoff * 1000);
        backoff = backoff < 30 ? backoff * 2 : 30;
    }
}
