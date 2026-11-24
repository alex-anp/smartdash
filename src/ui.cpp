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
constexpr int RPM_SEGMENT_RPM = 250;  // 0.25k rpm per indicator segment

LGFX_Sprite canvas(&display_get());
bool canvas_ok = false;

DashData cur;
DashData drawn;
bool dirty = true;
bool static_drawn = false;

// One-shot startup sweep instead of an endless demo.
constexpr uint32_t SWEEP_INTERVAL_MS = 100;
constexpr int SWEEP_STEPS = 40;
uint32_t last_sweep_ms = 0;
int sweep_step = 0;
bool sweep_done = false;

enum class Screen { Dash, ElmConnect };
Screen screen = Screen::Dash;

constexpr uint32_t DOT_PERIOD_MS = 260;
uint32_t last_dot_ms = 0;
int dot_phase = 0;
int drawn_dot_phase = -1;

uint32_t last_elm_draw = 0;
bool elm_screen_dirty = true;
String connectTarget;

struct TouchInput {
  bool pressed = false;
  bool swipeHandled = false;
  int32_t x = 0;
  int32_t y = 0;
  int32_t startX = 0;
  int32_t startY = 0;
  uint32_t startedAt = 0;
  uint32_t lastClick = 0;
};
TouchInput touch;

float rpm_angle(int rpm) {
  rpm = constrain(rpm, 0, RPM_MAX);
  return RPM_ARC_START + RPM_ARC_SWEEP * ((float)rpm / (float)RPM_MAX);
}

lgfx::LGFX_Device& g() { return canvas_ok ? (lgfx::LGFX_Device&)canvas : display_get(); }

void flush() {
  if (!canvas_ok) return;
  canvas.pushSprite(0, 0);
}

void draw_status_dots(lgfx::LGFX_Device& d, int cx, int y, uint16_t active, uint16_t inactive, int phase) {
  for (int i = 0; i < 3; i++) {
    bool lit = (i == phase);
    d.fillCircle(cx + (i - 1) * 14, y, lit ? 4 : 3, lit ? active : inactive);
  }
}

bool in_rect(int32_t x, int32_t y, int x0, int y0, int x1, int y1) {
  return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

void draw_arc_bg() {
  auto& d = g();
  d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START,
            RPM_ARC_START + RPM_ARC_SWEEP, 0x2104);
}

