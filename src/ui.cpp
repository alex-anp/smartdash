/*
 * Отрисовка дашборда и обработка ввода.
 *
 * Компоновка рассчитана на круглую панель 360x360:
 *   - внешняя дуга : сплошной индикатор RPM, радиусы 160..176, 150°..390°
 *   - внутренняя шкала: риски каждые 1000 об/мин, подписи каждые 2000, радиусы 138..150
 *   - центр        : семисегментная скорость 0..99 км/ч
 *   - низ          : температура, напряжение и статус ELM
 *
 * Если доступен полноэкранный 16-битный спрайт в PSRAM, весь вывод идёт в него.
 * За кадр перерисовываются только изменившиеся виджеты, затем спрайт один раз
 * выводится на панель — это убирает разрывы и мерцание. Статичная геометрия
 * шкалы рисуется только при инициализации или после выхода из экрана ELM Connect.
 */
#include "ui.h"

#include <math.h>

#include "config.h"
#include "display.h"
#include "elm327.h"

namespace {

// Внешняя дуга индикатора. Центр (180,180); старт 150° ставит ноль внизу слева,
// разворот 240° заканчивается внизу справа. Дуга намеренно сплошная: ранние
// сегментированные варианты конфликтовали с рисками и выглядели шумно.
constexpr int RPM_ARC_R_OUT = 176;
constexpr int RPM_ARC_R_IN = 160;
constexpr float RPM_ARC_START = 150.0f;  // всего 240 градусов
constexpr float RPM_ARC_SWEEP = 240.0f;
constexpr uint16_t RPM_ARC_BG_COLOR = 0x2104;

// Отдельная внутренняя шкала: риски каждые 1000 об/мин, цифры каждые 2000.
// Меньший радиус делает её читаемой и не задевает закрашенную дугу.
constexpr int RPM_SCALE_R_OUT = 150;
constexpr int RPM_SCALE_R_IN = 138;
constexpr int RPM_SCALE_TICK_PX = 2;
constexpr int RPM_SCALE_LABEL_R = 122;

// Полноэкранный back-буфер. PSRAM разгружает внутреннюю ОЗУ от кадра ~252 КБ;
// pushSprite() — единственная прямая запись в панель за кадр.
LGFX_Sprite canvas(&display_get());
bool canvas_ok = false;

DashData cur;       // Последние значения (анимация или ELM)
DashData drawn;     // Что сейчас нарисовано в спрайте
bool dirty = true;  // Хотя бы один виджет отличается от drawn
bool static_drawn = false;

// Одноразовая стартовая анимация вместо бесконечного демо. Идёт, только пока ELM
// не подключён; 40 шагов × 100 мс ≈ 4 секунды, затем все значения остаются нулями.
constexpr uint32_t SWEEP_INTERVAL_MS = 100;
constexpr int SWEEP_STEPS = 40;
uint32_t last_sweep_ms = 0;
int sweep_step = 0;
bool sweep_done = false;

enum class Screen { Dash, ElmConnect };
Screen screen = Screen::Dash;

// Три точки «поиска» сменяются каждые 260 мс. Та же фаза используется для точек
// в заголовке ELM Connect; drawn_dot_phase нужен для частичной перерисовки.
constexpr uint32_t DOT_PERIOD_MS = 260;
uint32_t last_dot_ms = 0;
int dot_phase = 0;
int drawn_dot_phase = -1;

uint32_t last_elm_draw = 0;
bool elm_screen_dirty = true;
String connectTarget;

// Конечный автомат тача. Свайп определяется, пока палец ещё на экране (маленькая
// задержка); тап формируется только при отпускании и смещении менее 24 px.
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

// Линейное преобразование RPM в угол с ограничением по RPM_MAX.
float rpm_angle(int rpm) {
  rpm = constrain(rpm, 0, RPM_MAX);
  return RPM_ARC_START + RPM_ARC_SWEEP * ((float)rpm / (float)RPM_MAX);
}

// Цель отрисовки: спрайт, если он создан; иначе панель напрямую.
lgfx::LGFX_Device& g() { return canvas_ok ? (lgfx::LGFX_Device&)canvas : display_get(); }

// Атомарный вывод кадра. Если спрайт не создан — ничего не делаем.
void flush() {
  if (!canvas_ok) return;
  canvas.pushSprite(0, 0);
}

// Три точки по центру cx. Активна ровно одна — простая анимация «работает»
// без рамки и вращающегося индикатора.
void draw_status_dots(lgfx::LGFX_Device& d, int cx, int y, uint16_t active, uint16_t inactive, int phase) {
  for (int i = 0; i < 3; i++) {
    bool lit = (i == phase);
    d.fillCircle(cx + (i - 1) * 14, y, lit ? 4 : 3, lit ? active : inactive);
  }
}

bool in_rect(int32_t x, int32_t y, int x0, int y0, int x1, int y1) {
  return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

// Незакрашенная часть тахометра. Вызывается и из draw_static(), и перед каждой
// перерисовкой RPM, потому что fillArc() затирает предыдущую цветную дугу.
void draw_arc_bg() {
  auto& d = g();
  d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START,
            RPM_ARC_START + RPM_ARC_SWEEP, RPM_ARC_BG_COLOR);
}

void draw_rpm_scale() {
  auto& d = g();

  // Статичная геометрия шкалы. Рисуется один раз в спрайт и больше не трогается:
  // draw_rpm() работает только с внешними радиусами 160..176.
  //
  // Тонкая опорная дуга внутри дуги индикатора.
  d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_SCALE_R_OUT, RPM_SCALE_R_OUT - 1,
            RPM_ARC_START, RPM_ARC_START + RPM_ARC_SWEEP, 0x4208);

