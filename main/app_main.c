#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "remote.h"
#include "remote_ui.h"

#define LVGL_BUFFER_LINES 24

static esp_lcd_panel_handle_t display_panel;
static volatile bool touch_pressed;
static volatile int32_t touch_x;
static volatile int32_t touch_y;

static uint32_t lvgl_tick(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    esp_lcd_panel_draw_bitmap(display_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1,
                              pixel_map);
    lv_display_flush_ready(display);
}

static void touch_read(lv_indev_t *input_device, lv_indev_data_t *data)
{
    static bool was_pressed;
    bool pressed = touch_pressed;
    data->point.x = touch_x;
    data->point.y = touch_y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (pressed != was_pressed) {
        ESP_LOGI("touch", "LVGL input state=%d, x=%ld, y=%ld",
                 data->state, (long)data->point.x, (long)data->point.y);
        was_pressed = pressed;
    }
}

static void touch_task(void *argument)
{
    bool first_poll = true;
    ESP_LOGI("touch", "Touch polling started");
    while (true) {
        int32_t x;
        int32_t y;
        bool pressed = board_touch_read(&x, &y);
        if (pressed) {
            touch_x = x;
            touch_y = y;
        }
        touch_pressed = pressed;
        if (first_poll) {
            ESP_LOGI("touch", "First GT911 poll completed");
            first_poll = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void lvgl_task(void *argument)
{
    lv_display_t *display = lv_display_create(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);

    const size_t buffer_size = BOARD_LCD_H_RES * LVGL_BUFFER_LINES * sizeof(uint16_t);
    void *draw_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(draw_buffer ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(display, draw_buffer, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, display_flush);

    lv_indev_t *input_device = lv_indev_create();
    lv_indev_set_type(input_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input_device, display);
    lv_indev_set_read_cb(input_device, touch_read);

    remote_ui_create();
    TickType_t last_status_update = 0;
    TickType_t last_heartbeat = 0;
    while (true) {
        lv_timer_handler();
        TickType_t now = xTaskGetTickCount();
        if (now - last_status_update >= pdMS_TO_TICKS(250)) {
            remote_ui_update();
            last_status_update = now;
        }
        if (now - last_heartbeat >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI("ui", "LVGL alive; connected=%d, pairing_failed=%d",
                     remote_is_connected(), remote_pairing_failed());
            last_heartbeat = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    ESP_ERROR_CHECK(board_display_init(&display_panel));
    ESP_ERROR_CHECK(board_touch_init());
    ESP_ERROR_CHECK(remote_init());

    lv_init();
    lv_tick_set_cb(lvgl_tick);
    ESP_ERROR_CHECK(xTaskCreate(touch_task, "touch_task", 4096, NULL, 3, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(lvgl_task, "lvgl_task", 12288, NULL, 4, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}
