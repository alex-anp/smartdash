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
  ui_init();
  ui_show_boot();
  delay(700);
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
  static uint32_t lastLoopStart = 0;
  static uint32_t lastStat = 0;
  static uint32_t maxGap = 0, maxUi = 0, maxWeb = 0, uiSlow = 0;

  uint32_t now = millis();
  if (lastLoopStart) {
    uint32_t gap = now - lastLoopStart;
    if (gap > maxGap) maxGap = gap;
  }
  lastLoopStart = now;

  uint32_t t0 = micros();
  ui_loop();
  uint32_t t1 = micros();
  web_loop();
  uint32_t t2 = micros();

  uint32_t du = t1 - t0, dw = t2 - t1;
  if (du > maxUi) maxUi = du;
  if (dw > maxWeb) maxWeb = dw;
  if (du > 30000) uiSlow++;

  // One compact line every 5 s: logging every slow frame would itself block
  // Serial for seconds and distort the measurement.
  if (now - lastStat >= 5000) {
    lastStat = now;
    Serial.printf("[STAT] gapMax=%u uiMax=%u uiSlow=%u webMax=%u\n",
                  (unsigned)maxGap, (unsigned)maxUi, (unsigned)uiSlow,
                  (unsigned)maxWeb);
    maxGap = maxUi = maxWeb = uiSlow = 0;
  }
  delay(2);
}
