#include "isp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "capture.h"
#include "util.h"

#define IR_MODE_FILE "/tmp/ir_mode"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *state; // guarded by lock
static char ir_mode[32]; // guarded by lock: last applied irLedMode
static const char *ir_script;

// What a G5 Pro reports; Protect overwrites most of it on every connection.
static cJSON *defaults(void) {
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
    cJSON_AddStringToObject(p, "aeMode", "auto");
    cJSON_AddStringToObject(p, "focusMode", "ztrig");
    cJSON_AddStringToObject(p, "irLedMode", "auto");
    cJSON_AddNullToObject(p, "masks");
    for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++)
        cJSON_AddNumberToObject(p, nums[i].k, nums[i].v);
    return p;
}

void isp_init(const char *script) {
    ir_script = script;
    pthread_mutex_lock(&lock);
    state = defaults();
    pthread_mutex_unlock(&lock);
}

static int num(const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(state, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : 50;
}

static void *ir_apply(void *arg) {
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "%s apply", ir_script);
    if (system(cmd) != 0)
        log_warn("isp: %s failed", cmd);
    return NULL;
}

// lock held. The IR LEDs and cut filter belong to nightmode.sh, which
// autonight calls with its day/night verdict; the mode decides whether that
// verdict is followed (auto), half followed (autoFilterOnly: no LEDs) or
// overridden (on/off).
static void ir_update(void) {
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state, "irLedMode"));
    if (!mode || !strcmp(mode, ir_mode) || !ir_script || access(ir_script, X_OK) < 0)
        return;
    snprintf(ir_mode, sizeof(ir_mode), "%s", mode);
    FILE *fp = fopen(IR_MODE_FILE, "w");
    if (!fp)
        return;
    fprintf(fp, "%s\n", mode);
    fclose(fp);
    log_info("isp: IR mode %s", mode);
    // Moving the cut filter takes a second; don't hold up Protect.
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, ir_apply, NULL);
    pthread_attr_destroy(&attr);
}

cJSON *isp_change(const cJSON *payload, int reset) {
    pthread_mutex_lock(&lock);
    if (reset) {
        cJSON_Delete(state);
        state = defaults();
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, payload) {
        if (!item->string)
            continue;
        cJSON_DeleteItemFromObjectCaseSensitive(state, item->string);
        cJSON_AddItemToObject(state, item->string, cJSON_Duplicate(item, 1));
    }
    struct isp_params p = {
        .brightness = num("brightness"),
        .contrast = num("contrast"),
        .saturation = num("saturation"),
        .sharpness = num("sharpness"),
        .denoise = num("denoise"),
        .flip = num("flip") == 1,
        .mirror = num("mirror") == 1,
    };
    ir_update();
    cJSON *res = cJSON_Duplicate(state, 1);
    pthread_mutex_unlock(&lock);
    capture_isp(&p);
    return res;
}
