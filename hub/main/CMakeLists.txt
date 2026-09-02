idf_component_register(
    SRCS "main.cpp" "inspector.cpp" "bindings.cpp" "logic.cpp" "schedule.cpp" "comm.cpp" "dash.cpp" "tlv_decode.cpp" "status_led.cpp" "ble_scan.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        espressif__esp_matter
        nvs_flash
        esp_event
        esp_netif
        esp_wifi
        lwip
        driver
        esp_driver_rmt
        bt
        freertos
        openthread
        json
)

# libopenthread_br.a (BORDER_ROUTER) calls mdns_service_* symbols.
# espressif__mdns must appear AFTER libopenthread_br.a in the link line.
# Adding it as an INTERFACE dep of openthread achieves this: the linker sees
# [openthread compiled lib] -> [libopenthread_br.a] -> [espressif__mdns].
idf_component_get_property(openthread_lib openthread COMPONENT_LIB)
target_link_libraries(${openthread_lib} INTERFACE __idf_espressif__mdns)