void draw_rpm_ticks() {
  auto& d = g();
  for (int i = 0; i <= RPM_MAX / 500; i++) {  // every 500 rpm
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
  for (int k = 0; k <= RPM_MAX / 1000; k += 2) {
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
    struct Zone { int from; int to; uint16_t color; };
    static const Zone zones[] = {
      {0, RPM_REDLINE, TFT_GREEN},
      {RPM_REDLINE, RPM_REDLINE + 2000, TFT_ORANGE},
      {RPM_REDLINE + 2000, RPM_MAX, TFT_RED},
    };
    for (const auto& z : zones) {
      if (rpm <= z.from) break;
      int segEnd = rpm < z.to ? rpm : z.to;
      d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN,
                rpm_angle(z.from), rpm_angle(segEnd), z.color);
      if (rpm <= z.to) break;
    }

    // Split the filled arc into 0.25k rpm segments with dark gaps.
    // Drawing separators is much cheaper than one fillArc() call per segment.
    for (int r = RPM_SEGMENT_RPM; r < rpm; r += RPM_SEGMENT_RPM) {
      float a = rpm_angle(r) * (float)M_PI / 180.0f;
      float ca = cosf(a), sa = sinf(a);
      float nx = -sa, ny = ca;
      int x1 = SCREEN_CENTER_X + (int)(ca * RPM_ARC_R_IN);
      int y1 = SCREEN_CENTER_Y + (int)(sa * RPM_ARC_R_IN);
      int x2 = SCREEN_CENTER_X + (int)(ca * RPM_ARC_R_OUT);
      int y2 = SCREEN_CENTER_Y + (int)(sa * RPM_ARC_R_OUT);
      // A 6 px gap keeps neighbouring 250 rpm segments visually separate.
      for (int off = -3; off <= 2; off++) {
        int ox = (int)(nx * off);
        int oy = (int)(ny * off);
        d.drawLine(x1 + ox, y1 + oy, x2 + ox, y2 + oy, 0x2104);
      }
    }
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

  if (d1 == 0) {
    // Keep 0..9 (especially zero after the startup sweep) clearly centered
    // instead of leaving the tens digit blank and showing only a shifted digit.
    draw_seg_digit(d, SCREEN_CENTER_X - digitW / 2, y0, digitW, digitH, segT, d2, on, off);
  } else {
    draw_seg_digit(d, x0, y0, digitW, digitH, segT, d1, on, off);
    draw_seg_digit(d, x0 + digitW + spacing, y0, digitW, digitH, segT, d2, on, off);
  }

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
  uint16_t tColor = 0xC618;
  if (coolant_c >= 0) {
    if (coolant_c < 60) tColor = TFT_CYAN;
    else if (coolant_c <= 90) tColor = TFT_GREEN;
    else tColor = TFT_RED;
  }

  uint16_t vColor = 0xC618;
  if (volt > 0.1f) {
    if (volt < 11.0f) vColor = TFT_RED;
    else if (volt < 12.0f) vColor = TFT_ORANGE;
    else if (volt <= 15.0f) vColor = TFT_GREEN;
    else vColor = TFT_RED;
  }

  draw_temp_icon(86, 298, tColor);
  d.setTextDatum(middle_left);
  d.setTextColor(tColor, TFT_BLACK);
  char tbuf[16];
  if (coolant_c < 0) snprintf(tbuf, sizeof(tbuf), "--C");
  else snprintf(tbuf, sizeof(tbuf), "%dC", coolant_c);
  d.drawString(tbuf, 100, 298, 4);

  draw_batt_icon(222, 298, vColor);
  d.setTextColor(vColor, TFT_BLACK);
  char vbuf[16];
  if (volt <= 0.1f) snprintf(vbuf, sizeof(vbuf), "--.-V");
  else snprintf(vbuf, sizeof(vbuf), "%.1fV", volt);
  d.drawString(vbuf, 238, 298, 4);

  // ELM status indicator. Three dots light up in turn while connecting.
  if (connected) {
    d.setTextDatum(middle_center);
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.drawString("ELM OK", SCREEN_CENTER_X, 324, 2);
  } else {
    d.setTextDatum(middle_right);
    d.setTextColor(TFT_ORANGE, TFT_BLACK);
    d.drawString("ELM", SCREEN_CENTER_X - 12, 324, 2);
    draw_status_dots(d, SCREEN_CENTER_X + 18, 324, TFT_ORANGE, 0x4208, dot_phase);
  }
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

String elm_status_text(ElmStatus s) {
  switch (s) {
    case ElmStatus::Idle:         return "Idle";
    case ElmStatus::Scanning:     return "Scanning ELM devices";
    case ElmStatus::Connecting:   return connectTarget.length() ? ("Connecting " + connectTarget) : String("Connecting...");
    case ElmStatus::Initializing: return "Initializing ELM...";
    case ElmStatus::Connected:    return connectTarget.length() ? ("Connected: " + connectTarget) : String("Connected to ELM");
  }
  return "";
}

void draw_elm_screen() {
  auto& d = g();
  ElmStatus st = elm_status();

  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);

  // Round screen: usable width shrinks near the top and bottom edges.
  // Keep all content inside the inscribed circle (radius ~175 from center 180,180).
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("ELM Connect", SCREEN_CENTER_X, 38, 4);

  uint16_t statusColor = (st == ElmStatus::Connected) ? TFT_GREEN :
                         (st == ElmStatus::Idle) ? 0x8410 : TFT_ORANGE;
  d.setTextColor(statusColor, TFT_BLACK);
  d.drawString(elm_status_text(st), SCREEN_CENTER_X, 74, 2);
  if (st == ElmStatus::Scanning || st == ElmStatus::Connecting || st == ElmStatus::Initializing) {
    draw_status_dots(d, 300, 74, statusColor, 0x4208, dot_phase);
  }

  d.drawRoundRect(130, 94, 100, 28, 6, 0x4208);
  d.setTextColor(0xC618, TFT_BLACK);
  d.drawString("Scan", SCREEN_CENTER_X, 108, 2);

  d.drawFastHLine(40, 128, 280, 0x2104);

  ElmScanEntry entries[5];
  size_t n = elm_get_found(entries, 5);
  if (n == 0) {
    d.setTextColor(0x8410, TFT_BLACK);
    d.drawString("No BLE devices yet", SCREEN_CENTER_X, 190, 2);
    d.drawString("Keep ELM nearby", SCREEN_CENTER_X, 214, 2);
  } else {
    for (size_t i = 0; i < n; i++) {
      const ElmScanEntry& e = entries[i];
      int y0 = 134 + (int)i * 30;
      uint16_t bg = e.compatible ? 0x0420 : 0x1082;
      d.fillRoundRect(40, y0, 280, 28, 5, bg);

      char name[24];
      snprintf(name, sizeof(name), "%s", e.name);
      d.setTextDatum(middle_left);
      d.setTextColor(TFT_WHITE, bg);
      d.drawString(name, 50, y0 + 9, 2);

      // Short MAC (last 8 chars) on a smaller second line.
      const char* shortAddr = e.address;
      size_t len = strlen(shortAddr);
      if (len > 8) shortAddr += len - 8;
      d.setTextColor(0x8410, bg);
      d.drawString(shortAddr, 50, y0 + 21, 1);

      char rssi[12];
      snprintf(rssi, sizeof(rssi), "%d dBm", e.rssi);
      d.setTextDatum(middle_right);
      d.setTextColor(e.rssi > -70 ? TFT_GREEN : TFT_ORANGE, bg);
      d.drawString(rssi, 310, y0 + 14, 2);
    }
  }

  d.drawFastHLine(40, 288, 280, 0x2104);

  d.setTextDatum(middle_center);
  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString(st == ElmStatus::Connected ? "Swipe down to return" : "Tap device to connect",
               SCREEN_CENTER_X, 310, 2);

  flush();
}

void handle_tap(int32_t x, int32_t y) {
  if (screen == Screen::Dash) return;  // Swipe up opens the ELM screen.

  if (in_rect(x, y, 130, 94, 230, 122)) {
    connectTarget = "";
    elm_start_scan();
    elm_screen_dirty = true;
    return;
  }
  if (y >= 134 && y <= 284 && x >= 40 && x <= 320) {
    ElmScanEntry entries[5];
    size_t n = elm_get_found(entries, 5);
    int row = (y - 134) / 30;
    if (row >= 0 && row < (int)n && y - (134 + row * 30) <= 28) {
      connectTarget = entries[row].address;
      elm_connect(connectTarget);
      elm_screen_dirty = true;
    }
  }
}

void poll_touch() {
  int32_t x = 0, y = 0;
  uint_fast8_t count = display_get().getTouch(&x, &y);

  if (count > 0) {
    if (!touch.pressed) {
      touch.pressed = true;
      touch.swipeHandled = false;
      touch.startX = x;
      touch.startY = y;
      touch.startedAt = millis();
    }
    touch.x = x;
    touch.y = y;

    // Detect the swipe as soon as the threshold is crossed, while the finger
    // is still moving -- waiting for release adds noticeable lag.
    if (!touch.swipeHandled) {
      int32_t dx = x - touch.startX;
      int32_t dy = y - touch.startY;
      int32_t adx = dx < 0 ? -dx : dx;
      int32_t ady = dy < 0 ? -dy : dy;
      uint32_t now = millis();
      if (now - touch.startedAt < 1000 && ady >= 60 && ady > adx * 2 &&
          now - touch.lastClick > 150) {
        touch.swipeHandled = true;
        touch.lastClick = now;
        if (dy < 0 && screen == Screen::Dash) {
          SD_LOGln("[UI] swipe up -> ELM connect");
          ui_show_elm_connect();
        } else if (dy > 0 && screen == Screen::ElmConnect) {
          SD_LOGln("[UI] swipe down -> dash");
          ui_hide_elm_connect();
        }
      }
    }
    return;
  }

  if (!touch.pressed) return;
  touch.pressed = false;

  if (touch.swipeHandled) return;

  uint32_t now = millis();
  int32_t dx = touch.x - touch.startX;
  int32_t dy = touch.y - touch.startY;
  int32_t adx = dx < 0 ? -dx : dx;
  int32_t ady = dy < 0 ? -dy : dy;

  if (now - touch.startedAt < 800 && adx < 24 && ady < 24 && now - touch.lastClick > 250) {
    touch.lastClick = now;
    SD_LOG("[UI] tap %d,%d\n", (int)touch.x, (int)touch.y);
    handle_tap(touch.x, touch.y);
  }
}

void poll_button() {
  static bool last = true;
  bool level = digitalRead(BTN_PIN) == LOW;
  if (level && !last && millis() - touch.lastClick > 250) {
    touch.lastClick = millis();
    SD_LOGln("[UI] button press");
    if (screen == Screen::Dash) ui_show_elm_connect();
    else ui_hide_elm_connect();
  }
  last = level;
}

}  // namespace

