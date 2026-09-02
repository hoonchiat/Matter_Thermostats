/*
 * portal.cpp - poll + state cache + time-sync (see portal.h).
 */
#include "portal.h"
#include "usb_bridge.h"
#include "net.h"
#include "web.h"
#include "cfg.h"

#include <cstring>
#include <cstdio>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>

#include "cJSON.h"

static const char *TAG = "portal";

#define BUFSZ     4096       /* status line for up to 7 devices fits comfortably */

static char            *s_cache      = nullptr;
static size_t           s_cache_len  = 0;
static SemaphoreHandle_t s_cache_mtx = nullptr;

/* ---- air-quality history (PSRAM ring per slot) ------------------------- *
 * Filled from every poll so the Charts tab shows real trends immediately on
 * open (not just from when the browser connected). Metrics: temp,hum,co2,pm25. */
#define HIST_DEVS  8
#define HIST_MAX   360               /* ~30 min at 5 s (scales with poll interval) */
struct hpt { uint32_t t; float v[4]; };
static hpt             *s_hist    = nullptr;             /* [HIST_DEVS*HIST_MAX] */
static int              s_hist_n[HIST_DEVS]    = {0};
static int              s_hist_head[HIST_DEVS] = {0};
static bool             s_hist_air[HIST_DEVS]  = {false};
static SemaphoreHandle_t s_hist_mtx = nullptr;
static const char      *HIST_KEYS[4] = { "temp", "humidity", "co2", "pm25" };

/* time-sync bookkeeping */
static bool             s_synced     = false;   /* synced since the current online session began */
static int              s_last_yday  = -1;      /* for the daily re-sync */
static c6_link_state_t  s_prev_link  = C6_OFFLINE;

size_t portal_get_status(char *out, size_t len)
{
    size_t n = 0;
    if (!out || !len) return 0;
    xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
    if (s_cache && s_cache_len) {
        n = (s_cache_len < len - 1) ? s_cache_len : len - 1;
        memcpy(out, s_cache, n);
        out[n] = '\0';
    }
    xSemaphoreGive(s_cache_mtx);
    return n;
}

static void build_local_iso(char *out, size_t len)
{
    time_t utc = time(nullptr);
    int off = net_tz_offset_min();
    time_t local = utc + (time_t)off * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    int a = off < 0 ? -off : off;
    char sign = off < 0 ? '-' : '+';
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d %c%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, sign, a / 60, a % 60);
}

static void poll_once(char *buf)
{
    if (!usb_bridge_request("{\"cmd\":\"status\"}", buf, BUFSZ, 3000)) return;
    size_t n = strlen(buf);
    xSemaphoreTake(s_cache_mtx, portMAX_DELAY);
    memcpy(s_cache, buf, n + 1);
    s_cache_len = n;
    xSemaphoreGive(s_cache_mtx);
}

/* Append one air-quality sample for a device to its history ring. */
static void hist_ingest(const char *status)
{
    if (!s_hist || !s_hist_mtx) return;
    time_t now = time(nullptr);
    if (now <= 946684800L) return;      /* clock not set yet - skip so all t are real epoch */
    uint32_t t = (uint32_t)now;
    cJSON *r = cJSON_Parse(status);
    if (!r) return;
    cJSON *res  = cJSON_GetObjectItem(r, "result");
    cJSON *devs = res ? cJSON_GetObjectItem(res, "devices") : nullptr;
    cJSON *d;
    cJSON_ArrayForEach(d, devs) {
        cJSON *sensors = cJSON_GetObjectItem(d, "sensors");
        cJSON *slot    = cJSON_GetObjectItem(d, "slot");
        if (!cJSON_IsObject(sensors) || !cJSON_IsNumber(slot)) continue;
        int s0 = slot->valueint - 1;
        if (s0 < 0 || s0 >= HIST_DEVS) continue;
        float v[4]; bool any = false;
        for (int k = 0; k < 4; ++k) {
            cJSON *m = cJSON_GetObjectItem(sensors, HIST_KEYS[k]);
            if (cJSON_IsNumber(m)) { v[k] = (float)m->valuedouble; any = true; } else v[k] = NAN;
        }
        if (!any) continue;
        xSemaphoreTake(s_hist_mtx, portMAX_DELAY);
        hpt *p = &s_hist[s0 * HIST_MAX + s_hist_head[s0]];
        p->t = t; for (int k = 0; k < 4; ++k) p->v[k] = v[k];
        s_hist_head[s0] = (s_hist_head[s0] + 1) % HIST_MAX;
        if (s_hist_n[s0] < HIST_MAX) s_hist_n[s0]++;
        s_hist_air[s0] = true;
        xSemaphoreGive(s_hist_mtx);
    }
    cJSON_Delete(r);
}

