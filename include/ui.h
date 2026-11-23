#ifndef UI_H
#define UI_H

#include <Arduino.h>

struct DashData {
  int speed_kmh = 0;
  int rpm = 0;
  int coolant_c = -1;
  float volt = 0.0f;
  bool connected = false;
};

void ui_init();
void ui_set_data(const DashData& data);
void ui_show_boot();
void ui_show_search();
void ui_show_dash();
void ui_loop();

#endif
