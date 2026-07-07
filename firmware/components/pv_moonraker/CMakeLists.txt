idf_component_register(
    SRCS "pv_moonraker.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash json esp_websocket_client mdns lwip
)
