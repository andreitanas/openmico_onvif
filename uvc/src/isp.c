#include "isp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "capture.h"
#include "daynight.h"
#include "util.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *state; // guarded by lock

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
        {"icrCustomValue", 2}, {"icrLightSensorNightThd", 0}, {"icrSensitivity", 0},
        {"irLedLevel", 215},
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
    cJSON_AddStringToObject(p, "icrSwitchMode", "sensitivity");
    cJSON_AddStringToObject(p, "irLedMode", "auto");
    cJSON_AddNullToObject(p, "masks");
    for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++)
        cJSON_AddNumberToObject(p, nums[i].k, nums[i].v);
    return p;
}

void isp_init(void) {
    pthread_mutex_lock(&lock);
    state = defaults();
    pthread_mutex_unlock(&lock);
}

static int num(const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(state, k);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : 50;
}

static const char *str(const char *k) {
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(state, k));
    return v ? v : "";
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
    // Protect's night mode setting, camera side: "manual" is always day
    // (irLedLevel 0) or always night, "auto" switches; irLedLevel 0 keeps the
    // IR LEDs off; "lux" uses the custom level.
    int manual = !strcmp(str("irLedMode"), "manual"), leds = num("irLedLevel") > 0;
    int lux = !strcmp(str("icrSwitchMode"), "lux"), custom = num("icrCustomValue");
    cJSON *res = cJSON_Duplicate(state, 1);
    pthread_mutex_unlock(&lock);
    capture_isp(&p);
    daynight_settings(manual, leds, lux, custom);
    return res;
}
