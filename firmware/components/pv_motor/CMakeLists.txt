idf_component_register(
    SRCS "pv_motor.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc pv_board
)
