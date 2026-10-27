/*
 * SmartDash - Honda Dunk AF78 OBD-II dashboard
 * Board: ESP32-2424S012 (240x240 round IPS)
 * Display: GC9A01 (SPI) - verify driver on arrival
 * Touch: CST816S/other - verify on arrival
 * BT: BluetoothSerial (Classic) + BLE support planned
 * ELM327 via BT, auto-connect to last paired
 */

#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "elm327.h"
#include "ui.h"
#include "storage.h"

void setup() {
  Serial.begin(115200);
  delay(100);

  storage_init();
  display_init();
  ui_show_boot();
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
  delay(10);
}
