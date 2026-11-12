#include "remote.h"

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define HID_REPORT_ID_KEYBOARD 1
#define HID_REPORT_ID_CONSUMER 2
#define HID_DEVICE_NAME "ESP32 GTV Remote"

static const char *tag = "ble_remote";
static esp_hidd_dev_t *hid_device;
static QueueHandle_t key_queue;
static volatile bool connected;
static volatile bool pairing_failed;
static esp_bd_addr_t peer_address;
static bool peer_address_valid;
static char peer_name[64];
static int peer_rssi = 127;
static esp_gatt_if_t peer_gattc_if = ESP_GATT_IF_NONE;
static uint16_t peer_conn_id;
static uint16_t gap_start_handle;
static uint16_t gap_end_handle;

static const uint8_t hid_report_map[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x95, 0x05,
    0x75, 0x01,
    0x05, 0x08,
    0x19, 0x01,
    0x29, 0x05,
    0x91, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x91, 0x01,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,

    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, HID_REPORT_ID_CONSUMER,
    0x09, 0x02,
    0xA1, 0x02,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x0A,
    0x15, 0x01,
    0x25, 0x0A,
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x00,
    0xC0,
    0x05, 0x0C,
    0x09, 0x86,
    0x15, 0xFF,
    0x25, 0x01,
    0x75, 0x02,
    0x95, 0x01,
    0x81, 0x46,
    0x09, 0xE9,
    0x09, 0xEA,
    0x15, 0x00,
    0x75, 0x01,
    0x95, 0x02,
    0x81, 0x02,
    0x09, 0xE2,
    0x09, 0x30,
    0x09, 0xCD,
    0x09, 0xB3,
    0x09, 0xB4,
    0x09, 0xB5,
    0x09, 0xB6,
    0x09, 0xB7,
    0x0A, 0x23, 0x02,
    0x0A, 0x24, 0x02,
    0x15, 0x01,
    0x25, 0x0A,
    0x75, 0x04,
    0x95, 0x01,
    0x81, 0x00,
    0x09, 0x80,
    0xA1, 0x02,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x01,
    0x25, 0x03,
    0x75, 0x02,
    0x95, 0x01,
    0x81, 0x00,
    0xC0,
    0x81, 0x03,
    0xC0,
};

static esp_hid_raw_report_map_t report_maps[] = {
    {
        .data = hid_report_map,
        .len = sizeof(hid_report_map),
    },
};

static const esp_hid_device_config_t hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0101,
    .device_name = HID_DEVICE_NAME,
    .manufacturer_name = "ESP32 Remote",
    .serial_number = "0001",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static esp_ble_adv_params_t advertising_parameters = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x30,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static bool hid_started;
static bool scan_response_ready;

static esp_err_t start_advertising(void)
{
    return esp_ble_gap_start_advertising(&advertising_parameters);
}

static void start_advertising_when_ready(void)
{
    if (hid_started && scan_response_ready) {
        start_advertising();
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id == 0x47 && param->reg.status == ESP_GATT_OK) {
            peer_gattc_if = gattc_if;
        }
        return;
    }
    if (gattc_if != peer_gattc_if) {
        return;
    }

    switch (event) {
    case ESP_GATTC_CONNECT_EVT:
        peer_conn_id = param->connect.conn_id;
        memcpy(peer_address, param->connect.remote_bda, ESP_BD_ADDR_LEN);
        peer_address_valid = true;
        peer_name[0] = '\0';
        peer_rssi = 127;
        esp_ble_gap_read_rssi(peer_address);
        {
            esp_bt_uuid_t gap_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid.uuid16 = 0x1800,
            };
            esp_ble_gattc_search_service(gattc_if, peer_conn_id, &gap_uuid);
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        gap_start_handle = param->search_res.start_handle;
        gap_end_handle = param->search_res.end_handle;
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (param->search_cmpl.status == ESP_GATT_OK && gap_start_handle && gap_end_handle) {
            esp_bt_uuid_t name_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid.uuid16 = ESP_GATT_UUID_GAP_DEVICE_NAME,
            };
            esp_gattc_char_elem_t characteristic;
            uint16_t count = 1;
            if (esp_ble_gattc_get_char_by_uuid(gattc_if, peer_conn_id, gap_start_handle,
                                               gap_end_handle, name_uuid, &characteristic,
                                               &count) == ESP_GATT_OK && count > 0) {
                esp_ble_gattc_read_char(gattc_if, peer_conn_id, characteristic.char_handle,
                                        ESP_GATT_AUTH_REQ_NONE);
            }
        }
        break;
    case ESP_GATTC_READ_CHAR_EVT:
        if (param->read.status == ESP_GATT_OK && param->read.value_len > 0) {
            size_t length = param->read.value_len;
            if (length >= sizeof(peer_name)) {
                length = sizeof(peer_name) - 1;
            }
            memcpy(peer_name, param->read.value, length);
            peer_name[length] = '\0';
            ESP_LOGI(tag, "Peer name: %s", peer_name);
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        peer_conn_id = 0;
        gap_start_handle = 0;
        gap_end_handle = 0;
        peer_rssi = 127;
        break;
    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        scan_response_ready = true;
        start_advertising_when_ready();
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        memcpy(peer_address, param->ble_security.auth_cmpl.bd_addr, ESP_BD_ADDR_LEN);
        peer_address_valid = true;
        pairing_failed = !param->ble_security.auth_cmpl.success;
        ESP_LOGI(tag, "Pairing %s", pairing_failed ? "failed" : "complete");
        break;
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
        peer_rssi = param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS
                        ? param->read_rssi_cmpl.rssi
                        : 127;
        break;
    default:
        break;
    }
}

