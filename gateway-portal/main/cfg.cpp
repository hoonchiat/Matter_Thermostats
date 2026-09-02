/*
 * cfg.cpp - persisted portal configuration (see cfg.h).
 */
#include "cfg.h"

#include <cstring>
#include <cstdio>

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "cfg";

#define CFG_NS   "portal"
#define CFG_KEY  "cfg"
#define CFG_VER  1

static portal_cfg_t g;

static void set_defaults(void)
{
    memset(&g, 0, sizeof(g));
    g.poll_s = 5;
    g.tz_min = 480;                         /* GMT+8 */
    strcpy(g.ntp,      "pool.ntp.org");
    strcpy(g.hostname, "esp32");
    g.ap_ssid[0] = '\0';                    /* -> auto "MatterGateway-XXXX" */
    strcpy(g.ap_pass,  "matterportal");
    strcpy(g.sta_ssid, "pjoshua2.4");
    strcpy(g.sta_pass, "pj135713");
}

void cfg_load(void)
{
    set_defaults();
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ver = 0;
        nvs_get_u8(h, "ver", &ver);
        portal_cfg_t t;
        size_t len = sizeof(t);
        if (ver == CFG_VER && nvs_get_blob(h, CFG_KEY, &t, &len) == ESP_OK && len == sizeof(t))
            g = t;
        nvs_close(h);
    }
    if (g.poll_s < 1)  g.poll_s = 1;
    if (g.poll_s > 60) g.poll_s = 60;
    if (g.tz_min < -720) g.tz_min = -720;
    if (g.tz_min > 840)  g.tz_min = 840;
    ESP_LOGI(TAG, "config loaded (poll %ds, tz %+dmin, sta \"%s\", ap \"%s\")",
             g.poll_s, g.tz_min, g.sta_ssid, g.ap_ssid[0] ? g.ap_ssid : "(auto)");
}

void cfg_save(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGW(TAG, "nvs open failed"); return; }
    nvs_set_u8(h, "ver", CFG_VER);
    nvs_set_blob(h, CFG_KEY, &g, sizeof(g));
    nvs_commit(h);
    nvs_close(h);
}

const portal_cfg_t *cfg_get(void) { return &g; }

cJSON *cfg_to_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "poll_s",   g.poll_s);
    cJSON_AddNumberToObject(o, "tz_min",   g.tz_min);
    cJSON_AddStringToObject(o, "ntp",      g.ntp);
    cJSON_AddStringToObject(o, "hostname", g.hostname);
    cJSON_AddStringToObject(o, "ap_ssid",  g.ap_ssid);
    cJSON_AddStringToObject(o, "sta_ssid", g.sta_ssid);
    cJSON_AddStringToObject(o, "ap_pass",  "");     /* masked - blank = keep existing */
    cJSON_AddStringToObject(o, "sta_pass", "");     /* masked */
    cJSON_AddBoolToObject  (o, "ap_pass_set",  g.ap_pass[0]  != '\0');
    cJSON_AddBoolToObject  (o, "sta_pass_set", g.sta_pass[0] != '\0');
    return o;
}

/* Copy a string field if present and changed; flags *changed. */
static void take_str(char *dst, size_t cap, const cJSON *req, const char *key, bool *changed)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(req, key));
    if (v && strcmp(v, dst) != 0) { strncpy(dst, v, cap - 1); dst[cap - 1] = '\0'; if (changed) *changed = true; }
}
/* Password: apply only if a non-empty value is supplied (blank = keep existing). */
static void take_pass(char *dst, size_t cap, const cJSON *req, const char *key, bool *changed)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(req, key));
    if (v && v[0] && strcmp(v, dst) != 0) { strncpy(dst, v, cap - 1); dst[cap - 1] = '\0'; if (changed) *changed = true; }
}

bool cfg_apply_json(const cJSON *req, char *err, size_t cap, bool *reboot, bool *tz_changed)
{
    *reboot = false; *tz_changed = false;

    /* validate a supplied AP password (empty = keep existing, allowed) */
    const char *ap_pw = cJSON_GetStringValue(cJSON_GetObjectItem(req, "ap_pass"));
    if (ap_pw && ap_pw[0] && strlen(ap_pw) < 8) { snprintf(err, cap, "AP password must be >= 8 characters"); return false; }

    /* live fields */
    cJSON *tz = cJSON_GetObjectItem(req, "tz_min");
    if (cJSON_IsNumber(tz)) {
        int v = tz->valueint;
        if (v < -720 || v > 840) { snprintf(err, cap, "tz out of range (-720..840 min)"); return false; }
        if (v != g.tz_min) { g.tz_min = v; *tz_changed = true; }
    }
    cJSON *pl = cJSON_GetObjectItem(req, "poll_s");
    if (cJSON_IsNumber(pl)) { int v = pl->valueint; if (v < 1) v = 1; if (v > 60) v = 60; g.poll_s = v; }

    /* reboot-only (network) fields */
    take_str (g.ntp,      sizeof(g.ntp),      req, "ntp",      reboot);
    take_str (g.hostname, sizeof(g.hostname), req, "hostname", reboot);
    take_str (g.ap_ssid,  sizeof(g.ap_ssid),  req, "ap_ssid",  reboot);
    take_str (g.sta_ssid, sizeof(g.sta_ssid), req, "sta_ssid", reboot);
    take_pass(g.ap_pass,  sizeof(g.ap_pass),  req, "ap_pass",  reboot);
    take_pass(g.sta_pass, sizeof(g.sta_pass), req, "sta_pass", reboot);

    cfg_save();
    return true;
}
