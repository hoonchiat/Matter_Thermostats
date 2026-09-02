/*
 * usb_bridge.cpp - USB-host CDC-ACM bridge to the ESP32-C6 (see usb_bridge.h).
 *
 * Tasks:
 *   usb_lib_task   - pumps usb_host_lib_handle_events() (USB Host Library).
 *   (cdc driver)   - the cdc_acm_host driver's own task (created by _install).
 *   rx_task        - pops reassembled NDJSON lines and correlates them to waiters.
 *   bridge_task    - open/reconnect state machine + a periodic status self-test.
 *
 * RX path: the CDC data_cb (driver-task context) reassembles '\n'-terminated lines
 * into PSRAM-allocated copies and queues them; rx_task parses each line's "id" and
 * either wakes the matching requester or logs it as unsolicited.
 */
#include "usb_bridge.h"

#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"

#include "cJSON.h"

static const char *TAG = "bridge";

/* The C6's native USB (USB-Serial-JTAG). We open by ANY vid/pid (only the C6 is on
 * the host port) but keep the expected identity for logging. */
#define C6_EXPECT_VID   0x303A
#define C6_EXPECT_PID   0x1001
#define CDC_IFACE_IDX   0

#define LINE_MAX        8192         /* max NDJSON line (backup can be large) */
#define RX_QUEUE_DEPTH  8

/* Watchdog: after this many consecutive request failures while the link still
 * claims ONLINE, force a CDC reopen. Covers the "zombie CDC" case - the C6 warm-
 * resets or the USB host wedges so the handle stays open but nothing replies and
 * no disconnect event fires. A reopen when the link is actually fine is cheap and
 * self-corrects, so err on recovering. */
#define BRIDGE_FAIL_LIMIT  5

/* ---- state ------------------------------------------------------------- */
static volatile c6_link_state_t s_link = C6_OFFLINE;
static cdc_acm_dev_hdl_t        s_cdc  = nullptr;
static volatile bool            s_need_reopen = false;
static volatile int             s_consec_fail = 0;   /* consecutive request failures (watchdog) */

static QueueHandle_t     s_rx_q       = nullptr;   /* holds char* (heap/PSRAM line copies) */
static SemaphoreHandle_t s_req_mutex  = nullptr;   /* one outstanding request */
static SemaphoreHandle_t s_resp_sem   = nullptr;   /* signalled when the pending response lands */

static volatile int  s_pending_id = -1;
static char         *s_resp_dst   = nullptr;
static size_t        s_resp_cap   = 0;
static volatile bool s_resp_ok    = false;
static int           s_id_counter = 0;

/* Line reassembly (driver-task context only). */
static char  *s_line = nullptr;
static size_t s_line_pos = 0;

/* ---- helpers ----------------------------------------------------------- */
const char *usb_bridge_link_name(void)
{
    switch (s_link) {
    case C6_ONLINE:        return "online";
    case C6_COMMISSIONING: return "commissioning";
    default:               return "offline";
    }
}
c6_link_state_t usb_bridge_link_state(void) { return s_link; }

/* Deliver a completed line to the pending waiter (if its id matches) else log it. */
static void handle_rx_line(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        ESP_LOGW(TAG, "rx: non-JSON line (%u bytes) dropped", (unsigned)strlen(line));
        return;
    }
    cJSON *jid = cJSON_GetObjectItem(root, "id");
    int id = cJSON_IsNumber(jid) ? jid->valueint : -1;

    if (id >= 0 && id == s_pending_id) {
        if (s_resp_dst && s_resp_cap) {
            strncpy(s_resp_dst, line, s_resp_cap - 1);
            s_resp_dst[s_resp_cap - 1] = '\0';
        }
        s_resp_ok = true;
        xSemaphoreGive(s_resp_sem);
    } else {
        /* Unsolicited or late (post-timeout) response - the C6 has no push channel,
         * so this is normally just a straggler. Log a short prefix. */
        ESP_LOGD(TAG, "rx: unmatched id=%d: %.80s", id, line);
    }
    cJSON_Delete(root);
}

/* ---- CDC callbacks (driver-task context) ------------------------------- */
static bool cdc_data_cb(const uint8_t *data, size_t len, void *arg)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = data[i];
        if (c == '\n') {
            if (s_line_pos > 0) {
                s_line[s_line_pos] = '\0';
                char *copy = (char *)heap_caps_malloc(s_line_pos + 1, MALLOC_CAP_SPIRAM);
                if (!copy) copy = (char *)malloc(s_line_pos + 1);
                if (copy) {
                    memcpy(copy, s_line, s_line_pos + 1);
                    if (xQueueSend(s_rx_q, &copy, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "rx queue full - line dropped");
                        free(copy);
                    }
                }
                s_line_pos = 0;
            }
        } else if (c != '\r') {
            if (s_line_pos < LINE_MAX - 1) {
                s_line[s_line_pos++] = (char)c;
            } else {
                ESP_LOGW(TAG, "rx: line > %d bytes - resetting accumulator", LINE_MAX);
                s_line_pos = 0;
            }
        }
    }
    return true;   /* data consumed - driver may reuse its RX buffer */
}

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *e, void *arg)
{
    switch (e->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "C6 CDC disconnected");
        s_need_reopen = true;              /* bridge_task closes + reopens */
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "C6 CDC error (err=%d)", e->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "C6 CDC serial-state 0x%04x", e->data.serial_state.val);
        break;
    default:
        break;
    }
}