/* {devices:[{slot,t:[...],temp:[...],hum:[...],co2:[...],pm25:[...]}]} for the Charts
 * tab. dev=0 -> all air-quality slots; else just that slot. Caller frees. */
cJSON *portal_history_json(int dev)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "devices");
    if (!s_hist || !s_hist_mtx) return o;
    xSemaphoreTake(s_hist_mtx, portMAX_DELAY);
    for (int s = 0; s < HIST_DEVS; ++s) {
        if (!s_hist_air[s]) continue;
        if (dev > 0 && dev != s + 1) continue;
        cJSON *dj = cJSON_CreateObject();
        cJSON_AddNumberToObject(dj, "slot", s + 1);
        cJSON *at = cJSON_AddArrayToObject(dj, "t");
        cJSON *aT = cJSON_AddArrayToObject(dj, "temp");
        cJSON *aH = cJSON_AddArrayToObject(dj, "hum");
        cJSON *aC = cJSON_AddArrayToObject(dj, "co2");
        cJSON *aP = cJSON_AddArrayToObject(dj, "pm25");
        int n = s_hist_n[s];
        int start = (s_hist_head[s] - n + HIST_MAX) % HIST_MAX;
        for (int i = 0; i < n; ++i) {
            hpt *p = &s_hist[s * HIST_MAX + (start + i) % HIST_MAX];
            cJSON_AddItemToArray(at, cJSON_CreateNumber(p->t));
            cJSON *dst[4] = { aT, aH, aC, aP };
            for (int k = 0; k < 4; ++k)
                cJSON_AddItemToArray(dst[k], isnan(p->v[k]) ? cJSON_CreateNull()
                                     : cJSON_CreateNumber(k < 2 ? roundf(p->v[k] * 10) / 10 : roundf(p->v[k])));
        }
        cJSON_AddItemToArray(arr, dj);
    }
    xSemaphoreGive(s_hist_mtx);
    return o;
}

/* Invoke a TimeSynchronization (0x0038) command on ep0 of a device, passing `args`
 * as an esp-matter JSON-TLV string. Built via cJSON so the args string is escaped
 * correctly inside the request. Returns true on an OK response. */
static bool invoke_timesync(int slot, int command, const char *args)
{
    cJSON *req = cJSON_CreateObject();
    if (!req) return false;
    cJSON_AddStringToObject(req, "cmd", "invoke");
    cJSON_AddNumberToObject(req, "dev", slot);
    cJSON_AddNumberToObject(req, "ep", 0);
    cJSON_AddNumberToObject(req, "cluster", 0x0038);
    cJSON_AddNumberToObject(req, "command", command);
    cJSON_AddStringToObject(req, "args", args);
    char *s = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!s) return false;
    char resp[256];
    bool ok = usb_bridge_request(s, resp, sizeof(resp), 9000);
    free(s);
    return ok;
}

/* Give a device local wall-clock time.
 *
 * The correct Matter way is SetUTCTime(UTC) + SetTimeZone + SetDSTOffset, letting the
 * device compute LocalTime = UTC + tz + dst. But this device family's SetDSTOffset is
 * broken (rejects every list, even empty), so LocalTime stays null and the only
 * readable clock is UTCTime. So we write LOCAL time (UTC + tz) into UTCTime and
 * normalize TimeZone to 0 - the device then reads local time directly. (User's call:
 * a readable local clock beats a spec-correct-but-null LocalTime on this hardware.) */
static bool devtime_local(int slot)
{
    time_t utc = time(nullptr);
    if (utc < 946684800L) return false;   /* clock not set (before the Matter epoch) */
    long long local_us =
        (long long)(utc + (time_t)net_tz_offset_min() * 60 - 946684800LL) * 1000000LL;

    char args[80];
    snprintf(args, sizeof(args), "{\"0:U64\":\"%lld\",\"1:U8\":2}", local_us);   /* Granularity=Seconds */
    if (!invoke_timesync(slot, 0x00, args)) return false;                        /* SetUTCTime(local) */

    /* Normalize TimeZone to 0 - the clock we just wrote already holds local wall time. */
    invoke_timesync(slot, 0x02, "{\"0:ARR-OBJ\":[{\"0:I32\":0,\"1:U64\":0}]}");  /* best-effort */
    return true;
}

/* Push local wall time (UTC+tz) into the HUB clock. The C6 loses its clock on
 * every reboot and then treats all schedule/calendar rules as false (fail-safe),
 * so app_task re-asserts this EVERY poll - one cheap round-trip that self-heals a
 * rebooted C6 within a single poll, without depending on catching the USB drop.
 * Returns true iff the C6 accepted it. */
