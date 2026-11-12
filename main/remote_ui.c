#include "remote_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "board.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "remote.h"

#define WAKE_GRACE_MS 500
#define STAR_COUNT 28

typedef enum {
    SCREEN_SAVER_OFF,
    SCREEN_SAVER_BOUNCE,
    SCREEN_SAVER_STARFIELD,
    SCREEN_SAVER_ORBIT,
} screen_saver_t;

static lv_obj_t *remote_screen;
static lv_obj_t *media_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *status_screen;
static lv_obj_t *forget_screen;
static lv_obj_t *lock_screen;
static lv_obj_t *screensaver_screen;
static lv_obj_t *screensaver_return_screen;
static lv_obj_t *status_label;
static lv_obj_t *hint_label;
static lv_obj_t *retry_button;
static lv_obj_t *clear_pairing_button;
static lv_obj_t *brightness_value_label;
static lv_obj_t *screensaver_brightness_value_label;
static lv_obj_t *status_details_label;
static lv_obj_t *bounce_label;
static lv_obj_t *stars[STAR_COUNT];
static lv_obj_t *orbit_center;
static lv_obj_t *orbiters[3];
static bool last_connection_state = true;
static bool last_pairing_failed;
static bool connection_status_initialized;
static uint8_t brightness = 100;
static uint8_t screensaver_brightness = 80;
static screen_saver_t screen_saver = SCREEN_SAVER_BOUNCE;
static uint8_t screen_saver_timeout = 1;
static bool screensaver_active;
static bool lock_active;
static uint32_t input_blocked_until;
static uint32_t last_rssi_request;
static int bounce_x = 145;
static int bounce_y = 200;
static int bounce_dx = 4;
static int bounce_dy = 3;
static int star_x[STAR_COUNT];
static int star_y[STAR_COUNT];
static int star_speed[STAR_COUNT];
static float orbit_angle;
static uint32_t random_state = 0x51A7E123;

static const uint32_t screen_saver_timeouts[] = {30000, 60000, 300000};

static uint32_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static bool input_allowed(void)
{
    return !screensaver_active && !lock_active &&
           (int32_t)(lv_tick_get() - input_blocked_until) >= 0;
}

static void settings_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("remote", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "brightness", brightness);
        nvs_set_u8(nvs, "saver_bright", screensaver_brightness);
        nvs_set_u8(nvs, "saver", (uint8_t)screen_saver);
        nvs_set_u8(nvs, "saver_delay", screen_saver_timeout);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void settings_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("remote", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint8_t value;
    if (nvs_get_u8(nvs, "brightness", &value) == ESP_OK && value <= 100) {
        brightness = value < 40 ? 40 : value;
    }
    if (nvs_get_u8(nvs, "saver_bright", &value) == ESP_OK && value <= 100) {
        screensaver_brightness = value < 40 ? 40 : value;
    }
    if (nvs_get_u8(nvs, "saver", &value) == ESP_OK && value <= SCREEN_SAVER_ORBIT) {
        screen_saver = (screen_saver_t)value;
    }
    if (nvs_get_u8(nvs, "saver_delay", &value) == ESP_OK &&
        value < sizeof(screen_saver_timeouts) / sizeof(screen_saver_timeouts[0])) {
        screen_saver_timeout = value;
    }
    nvs_close(nvs);
}

static void key_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    remote_key_t key = (remote_key_t)(uintptr_t)lv_event_get_user_data(event);
    esp_err_t result = remote_send_key(key);
    ESP_LOGI("ui", "Button key=%d, queue result=%s", key, esp_err_to_name(result));
}

static void show_screen_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    lv_obj_t *screen = lv_event_get_user_data(event);
    lv_screen_load(screen);
    if (screen == status_screen) {
        remote_refresh_status();
    }
}

static void show_media_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        lv_screen_load(media_screen);
    }
}

static void show_settings_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        lv_screen_load(settings_screen);
    }
}

static void show_status_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        lv_screen_load(status_screen);
        remote_refresh_status();
    }
}

static void show_forget_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        lv_screen_load(forget_screen);
    }
}

