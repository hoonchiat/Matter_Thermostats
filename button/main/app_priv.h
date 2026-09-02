/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

typedef void *app_driver_handle_t;

/** Initialize the onboard RGB LED (used only as the pairing-mode indicator). */
app_driver_handle_t app_driver_led_init();

/** Initialize the BOOT button and wire single/double/long-press to Switch events.
 *  @param switch_endpoint_id  the Generic Switch endpoint the events are raised on. */
app_driver_handle_t app_driver_button_init(uint16_t switch_endpoint_id);

/** Pairing-mode indicator: slow-flash the RGB LED (blue) while a commissioning
 *  window is open, LED off when it closes.
 *  @param[in] pairing true = enter pairing indication, false = leave it. */
void app_driver_pairing_indicate(bool pairing);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif
