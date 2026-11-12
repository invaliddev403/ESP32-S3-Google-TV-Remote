#include "board.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_CS_GPIO GPIO_NUM_39
#define LCD_SCK_GPIO GPIO_NUM_48
#define LCD_SDA_GPIO GPIO_NUM_47
#define LCD_BACKLIGHT_GPIO GPIO_NUM_38

#define TOUCH_I2C_PORT I2C_NUM_0
#define TOUCH_SDA_GPIO GPIO_NUM_19
#define TOUCH_SCL_GPIO GPIO_NUM_45
#define TOUCH_ADDRESS 0x5D
#define TOUCH_STATUS_REG 0x4E
#define TOUCH_REG_HIGH 0x81

typedef struct {
    uint8_t command;
    uint8_t data[16];
    uint8_t length;
    uint16_t delay_ms;
} lcd_command_t;

#define LCD_CMD(command, ...) \
    {command, {__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__}), 0}
#define LCD_CMD_NO_DATA_DELAY(command, delay) \
    {command, {0}, 0, delay}

static const lcd_command_t lcd_init_commands[] = {
    LCD_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10),
    LCD_CMD(0xC0, 0x3B, 0x00),
    LCD_CMD(0xC1, 0x0D, 0x02),
    LCD_CMD(0xC2, 0x31, 0x05),
    LCD_CMD(0xCD, 0x00),
    LCD_CMD(0xB0, 0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08,
            0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18),
    LCD_CMD(0xB1, 0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08,
            0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18),
    LCD_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11),
    LCD_CMD(0xB0, 0x60),
    LCD_CMD(0xB1, 0x32),
    LCD_CMD(0xB2, 0x07),
    LCD_CMD(0xB3, 0x80),
    LCD_CMD(0xB5, 0x49),
    LCD_CMD(0xB7, 0x85),
    LCD_CMD(0xB8, 0x21),
    LCD_CMD(0xC1, 0x78),
    LCD_CMD(0xC2, 0x78),
    LCD_CMD(0xE0, 0x00, 0x1B, 0x02),
    LCD_CMD(0xE1, 0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44),
    LCD_CMD(0xE2, 0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00),
    LCD_CMD(0xE3, 0x00, 0x00, 0x11, 0x11),
    LCD_CMD(0xE4, 0x44, 0x44),
    LCD_CMD(0xE5, 0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0,
            0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0),
    LCD_CMD(0xE6, 0x00, 0x00, 0x11, 0x11),
    LCD_CMD(0xE7, 0x44, 0x44),
    LCD_CMD(0xE8, 0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0,
            0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0),
    LCD_CMD(0xEB, 0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40),
    LCD_CMD(0xEC, 0x3C, 0x00),
    LCD_CMD(0xED, 0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA),
    LCD_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x13),
    LCD_CMD(0xE5, 0xE4),
    LCD_CMD(0xFF, 0x77, 0x01, 0x00, 0x00, 0x00),
    LCD_CMD(0x3A, 0x60),
    LCD_CMD_NO_DATA_DELAY(0x11, 120),
    LCD_CMD_NO_DATA_DELAY(0x29, 20),
};

static i2c_master_dev_handle_t touch_device;

static void lcd_spi_write(uint8_t value, bool data)
{
    gpio_set_level(LCD_SDA_GPIO, data);
    gpio_set_level(LCD_SCK_GPIO, 0);
    esp_rom_delay_us(1);
    gpio_set_level(LCD_SCK_GPIO, 1);
    esp_rom_delay_us(1);

    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(LCD_SDA_GPIO, (value >> bit) & 1);
        gpio_set_level(LCD_SCK_GPIO, 0);
        esp_rom_delay_us(1);
        gpio_set_level(LCD_SCK_GPIO, 1);
        esp_rom_delay_us(1);
    }
}

static void lcd_write_command(const lcd_command_t *command)
{
    gpio_set_level(LCD_CS_GPIO, 0);
    lcd_spi_write(command->command, false);
    for (size_t i = 0; i < command->length; i++) {
        lcd_spi_write(command->data[i], true);
    }
    gpio_set_level(LCD_CS_GPIO, 1);
    if (command->delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(command->delay_ms));
    }
}

static esp_err_t lcd_control_init(void)
{
    const uint64_t pins = BIT64(LCD_CS_GPIO) | BIT64(LCD_SCK_GPIO) | BIT64(LCD_SDA_GPIO) |
                          BIT64(LCD_BACKLIGHT_GPIO);
    const gpio_config_t config = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), "board", "LCD control GPIO configuration failed");
    gpio_set_level(LCD_BACKLIGHT_GPIO, 0);
    gpio_set_level(LCD_CS_GPIO, 1);
    gpio_set_level(LCD_SCK_GPIO, 1);
    gpio_set_level(LCD_SDA_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    for (size_t i = 0; i < sizeof(lcd_init_commands) / sizeof(lcd_init_commands[0]); i++) {
        lcd_write_command(&lcd_init_commands[i]);
    }
    return ESP_OK;
}

static uint8_t backlight_brightness = 100;
static bool backlight_on;

static void backlight_apply(void)
{
    uint32_t duty = backlight_on
                        ? (uint32_t)(sqrtf((float)backlight_brightness / 100.0f) * 255.0f)
                        : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "board", "Backlight timer configuration failed");
    const ledc_channel_config_t channel = {
        .gpio_num = LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel);
}

esp_err_t board_display_init(esp_lcd_panel_handle_t *panel)
{
    ESP_RETURN_ON_FALSE(panel != NULL, ESP_ERR_INVALID_ARG, "board", "Panel output is NULL");
    ESP_RETURN_ON_ERROR(lcd_control_init(), "board", "ST7701S initialization failed");

    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 12000000,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 8,
            .hsync_back_porch = 20,
            .hsync_front_porch = 10,
            .vsync_pulse_width = 8,
            .vsync_back_porch = 10,
            .vsync_front_porch = 10,
            .flags = {
                .hsync_idle_low = 0,
                .vsync_idle_low = 0,
                .pclk_active_neg = 0,
            },
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 10,
        .hsync_gpio_num = GPIO_NUM_16,
        .vsync_gpio_num = GPIO_NUM_17,
        .de_gpio_num = GPIO_NUM_18,
        .pclk_gpio_num = GPIO_NUM_21,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = {
            GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_0,
            GPIO_NUM_8, GPIO_NUM_20, GPIO_NUM_3, GPIO_NUM_46, GPIO_NUM_9, GPIO_NUM_10,
            GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7, GPIO_NUM_15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, panel), "board", "RGB panel creation failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), "board", "RGB panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), "board", "RGB panel initialization failed");
    ESP_RETURN_ON_ERROR(backlight_init(), "board", "Backlight PWM initialization failed");
    board_backlight_set(true);
    return ESP_OK;
}

void board_backlight_set(bool on)
{
    backlight_on = on;
    backlight_apply();
}

void board_backlight_set_brightness(uint8_t percent)
{
    backlight_brightness = percent > 100 ? 100 : percent;
    backlight_apply();
}

esp_err_t board_touch_init(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), "board", "Touch I2C bus creation failed");

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDRESS,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device_config, &touch_device), "board",
                        "Touch device registration failed");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, TOUCH_ADDRESS, 100), "board",
                        "GT911 did not acknowledge at 0x5D");
    const uint8_t product_id_register[] = {0x81, 0x40};
    uint8_t product_id[4] = {0};
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(touch_device, product_id_register,
                                                    sizeof(product_id_register), product_id,
                                                    sizeof(product_id), 100), "board",
                        "GT911 product ID read failed");
    ESP_LOGI("board", "GT911 product ID: %02X %02X %02X %02X",
             product_id[0], product_id[1], product_id[2], product_id[3]);
    return ESP_OK;
}

bool board_touch_read(int32_t *x, int32_t *y)
{
    static bool was_pressed;
    static uint32_t error_logs;
    static uint32_t poll_count;

    if (!touch_device || !x || !y) {
        return false;
    }

    const uint8_t register_address[] = {TOUCH_REG_HIGH, TOUCH_STATUS_REG};
    uint8_t data[7];
    esp_err_t result = i2c_master_transmit_receive(touch_device, register_address,
                                                   sizeof(register_address), data, sizeof(data), 50);
    if (result != ESP_OK) {
        if (error_logs++ < 5) {
            ESP_LOGW("board", "GT911 read failed: %s", esp_err_to_name(result));
        }
        return false;
    }
    poll_count++;
    if (poll_count % 500 == 0) {
        ESP_LOGI("board", "GT911 polls=%lu, status=0x%02X",
                 (unsigned long)poll_count, data[0]);
    }
    const uint8_t clear[] = {TOUCH_REG_HIGH, TOUCH_STATUS_REG, 0x00};
    i2c_master_transmit(touch_device, clear, sizeof(clear), 50);
    if ((data[0] & 0x80) == 0 || (data[0] & 0x0F) == 0) {
        was_pressed = false;
        return false;
    }

    *x = data[2] | (data[3] << 8);
    *y = data[4] | (data[5] << 8);
    bool valid = *x >= 0 && *x < BOARD_LCD_H_RES && *y >= 0 && *y < BOARD_LCD_V_RES;
    if (!was_pressed) {
        ESP_LOGI("board", "GT911 touch status=0x%02X, x=%ld, y=%ld, valid=%d",
                 data[0], (long)*x, (long)*y, valid);
    }
    was_pressed = valid;
    return valid;
}
