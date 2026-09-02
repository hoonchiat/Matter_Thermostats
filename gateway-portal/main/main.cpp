/*
 * Matter Gateway Portal - ESP32-S3 firmware (mh_s3_portal).
 *
 * Bridges browsers on the office Wi-Fi to an ESP32-C6 Matter hub running the v1.8/
 * v1.9 USB JSON protocol. The S3 is:
 *   - a USB-host CDC-ACM bridge to the C6's native USB (NDJSON, 115200 8N1),
 *   - a Wi-Fi STA serving a single-page app + WebSocket at http://esp32.local/,
 *   - a state cache + time-sync driver (NTP -> hub `settime` -> device `devtime`).
 *
 * Stack: pure ESP-IDF v5.4.4 - esp_http_server (native WS), usb_host + cdc_acm_host,
 * cJSON, esp_wifi / mdns / esp_sntp. See "Matter Gateway Portal - Design Spec v2".
 *
 * Phase 2 (current): + Wi-Fi STA / mDNS / SNTP (net) and the portal app task (poll +
 * time-sync). Web server + WebSocket + SPA land in later phases.
 */
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "usb_bridge.h"
#include "net.h"
#include "portal.h"
#include "web.h"
#include "cfg.h"

static const char *TAG = "s3portal";

#define MH_S3_VERSION "0.5.0-dev (phase5 config)"

extern "C" void app_main(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    printf("\r\n=== Matter Gateway Portal (ESP32-S3) %s ===\r\n", MH_S3_VERSION);
    printf("[s3] chip=%s cores=%d silicon-rev=%d\r\n",
           CONFIG_IDF_TARGET, ci.cores, ci.revision);

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t iram_free   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    printf("[s3] PSRAM total=%u KB free=%u KB | internal free=%u KB\r\n",
           (unsigned)(psram_total / 1024), (unsigned)(psram_free / 1024),
           (unsigned)(iram_free / 1024));
    if (psram_total == 0)
        ESP_LOGW(TAG, "PSRAM not detected - check SPIRAM sdkconfig or that the board is N16R8");

    /* NVS is needed later (Wi-Fi, config); init it early so all phases can use it. */
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Phase 5: load persisted config (Wi-Fi/AP/hostname/NTP/tz/poll) before net uses it. */
    cfg_load();

    /* Phase 1: USB-host CDC-ACM bridge to the C6. */
    usb_bridge_start();
    /* Phase 2: Wi-Fi STA + mDNS + SNTP. */
    net_start();
    /* Phase 3: HTTP server + WebSocket - start BEFORE the portal task, which pushes
     * state/link to it (and would otherwise touch web mutexes before they exist). */
    web_start();
    /* Portal app task (poll + time-sync + web pushes). */
    portal_start();

    ESP_LOGI(TAG, "phase-3 up; bridge + Wi-Fi + portal + web/WS running");

    /* Idle - the bridge runs in its own tasks. Later phases start Wi-Fi + web/WS here. */
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
