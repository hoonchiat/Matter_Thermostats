/*
 * web.cpp - HTTP server + WebSocket relay (see web.h).
 *
 * Tasks/flow:
 *   httpd (esp_http_server)  - serves "/" (the SPA) and "/ws" (WebSocket).
 *   web_worker               - dequeues browser commands, calls usb_bridge_request()
 *                              (blocking on USB), and async-sends the reply back. This
 *                              keeps the httpd task responsive (never blocks on USB).
 *   async sends              - state/link/reply frames are queued onto the httpd task
 *                              via httpd_queue_work + httpd_ws_send_frame_async.
 */
#include "web.h"
#include "usb_bridge.h"
#include "net.h"
#include "portal.h"
#include "cfg.h"
#include "esp_system.h"

#include <cstring>
#include <cstdio>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "lwip/sockets.h"

#include "cJSON.h"

static const char *TAG = "web";

#define MAX_WS_CLIENTS   4
#define REQ_Q_DEPTH      8
#define RESP_SZ          12288     /* C6 response buffer (backup can be several KB) */
#define STATE_SZ         4096

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t   s_httpd = nullptr;
static int              s_clients[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_clients_mtx = nullptr;
static QueueHandle_t    s_req_q = nullptr;

/* A browser command in flight to the C6 (owned by the worker). */
struct web_req_t {
    int    fd;          /* WS socket to reply on */
    char  *cmd_json;    /* {cmd,...} without tag/id (malloc'd) */
    cJSON *tag;         /* client correlation tag (detached; may be NULL) */
};

/* ---- client registry --------------------------------------------------- */
static void client_add(int fd)
{
    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) if (s_clients[i] == fd) { xSemaphoreGive(s_clients_mtx); return; }
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) if (s_clients[i] < 0) { s_clients[i] = fd; break; }
    xSemaphoreGive(s_clients_mtx);
}
static void client_remove(int fd)
{
    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) if (s_clients[i] == fd) s_clients[i] = -1;
    xSemaphoreGive(s_clients_mtx);
}

/* ---- async frame send -------------------------------------------------- */
struct async_send_t { int fd; char *data; };

static void async_send_cb(void *arg)
{
    async_send_t *a = (async_send_t *)arg;
    httpd_ws_frame_t f = {};
    f.type    = HTTPD_WS_TYPE_TEXT;
    f.payload = (uint8_t *)a->data;
    f.len     = strlen(a->data);
    if (httpd_ws_send_frame_async(s_httpd, a->fd, &f) != ESP_OK) client_remove(a->fd);
    free(a->data);
    free(a);
}

/* Queue a text frame to one client (runs the send on the httpd task). */
static void ws_send(int fd, const char *json)
{
    if (!s_httpd || fd < 0) return;
    async_send_t *a = (async_send_t *)malloc(sizeof(*a));
    if (!a) return;
    a->fd = fd;
    a->data = strdup(json);
    if (!a->data) { free(a); return; }
    if (httpd_queue_work(s_httpd, async_send_cb, a) != ESP_OK) { free(a->data); free(a); }
}

static void ws_broadcast(const char *json)
{
    int fds[MAX_WS_CLIENTS], n = 0;
    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) if (s_clients[i] >= 0) fds[n++] = s_clients[i];
    xSemaphoreGive(s_clients_mtx);
    for (int i = 0; i < n; ++i) ws_send(fds[i], json);
}

static bool any_clients(void)
{
    xSemaphoreTake(s_clients_mtx, portMAX_DELAY);
    bool any = false;
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) if (s_clients[i] >= 0) { any = true; break; }
    xSemaphoreGive(s_clients_mtx);
    return any;
}

/* ---- push builders ----------------------------------------------------- */
/* {"type":"state","devices":[...]} from the portal's cached status; NULL if none. */
static char *build_state_json(void)
{
    char *status = (char *)heap_caps_malloc(STATE_SZ, MALLOC_CAP_SPIRAM);
    if (!status) return nullptr;
    char *out = nullptr;
    if (portal_get_status(status, STATE_SZ) > 0) {
        cJSON *r = cJSON_Parse(status);
        cJSON *res  = r ? cJSON_GetObjectItem(r, "result") : nullptr;
        cJSON *devs = res ? cJSON_GetObjectItem(res, "devices") : nullptr;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "state");
        cJSON_AddItemToObject(o, "devices",
                              devs ? cJSON_Duplicate(devs, true) : cJSON_CreateArray());
        if (res && cJSON_IsTrue(cJSON_GetObjectItem(res, "restore_pending")))
            cJSON_AddBoolToObject(o, "restore_pending", true);
        out = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        cJSON_Delete(r);
    }
    free(status);
    return out;
}

