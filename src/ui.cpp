#include "ui.h"

#include <math.h>

#include "config.h"
#include "display.h"
#include "elm327.h"

namespace {

constexpr int RPM_ARC_R_OUT = 176;
constexpr int RPM_ARC_R_IN = 160;
constexpr float RPM_ARC_START = 150.0f;  // 240 degrees total
constexpr float RPM_ARC_SWEEP = 240.0f;

LGFX_Sprite canvas(&display_get());
bool canvas_ok = false;

DashData cur;
DashData drawn;
bool dirty = true;
bool static_drawn = false;
uint32_t last_demo_ms = 0;

float rpm_angle(int rpm) {
  rpm = constrain(rpm, 0, RPM_MAX);
  return RPM_ARC_START + RPM_ARC_SWEEP * ((float)rpm / (float)RPM_MAX);
}

uint16_t rpm_color(int rpm) {
  if (rpm >= RPM_REDLINE) return TFT_RED;
  if (rpm >= RPM_REDLINE - 1000) return TFT_ORANGE;
  if (rpm >= RPM_REDLINE - 2000) return TFT_YELLOW;
  return TFT_GREEN;
}

lgfx::LGFX_Device& g() { return canvas_ok ? (lgfx::LGFX_Device&)canvas : display_get(); }

void flush() {
  if (canvas_ok) canvas.pushSprite(0, 0);
}

void draw_arc_bg() {
  auto& d = g();
  d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START,
            RPM_ARC_START + RPM_ARC_SWEEP, 0x2104);
}

void draw_rpm_ticks() {
  auto& d = g();
  for (int i = 0; i <= 20; i++) {  // every 500 rpm
    int rpm = i * 500;
    float a = rpm_angle(rpm) * (float)M_PI / 180.0f;
    bool thousand = (i % 2 == 0);
    int r1 = RPM_ARC_R_OUT + 2;
    int r2 = thousand ? (RPM_ARC_R_IN - 12) : (RPM_ARC_R_IN - 5);
    uint16_t c = thousand ? TFT_WHITE : 0x8410;

    float ca = cosf(a), sa = sinf(a);
    int x1 = SCREEN_CENTER_X + (int)(ca * r1);
    int y1 = SCREEN_CENTER_Y + (int)(sa * r1);
    int x2 = SCREEN_CENTER_X + (int)(ca * r2);
    int y2 = SCREEN_CENTER_Y + (int)(sa * r2);

    if (thousand) {
      float nx = -sa, ny = ca;
      for (int dd = -1; dd <= 1; dd++) {
        int ox = (int)(nx * dd);
        int oy = (int)(ny * dd);
        d.drawLine(x1 + ox, y1 + oy, x2 + ox, y2 + oy, c);
      }
    } else {
      d.drawLine(x1, y1, x2, y2, c);
    }
  }
}

void draw_rpm_labels() {
  auto& d = g();
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int k = 0; k <= 10; k += 2) {
    float a = rpm_angle(k * 1000) * (float)M_PI / 180.0f;
    int r = RPM_ARC_R_IN - 24;
    int x = SCREEN_CENTER_X + (int)(cosf(a) * r);
    int y = SCREEN_CENTER_Y + (int)(sinf(a) * r);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", k);
    d.drawString(buf, x, y, 4);
  }
}

void draw_rpm(int rpm) {
  rpm = constrain(rpm, 0, RPM_MAX);
  auto& d = g();
  draw_arc_bg();
  if (rpm > 0) {
    d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START,
              rpm_angle(rpm), rpm_color(rpm));
  }
  draw_rpm_ticks();
}

void draw_seg_digit(lgfx::LGFX_Device& d, int x, int y, int w, int h, int t, int digit, uint16_t on, uint16_t off) {
  static const uint8_t seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  uint8_t m = (digit >= 0 && digit <= 9) ? seg[digit] : 0;
  int gap = 2;
  int hh = (h - 3 * t) / 2;
  int mid = y + h / 2 - t / 2;
  auto col = [&](bool en) { return en ? on : off; };
  int r = t / 3;
  d.fillRoundRect(x + t + gap, y, w - 2 * t - 2 * gap, t, r, col(m & 0x01));                 // A
  d.fillRoundRect(x + w - t, y + t + gap, t, hh - gap, r, col(m & 0x02));                     // B
  d.fillRoundRect(x + w - t, mid + t / 2 + gap, t, hh - gap, r, col(m & 0x04));               // C
  d.fillRoundRect(x + t + gap, y + h - t, w - 2 * t - 2 * gap, t, r, col(m & 0x08));         // D
  d.fillRoundRect(x, mid + t / 2 + gap, t, hh - gap, r, col(m & 0x10));                       // E
  d.fillRoundRect(x, y + t + gap, t, hh - gap, r, col(m & 0x20));                             // F
  d.fillRoundRect(x + t + gap, mid, w - 2 * t - 2 * gap, t, r, col(m & 0x40));               // G
}

