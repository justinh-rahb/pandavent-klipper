idf_component_register(
    SRCS "pv_policy.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash pv_motor pv_moonraker pv_evlog
)
