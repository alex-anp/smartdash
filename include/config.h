#ifndef CONFIG_H
#define CONFIG_H

// Single source of truth for board pins, display geometry and vehicle limits.
// Keep runtime-tunable values here instead of spreading magic numbers across
// the UI and BLE modules.
//
// Board: JC3636W518C (ESP32-S3R8, 1.8" round 360x360 IPS, ST77916 QSPI, CST816S touch)
// Pins from vendor demo: JC3636W518EN/2-Program Example/Demo_Arduino_V1.0/ST77916_LVGL_DEMO/pincfg.h

#define SCREEN_WIDTH  360
#define SCREEN_HEIGHT 360

// USB CDC printf can block the calling task for hundreds of milliseconds.
// Runtime logging is therefore disabled by default; enable only for debugging
// with -DSMARTDASH_SERIAL_DEBUG=1.
#ifndef SMARTDASH_SERIAL_DEBUG
#define SMARTDASH_SERIAL_DEBUG 0
#endif
#if SMARTDASH_SERIAL_DEBUG
#define SD_LOG(...)   Serial.printf(__VA_ARGS__)
#define SD_LOGln(...) Serial.println(__VA_ARGS__)
#else
#define SD_LOG(...)   do {} while (0)
#define SD_LOGln(...) do {} while (0)
#endif

// Display QSPI (ST77916)
#define TFT_BLK  15
#define TFT_RST  47
#define TFT_CS   10
#define TFT_SCK  9
#define TFT_SDA0 11
#define TFT_SDA1 12
#define TFT_SDA2 13
#define TFT_SDA3 14

// Touch I2C (CST816S). Physical controller lives on I2C port 0; touch is used
// for swipes and selecting an ELM device, while configuration stays in Wi-Fi AP.
#define TOUCH_PIN_NUM_I2C_SCL 8
#define TOUCH_PIN_NUM_I2C_SDA 7
#define TOUCH_PIN_NUM_INT     41
#define TOUCH_PIN_NUM_RST     40

// Button
#define BTN_PIN 0

// Wi-Fi AP for web config
#define WIFI_AP_SSID "SmartDash"
#define WIFI_AP_PASS "smartdash"

// Vehicle specific
#define VEHICLE_NAME "Honda Dunk AF78"
#define RPM_REDLINE 8000
#define RPM_MAX 12000  // tachometer full scale

// Units. The current renderer is fixed to km/h and °C; these flags document
// the intended direction for a future configurable web UI.
#define USE_KMH true
#define USE_CELSIUS true

// ELM. Auto-connect uses the address stored in NVS; the reconnect gap in
// src/elm327.cpp is kept separate so the BLE task can react without an API call.
#define ELM_AUTO_CONNECT true
#define ELM_RECONNECT_INTERVAL 3000

// UI
#define SCREEN_CENTER_X (SCREEN_WIDTH / 2)
#define SCREEN_CENTER_Y (SCREEN_HEIGHT / 2)
#define SCREEN_RADIUS   (SCREEN_WIDTH / 2)

#endif