static char *build_link_json(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "link");
    cJSON_AddStringToObject(o, "c6", usb_bridge_link_name());
    cJSON_AddStringToObject(o, "ip", net_ip_str());
    cJSON_AddStringToObject(o, "ap", net_ap_ssid());
    if (net_time_valid()) {
        time_t local = time(nullptr) + (time_t)net_tz_offset_min() * 60;
        struct tm tmv; gmtime_r(&local, &tmv);
        char ts[24];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        cJSON_AddStringToObject(o, "clock", ts);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

void web_push_state(void)
{
    if (!s_httpd || !any_clients()) return;
    char *s = build_state_json();
    if (s) { ws_broadcast(s); free(s); }
}
void web_push_link(void)
{
    if (!s_httpd || !any_clients()) return;
    char *s = build_link_json();
    if (s) { ws_broadcast(s); free(s); }
}

/* Reboot shortly after, so the s3_reboot reply reaches the browser first. */
static void reboot_task(void *arg) { vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }

/* ---- worker: browser command -> C6 -> reply ---------------------------- */
static void web_worker(void *arg)
{
    char *resp = (char *)heap_caps_malloc(RESP_SZ, MALLOC_CAP_SPIRAM);
    if (!resp) { ESP_LOGE(TAG, "worker buf alloc failed"); vTaskDelete(nullptr); return; }

    web_req_t *rq;
    while (xQueueReceive(s_req_q, &rq, portMAX_DELAY) == pdTRUE) {
        cJSON *out = cJSON_CreateObject();
        cJSON_AddStringToObject(out, "type", "reply");
        if (rq->tag) cJSON_AddItemToObject(out, "tag", rq->tag);   /* out now owns tag */

        /* S3-local commands (config / reboot) are handled here, NOT forwarded to the C6. */
        cJSON *in = cJSON_Parse(rq->cmd_json);
        const char *cmd = in ? cJSON_GetStringValue(cJSON_GetObjectItem(in, "cmd")) : nullptr;
        bool handled = true;
        if (cmd && !strcmp(cmd, "cfg_get")) {
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON_AddItemToObject(out, "result", cfg_to_json());
        } else if (cmd && !strcmp(cmd, "history")) {
            cJSON *dv = cJSON_GetObjectItem(in, "dev");
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON_AddItemToObject(out, "result", portal_history_json(cJSON_IsNumber(dv) ? dv->valueint : 0));
        } else if (cmd && !strcmp(cmd, "cfg_set")) {
            char err[96] = ""; bool reboot = false, tz_changed = false;
            bool ok2 = cfg_apply_json(in, err, sizeof(err), &reboot, &tz_changed);
            if (ok2 && tz_changed) {                     /* keep the C6's tz + clock in step */
                char tzcmd[64];
                snprintf(tzcmd, sizeof(tzcmd), "{\"cmd\":\"tz\",\"offset\":%d}", cfg_get()->tz_min);
                usb_bridge_request(tzcmd, resp, RESP_SZ, 4000);
                portal_resync();
            }
            cJSON_AddBoolToObject(out, "ok", ok2);
            if (ok2) { cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "reboot", reboot); cJSON_AddItemToObject(out, "result", r); }
            else cJSON_AddStringToObject(out, "error", err);
        } else if (cmd && !strcmp(cmd, "s3_reboot")) {
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON *r = cJSON_CreateObject(); cJSON_AddBoolToObject(r, "rebooting", true); cJSON_AddItemToObject(out, "result", r);
            xTaskCreate(reboot_task, "reboot", 2048, nullptr, 5, nullptr);
        } else {
            handled = false;                             /* forward to the C6 */
            bool ok = usb_bridge_request(rq->cmd_json, resp, RESP_SZ, 9000);
            if (ok) {
                cJSON *r = cJSON_Parse(resp);
                if (r) {
                    cJSON_AddBoolToObject(out, "ok", cJSON_IsTrue(cJSON_GetObjectItem(r, "ok")));
                    cJSON *res = cJSON_GetObjectItem(r, "result");
                    cJSON *err = cJSON_GetObjectItem(r, "error");
                    if (res) cJSON_AddItemToObject(out, "result", cJSON_Duplicate(res, true));
                    if (err) cJSON_AddItemToObject(out, "error",  cJSON_Duplicate(err, true));
                    cJSON_Delete(r);
                } else {
                    cJSON_AddBoolToObject(out, "ok", false);
                    cJSON_AddStringToObject(out, "error", "malformed hub response");
                }
            } else {
                cJSON_AddBoolToObject(out, "ok", false);
                cJSON_AddStringToObject(out, "error", "hub not responding");
            }
        }
        cJSON_Delete(in);
        (void)handled;

        char *outs = cJSON_PrintUnformatted(out);
        cJSON_Delete(out);                 /* frees tag too */
        if (outs) { ws_send(rq->fd, outs); free(outs); }

        free(rq->cmd_json);
        free(rq);

        /* A command may have changed device state - refresh everyone promptly. */
        web_push_state();
    }
}

