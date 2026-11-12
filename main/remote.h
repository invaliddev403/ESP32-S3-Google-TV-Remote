#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    REMOTE_KEY_UP,
    REMOTE_KEY_DOWN,
    REMOTE_KEY_LEFT,
    REMOTE_KEY_RIGHT,
    REMOTE_KEY_OK,
    REMOTE_KEY_BACK,
    REMOTE_KEY_HOME,
    REMOTE_KEY_POWER,
    REMOTE_KEY_VOLUME_UP,
    REMOTE_KEY_VOLUME_DOWN,
    REMOTE_KEY_MUTE,
    REMOTE_KEY_PLAY_PAUSE,
    REMOTE_KEY_REWIND,
    REMOTE_KEY_FAST_FORWARD,
    REMOTE_KEY_PREVIOUS,
    REMOTE_KEY_NEXT,
    REMOTE_KEY_STOP,
} remote_key_t;

esp_err_t remote_init(void);
bool remote_is_connected(void);
bool remote_pairing_failed(void);
const char *remote_peer_name(void);
bool remote_peer_address(uint8_t *address);
int remote_peer_rssi(void);
int remote_saved_device_count(void);
void remote_refresh_status(void);
esp_err_t remote_retry_pairing(void);
esp_err_t remote_clear_pairing(void);
esp_err_t remote_send_key(remote_key_t key);
