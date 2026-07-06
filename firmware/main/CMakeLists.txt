idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES driver pv_board pv_motor pv_wifi pv_moonraker pv_policy pv_portal
)
