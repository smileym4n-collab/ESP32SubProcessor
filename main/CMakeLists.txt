idf_component_register(
    SRCS "main.c" "board_io.c" "pcm1863.c" "audio.c" "protection.c" "settings.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES dsp_core esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_timer hal
    PRIV_REQUIRES console esp_system nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