static void lock_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    lock_active = true;
    lv_screen_load(lock_screen);
    lv_display_trigger_activity(NULL);
}

static void unlock_gesture_event_handler(lv_event_t *event)
{
    if (!lock_active || lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_RIGHT) {
        return;
    }
    input_blocked_until = lv_tick_get() + WAKE_GRACE_MS;
    lock_active = false;
    lv_screen_load(remote_screen);
    lv_display_trigger_activity(NULL);
}

static void retry_pairing_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        remote_retry_pairing();
    }
}

static void clear_pairing_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        remote_clear_pairing();
    }
}

static void brightness_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    brightness = (uint8_t)lv_slider_get_value(lv_event_get_target(event));
    board_backlight_set_brightness(brightness);
    lv_label_set_text_fmt(brightness_value_label, "%u%%", brightness);
}

static void screensaver_brightness_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    screensaver_brightness = (uint8_t)lv_slider_get_value(lv_event_get_target(event));
    lv_label_set_text_fmt(screensaver_brightness_value_label, "%u%%", screensaver_brightness);
}

static void brightness_save_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        settings_save();
    }
}

static void screen_saver_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    screen_saver = (screen_saver_t)lv_dropdown_get_selected(lv_event_get_target(event));
    settings_save();
}

static void screen_saver_timeout_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    screen_saver_timeout = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(event));
    settings_save();
}

static void confirm_forget_event_handler(lv_event_t *event)
{
    if (!input_allowed()) {
        return;
    }
    esp_err_t result = remote_clear_pairing();
    ESP_LOGI("ui", "Forget device result=%s", esp_err_to_name(result));
    lv_screen_load(remote_screen);
}

static void refresh_status_event_handler(lv_event_t *event)
{
    if (input_allowed()) {
        remote_refresh_status();
    }
}

static void style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x10141F), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xF5F7FF), 0);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                              int32_t width, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, width);
    lv_obj_set_pos(label, x, y);
    return label;
}

static lv_obj_t *create_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                               int32_t width, int32_t height, uint32_t color,
                               const lv_font_t *font)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *create_nav_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                                   int32_t width, int32_t height, lv_obj_t *target,
                                   uint32_t color, const lv_font_t *font)
{
    lv_obj_t *button = create_button(parent, text, x, y, width, height, color, font);
    lv_obj_add_event_cb(button, show_screen_event_handler, LV_EVENT_CLICKED, target);
    return button;
}

static lv_obj_t *create_key_button(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                                   int32_t width, int32_t height, remote_key_t key,
                                   uint32_t color, bool repeat)
{
    lv_obj_t *button = create_button(parent, text, x, y, width, height, color,
                                     &lv_font_montserrat_28);
    lv_obj_add_event_cb(button, key_event_handler, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)key);
    if (repeat) {
        lv_obj_add_event_cb(button, key_event_handler, LV_EVENT_LONG_PRESSED_REPEAT,
                            (void *)(uintptr_t)key);
    }
    return button;
}

static void create_header(lv_obj_t *screen, const char *title, lv_obj_t *back_screen,
                          bool show_settings)
{
    int32_t title_x = back_screen ? 82 : 16;
    if (back_screen) {
        create_nav_button(screen, LV_SYMBOL_LEFT, 16, 8, 56, 48, back_screen,
                          0x3A4358, &lv_font_montserrat_28);
    }
    create_label(screen, title, title_x, 12, 300, &lv_font_montserrat_24);
    if (show_settings) {
        lv_obj_t *settings_button = create_button(screen, LV_SYMBOL_SETTINGS, 326, 8, 56, 48,
                                                  0x3A4358, &lv_font_montserrat_28);
        lv_obj_add_event_cb(settings_button, show_settings_event_handler, LV_EVENT_CLICKED, NULL);
    }
    create_key_button(screen, LV_SYMBOL_POWER, 398, 8, 66, 48, REMOTE_KEY_POWER,
                      0xB82E42, false);
}

