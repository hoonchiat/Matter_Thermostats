/*
 * app_main.cpp — boot the Matter Thread Thermostat.
 *   1. init NVS, load persisted config into g_state
 *   2. bring up the Matter data model + stack (Thread + BLE commissioning)
 *   3. start the sensor / control / UI tasks
 */
#include "app_priv.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "ui_oled.h"   /* UI_SCREEN_* enum */

#define TAG "app_main"

/* ---- global state definitions (declared extern in app_priv.h) ------------- */
app_state_t      g_state;
QueueHandle_t    g_event_q   = NULL;
SemaphoreHandle_t g_state_mtx = NULL;

void app_post_event(app_event_type_t type, int32_t value)
{
    if (!g_event_q) return;
    app_event_t e = { .type = type, .value = value };
    /* Safe from ISR-free contexts (drivers/timers/Matter callbacks). */
    xQueueSend(g_event_q, &e, 0);
}

extern "C" void app_main(void)
{
    /* NVS is needed by both our config and the Matter stack. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    g_state_mtx = xSemaphoreCreateMutex();
    g_event_q   = xQueueCreate(16, sizeof(app_event_t));

    memset(&g_state, 0, sizeof(g_state));
    app_nvs_load(&g_state.cfg);
    g_state.screen = UI_SCREEN_HOME;
    g_state.active_setpoint = 0;
    g_state.commissioned = false;   /* set true by the commissioning event */

    ESP_LOGI(TAG, "boot: mode=%d heat=%d cool=%d units=%s ntc=%d",
             g_state.cfg.mode, g_state.cfg.heat_set_c100, g_state.cfg.cool_set_c100,
             g_state.cfg.fahrenheit ? "F" : "C", g_state.cfg.ntc_type);

    /* Bring up local I/O + control first so the device is usable even before
     * it is commissioned (standalone thermostat operation). */
    app_control_start();

    /* Then start Matter (Thread + BLE commissioning). */
    if (app_matter_start() != ESP_OK) {
        ESP_LOGE(TAG, "Matter failed to start");
    }
}
