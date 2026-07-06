idf_component_register(
    SRCS "pv_portal.c" "pv_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_netif lwip pv_wifi pv_moonraker pv_policy
)
