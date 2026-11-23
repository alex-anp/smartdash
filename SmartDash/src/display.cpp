#include "display.h"
#include "config.h"

// JC3636W518C: ST77916 QSPI, 360x360
// Vendor demo uses ESP32_Display_Panel + LVGL 8.x
// TODO: integrate display driver after verifying demo on real board

void display_init() {
  // TODO: init ST77916 QSPI panel, backlight (TFT_BLK), and LVGL/display buffer
}

void display_clear(uint16_t color) {
  (void)color;
}
