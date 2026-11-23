#include "ui.h"

#include <math.h>

#include "config.h"
#include "display.h"
#include "elm327.h"

namespace {

constexpr int RPM_ARC_R_OUT = 176;
constexpr int RPM_ARC_R_IN = 162;
constexpr float RPM_ARC_START = 135.0f;
constexpr float RPM_ARC_SWEEP = 270.0f;

DashData cur;
DashData drawn;
bool dirty = true;
bool static_drawn = false;
uint32_t last_demo_ms = 0;

uint16_t rpm_color(int rpm) {
  if (rpm >= RPM_REDLINE) return TFT_RED;
  if (rpm >= RPM_REDLINE - 1000) return TFT_ORANGE;
  if (rpm >= RPM_REDLINE - 2000) return TFT_YELLOW;
  return TFT_GREEN;
}

void draw_arc_bg() {
  auto& g = display_get();
  g.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START,
            RPM_ARC_START + RPM_ARC_SWEEP, 0x2104);
}

void draw_rpm(int rpm) {
  rpm = constrain(rpm, 0, RPM_MAX);
  float frac = (float)rpm / (float)RPM_MAX;
  float end = RPM_ARC_START + RPM_ARC_SWEEP * frac;
  auto& g = display_get();
  draw_arc_bg();
  if (frac > 0.001f) {
    g.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START, end,
              rpm_color(rpm));
  }
}

void draw_speed(int speed) {
  auto& g = display_get();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", constrain(speed, 0, 199));
  g.setTextDatum(middle_center);
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.fillRect(70, 120, 220, 90, TFT_BLACK);
  g.drawString(buf, SCREEN_CENTER_X, SCREEN_CENTER_Y - 8, 8);
  g.setTextColor(0xC618, TFT_BLACK);
  g.drawString("km/h", SCREEN_CENTER_X, SCREEN_CENTER_Y + 52, 4);
}

void draw_bottom(int coolant_c, float volt, bool connected) {
  auto& g = display_get();
  g.fillRect(40, 285, 280, 50, TFT_BLACK);
  g.setTextDatum(middle_center);
  g.setTextColor(0xAD55, TFT_BLACK);

  char tbuf[16];
  if (coolant_c < 0) {
    snprintf(tbuf, sizeof(tbuf), "T -- C");
  } else {
    snprintf(tbuf, sizeof(tbuf), "T %d C", coolant_c);
  }
  char vbuf[16];
  if (volt <= 0.1f) {
    snprintf(vbuf, sizeof(vbuf), "U --.-V");
  } else {
    snprintf(vbuf, sizeof(vbuf), "U %.1fV", volt);
  }
  char line[40];
  snprintf(line, sizeof(line), "%s   %s", tbuf, vbuf);
  g.drawString(line, SCREEN_CENTER_X, 300, 4);

  g.setTextColor(connected ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  g.drawString(connected ? "ELM: OK" : "ELM: ...", SCREEN_CENTER_X, 324, 2);
}

void draw_static() {
  auto& g = display_get();
  g.fillScreen(TFT_BLACK);
  g.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 179, 0x1082);
  draw_arc_bg();
  static_drawn = true;
  dirty = true;
}

}  // namespace

void ui_init() {
  cur = {};
  drawn = {};
  drawn.speed_kmh = -999;
  drawn.rpm = -999;
  drawn.coolant_c = -999;
  drawn.volt = -999;
  drawn.connected = !cur.connected;
  static_drawn = false;
  dirty = true;
}

void ui_set_data(const DashData& data) {
  cur = data;
  dirty = true;
}

void ui_show_boot() {
  auto& g = display_get();
  g.fillScreen(TFT_BLACK);
  g.setTextDatum(middle_center);
  g.setTextColor(TFT_GREEN, TFT_BLACK);
  g.drawString("SmartDash", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 4);
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.drawString(VEHICLE_NAME, SCREEN_CENTER_X, SCREEN_CENTER_Y + 24, 2);
}

void ui_show_search() {
  auto& g = display_get();
  g.fillScreen(TFT_BLACK);
  g.setTextDatum(middle_center);
  g.setTextColor(TFT_ORANGE, TFT_BLACK);
  g.drawString("Searching ELM...", SCREEN_CENTER_X, SCREEN_CENTER_Y, 4);
}

void ui_show_dash() {
  if (!static_drawn) draw_static();

  if (!dirty) return;
  if (drawn.rpm != cur.rpm) {
    draw_rpm(cur.rpm);
    drawn.rpm = cur.rpm;
  }
  if (drawn.speed_kmh != cur.speed_kmh) {
    draw_speed(cur.speed_kmh);
    drawn.speed_kmh = cur.speed_kmh;
  }
  if (drawn.coolant_c != cur.coolant_c || fabsf(drawn.volt - cur.volt) > 0.05f || drawn.connected != cur.connected) {
    draw_bottom(cur.coolant_c, cur.volt, cur.connected);
    drawn.coolant_c = cur.coolant_c;
    drawn.volt = cur.volt;
    drawn.connected = cur.connected;
  }
  dirty = false;
}

void ui_loop() {
  uint32_t now = millis();
  if (now - last_demo_ms > 120) {
    last_demo_ms = now;
    // Demo animation until ELM is connected
    if (!elm_is_connected()) {
      static uint32_t t = 0;
      t++;
      cur.connected = false;
      cur.rpm = (int)((sinf(t * 0.08f) * 0.5f + 0.5f) * 9200);
      cur.speed_kmh = (int)((sinf(t * 0.05f + 1.0f) * 0.5f + 0.5f) * 85);
      cur.coolant_c = 78 + (int)(sinf(t * 0.02f) * 3);
      cur.volt = 13.8f + sinf(t * 0.03f) * 0.4f;
      dirty = true;
    } else {
      cur.connected = true;
    }
  }

  ui_show_dash();
}