/* ---- HTTP handlers ----------------------------------------------------- */
static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

/* Captive portal: any unknown path (incl. the OS connectivity-check URLs) 302s to
 * the portal root, which makes the phone/laptop pop the captive sheet on connect and
 * lets the user type any address. Served on all interfaces incl. the SoftAP. */
static esp_err_t captive_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake done - register the client and push current state+link. */
        int fd = httpd_req_to_sockfd(req);
        client_add(fd);
        ESP_LOGI(TAG, "WS client connected (fd=%d)", fd);
        char *st = build_state_json(); if (st) { ws_send(fd, st); free(st); }
        char *lk = build_link_json();  if (lk) { ws_send(fd, lk); free(lk); }
        return ESP_OK;
    }

    /* Incoming frame: get length, then payload. */
    httpd_ws_frame_t f = {};
    f.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
    if (e != ESP_OK) return e;
    if (f.len == 0 || f.len > 12288) return ESP_OK;   /* restore carries the whole config inline */

    uint8_t *buf = (uint8_t *)calloc(1, f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    e = httpd_ws_recv_frame(req, &f, f.len);
    if (e != ESP_OK) { free(buf); return e; }

    /* Parse {cmd,...,tag}; detach tag, forward the rest to the worker. */
    cJSON *in = cJSON_Parse((char *)buf);
    free(buf);
    if (!in) return ESP_OK;
    cJSON *tag = cJSON_DetachItemFromObject(in, "tag");
    char *cmd_json = cJSON_PrintUnformatted(in);
    cJSON_Delete(in);
    if (!cmd_json) { cJSON_Delete(tag); return ESP_OK; }

    web_req_t *rq = (web_req_t *)malloc(sizeof(*rq));
    if (!rq) { free(cmd_json); cJSON_Delete(tag); return ESP_OK; }
    rq->fd = httpd_req_to_sockfd(req);
    rq->cmd_json = cmd_json;
    rq->tag = tag;
    if (xQueueSend(s_req_q, &rq, 0) != pdTRUE) {
        ESP_LOGW(TAG, "req queue full - dropping command");
        free(rq->cmd_json); cJSON_Delete(rq->tag); free(rq);
    }
    return ESP_OK;
}

/* Socket closed - drop it from the client registry (we own the close now). */
static void on_close(httpd_handle_t hd, int fd)
{
    client_remove(fd);
    close(fd);
}

/* ---- start ------------------------------------------------------------- */
void web_start(void)
{
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) s_clients[i] = -1;
    s_clients_mtx = xSemaphoreCreateMutex();
    s_req_q = xQueueCreate(REQ_Q_DEPTH, sizeof(web_req_t *));
    if (!s_clients_mtx || !s_req_q) { ESP_LOGE(TAG, "alloc failed - web not started"); return; }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;
    cfg.max_open_sockets  = 6;
    cfg.lru_purge_enable = true;
    cfg.close_fn         = on_close;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed"); return; }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get, .user_ctx = nullptr };
    httpd_register_uri_handler(s_httpd, &root);

    httpd_uri_t ws = {};
    ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = ws_handler; ws.is_websocket = true;
    httpd_register_uri_handler(s_httpd, &ws);

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, captive_redirect);  /* captive portal */

    xTaskCreate(web_worker, "web_worker", 6144, nullptr, 5, nullptr);

    /* Advertise _http._tcp so browsers find http://esp32.local/. */
    mdns_service_add("Matter Gateway Portal", "_http", "_tcp", 80, nullptr, 0);

    ESP_LOGI(TAG, "web server up (port 80, /ws WebSocket)");
}
