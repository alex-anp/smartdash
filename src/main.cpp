/*
 * SmartDash - Honda Dunk AF78 OBD-II dashboard
 *
 * Board: JC3636W518C (ESP32-S3, 360x360 round IPS, ST77916 QSPI)
 * BT:    BLE only (ESP32-S3 has no Bluetooth Classic)
 * ELM:   ELM327-compatible adapter, auto-connect to the last saved MAC
 * UI:    Full-screen PSRAM sprite, one-shot startup sweep, swipe navigation
 * Config: Wi-Fi AP web interface (basic page implemented, forms are future work)
 *
 * setup() is strictly sequential: NVS -> display -> UI -> AP -> BLE.
 * loop() stays lightweight; the blocking BLE state machine lives in elmTask
 * on core 0 (see src/elm327.cpp).
 */

#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "elm327.h"
#include "ui.h"
#include "storage.h"
#include "web.h"

void setup() {
  // USB CDC is initialized even though runtime logging is compiled out by
  // default; it is useful when SMARTDASH_SERIAL_DEBUG=1 is enabled.
  Serial.begin(115200);
  delay(100);

  storage_init();  // Open NVS before anything asks for the saved ELM address
  display_init();  // ST77916 + backlight + CST816S
  ui_init();       // Allocate sprite and reset all render caches
  ui_show_boot();  // Short splash before the dashboard appears
  delay(700);
  web_init();      // Start SmartDash-<id> access point
  elm_init();      // Start BLE worker task on core 0

  // Auto-connect to last saved ELM327, or discover any compatible adapter.
  String last = storage_get_last_elm();
  if (last.length() > 0) {
    elm_connect_saved(last);
  } else {
    elm_start_scan();
  }
}

void loop() {
  ui_loop();    // Input, startup sweep, status dots and dirty-widget rendering
  web_loop();   // Handle one pending HTTP client, if any
  delay(2);     // Yield to the scheduler; the 120 ms OBD poll is in elmTask
}
