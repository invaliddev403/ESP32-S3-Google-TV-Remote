idf_component_register(
    SRCS
        "app_main.c"
        "board.c"
        "ble_remote.c"
        "remote_ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hid esp_lcd esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_timer nvs_flash lvgl
)
