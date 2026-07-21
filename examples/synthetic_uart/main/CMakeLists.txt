idf_component_register(
    SRCS "app_main.c" "synthetic_data.c"
    INCLUDE_DIRS "."
    REQUIRES driver json esp_timer
)
