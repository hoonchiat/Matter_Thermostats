# The managed components (usb_host_cdc_acm, mdns) are added automatically from
# idf_component.yml, so they need not be listed in REQUIRES. Core deps only here.
idf_component_register(
    SRCS
        "main.cpp"
        "usb_bridge.cpp"
        "net.cpp"
        "portal.cpp"
        "web.cpp"
        "cfg.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_timer
        nvs_flash
        json
        usb
    EMBED_TXTFILES
        "www/index.html"
)
# Note: espressif__usb_host_cdc_acm + espressif__mdns are pulled in automatically
# from main/idf_component.yml, so they need not be listed in REQUIRES.