static void hid_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        hid_started = true;
        ESP_LOGI(tag, "HID started");
        start_advertising_when_ready();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        connected = true;
        ESP_LOGI(tag, "Connected to Google TV");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        connected = false;
        ESP_LOGI(tag, "Disconnected; advertising");
        start_advertising();
        break;
    default:
        break;
    }
}

static bool keyboard_code(remote_key_t key, uint8_t *code)
{
    switch (key) {
    case REMOTE_KEY_UP:
        *code = 0x52;
        return true;
    case REMOTE_KEY_DOWN:
        *code = 0x51;
        return true;
    case REMOTE_KEY_LEFT:
        *code = 0x50;
        return true;
    case REMOTE_KEY_RIGHT:
        *code = 0x4F;
        return true;
    case REMOTE_KEY_OK:
        *code = 0x28;
        return true;
    default:
        return false;
    }
}

static bool consumer_report(remote_key_t key, uint16_t *report_value)
{
    switch (key) {
    case REMOTE_KEY_BACK:
        *report_value = 0x0A00;
        return true;
    case REMOTE_KEY_HOME:
        *report_value = 0x0900;
        return true;
    case REMOTE_KEY_POWER:
        *report_value = 0x0200;
        return true;
    case REMOTE_KEY_VOLUME_UP:
        *report_value = 0x0040;
        return true;
    case REMOTE_KEY_VOLUME_DOWN:
        *report_value = 0x0080;
        return true;
    case REMOTE_KEY_MUTE:
        *report_value = 0x0100;
        return true;
    case REMOTE_KEY_PLAY_PAUSE:
        *report_value = 0x0300;
        return true;
    case REMOTE_KEY_REWIND:
        *report_value = 0x0500;
        return true;
    case REMOTE_KEY_FAST_FORWARD:
        *report_value = 0x0400;
        return true;
    case REMOTE_KEY_PREVIOUS:
        *report_value = 0x0700;
        return true;
    case REMOTE_KEY_NEXT:
        *report_value = 0x0600;
        return true;
    case REMOTE_KEY_STOP:
        *report_value = 0x0800;
        return true;
    default:
        return false;
    }
}

static void release_reports(void)
{
    uint8_t keyboard[8] = {0};
    uint8_t consumer[2] = {0};
    esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_KEYBOARD, keyboard, sizeof(keyboard));
    esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_CONSUMER, consumer, sizeof(consumer));
}

static void send_key(remote_key_t key)
{
    uint8_t keyboard_code_value;
    uint16_t consumer_report_value;
    esp_err_t result = ESP_FAIL;
    if (keyboard_code(key, &keyboard_code_value)) {
        uint8_t report[8] = {0};
        report[2] = keyboard_code_value;
        result = esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_KEYBOARD, report, sizeof(report));
    } else if (consumer_report(key, &consumer_report_value)) {
        uint8_t report[] = {consumer_report_value & 0xFF, consumer_report_value >> 8};
        result = esp_hidd_dev_input_set(hid_device, 0, HID_REPORT_ID_CONSUMER, report, sizeof(report));
    }
    ESP_LOGI(tag, "HID key=%d, press result=%s", key, esp_err_to_name(result));
    vTaskDelay(pdMS_TO_TICKS(100));
    release_reports();
}