static void create_volume_controls(lv_obj_t *screen, int32_t y)
{
    create_key_button(screen, LV_SYMBOL_VOLUME_MAX, 20, y, 136, 62,
                      REMOTE_KEY_VOLUME_UP, 0x2455A4, true);
    create_key_button(screen, LV_SYMBOL_VOLUME_MID, 172, y, 136, 62,
                      REMOTE_KEY_VOLUME_DOWN, 0x2455A4, true);
    create_key_button(screen, "MUTE", 324, y, 136, 62,
                      REMOTE_KEY_MUTE, 0x3A4358, false);
}

static void create_remote_screen(void)
{
    remote_screen = lv_obj_create(NULL);
    style_screen(remote_screen);
    create_header(remote_screen, "GTV Remote", NULL, true);
    lv_obj_t *lock_button = create_button(remote_screen, "LOCK", 264, 8, 56, 48,
                                          0x3A4358, &lv_font_montserrat_20);
    lv_obj_add_event_cb(lock_button, lock_event_handler, LV_EVENT_CLICKED, NULL);
    status_label = create_label(remote_screen, "Bluetooth starting", 16, 52, 300,
                                &lv_font_montserrat_20);

    create_key_button(remote_screen, LV_SYMBOL_UP, 118, 78, 90, 62,
                      REMOTE_KEY_UP, 0x29344A, true);
    create_key_button(remote_screen, LV_SYMBOL_LEFT, 20, 148, 90, 62,
                      REMOTE_KEY_LEFT, 0x29344A, true);
    create_key_button(remote_screen, "OK", 118, 148, 90, 62,
                      REMOTE_KEY_OK, 0x16745B, false);
    create_key_button(remote_screen, LV_SYMBOL_RIGHT, 216, 148, 90, 62,
                      REMOTE_KEY_RIGHT, 0x29344A, true);
    create_key_button(remote_screen, LV_SYMBOL_DOWN, 118, 218, 90, 62,
                      REMOTE_KEY_DOWN, 0x29344A, true);

    create_key_button(remote_screen, LV_SYMBOL_BACKSPACE " BACK", 326, 82, 134, 56,
                      REMOTE_KEY_BACK, 0x3A4358, false);
    create_key_button(remote_screen, LV_SYMBOL_HOME " HOME", 326, 150, 134, 56,
                      REMOTE_KEY_HOME, 0x3A4358, false);
    lv_obj_t *media_button = create_button(remote_screen, "MEDIA", 326, 218, 134, 56,
                                           0x67459B, &lv_font_montserrat_24);
    lv_obj_add_event_cb(media_button, show_media_event_handler, LV_EVENT_CLICKED, NULL);

    create_volume_controls(remote_screen, 322);
    hint_label = create_label(remote_screen, "Pair 'ESP32 GTV Remote'\nin Google TV Bluetooth settings",
                              20, 408, 440, &lv_font_montserrat_20);
    retry_button = create_button(remote_screen, "RETRY", 20, 404, 200, 56,
                                 0x2455A4, &lv_font_montserrat_24);
    clear_pairing_button = create_button(remote_screen, "CLEAR PAIRING", 240, 404, 220, 56,
                                         0xB82E42, &lv_font_montserrat_20);
    lv_obj_add_event_cb(retry_button, retry_pairing_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(clear_pairing_button, clear_pairing_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(retry_button, true);
    lv_obj_set_hidden(clear_pairing_button, true);
}

static void create_media_screen(void)
{
    media_screen = lv_obj_create(NULL);
    style_screen(media_screen);
    create_header(media_screen, "Media controls", remote_screen, true);

    create_key_button(media_screen, LV_SYMBOL_PREV LV_SYMBOL_PREV, 20, 154, 136, 72,
                      REMOTE_KEY_REWIND, 0x29344A, true);
    create_key_button(media_screen, LV_SYMBOL_PLAY " " LV_SYMBOL_PAUSE, 172, 154, 136, 72,
                      REMOTE_KEY_PLAY_PAUSE, 0x16745B, false);
    create_key_button(media_screen, LV_SYMBOL_NEXT LV_SYMBOL_NEXT, 324, 154, 136, 72,
                      REMOTE_KEY_FAST_FORWARD, 0x29344A, true);

    create_key_button(media_screen, LV_SYMBOL_PREV " PREV", 20, 246, 136, 62,
                      REMOTE_KEY_PREVIOUS, 0x3A4358, false);
    create_key_button(media_screen, LV_SYMBOL_STOP, 172, 246, 136, 62,
                      REMOTE_KEY_STOP, 0x3A4358, false);
    create_key_button(media_screen, "NEXT " LV_SYMBOL_NEXT, 324, 246, 136, 62,
                      REMOTE_KEY_NEXT, 0x3A4358, false);

    create_volume_controls(media_screen, 350);
    create_label(media_screen, "Playback and volume", 20, 432, 440, &lv_font_montserrat_20);
}

static lv_obj_t *create_dropdown(lv_obj_t *parent, const char *options, int32_t y,
                                 lv_event_cb_t event_handler)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_dropdown_set_options(dropdown, options);
    lv_obj_set_pos(dropdown, 40, y);
    lv_obj_set_size(dropdown, 400, 46);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x29344A), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(dropdown, event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    return dropdown;
}

static void create_settings_screen(void)
{
    settings_screen = lv_obj_create(NULL);
    style_screen(settings_screen);
    create_header(settings_screen, "Settings", remote_screen, false);

    create_label(settings_screen, "Brightness", 24, 66, 220, &lv_font_montserrat_20);
    brightness_value_label = create_label(settings_screen, "100%", 350, 66, 90,
                                          &lv_font_montserrat_20);
    lv_obj_t *brightness_slider = lv_slider_create(settings_screen);
    lv_obj_set_pos(brightness_slider, 40, 99);
    lv_obj_set_size(brightness_slider, 400, 16);
    lv_slider_set_range(brightness_slider, 40, 100);
    lv_slider_set_value(brightness_slider, brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, brightness_event_handler, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(brightness_slider, brightness_save_event_handler, LV_EVENT_RELEASED, NULL);

    create_label(settings_screen, "Screen saver", 24, 137, 300, &lv_font_montserrat_20);
    lv_obj_t *saver_dropdown = create_dropdown(settings_screen,
                                               "Off\nBouncing Logo\nStarfield\nOrbit", 167,
                                               screen_saver_event_handler);
    lv_dropdown_set_selected(saver_dropdown, screen_saver);

    create_label(settings_screen, "Saver brightness", 24, 225, 280, &lv_font_montserrat_20);
    screensaver_brightness_value_label = create_label(settings_screen, "80%", 350, 225, 90,
                                                      &lv_font_montserrat_20);
    lv_obj_t *saver_brightness_slider = lv_slider_create(settings_screen);
    lv_obj_set_pos(saver_brightness_slider, 40, 257);
    lv_obj_set_size(saver_brightness_slider, 400, 16);
    lv_slider_set_range(saver_brightness_slider, 40, 100);
    lv_slider_set_value(saver_brightness_slider, screensaver_brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(saver_brightness_slider, screensaver_brightness_event_handler,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(saver_brightness_slider, brightness_save_event_handler,
                        LV_EVENT_RELEASED, NULL);

    create_label(settings_screen, "Start after inactivity", 24, 289, 340,
                 &lv_font_montserrat_20);
    lv_obj_t *timeout_dropdown = create_dropdown(settings_screen,
                                                 "30 seconds\n1 minute\n5 minutes", 317,
                                                 screen_saver_timeout_event_handler);
    lv_dropdown_set_selected(timeout_dropdown, screen_saver_timeout);

    lv_obj_t *status_button = create_button(settings_screen, "STATUS", 20, 396, 200, 56,
                                            0x2455A4, &lv_font_montserrat_24);
    lv_obj_add_event_cb(status_button, show_status_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget_button = create_button(settings_screen, "FORGET DEVICE", 240, 396, 220, 56,
                                            0xB82E42, &lv_font_montserrat_20);
    lv_obj_add_event_cb(forget_button, show_forget_event_handler, LV_EVENT_CLICKED, NULL);
}

static void create_status_screen(void)
{
    status_screen = lv_obj_create(NULL);
    style_screen(status_screen);
    create_header(status_screen, "Connection status", settings_screen, false);
    status_details_label = create_label(status_screen, "Reading Bluetooth status...", 36, 92,
                                        408, &lv_font_montserrat_20);
    lv_obj_set_style_text_align(status_details_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *refresh_button = create_button(status_screen, "REFRESH", 130, 400, 220, 56,
                                             0x2455A4, &lv_font_montserrat_24);
    lv_obj_add_event_cb(refresh_button, refresh_status_event_handler, LV_EVENT_CLICKED, NULL);
}

static void create_forget_screen(void)
{
    forget_screen = lv_obj_create(NULL);
    style_screen(forget_screen);
    create_header(forget_screen, "Forget device", settings_screen, false);
    create_label(forget_screen, "Forget the saved Bluetooth device?", 30, 110, 420,
                 &lv_font_montserrat_24);
    create_label(forget_screen,
                 "This removes the bond on this remote.\nAlso remove 'ESP32 GTV Remote' in\nGoogle TV > Remotes & Accessories.",
                 30, 180, 420, &lv_font_montserrat_20);
    create_nav_button(forget_screen, "CANCEL", 30, 350, 190, 62, settings_screen,
                      0x3A4358, &lv_font_montserrat_24);
    lv_obj_t *confirm_button = create_button(forget_screen, "FORGET", 250, 350, 200, 62,
                                             0xB82E42, &lv_font_montserrat_24);
    lv_obj_add_event_cb(confirm_button, confirm_forget_event_handler, LV_EVENT_CLICKED, NULL);
}

static void create_lock_screen(void)
{
    lock_screen = lv_obj_create(NULL);
    style_screen(lock_screen);
    lv_obj_set_clickable(lock_screen, true);
    lv_obj_set_scrollable(lock_screen, false);
    lv_obj_add_event_cb(lock_screen, unlock_gesture_event_handler, LV_EVENT_GESTURE, NULL);

    lv_obj_t *shackle = lv_obj_create(lock_screen);
    lv_obj_set_pos(shackle, 210, 70);
    lv_obj_set_size(shackle, 60, 52);
    lv_obj_set_style_radius(shackle, 30, 0);
    lv_obj_set_style_bg_opa(shackle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(shackle, lv_color_hex(0x65D6A6), 0);
    lv_obj_set_style_border_width(shackle, 8, 0);
    lv_obj_set_clickable(shackle, false);
    lv_obj_set_scrollable(shackle, false);

    lv_obj_t *lock_body = lv_obj_create(lock_screen);
    lv_obj_set_pos(lock_body, 200, 105);
    lv_obj_set_size(lock_body, 80, 75);
    lv_obj_set_style_radius(lock_body, 12, 0);
    lv_obj_set_style_bg_color(lock_body, lv_color_hex(0x65D6A6), 0);
    lv_obj_set_style_border_width(lock_body, 0, 0);
    lv_obj_set_clickable(lock_body, false);
    lv_obj_set_scrollable(lock_body, false);

    create_label(lock_screen, "Screen locked", 40, 195, 400, &lv_font_montserrat_32);
    create_label(lock_screen, "Swipe right to unlock", 40, 250, 400, &lv_font_montserrat_24);
    lv_obj_t *swipe_hint = create_label(lock_screen, LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT,
                                        165, 330, 150, &lv_font_montserrat_40);
    lv_obj_set_style_text_color(swipe_hint, lv_color_hex(0x65D6A6), 0);
}

static void wake_event_handler(lv_event_t *event)
{
    if (!screensaver_active) {
        return;
    }
    lv_indev_t *input_device = lv_indev_active();
    if (input_device) {
        lv_indev_wait_release(input_device);
    }
    input_blocked_until = lv_tick_get() + WAKE_GRACE_MS;
    screensaver_active = false;
    board_backlight_set_brightness(brightness);
    lv_screen_load(screensaver_return_screen ? screensaver_return_screen : remote_screen);
    lv_display_trigger_activity(NULL);
}

static lv_obj_t *create_dot(lv_obj_t *parent, int32_t size, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_clickable(dot, false);
    lv_obj_set_scrollable(dot, false);
    lv_obj_set_hidden(dot, true);
    return dot;
}

static void create_screensaver_screen(void)
{
    screensaver_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screensaver_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screensaver_screen, LV_OPA_COVER, 0);
    lv_obj_set_clickable(screensaver_screen, true);
    lv_obj_add_event_cb(screensaver_screen, wake_event_handler, LV_EVENT_PRESSED, NULL);

    bounce_label = create_label(screensaver_screen, "Google TV\nRemote", 145, 200, 190,
                                &lv_font_montserrat_32);
    lv_obj_set_style_text_color(bounce_label, lv_color_hex(0x65D6A6), 0);
    lv_obj_set_hidden(bounce_label, true);

    for (int i = 0; i < STAR_COUNT; i++) {
        uint32_t color = i % 3 == 0 ? 0x65D6A6 : (i % 3 == 1 ? 0x78A7FF : 0xC59AFF);
        stars[i] = create_dot(screensaver_screen, 2 + i % 3, color);
    }

    orbit_center = create_label(screensaver_screen, "GTV", 200, 220, 80,
                                &lv_font_montserrat_32);
    lv_obj_set_style_text_color(orbit_center, lv_color_hex(0xF5F7FF), 0);
    lv_obj_set_hidden(orbit_center, true);
    for (int i = 0; i < 3; i++) {
        orbiters[i] = create_dot(screensaver_screen, 12, i == 0 ? 0x65D6A6 :
                                 (i == 1 ? 0x78A7FF : 0xC59AFF));
    }
}

static void start_screensaver(void)
{
    if (screensaver_active || screen_saver == SCREEN_SAVER_OFF) {
        return;
    }
    screensaver_return_screen = lv_screen_active();
    screensaver_active = true;
    board_backlight_set_brightness(screensaver_brightness);

    lv_obj_set_hidden(bounce_label, screen_saver != SCREEN_SAVER_BOUNCE);
    lv_obj_set_hidden(orbit_center, screen_saver != SCREEN_SAVER_ORBIT);
    for (int i = 0; i < STAR_COUNT; i++) {
        lv_obj_set_hidden(stars[i], screen_saver != SCREEN_SAVER_STARFIELD);
        star_x[i] = next_random() % BOARD_LCD_H_RES;
        star_y[i] = next_random() % BOARD_LCD_V_RES;
        star_speed[i] = 2 + next_random() % 5;
        lv_obj_set_pos(stars[i], star_x[i], star_y[i]);
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_hidden(orbiters[i], screen_saver != SCREEN_SAVER_ORBIT);
    }
    bounce_x = 145;
    bounce_y = 200;
    bounce_dx = 4;
    bounce_dy = 3;
    lv_obj_set_pos(bounce_label, bounce_x, bounce_y);
    orbit_angle = 0;
    lv_screen_load(screensaver_screen);
}

static void screensaver_timer(lv_timer_t *timer)
{
    if (!screensaver_active) {
        return;
    }
    if (screen_saver == SCREEN_SAVER_BOUNCE) {
        bounce_x += bounce_dx;
        bounce_y += bounce_dy;
        if (bounce_x <= 0 || bounce_x + 190 >= BOARD_LCD_H_RES) {
            bounce_dx = -bounce_dx;
        }
        if (bounce_y <= 0 || bounce_y + 80 >= BOARD_LCD_V_RES) {
            bounce_dy = -bounce_dy;
        }
        lv_obj_set_pos(bounce_label, bounce_x, bounce_y);
    } else if (screen_saver == SCREEN_SAVER_STARFIELD) {
        for (int i = 0; i < STAR_COUNT; i++) {
            star_x[i] -= star_speed[i];
            if (star_x[i] < 0) {
                star_x[i] = BOARD_LCD_H_RES - 1;
                star_y[i] = next_random() % BOARD_LCD_V_RES;
            }
            lv_obj_set_pos(stars[i], star_x[i], star_y[i]);
        }
    } else if (screen_saver == SCREEN_SAVER_ORBIT) {
        orbit_angle += 0.06f;
        for (int i = 0; i < 3; i++) {
            float angle = orbit_angle + i * 2.0943951f;
            int radius = 75 + i * 35;
            int x = 234 + (int)(cosf(angle) * radius);
            int y = 234 + (int)(sinf(angle) * radius);
            lv_obj_set_pos(orbiters[i], x, y);
        }
    }
}

static void update_status_details(void)
{
    uint8_t address[6];
    char address_text[18] = "Not available";
    if (remote_peer_address(address)) {
        snprintf(address_text, sizeof(address_text), "%02X:%02X:%02X:%02X:%02X:%02X",
                 address[0], address[1], address[2], address[3], address[4], address[5]);
    }
    int rssi = remote_peer_rssi();
    char signal_text[24];
    if (remote_is_connected() && rssi != 127) {
        snprintf(signal_text, sizeof(signal_text), "%d dBm", rssi);
    } else {
        snprintf(signal_text, sizeof(signal_text), "Not available");
    }
    int bonds = remote_saved_device_count();
    lv_label_set_text_fmt(status_details_label,
                          "Device: %s\nState: %s\nAddress: %s\nSignal: %s\nSaved bonds: %d\nRemote name: ESP32 GTV Remote",
                          remote_peer_name(),
                          remote_is_connected() ? "Connected" :
                              (remote_pairing_failed() ? "Pairing failed" : "Not connected"),
                          address_text, signal_text, bonds < 0 ? 0 : bonds);
}

void remote_ui_create(void)
{
    settings_load();
    board_backlight_set_brightness(brightness);
    create_remote_screen();
    create_media_screen();
    create_settings_screen();
    create_status_screen();
    create_forget_screen();
    create_lock_screen();
    create_screensaver_screen();
    lv_timer_create(screensaver_timer, 40, NULL);
    lv_label_set_text_fmt(brightness_value_label, "%u%%", brightness);
    lv_label_set_text_fmt(screensaver_brightness_value_label, "%u%%", screensaver_brightness);
    lv_screen_load(remote_screen);
    remote_ui_update();
}

void remote_ui_update(void)
{
    bool is_connected = remote_is_connected();
    bool pairing_failed = remote_pairing_failed();
    if (!connection_status_initialized || is_connected != last_connection_state ||
        pairing_failed != last_pairing_failed) {
        connection_status_initialized = true;
        last_connection_state = is_connected;
        last_pairing_failed = pairing_failed;

        if (pairing_failed) {
            lv_label_set_text(status_label, "Pairing failed");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF667A), 0);
            lv_obj_set_hidden(hint_label, true);
            lv_obj_set_hidden(retry_button, false);
            lv_obj_set_hidden(clear_pairing_button, false);
        } else {
            lv_obj_set_hidden(hint_label, false);
            lv_obj_set_hidden(retry_button, true);
            lv_obj_set_hidden(clear_pairing_button, true);
            if (is_connected) {
                lv_label_set_text(status_label, "Bluetooth connected");
                lv_obj_set_style_text_color(status_label, lv_color_hex(0x65D6A6), 0);
                lv_label_set_text(hint_label, "Ready to control your Google TV");
            } else {
                lv_label_set_text(status_label, "Bluetooth not connected");
                lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFAD66), 0);
                lv_label_set_text(hint_label, "Remove old accessory on Onn, then pair\n'ESP32 GTV Remote' in Bluetooth settings");
            }
        }
    }

    uint32_t now = lv_tick_get();
    if (lv_screen_active() == status_screen) {
        update_status_details();
        if (is_connected && now - last_rssi_request >= 5000) {
            remote_refresh_status();
            last_rssi_request = now;
        }
    }
    if (!screensaver_active && screen_saver != SCREEN_SAVER_OFF &&
        lv_display_get_inactive_time(NULL) >= screen_saver_timeouts[screen_saver_timeout]) {
        start_screensaver();
    }
}
