#ifndef UI_H
#define UI_H

#include <Arduino.h>

// Snapshot of values coming from the ELM327 task (or from the startup sweep).
// -1 coolant / 0 volt mean "no data yet" and are rendered as "--".
struct DashData {
  int speed_kmh = 0;   // 0..99 km/h
  int rpm = 0;         // 0..RPM_MAX
  int coolant_c = -1;  // °C, negative = unavailable
  float volt = 0.0f;   // V, <= 0.1 = unavailable
  bool connected = false;
};

void ui_init();                              // Allocate sprite, reset caches and sweep state
void ui_set_data(const DashData& data);      // Called from the BLE task; marks dashboard dirty
void ui_show_boot();                         // One-time "SmartDash" splash
void ui_show_search();                       // "Searching ELM..." text screen
void ui_show_dash();                         // Draw changed widgets into the sprite
void ui_show_elm_connect();                  // Open the ELMConnect screen and start scanning
void ui_hide_elm_connect();                  // Return to dashboard, forcing a full redraw
void ui_loop();                              // Poll input, run sweep/ticker, render active screen

#endif