static void remote_task(void *argument)
{
    remote_key_t key;
    while (true) {
        if (xQueueReceive(key_queue, &key, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!connected || !hid_device || !esp_hidd_dev_connected(hid_device)) {
            continue;
        }
        send_key(key);
    }
}

static esp_err_t bluetooth_init(void)
{
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&controller_config), tag, "Controller initialization failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), tag, "Controller enable failed");

    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_config), tag, "Bluedroid initialization failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), tag, "Bluedroid enable failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_event_handler), tag, "GAP callback registration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(gattc_event_handler), tag, "GATTC callback registration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler), tag, "GATTS callback registration failed");

    esp_ble_auth_req_t authentication = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_capability = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t key_distribution = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &authentication, sizeof(authentication)), tag, "Authentication configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_capability, sizeof(io_capability)), tag, "I/O capability configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)), tag, "Key size configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &key_distribution, sizeof(key_distribution)), tag, "Initiator key configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &key_distribution, sizeof(key_distribution)), tag, "Responder key configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_app_register(0x47), tag, "GATTC application registration failed");
    return ESP_OK;
}

esp_err_t remote_init(void)
{
    ESP_RETURN_ON_ERROR(bluetooth_init(), tag, "Bluetooth initialization failed");

    const uint8_t hid_service_uuid[] = {
        0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
    };
    esp_ble_adv_data_t advertising_data = {
        .set_scan_rsp = false,
        .include_name = false,
        .include_txpower = false,
        .appearance = ESP_HID_APPEARANCE_KEYBOARD,
        .service_uuid_len = sizeof(hid_service_uuid),
        .p_service_uuid = (uint8_t *)hid_service_uuid,
        .flag = 0x06,
    };
    uint8_t scan_response[2 + sizeof(HID_DEVICE_NAME) - 1] = {
        sizeof(HID_DEVICE_NAME),
        ESP_BLE_AD_TYPE_NAME_CMPL,
    };
    memcpy(&scan_response[2], HID_DEVICE_NAME, sizeof(HID_DEVICE_NAME) - 1);
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_device_name(HID_DEVICE_NAME), tag, "Device name configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_adv_data(&advertising_data), tag, "Advertising configuration failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_config_scan_rsp_data_raw(scan_response, sizeof(scan_response)), tag, "Scan response configuration failed");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hid_event_handler, &hid_device), tag, "HID initialization failed");
    ESP_RETURN_ON_ERROR(esp_hidd_dev_battery_set(hid_device, 100), tag, "Battery report configuration failed");

    key_queue = xQueueCreate(16, sizeof(remote_key_t));
    ESP_RETURN_ON_FALSE(key_queue != NULL, ESP_ERR_NO_MEM, tag, "Key queue creation failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(remote_task, "remote_task", 4096, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, tag, "Remote task creation failed");
    return ESP_OK;
}

bool remote_is_connected(void)
{
    return connected;
}

bool remote_pairing_failed(void)
{
    return pairing_failed;
}

const char *remote_peer_name(void)
{
    return peer_name[0] ? peer_name : "Name unavailable";
}

bool remote_peer_address(uint8_t *address)
{
    if (!address || !peer_address_valid) {
        return false;
    }
    memcpy(address, peer_address, ESP_BD_ADDR_LEN);
    return true;
}

int remote_peer_rssi(void)
{
    return peer_rssi;
}

int remote_saved_device_count(void)
{
    return esp_ble_get_bond_device_num();
}

void remote_refresh_status(void)
{
    if (connected && peer_address_valid) {
        esp_ble_gap_read_rssi(peer_address);
    }
}

esp_err_t remote_retry_pairing(void)
{
    pairing_failed = false;
    if (connected) {
        return esp_ble_gap_disconnect(peer_address);
    }
    return start_advertising();
}

esp_err_t remote_clear_pairing(void)
{
    int count = esp_ble_get_bond_device_num();
    if (count < 0) {
        return ESP_FAIL;
    }
    if (count > 16) {
        count = 16;
    }

    esp_ble_bond_dev_t devices[16];
    if (count > 0) {
        ESP_RETURN_ON_ERROR(esp_ble_get_bond_device_list(&count, devices), tag,
                            "Could not read bonded devices");
        for (int i = 0; i < count; i++) {
            ESP_RETURN_ON_ERROR(esp_ble_remove_bond_device(devices[i].bd_addr), tag,
                                "Could not remove bonded device");
        }
    }
    ESP_LOGI(tag, "Cleared %d BLE bond(s)", count);
    return remote_retry_pairing();
}

esp_err_t remote_send_key(remote_key_t key)
{
    if (!key_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(key_queue, &key, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}