/* ---- request/response -------------------------------------------------- */
bool usb_bridge_request(const char *cmd_json, char *resp, size_t resp_len, int timeout_ms)
{
    if (!s_cdc || s_link != C6_ONLINE) return false;

    /* Inject a fresh id into the caller's command object. */
    cJSON *root = cJSON_Parse(cmd_json);
    if (!root) return false;

    xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    int id = ++s_id_counter;
    cJSON_DeleteItemFromObject(root, "id");
    cJSON_AddNumberToObject(root, "id", id);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { xSemaphoreGive(s_req_mutex); return false; }

    size_t blen = strlen(body);
    char *line = (char *)malloc(blen + 2);
    bool ok = false;
    if (line) {
        memcpy(line, body, blen);
        line[blen] = '\n';
        line[blen + 1] = '\0';

        /* Arm the waiter before sending so a fast reply can't be missed. */
        s_resp_dst  = resp;
        s_resp_cap  = resp_len;
        s_resp_ok   = false;
        s_pending_id = id;
        xSemaphoreTake(s_resp_sem, 0);     /* drain any stale signal */

        esp_err_t err = cdc_acm_host_data_tx_blocking(s_cdc, (const uint8_t *)line, blen + 1,
                                                      timeout_ms > 0 ? timeout_ms : 1000);
        if (err == ESP_OK) {
            ok = (xSemaphoreTake(s_resp_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) && s_resp_ok;
        } else {
            ESP_LOGW(TAG, "tx failed: %s", esp_err_to_name(err));
        }
        s_pending_id = -1;
        free(line);

        /* Watchdog: a reply came back -> healthy; a run of misses while the link
         * still claims ONLINE means a zombie CDC (open but silent, no disconnect
         * event) -> force bridge_task to close + reopen it. */
        if (ok) {
            s_consec_fail = 0;
        } else if (++s_consec_fail >= BRIDGE_FAIL_LIMIT) {
            ESP_LOGW(TAG, "watchdog: %d consecutive request failures while online - reopening CDC",
                     s_consec_fail);
            s_consec_fail = 0;
            s_need_reopen = true;
        }
    }
    free(body);
    xSemaphoreGive(s_req_mutex);
    return ok;
}

/* ---- tasks ------------------------------------------------------------- */
static void usb_lib_task(void *arg)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void rx_task(void *arg)
{
    char *line;
    while (xQueueReceive(s_rx_q, &line, portMAX_DELAY) == pdTRUE) {
        handle_rx_line(line);
        free(line);
    }
}

static void bridge_task(void *arg)
{
    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 3000,
        .out_buffer_size = 512,
        .in_buffer_size  = 512,
        .event_cb = cdc_event_cb,
        .data_cb  = cdc_data_cb,
        .user_arg = nullptr,
    };

    while (true) {
        ESP_LOGI(TAG, "waiting for the C6 on the USB host port...");
        esp_err_t err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, CDC_IFACE_IDX,
                                          &dev_cfg, &s_cdc);
        if (err != ESP_OK || !s_cdc) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        s_need_reopen = false;
        s_line_pos = 0;
        s_consec_fail = 0;
        s_link = C6_ONLINE;
        ESP_LOGI(TAG, "C6 CDC open - link ONLINE");
        cdc_acm_host_desc_print(s_cdc);

        /* Hold the link until the device drops. The portal app task drives polling
         * + time-sync via usb_bridge_request(); the bridge is transport-only. */
        while (!s_need_reopen) vTaskDelay(pdMS_TO_TICKS(100));

        s_link = C6_OFFLINE;
        cdc_acm_host_close(s_cdc);
        s_cdc = nullptr;
        ESP_LOGW(TAG, "C6 CDC closed - link OFFLINE, reopening");
    }
}

/* ---- public start ------------------------------------------------------ */
void usb_bridge_start(void)
{
    s_line = (char *)heap_caps_malloc(LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!s_line) s_line = (char *)malloc(LINE_MAX);
    s_rx_q      = xQueueCreate(RX_QUEUE_DEPTH, sizeof(char *));
    s_req_mutex = xSemaphoreCreateMutex();
    s_resp_sem  = xSemaphoreCreateBinary();
    if (!s_line || !s_rx_q || !s_req_mutex || !s_resp_sem) {
        ESP_LOGE(TAG, "alloc failed - bridge not started");
        return;
    }

    usb_host_config_t host_config = {};      /* zero all fields (incl. fifo/peripheral defaults) */
    host_config.skip_phy_setup = false;
    host_config.intr_flags     = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr, 10, nullptr, 0);

    const cdc_acm_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = 11,
        .xCoreID                = 0,
        .new_dev_cb             = nullptr,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&drv_cfg));

    xTaskCreate(rx_task,     "bridge_rx", 4096, nullptr, 6, nullptr);
    xTaskCreate(bridge_task, "bridge",    5120, nullptr, 5, nullptr);
    ESP_LOGI(TAG, "USB bridge started");
}