void draw_speed(int speed) {
  auto& d = g();
  speed = constrain(speed, 0, 99);

  const int digitW = 56;
  const int digitH = 96;
  const int segT = 10;
  const int spacing = 14;
  const int totalW = digitW * 2 + spacing;
  const int x0 = SCREEN_CENTER_X - totalW / 2;
  const int y0 = SCREEN_CENTER_Y - 56;

  d.fillRect(SCREEN_CENTER_X - 90, y0 - 8, 180, digitH + 44, TFT_BLACK);

  uint16_t on = 0xFFFF;
  uint16_t off = 0x1082;
  int d1 = speed / 10;
  int d2 = speed % 10;

  draw_seg_digit(d, x0, y0, digitW, digitH, segT, (d1 == 0) ? -1 : d1, on, off);
  draw_seg_digit(d, x0 + digitW + spacing, y0, digitW, digitH, segT, d2, on, off);

  d.setTextDatum(middle_center);
  d.setTextColor(0xC618, TFT_BLACK);
  d.drawString("km/h", SCREEN_CENTER_X, y0 + digitH + 18, 4);
}

void draw_temp_icon(int x, int y, uint16_t c) {
  auto& d = g();
  d.drawRoundRect(x - 4, y - 12, 8, 16, 3, c);
  d.fillCircle(x, y + 7, 5, c);
  d.fillRect(x - 1, y - 7, 2, 12, c);
  d.drawFastHLine(x + 5, y - 8, 3, c);
  d.drawFastHLine(x + 5, y - 4, 3, c);
  d.drawFastHLine(x + 5, y, 3, c);
}

void draw_batt_icon(int x, int y, uint16_t c) {
  auto& d = g();
  d.drawRoundRect(x - 12, y - 8, 22, 16, 2, c);
  d.fillRect(x + 10, y - 3, 3, 6, c);
  d.drawFastHLine(x - 7, y, 5, c);
  d.drawFastVLine(x - 5, y - 2, 5, c);
  d.drawFastHLine(x + 1, y, 5, c);
}

void draw_bottom(int coolant_c, float volt, bool connected) {
  auto& d = g();
  d.fillRect(30, 278, 300, 60, TFT_BLACK);
  uint16_t iconColor = 0xC618;
  uint16_t textColor = 0xD6BA;

  draw_temp_icon(86, 298, iconColor);
  d.setTextDatum(middle_left);
  d.setTextColor(textColor, TFT_BLACK);
  char tbuf[16];
  if (coolant_c < 0) snprintf(tbuf, sizeof(tbuf), "--C");
  else snprintf(tbuf, sizeof(tbuf), "%dC", coolant_c);
  d.drawString(tbuf, 100, 298, 4);

  draw_batt_icon(222, 298, iconColor);
  char vbuf[16];
  if (volt <= 0.1f) snprintf(vbuf, sizeof(vbuf), "--.-V");
  else snprintf(vbuf, sizeof(vbuf), "%.1fV", volt);
  d.drawString(vbuf, 238, 298, 4);

  d.setTextDatum(middle_center);
  d.setTextColor(connected ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  d.drawString(connected ? "ELM: OK" : "ELM: ...", SCREEN_CENTER_X, 324, 2);
}

void draw_static() {
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 179, 0x1082);
  draw_arc_bg();
  draw_rpm_ticks();
  draw_rpm_labels();
  static_drawn = true;
  dirty = true;
}

}  // namespace

void ui_init() {
  if (!canvas_ok) {
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    canvas_ok = canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT) != nullptr;
  }

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
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString("SmartDash", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 4);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(VEHICLE_NAME, SCREEN_CENTER_X, SCREEN_CENTER_Y + 24, 2);
  flush();
}

void ui_show_search() {
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  d.drawString("Searching ELM...", SCREEN_CENTER_X, SCREEN_CENTER_Y, 4);
  flush();
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
  flush();
}

void ui_loop() {
  uint32_t now = millis();
  if (now - last_demo_ms > 120) {
    last_demo_ms = now;
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