void ui_init() {
  pinMode(BTN_PIN, INPUT_PULLUP);
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
  last_sweep_ms = millis();
  sweep_step = 0;
  sweep_done = false;
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

void ui_show_elm_connect() {
  if (screen == Screen::ElmConnect) return;
  screen = Screen::ElmConnect;
  connectTarget = "";
  elm_screen_dirty = true;
  last_elm_draw = 0;
  // Refresh the device list unless we already have a live ELM connection.
  if (!elm_is_connected()) elm_start_scan();
  draw_elm_screen();
}

void ui_hide_elm_connect() {
  if (screen == Screen::Dash) return;
  screen = Screen::Dash;
  static_drawn = false;  // ELM screen overwrote the sprite

  // The cached values still match cur, so ui_show_dash() would skip every
  // dynamic element after redrawing only the static background. Force a full
  // redraw of all gauges after returning from the ELM screen.
  drawn.rpm = -999;
  drawn.speed_kmh = -999;
  drawn.coolant_c = -999;
  drawn.volt = -999;
  drawn.connected = !cur.connected;
  drawn_dot_phase = -1;
  dirty = true;
  ui_show_dash();
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
  int phase = cur.connected ? -1 : dot_phase;
  if (drawn.coolant_c != cur.coolant_c || fabsf(drawn.volt - cur.volt) > 0.05f ||
      drawn.connected != cur.connected || drawn_dot_phase != phase) {
    draw_bottom(cur.coolant_c, cur.volt, cur.connected);
    drawn.coolant_c = cur.coolant_c;
    drawn.volt = cur.volt;
    drawn.connected = cur.connected;
    drawn_dot_phase = phase;
  }
  dirty = false;
  flush();
}

void ui_loop() {
  uint32_t now = millis();
  bool connected = elm_is_connected();

  poll_touch();
  poll_button();

  if (!connected && !sweep_done && now - last_sweep_ms >= SWEEP_INTERVAL_MS) {
    last_sweep_ms = now;

    int s = sweep_step;
    int half = SWEEP_STEPS / 2;

    // RPM: 0 -> RPM_MAX -> 0.
    cur.rpm = (s <= half) ? (RPM_MAX * s / half)
                          : (RPM_MAX * (SWEEP_STEPS - s) / half);
    // Speed: 99 -> 0.
    cur.speed_kmh = 99 * (SWEEP_STEPS - s) / SWEEP_STEPS;
    // Coolant: 99 -> 0 in 5-degree steps.
    cur.coolant_c = 99 - min(s * 5 / 2, 99);
    // Voltage: 16.0 -> 0 in 0.5 V steps.
    float volt = 16.0f - s * 0.5f;
    cur.volt = volt > 0.0f ? volt : 0.0f;
    cur.connected = false;
    dirty = true;

    if (s >= SWEEP_STEPS) {
      sweep_done = true;
      cur.rpm = 0;
      cur.speed_kmh = 0;
      cur.coolant_c = 0;
      cur.volt = 0.0f;
    } else {
      sweep_step++;
    }
  }

  if ((!connected || screen == Screen::ElmConnect) && now - last_dot_ms >= DOT_PERIOD_MS) {
    last_dot_ms = now;
    dot_phase = (dot_phase + 1) % 3;
    dirty = true;
  }

  if (screen == Screen::ElmConnect) {
    // Full-screen push costs ~50 ms; avoid doing it on every dot tick.
    if (elm_screen_dirty || now - last_elm_draw >= 600) {
      last_elm_draw = now;
      elm_screen_dirty = false;
      draw_elm_screen();
    }
  } else {
    ui_show_dash();
  }
}
