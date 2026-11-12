# ESP32 GTV Remote

ESP-IDF firmware for an Onn 4K Pro using the AITRIP/Guition-compatible ESP32-4848S040 4-inch display board. Bluetooth LE HID is implemented first; the UI and key definitions are transport-independent so Android TV Remote over Wi-Fi can be added next.

## Controls

- Directional pad and OK
- Back, Home, and Power
- Volume up/down and Mute, including press-and-hold repeat for volume and navigation
- Media screen with Play/Pause, Stop, Rewind, Fast Forward, Previous, and Next
- Bluetooth connection status and pairing instructions on screen

Bluetooth power control normally puts the Onn into standby or wakes it if it accepts BLE remote wake. It cannot cold-start a device with no power. Volume behavior depends on the Onn's active audio output and HDMI-CEC setup. Voice search is not included because this firmware does not contain a microphone or Google Assistant integration.

## Hardware configuration

The code targets the common ESP32-4848S040 layout:

- ST7701S 480 x 480 RGB display, 16-bit RGB565
- GT911 capacitive touch at I2C address `0x5D`, SDA GPIO19 and SCL GPIO45
- Backlight GPIO38
- 16 MB QIO flash and 8 MB octal PSRAM at 80 MHz

All board-specific pins, display timings, and initialization commands are in `main/board.c`. Check that file before flashing if your board is not ESP32-4848S040-compatible. Pin references: [HomeDing board documentation](https://homeding.github.io/boards/esp32s3/panel-4848S040.htm) and [ESPHome device configuration](https://devices.esphome.io/devices/guition-esp32-s3-4848s040/).

## Build and flash

Use the ESP-IDF 6.1 PowerShell terminal installed on this machine:

```powershell
cd C:\tools\esp32-gtv-remote
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the board's USB serial port. Use the USB port connected to the board's USB-to-UART converter. Leave monitor with `Ctrl+]`.

The LVGL dependency is fetched by the ESP-IDF component manager. The exact resolved version is recorded in `dependencies.lock`.

## Pair with the Onn 4K Pro

1. Flash and power the remote.
2. On the Onn, open **Settings > Remotes & Accessories > Pair accessory**.
3. Select **ESP32 GTV Remote**.
4. Accept the pairing request if one appears. The display should change to **Bluetooth connected**.
5. Test the directional pad, OK, Back, and Home before testing Power.

The remote uses bonded BLE pairing and resumes advertising after disconnect. If authentication fails, the screen shows **Retry** and **Clear pairing** buttons. **Clear pairing** removes all stored BLE bonds from the ESP32 and restarts pairing. Also remove **ESP32 GTV Remote** from the Onn's accessory list before pairing again.

Erasing and reflashing is an alternative way to clear the ESP32's bonds, but it also clears the installed application:

```powershell
idf.py -p COMx erase-flash
idf.py -p COMx flash monitor
```

## Wi-Fi implementation path

Wi-Fi support is not enabled in this first version. The next transport should implement Google's Android TV Remote protocol: pair over TLS on port 6466, persist the client certificate and device identity, then send key events over TLS on port 6467. It will need a Wi-Fi credentials screen, a pairing-code screen, reconnect handling, and a transport selector. `main/remote.h` already defines the shared key interface used by the UI.

## Source layout

- `main/app_main.c`: startup, LVGL display/touch integration, and UI task
- `main/board.c`: ST7701S, GT911, backlight, and board pinout
- `main/ble_remote.c`: BLE pairing, HID reports, and key queue
- `main/remote_ui.c`: remote and media screens
- `main/remote.h`: transport-neutral remote key definitions