  // Одна тонкая риска на каждые 1000 об/мин, полностью отделена от внешней дуги.
  for (int rpm = 0; rpm <= RPM_MAX; rpm += 1000) {
    float a = rpm_angle(rpm) * (float)M_PI / 180.0f;
    float ca = cosf(a), sa = sinf(a);
    float nx = -sa, ny = ca;
    int x1 = SCREEN_CENTER_X + (int)(ca * RPM_SCALE_R_OUT);
    int y1 = SCREEN_CENTER_Y + (int)(sa * RPM_SCALE_R_OUT);
    int x2 = SCREEN_CENTER_X + (int)(ca * RPM_SCALE_R_IN);
    int y2 = SCREEN_CENTER_Y + (int)(sa * RPM_SCALE_R_IN);

    int half = RPM_SCALE_TICK_PX / 2;
    for (int off = -half; off < RPM_SCALE_TICK_PX - half; off++) {
      int ox = (int)(nx * off);
      int oy = (int)(ny * off);
      d.drawLine(x1 + ox, y1 + oy, x2 + ox, y2 + oy, 0x8410);
    }
  }

  // Цифры каждые 2000 об/мин (в тысячах).
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int k = 0; k <= RPM_MAX / 1000; k += 2) {
    float a = rpm_angle(k * 1000) * (float)M_PI / 180.0f;
    int x = SCREEN_CENTER_X + (int)(cosf(a) * RPM_SCALE_LABEL_R);
    int y = SCREEN_CENTER_Y + (int)(sinf(a) * RPM_SCALE_LABEL_R);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", k);
    d.drawString(buf, x, y, 4);
  }
}

// Закрашиваем сплошной индикатор от нуля до текущих RPM. Зоны рисуются по очереди;
// каждый сегмент fillArc() ограничен интервалом [zone.from, zone.to].
void draw_rpm(int rpm) {
  rpm = constrain(rpm, 0, RPM_MAX);
  auto& d = g();
  draw_arc_bg();
  if (rpm > 0) {
    // Цвета из config.h: зелёный до красной зоны, оранжевый до 10k, красный до 12k.
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
  }
}

// Семисегментная цифра из скруглённых прямоугольников. Битовые маски — классическая
// раскладка abcdefg: бит0=A (верх), бит1=B (верхний правый), ... бит6=G (середина).
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

// Блок скорости. Сначала перекрашиваем чёрную область, чтобы старые сегменты
// исчезали при уменьшении значения (например, 99 -> 1). Значения 0..9 намеренно
// рисуются одной цифрой по центру.
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
    // Оставляем 0..9 (особенно ноль после анимации) явно по центру, а не с
    // пустым разрядом десятков и смещённой влево единственной цифрой.
    draw_seg_digit(d, SCREEN_CENTER_X - digitW / 2, y0, digitW, digitH, segT, d2, on, off);
  } else {
    draw_seg_digit(d, x0, y0, digitW, digitH, segT, d1, on, off);
    draw_seg_digit(d, x0 + digitW + spacing, y0, digitW, digitH, segT, d2, on, off);
  }

  d.setTextDatum(middle_center);
  d.setTextColor(0xC618, TFT_BLACK);
  d.drawString("km/h", SCREEN_CENTER_X, y0 + digitH + 18, 4);
}

// Маленькие контурные иконки рядом с числами в нижней части циферблата.
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

// Нижняя полоса: температура слева, напряжение справа, статус ELM ниже по центру.
// Цвета по фиксированным порогам, чтобы состояние читалось без цифр.
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

  // Индикатор ELM: три точки загораются по очереди во время подключения.
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

// Однократный фон: чёрная заливка, тонкая внешняя окружность, не закрашенная дуга
// RPM и внутренняя шкала. Динамические виджеты накладывает ui_show_dash().
void draw_static() {
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 179, 0x1082);
  draw_arc_bg();
  draw_rpm_scale();
  static_drawn = true;
  dirty = true;
}

