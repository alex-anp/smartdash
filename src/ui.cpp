#include "ui.h"
#include "config.h"
#include "elm327.h"

// Minimal round UI: RPM arc on perimeter, speed center, temp+volt bottom

struct DashData {
  int speed_kmh = 0;
  int rpm = 0;
  int coolant_c = -1;
  float volt = 0.0;
  bool valid = false;
};

static DashData data;

void ui_show_boot() {
  // TODO: draw boot screen
}

void ui_show_search() {
  // TODO: draw BT search/list for touch selection
}

void ui_show_dash() {
  // TODO: draw main dashboard
}

void ui_loop() {
  // TODO: update dash when connected, redraw only changed elements
  if (elm_is_connected()) {
    ui_show_dash();
  }
}