static bool hub_push_time(void)
{
    char iso[40];
    build_local_iso(iso, sizeof(iso));
    char cmd[96], resp[256];
    snprintf(cmd, sizeof(cmd), "{\"cmd\":\"settime\",\"iso\":\"%s\"}", iso);
    if (!usb_bridge_request(cmd, resp, sizeof(resp), 3000)) {
        ESP_LOGW(TAG, "settime -> C6 failed");
        return false;
    }
    ESP_LOGD(TAG, "settime -> C6: %s", iso);       /* routine: quiet (runs every poll) */
    return true;
}

/* Push local time (UTC+tz) to each online device's TimeSync cluster. Runs only on
 * (re)connect + daily: devices keep their own clocks across a C6 reboot, so this is
 * the slow-cadence half. The C6 skips devices with no TimeSync cluster. */
static void devices_push_time(char *buf)
{
    if (!usb_bridge_request("{\"cmd\":\"status\"}", buf, BUFSZ, 3000)) return;
    cJSON *r = cJSON_Parse(buf);
    if (!r) return;
    cJSON *res  = cJSON_GetObjectItem(r, "result");
    cJSON *devs = res ? cJSON_GetObjectItem(res, "devices") : nullptr;
    int done = 0;
    if (cJSON_IsArray(devs)) {
        cJSON *d;
        cJSON_ArrayForEach(d, devs) {
            cJSON *on   = cJSON_GetObjectItem(d, "online");
            cJSON *slot = cJSON_GetObjectItem(d, "slot");
            if (cJSON_IsTrue(on) && cJSON_IsNumber(slot)) {
                if (devtime_local(slot->valueint)) done++;
            }
        }
    }
    cJSON_Delete(r);
    ESP_LOGI(TAG, "local time (UTC+tz) pushed to %d online device(s)", done);
}

static void app_task(void *arg)
{
    char *buf = (char *)heap_caps_malloc(BUFSZ, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (char *)malloc(BUFSZ);
    if (!buf) { ESP_LOGE(TAG, "buf alloc failed"); vTaskDelete(nullptr); return; }

    for (;;) {
        c6_link_state_t link = usb_bridge_link_state();
        /* Re-push per-device time whenever the C6 (re)connects. (The hub clock is
         * re-asserted every poll below, so it self-heals regardless of this edge.) */
        if (link != s_prev_link) {
            if (link == C6_ONLINE) s_synced = false;
            s_prev_link = link;
            web_push_link();                 /* tell browsers the hub link changed */
        }

        if (link == C6_ONLINE) {
            poll_once(buf);
            hist_ingest(buf);                /* record air-quality trend samples */
            web_push_state();                /* broadcast the fresh snapshot */
            if (net_time_valid()) {
                /* Re-assert the hub clock EVERY poll: the C6 boots with no clock
                 * (schedule/calendar rules stay dormant until it is set), so this
                 * self-heals within one poll of any C6 reboot - not only a detected
                 * USB drop. Only latch the daily/reconnect device push on success,
                 * so a transient failure retries instead of silently giving up. */
                bool hub_ok = hub_push_time();
                time_t t = time(nullptr);
                struct tm tmv;
                gmtime_r(&t, &tmv);
                if (hub_ok && (!s_synced || tmv.tm_yday != s_last_yday)) {
                    devices_push_time(buf);  /* per-device time: on (re)connect + daily */
                    s_synced    = true;
                    s_last_yday = tmv.tm_yday;
                }
            }
        }
        int poll_s = cfg_get()->poll_s;
        vTaskDelay(pdMS_TO_TICKS((poll_s < 1 ? 1 : poll_s) * 1000));
    }
}

/* Force a time re-sync on the next loop (e.g. after the tz changed). */
void portal_resync(void) { s_synced = false; }

void portal_start(void)
{
    s_cache     = (char *)heap_caps_malloc(BUFSZ, MALLOC_CAP_SPIRAM);
    s_cache_mtx = xSemaphoreCreateMutex();
    if (!s_cache || !s_cache_mtx) { ESP_LOGE(TAG, "alloc failed - portal not started"); return; }
    s_cache_len = 0;
    s_hist      = (hpt *)heap_caps_calloc(HIST_DEVS * HIST_MAX, sizeof(hpt), MALLOC_CAP_SPIRAM);
    s_hist_mtx  = xSemaphoreCreateMutex();
    if (!s_hist || !s_hist_mtx) ESP_LOGW(TAG, "history buffer alloc failed - charts disabled");
    xTaskCreate(app_task, "portal", 6144, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "portal app task started (poll %d s)", cfg_get()->poll_s);
}