// Человекочитаемая строка статуса для ELM Connect. Если задан конкретный адрес,
// он добавлен в текст — так проще понять, какое подключение не удалось.
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

// Полноэкранный экран подключения. Весь контент держится внутри вписанной окружности;
// показывается до пяти результатов BLE с цветом по уровню RSSI.
void draw_elm_screen() {
  auto& d = g();
  ElmStatus st = elm_status();

  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);

  // Круглый экран: у верхнего и нижнего краёв доступная ширина меньше.
  // Держим всё внутри вписанной окружности (радиус ~175 от центра 180,180).
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

      // Короткий MAC (последние 8 символов) меньшим шрифтом на второй строке.
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

// Тап-зоны только на экране ELM. На дашборде тапы игнорируются: навигация там
// свайпом или кнопкой, чтобы во время езды случайно не сменить режим.
void handle_tap(int32_t x, int32_t y) {
  if (screen == Screen::Dash) return;  // Свайп вверх открывает экран ELM.

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

// Опрос CST816S. Вертикальный свайп >=60 px за 1 с переключает экраны;
// проверка adx не даёт принять диагональное движение за свайп.
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

    // Определяем свайп сразу после превышения порога, пока палец ещё движется:
    // ожидание отпускания добавляет заметную задержку.
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

// Пин BOOT — резервная кнопка навигации (активный низкий уровень, INPUT_PULLUP).
// Использует тот же интервал подавления дребезга, что и тач, чтобы не было двойных событий.
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

// Выделяем спрайт и заполняем кэш отрисовки невозможными значениями, чтобы первая
// же ui_show_dash() перерисовала все виджеты, включая видимый ноль скорости.
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

// Вызывается из BLE-задачи. Только сохраняет снимок и поднимает флаг dirty;
// сама отрисовка остаётся на ядре UI, поэтому LGFX не используется из двух потоков.
void ui_set_data(const DashData& data) {
  cur = data;
  dirty = true;
}

// Стартовая заставка на 700 мс перед первым кадром дашборда.
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

// Простой экран статуса оставлен для совместимости/отладки; обычный сценарий
// идёт от заставки сразу к дашборду с мигающими точками ELM.
void ui_show_search() {
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_ORANGE, TFT_BLACK);
  d.drawString("Searching ELM...", SCREEN_CENTER_X, SCREEN_CENTER_Y, 4);
  flush();
}

// Открываем ELM Connect. Новый поиск запускается только если нет активной связи,
// поэтому просмотр экрана при подключении не рвёт сессию OBD.
void ui_show_elm_connect() {
  if (screen == Screen::ElmConnect) return;
  screen = Screen::ElmConnect;
  connectTarget = "";
  elm_screen_dirty = true;
  last_elm_draw = 0;
  // Обновляем список устройств, только если нет работающего соединения с ELM.
  if (!elm_is_connected()) elm_start_scan();
  draw_elm_screen();
}

// Возврат на дашборд. Экран ELM перезаписал весь спрайт, поэтому и статический фон,
// и все динамические значения нужно принудительно перерисовать.
void ui_hide_elm_connect() {
  if (screen == Screen::Dash) return;
  screen = Screen::Dash;
  static_drawn = false;  // Экран ELM перезаписал спрайт

  // Кэшированные значения всё ещё равны cur, поэтому после перерисовки одного фона
  // ui_show_dash() пропустила бы все динамические элементы. Принудительно обновляем
  // все шкалы после возврата с экрана ELM.
  drawn.rpm = -999;
  drawn.speed_kmh = -999;
  drawn.coolant_c = -999;
  drawn.volt = -999;
  drawn.connected = !cur.connected;
  drawn_dot_phase = -1;
  dirty = true;
  ui_show_dash();
}

// Рендер по «грязным» виджетам. У каждого виджета своё сравнение с кэшем, чтобы
// при изменении только напряжения или точки статуса не перерисовывать дугу RPM.
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

// Основной тик UI. Порядок важен: сначала ввод может сменить экран, затем
// отрисовывается активный. Данные от реального ELM сразу останавливают анимацию.
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
    // Скорость: 99 -> 0.
    cur.speed_kmh = 99 * (SWEEP_STEPS - s) / SWEEP_STEPS;
    // Температура: 99 -> 0 шагами по 5 градусов.
    cur.coolant_c = 99 - min(s * 5 / 2, 99);
    // Напряжение: 16.0 -> 0 шагами по 0.5 В.
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
    // Вывод всего экрана стоит ~50 мс; не делаем его на каждый шаг точек.
    if (elm_screen_dirty || now - last_elm_draw >= 600) {
      last_elm_draw = now;
      elm_screen_dirty = false;
      draw_elm_screen();
    }
  } else {
    ui_show_dash();
  }
}
