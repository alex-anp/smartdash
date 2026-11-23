/*
 * SmartDash - Honda Dunk AF78 OBD-II dashboard
 * Board: JC3636W518C (ESP32-S3, 360x360 round IPS, ST77916 QSPI)
 * BT: BLE (ESP32-S3 has no Bluetooth Classic)
 * ELM327 via BLE, auto-connect to last paired
 * Config: built-in Wi-Fi AP web interface
 */

#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "elm327.h"
#include "ui.h"
#include "storage.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  delay(100);

  storage_init();
  display_init();
  ui_show_boot();
  web_init();
  elm_init();

  // Auto-connect to last saved ELM327
  String last = storage_get_last_elm();
  if (last.length() > 0) {
    elm_connect_saved(last);
  } else {
    elm_start_scan();
  }
}

void loop() {
  elm_loop();
  ui_loop();
  web_loop();
  delay(10);
}
