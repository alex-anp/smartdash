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
 * шкалы рисуется только при инициализации или после выхода из экрана ELM Monitor.
 */
#include "ui.h"

#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "display.h"
#include "dragrace.h"
#include "elm327.h"
#include "font_euro_digits.h"
#include "perf.h"
#include "settings.h"
#include "sim.h"
#include "storage.h"
#include "web.h"

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
constexpr uint16_t RPM_SCALE_COLOR = 0x8044;  // #820826, тёмно-красная шкала с рисками

// Блок скорости. "km/h" рисуется один раз в статике; при изменении значения
// перекрашиваем только прямоугольник цифр, а не всю центральную область.
// Оба разряда (десятки и единицы) существуют всегда: у значений 0..9 старший
// разряд остаётся погашенным, но его сегменты видны как часть индикатора.
constexpr int SPEED_DIGIT_W = 56;
constexpr int SPEED_DIGIT_H = 96;
constexpr int SPEED_SEG_T = 10;
constexpr int SPEED_SPACING = 14;
constexpr int SPEED_Y0 = SCREEN_CENTER_Y - 56;
constexpr int SPEED_CLEAR_X = 110;
constexpr int SPEED_CLEAR_Y = 118;
constexpr int SPEED_CLEAR_W = 140;
constexpr int SPEED_CLEAR_H = 104;  // до y=221; подпись km/h начинается с y=225

constexpr int BOTTOM_X = 30;
constexpr int BOTTOM_Y = 278;
constexpr int BOTTOM_W = 300;
constexpr int BOTTOM_H = 60;

// Полноэкранный back-буфер. PSRAM разгружает внутреннюю ОЗУ от кадра ~252 КБ;
// pushSprite() — единственная прямая запись в панель за кадр.
LGFX_Sprite canvas(&display_get());
bool canvas_ok = false;

// Частичный вывод нельзя отдавать построчно из PSRAM: на этой QSPI-панели
// такой путь даёт битые пиксели. Копируем полосу «грязного» прямоугольника в
// небольшой бустер внутренней DMA-памяти и отдаём её одним блоком — тем же
// путём, которым работает полный кадр.
constexpr int PARTIAL_SLICE_H = 32;
uint16_t* partialBuf = nullptr;

// Отложенные области спрайта, которые нужно вывести на панель. Обычно их 1–3:
// дуга RPM, блок скорости, нижняя строка. Полный кадр — только при смене экрана.
constexpr uint8_t DIRTY_RECT_MAX = 8;
struct DirtyRect { int16_t x, y, w, h; };
DirtyRect dirtyRects[DIRTY_RECT_MAX];
uint8_t dirtyRectCount = 0;
bool needFullFlush = false;

DashData cur;       // Последние значения (анимация или ELM), только UI-задача
DashData drawn;     // Что сейчас нарисовано в спрайте
bool dirty = true;  // Хотя бы один виджет отличается от drawn

// Инерция стрелки тахометра: дуга рисуется по сглаженному значению, а не по
// сырому cur.rpm. Падение медленнее роста — как у механической стрелки.
// Сглаживание идёт каждый тик UI, а не только по новым снимкам: после резкого
// падения оборотов стрелка плавно возвращается к цели в течение ~0.6 с.
constexpr float RPM_TAU_UP_MS = 250.0f;
constexpr float RPM_TAU_DOWN_MS = 600.0f;
float shownRpm = 0.0f;
uint32_t lastRpmSmoothMs = 0;
bool static_drawn = false;

// Snapshot из BLE-задачи. Он копируется под мьютексом и забирается в ui_loop(),
// чтобы cur не менялся прямо во время отрисовки (и чтобы был таймштамп сэмпла).
DashData latest;
bool haveLatest = false;
portMUX_TYPE dataMux = portMUX_INITIALIZER_UNLOCKED;

// Одноразовая стартовая анимация вместо бесконечного демо. Идёт, только пока ELM
// не подключён; 40 шагов × 100 мс ≈ 4 секунды, затем все значения остаются нулями.
constexpr uint32_t SWEEP_INTERVAL_MS = 100;
constexpr int SWEEP_STEPS = 40;
uint32_t last_sweep_ms = 0;
int sweep_step = 0;
bool sweep_done = false;

enum class Screen { Dash, ElmConnect, Drag, Settings };
Screen screen = Screen::Dash;
Screen elmReturnScreen = Screen::Dash;  // куда вернуться при свайпе вниз с ELM

// Навигация разделена по осям и по текущему экрану (экраны ELM/Settings — модальные):
//   с Dash: вверх — ELM Monitor, вниз — Settings, влево/вправо — Drag;
//   с ELM Monitor: вниз — вернуться туда, откуда пришли;
//   с Settings: вверх — вернуться на Dash;
//   на Drag вертикальные свайпы игнорируются, чтобы не прервать замер.
Screen otherMainScreen(Screen s) { return s == Screen::Drag ? Screen::Dash : Screen::Drag; }

void switchScreen(Screen next);  // определено ниже, после draw_* и saveDragResult

// Состояние экрана замера разгона. Цель фиксированная: замер завершается по
// сбросу оборотов, а результат пишется по реально достигнутой скорости.
uint32_t lastDragDraw = 0;
bool dragScreenDirty = true;
DragState lastDragState = DragState::Disabled;

// Три точки «поиска» сменяются каждые 260 мс. Та же фаза используется для точек
// в заголовке ELM Monitor; drawn_dot_phase нужен для частичной перерисовки.
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
  uint32_t lastClick = 0;   // защита от повторного свайпа/тапа/кнопки
  uint32_t lastTapAt = 0;   // для двойного тапа на экране замера
};
TouchInput touch;

// Экран Settings только показывает значения: редактирование перенесено в веб.
// web_loop() выставляет флаг, ui_loop() инвалидирует кэши и перерисовывает экран.
enum class SettingId : uint8_t {
  RpmMax, Redline, CoolLo, CoolHi, VoltLo, VoltMid, VoltHi, Rotation, Anim, Model, Count
};
bool settingsRedrawPending = false;
int appliedRotation = -1;  // уже применённый к панели поворот, градусы

// Линейное преобразование RPM в угол с ограничением по текущей шкале.
float rpm_angle(int rpm) {
  rpm = constrain(rpm, 0, gSettings.rpmMax);
  return RPM_ARC_START + RPM_ARC_SWEEP * ((float)rpm / (float)gSettings.rpmMax);
}

// Цель отрисовки: спрайт, если он создан; иначе панель напрямую.
lgfx::LGFX_Device& g() { return canvas_ok ? (lgfx::LGFX_Device&)canvas : display_get(); }

// Поворот меняет MADCTL панели и адресацию спрайта, поэтому применяется только
// здесь — в UI-задаче — и всегда сопровождается полной перерисовкой кадра.
void apply_rotation() {
  if (appliedRotation == gSettings.rotation) return;
  appliedRotation = gSettings.rotation;
  display_set_rotation(appliedRotation);
  dirtyRectCount = 0;
  needFullFlush = true;
}

// Полный вывод кадра: используется при смене экрана и после перерисовки статики.
// Все отложенные «грязные» прямоугольники при этом уже не нужны.
void flush() {
  dirtyRectCount = 0;
  needFullFlush = false;
  if (!canvas_ok) return;
  display_get().clearClipRect();  // полный кадр не должен попасть под старый clip
  canvas.pushSprite(0, 0);
}

// Частичный вывод. Прямоугольник задаётся в координатах спрайта; clipRect на
// панели заставляет pushSprite() отдать только его, а не весь кадр 360×360.
// За счёт этого одно изменение оборотов передаёт не 252 КБ, а обычно 1–5 КБ.
void addDirtyRect(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
  if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;
  if (w <= 0 || h <= 0) return;

  // Слипаем пересекающиеся области: их всё равно нельзя вывести независимо
  // без риска затереть соседний виджет.
  for (uint8_t i = 0; i < dirtyRectCount; i++) {
    DirtyRect& r = dirtyRects[i];
    const int rx = r.x, ry = r.y, rw = r.w, rh = r.h;
    const int x1 = max(rx, x);
    const int y1 = max(ry, y);
    const int x2 = min(rx + rw, x + w);
    const int y2 = min(ry + rh, y + h);
    if (x1 < x2 && y1 < y2) {
      const int nx = min(rx, x);
      const int ny = min(ry, y);
      r.w = max(rx + rw, x + w) - nx;
      r.h = max(ry + rh, y + h) - ny;
      r.x = nx;
      r.y = ny;
      return;
    }
  }
  if (dirtyRectCount < DIRTY_RECT_MAX) {
    dirtyRects[dirtyRectCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
  } else {
    // Хуже всего — вывести один кадр целиком, но никогда не теряем обновление.
    needFullFlush = true;
  }
}

// Вывести один «грязный» прямоугольник одним DMA-блоком. Источник — внутренняя
// память, поэтому LovyanGFX не идёт построчным CPU-путем из PSRAM (он давал
// битые сектора на дуге). Ширина буфера равна ширине передаваемой полосы,
// как и у полного кадра, — это проверенный путь.
void partialFlushRect(const DirtyRect& r) {
  if (!partialBuf) { needFullFlush = true; return; }
  auto& disp = display_get();
  const uint16_t* src = static_cast<const uint16_t*>(canvas.getBuffer());
  for (int y = r.y; y < r.y + r.h; y += PARTIAL_SLICE_H) {
    const int rows = min(PARTIAL_SLICE_H, r.y + r.h - y);
    for (int row = 0; row < rows; ++row) {
      memcpy(partialBuf + row * r.w, src + (y + row) * SCREEN_WIDTH + r.x,
             (size_t)r.w * sizeof(uint16_t));
    }
    disp.pushImageDMA(r.x, y, r.w, rows, partialBuf);
  }
}

void flushDirty() {
  if (!canvas_ok) { dirtyRectCount = 0; needFullFlush = false; return; }
  // Стартовая анимация меняет за 100 мс сразу дугу, скорость и низ — за 4 с
  // это десятки передач. Выводим её целым кадром: анимация не влияет на опрос.
  if (needFullFlush || !sweep_done || !partialBuf) { flush(); return; }
  if (dirtyRectCount == 0) return;

  for (uint8_t i = 0; i < dirtyRectCount; i++) partialFlushRect(dirtyRects[i]);
  dirtyRectCount = 0;
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

// Ограничивающий прямоугольник участка дуги. Учитываем ОБА радиуса кольца:
// оценка только по наружному (176) теряет до 16 px у внутреннего края (160),
// из-за чего на панели оставались старые пиксели — «битые сектора» при анимации.
// Шаг 2° и запас 2 px; для небольшой дельты RPM это обычно 10–30 px.
void markArcDirty(float fromAngle, float toAngle) {
  if (toAngle < fromAngle) { const float t = fromAngle; fromAngle = toAngle; toAngle = t; }
  int minX = SCREEN_WIDTH, minY = SCREEN_HEIGHT, maxX = 0, maxY = 0;
  auto addPoint = [&](float deg) {
    const float a = deg * (float)M_PI / 180.0f;
    const float ca = cosf(a), sa = sinf(a);
    // Оба края кольца; промежуточные радиусы лежат внутри их биссектрисы.
    for (int r : {RPM_ARC_R_OUT, RPM_ARC_R_IN}) {
      const int x = SCREEN_CENTER_X + (int)lroundf(ca * r);
      const int y = SCREEN_CENTER_Y + (int)lroundf(sa * r);
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
  };
  addPoint(fromAngle);
  for (float a = ceilf(fromAngle / 2.0f) * 2.0f; a < toAngle; a += 2.0f) addPoint(a);
  addPoint(toAngle);
  addDirtyRect(minX - 2, minY - 2, maxX - minX + 5, maxY - minY + 5);
}

// Незакрашенная часть тахометра. Рисуется только в статике и при полном
// обновлении дуги; при обычном изменении RPM стирается лишь изменившийся кусок.
void draw_arc_bg() {
  auto& d = g();
  d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, RPM_ARC_START,
            RPM_ARC_START + RPM_ARC_SWEEP, RPM_ARC_BG_COLOR);
  markArcDirty(RPM_ARC_START, RPM_ARC_START + RPM_ARC_SWEEP);
}

void draw_rpm_scale() {
  auto& d = g();

  // Статичная геометрия шкалы. Рисуется один раз в спрайт и больше не трогается:
  // draw_rpm() работает только с внешними радиусами 160..176.
  //
  // Тонкая опорная дуга внутри дуги индикатора.
  d.fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_SCALE_R_OUT, RPM_SCALE_R_OUT - 1,
            RPM_ARC_START, RPM_ARC_START + RPM_ARC_SWEEP, RPM_SCALE_COLOR);

  // Одна тонкая риска на каждые 1000 об/мин, полностью отделена от внешней дуги.
  for (int rpm = 0; rpm <= gSettings.rpmMax; rpm += 1000) {
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
      d.drawLine(x1 + ox, y1 + oy, x2 + ox, y2 + oy, RPM_SCALE_COLOR);
    }
  }

  // Цифры каждые 2000 об/мин (в тысячах).
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int k = 0; k <= gSettings.rpmMax / 1000; k += 2) {
    float a = rpm_angle(k * 1000) * (float)M_PI / 180.0f;
    int x = SCREEN_CENTER_X + (int)(cosf(a) * RPM_SCALE_LABEL_R);
    int y = SCREEN_CENTER_Y + (int)(sinf(a) * RPM_SCALE_LABEL_R);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", k);
    d.drawString(buf, x, y, &EURO_Digits);
  }
}

// Внешние зоны индикатора. Границы берутся из настроек: красная зона начинается
// через SETTINGS_RPM_ZONE_SPAN об/мин после redline.
struct RpmZone { int from; int to; uint16_t color; };

// Залить один интервал дуги и пометить только его границы как грязные.
void fillArcRange(int rpmFrom, int rpmTo, uint16_t color) {
  rpmFrom = constrain(rpmFrom, 0, gSettings.rpmMax);
  rpmTo = constrain(rpmTo, 0, gSettings.rpmMax);
  if (rpmFrom >= rpmTo) return;
  const float a0 = rpm_angle(rpmFrom);
  const float a1 = rpm_angle(rpmTo);
  g().fillArc(SCREEN_CENTER_X, SCREEN_CENTER_Y, RPM_ARC_R_OUT, RPM_ARC_R_IN, a0, a1, color);
  markArcDirty(a0, a1);
}

// Цветной участок [from, to] с разбивкой по зонам. Зоны считаются от текущих
// настроек, поэтому после их изменения статику нужно перерисовать целиком.
void drawRpmColorRange(int from, int to) {
  const int mid = min(gSettings.rpmRedline + SETTINGS_RPM_ZONE_SPAN, gSettings.rpmMax);
  const RpmZone zones[] = {
      {0, gSettings.rpmRedline, TFT_GREEN},
      {gSettings.rpmRedline, mid, TFT_ORANGE},
      {mid, gSettings.rpmMax, TFT_RED},
  };
  for (const auto& z : zones) {
    const int a = max(from, z.from);
    const int b = min(to, z.to);
    if (a < b) fillArcRange(a, b, z.color);
  }
}

// Полная перерисовка дуги (после смены экрана/статики).
void draw_rpm(int rpm) {
  rpm = constrain(rpm, 0, gSettings.rpmMax);
  draw_arc_bg();
  drawRpmColorRange(0, rpm);
}

// Инкрементальное обновление: при росте заливаем только новый участок,
// при падении — стираем только убранный. Вместо ~(352×264) пикселей обычно
// перерисовываются единицы градусов дуги.
void update_rpm(int oldRpm, int newRpm) {
  oldRpm = constrain(oldRpm, 0, gSettings.rpmMax);
  newRpm = constrain(newRpm, 0, gSettings.rpmMax);
  if (newRpm == oldRpm) return;
  if (newRpm < oldRpm) fillArcRange(newRpm, oldRpm, RPM_ARC_BG_COLOR);
  else drawRpmColorRange(oldRpm, newRpm);
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

// Блок скорости. Подпись "km/h" рисуется один раз в draw_static(), поэтому здесь
// перекрашивается только прямоугольник цифр. Оба разряда рисуются всегда: у значений
// 0..9 десяткам передаётся -1 — все сегменты гасятся, но индикатор остаётся видимым.
void draw_speed(int speed) {
  auto& d = g();
  speed = constrain(speed, 0, 99);

  const int totalW = SPEED_DIGIT_W * 2 + SPEED_SPACING;
  const int x0 = SCREEN_CENTER_X - totalW / 2;

  d.fillRect(SPEED_CLEAR_X, SPEED_CLEAR_Y, SPEED_CLEAR_W, SPEED_CLEAR_H, TFT_BLACK);
  addDirtyRect(SPEED_CLEAR_X, SPEED_CLEAR_Y, SPEED_CLEAR_W, SPEED_CLEAR_H);

  const uint16_t on = 0xFFFF;
  const uint16_t off = 0x1082;
  const int d1 = speed / 10;
  const int d2 = speed % 10;

  draw_seg_digit(d, x0, SPEED_Y0, SPEED_DIGIT_W, SPEED_DIGIT_H, SPEED_SEG_T,
                 d1 == 0 ? -1 : d1, on, off);
  draw_seg_digit(d, x0 + SPEED_DIGIT_W + SPEED_SPACING, SPEED_Y0, SPEED_DIGIT_W,
                 SPEED_DIGIT_H, SPEED_SEG_T, d2, on, off);
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
  d.fillRect(BOTTOM_X, BOTTOM_Y, BOTTOM_W, BOTTOM_H, TFT_BLACK);
  addDirtyRect(BOTTOM_X, BOTTOM_Y, BOTTOM_W, BOTTOM_H);
  uint16_t tColor = 0xC618;
  if (coolant_c >= 0) {
    if (coolant_c < gSettings.coolantLow) tColor = TFT_CYAN;
    else if (coolant_c <= gSettings.coolantHigh) tColor = TFT_GREEN;
    else tColor = TFT_RED;
  }

  uint16_t vColor = 0xC618;
  if (volt > 0.1f) {
    if (volt < gSettings.voltLow) vColor = TFT_RED;
    else if (volt < gSettings.voltMid) vColor = TFT_ORANGE;
    else if (volt <= gSettings.voltHigh) vColor = TFT_GREEN;
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
// RPM, внутренняя шкала и подпись скорости. Динамические виджеты накладывает
// ui_show_dash(). Статику нужно вывести целиком — partial flush здесь не годится.
void draw_static() {
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.drawCircle(SCREEN_CENTER_X, SCREEN_CENTER_Y, 179, 0x1082);
  draw_arc_bg();
  draw_rpm_scale();
  // Подпись скорости тем же шрифтом, что и статус ELM (Font2, без масштабирования).
  // Правый край совпадает с правым краем второго (младшего) разряда.
  const int speedRight = SCREEN_CENTER_X + (SPEED_DIGIT_W * 2 + SPEED_SPACING) / 2;
  d.setTextDatum(middle_right);
  d.setTextColor(0xC618, TFT_BLACK);
  d.drawString("km/h", speedRight, SPEED_Y0 + SPEED_DIGIT_H + 18, 2);
  d.setTextDatum(middle_center);

  // После полной перерисовки кэш виджетов недействителен: RPM рисуем целиком,
  // остальные — обычным сравнением с sentinel-значениями.
  drawn.rpm = -999;
  drawn.speed_kmh = -999;
  drawn.coolant_c = -999;
  drawn.volt = -999;
  drawn.connected = !cur.connected;
  drawn_dot_phase = -1;
  static_drawn = true;
  dirty = true;
  needFullFlush = true;
}

// Человекочитаемая строка статуса для ELM Monitor. Если задан конкретный адрес,
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

// Сведения об уже подключённом адаптере. Вместо списка сканирования показываем
// имя, адрес, тип адреса, протокол и время соединения; searching-индикатор не нужен.
void draw_connected_info(lgfx::LGFX_Device& d, const ElmConnectedInfo& info) {
  char buf[64];
  d.setTextDatum(middle_center);

  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString("NAME", SCREEN_CENTER_X, 148, 1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.22s", info.name);
  d.drawString(buf, SCREEN_CENTER_X, 166, 2);

  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString("ADDRESS", SCREEN_CENTER_X, 188, 1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(info.address, SCREEN_CENTER_X, 206, 2);

  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString("PROTOCOL", SCREEN_CENTER_X, 226, 1);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString(info.proto, SCREEN_CENTER_X, 244, 2);

  const char* type = !info.typeKnown ? "?" : (info.addrType == 0 ? "Public" : "Random");
  const uint32_t upS = (millis() - info.sinceMs) / 1000;
  if (info.rssi > -127) {
    snprintf(buf, sizeof(buf), "%s  %d dBm  UP %u:%02u", type, info.rssi,
             (unsigned)(upS / 60), (unsigned)(upS % 60));
  } else {
    snprintf(buf, sizeof(buf), "%s  RSSI --  UP %u:%02u", type,
             (unsigned)(upS / 60), (unsigned)(upS % 60));
  }
  d.setTextColor(0xC618, TFT_BLACK);
  d.drawString(buf, SCREEN_CENTER_X, 266, 1);
}

// Полноэкранный экран подключения. Весь контент держится внутри вписанной окружности.
// До подключения — статус и до пяти результатов BLE с цветом по уровню RSSI.
// После подключения — данные активного ELM; новый поиск только по тапу Scan.
void draw_elm_screen() {
  auto& d = g();
  ElmStatus st = elm_status();

  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);

  // Круглый экран: у верхнего и нижнего краёв доступная ширина меньше.
  // Держим всё внутри вписанной окружности (радиус ~175 от центра 180,180).
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("ELM", SCREEN_CENTER_X, 38, 4);

  ElmConnectedInfo info;
  const bool linked = elm_get_connected(info);
  uint16_t statusColor = linked ? TFT_GREEN :
                         (st == ElmStatus::Idle) ? 0x8410 : TFT_ORANGE;
  d.setTextColor(statusColor, TFT_BLACK);
  d.drawString(linked ? "ELM CONNECTED" : elm_status_text(st), SCREEN_CENTER_X, 74, 2);
  if (!linked && (st == ElmStatus::Scanning || st == ElmStatus::Connecting ||
                  st == ElmStatus::Initializing)) {
    draw_status_dots(d, 300, 74, statusColor, 0x4208, dot_phase);
  }

  d.drawRoundRect(130, 94, 100, 28, 6, 0x4208);
  d.setTextColor(0xC618, TFT_BLACK);
  d.drawString("Scan", SCREEN_CENTER_X, 108, 2);

  d.drawFastHLine(40, 128, 280, 0x2104);

  if (linked) {
    draw_connected_info(d, info);
  } else {
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
  }

  d.drawFastHLine(40, 288, 280, 0x2104);

  d.setTextDatum(middle_center);
  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString(linked ? "TAP SCAN TO RECONNECT" : "Tap device to connect",
               SCREEN_CENTER_X, 308, 1);
  d.drawString("SWIPE DOWN - BACK", SCREEN_CENTER_X, 322, 1);

  flush();
}

// "3.10" = 3 с 10 сотых. Для замеров длиннее 100 с добавляем минуты.
void format_cs(uint32_t cs, char* buf, size_t len) {
  if (cs >= 10000) {
    snprintf(buf, len, "%u:%02u.%02u", (unsigned)(cs / 6000),
             (unsigned)((cs % 6000) / 100), (unsigned)(cs % 100));
  } else {
    snprintf(buf, len, "%u.%02u", (unsigned)(cs / 100), (unsigned)(cs % 100));
  }
}

// Сохраняем последний и лучший замер. В общий журнал пишем только итог; все
// сэмплы уже лежат в отдельном CSV заезда.
void saveDragResult() {
  DragResult r = drag_result();
  String line = drag_serialize(r);
  storage_save_drag_last(line);

  // Лучший результат хранится отдельно по целям: время 0-60 не должно
  // считаться рекордом 0-100. Прерванный замер в рекорды не идёт.
  if (r.abort == DragAbort::None) {
    String best = storage_get_drag_best(r.target);
    if (best.length() == 0 || drag_serialized_total(line) < drag_serialized_total(best)) {
      storage_save_drag_best(r.target, line);
    }
  }
  storage_log("DRAG %s", line.c_str());
}

// Экран замера: состояние, живой таймер и таблица отметок 10..100 км/ч.
// Полная перерисовка раз в 100 мс дешевле, чем отслеживание отдельных зон,
// и не мешает BLE-задаче (весь вывод идёт в спрайт на UI-ядре).
void draw_drag_screen() {
  auto& d = g();
  DragState st = drag_state();
  DragResult r = drag_result();

  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);

  // Заголовок экрана фиксированный: цель заранее не выбирается, а достигнутая
  // скорость выводится вместе со временем в основной строке ниже.
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("DRAG MOD", SCREEN_CENTER_X, 24, 2);

  const char* stateText = "IDLE";
  uint16_t stateColor = 0x8410;
  switch (st) {
    case DragState::Waiting:  stateText = "WAIT IDLE RPM"; stateColor = TFT_ORANGE; break;
    case DragState::Armed:    stateText = "ARMED - GO!";   stateColor = TFT_GREEN;  break;
    case DragState::Running:  stateText = "RUNNING";       stateColor = TFT_GREEN;  break;
    case DragState::Finished: stateText = "DONE";          stateColor = TFT_CYAN;   break;
    case DragState::Aborted:  stateText = drag_abort_text(r.abort); stateColor = TFT_RED; break;
    default: break;
  }
  d.setTextColor(stateColor, TFT_BLACK);
  d.drawString(stateText, SCREEN_CENTER_X, 48, 2);

  uint32_t shown_cs = 0;
  if (st == DragState::Running) shown_cs = drag_elapsed_cs();
  else if (st == DragState::Finished || st == DragState::Aborted) shown_cs = r.total_cs;
  // В одной строке и текущая (или достигнутая) скорость, и время: "55 - 5.35".
  int shown_speed = cur.speed_kmh;
  if (st == DragState::Finished) shown_speed = r.target;
  char tbuf[16];
  format_cs(shown_cs, tbuf, sizeof(tbuf));
  char lineBuf[32];
  snprintf(lineBuf, sizeof(lineBuf), "%d - %s", shown_speed, tbuf);
  // Крупная строка: Font6 с масштабом 0.8 (~50 px) вместо Font4 (~32 px).
  // Для длинных значений (трёхзначная скорость) падаем обратно на Font4.
  d.setTextSize(0.8f);
  const uint8_t bigFont =
      (d.textWidth(lineBuf, &fonts::Font6) <= SCREEN_WIDTH - 24) ? 6 : 4;
  if (bigFont == 4) d.setTextSize(1.0f);
  d.setTextColor(st == DragState::Aborted ? TFT_RED : TFT_WHITE, TFT_BLACK);
  d.drawString(lineBuf, SCREEN_CENTER_X, 90, bigFont);
  d.setTextSize(1.0f);

  // Три равные колонки по 72 px, текст выровнен по центру каждой.
  // Таблица 216 px с полями по 72 px слева и справа — симметрия по центру экрана.
  constexpr int TABLE_LEFT = 72;
  constexpr int TABLE_WIDTH = 216;
  constexpr int COL_W = 72;
  constexpr int COL_SPEED_X = TABLE_LEFT + COL_W / 2;
  constexpr int COL_TIME_X = TABLE_LEFT + COL_W * 3 / 2;
  constexpr int COL_SPLIT_X = TABLE_LEFT + COL_W * 5 / 2;

  d.setTextDatum(middle_center);
  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString("KM/H", COL_SPEED_X, 122, 1);
  d.drawString("TIME", COL_TIME_X, 122, 1);
  d.drawString("SPLIT", COL_SPLIT_X, 122, 1);
  d.drawFastHLine(TABLE_LEFT, 132, TABLE_WIDTH, 0x2104);

  for (int i = 0; i < DRAG_MAX_SPLITS; i++) {
    int k = (i + 1) * DRAG_SPLIT_STEP;
    int y = 142 + i * 17;

    // Следующая недостигнутая отметка подсвечивается, чтобы было видно цель.
    bool next = st == DragState::Running && !r.has[i] && (i == 0 || r.has[i - 1]);
    uint16_t color = r.has[i] ? (next ? TFT_GREEN : TFT_WHITE) : (next ? TFT_ORANGE : 0x4208);

    char buf[12];
    snprintf(buf, sizeof(buf), "%d", k);
    d.setTextColor(color, TFT_BLACK);
    d.drawString(buf, COL_SPEED_X, y, 2);

    if (r.has[i]) {
      format_cs(r.split_cs[i], buf, sizeof(buf));
      d.drawString(buf, COL_TIME_X, y, 2);
      format_cs(r.delta_cs[i], buf, sizeof(buf));
      d.drawString(buf, COL_SPLIT_X, y, 2);
    } else {
      d.drawString("--.--", COL_TIME_X, y, 2);
      d.drawString("--.--", COL_SPLIT_X, y, 2);
    }
  }

  d.setTextDatum(middle_center);
  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString("DOUBLE TAP - RESET", SCREEN_CENTER_X, 328, 1);
  d.drawString("SWIPE L/R - DASH", SCREEN_CENTER_X, 348, 1);
  flush();
}

// --- Экран настроек -------------------------------------------------------
// Read-only список: подписи и текущие значения тех же полей, что и в веб-форме.
// Напряжение хранится в settings.cpp в десятых долях вольта.

const char* settingLabel(SettingId id) {
  switch (id) {
    case SettingId::RpmMax:  return "RPM MAX";
    case SettingId::Redline: return "REDLINE";
    case SettingId::CoolLo:  return "COOL LOW";
    case SettingId::CoolHi:  return "COOL HIGH";
    case SettingId::VoltLo:  return "VOLT LOW";
    case SettingId::VoltMid: return "VOLT MID";
    case SettingId::VoltHi:  return "VOLT HIGH";
    case SettingId::Rotation: return "ROTATION";
    case SettingId::Anim:    return "BOOT ANIM";
    case SettingId::Model:   return "MODEL";
    default:                 return "";
  }
}

String settingValue(SettingId id) {
  switch (id) {
    case SettingId::RpmMax:  return String(gSettings.rpmMax);
    case SettingId::Redline: return String(gSettings.rpmRedline);
    case SettingId::CoolLo:  return String(gSettings.coolantLow) + "C";
    case SettingId::CoolHi:  return String(gSettings.coolantHigh) + "C";
    case SettingId::VoltLo:  return String(gSettings.voltLow, 1) + "V";
    case SettingId::VoltMid: return String(gSettings.voltMid, 1) + "V";
    case SettingId::VoltHi:  return String(gSettings.voltHigh, 1) + "V";
    case SettingId::Rotation: return String(gSettings.rotation);
    case SettingId::Anim:    return gSettings.bootAnim ? String("ON") : String("OFF");
    case SettingId::Model:   return String(gSettings.model);
    default:                 return String();
  }
}

// Read-only список текущих значений плюс кнопка включения веба. Править поля
// надо с телефона; радио включается только по тапу и сохраняется в NVS.
void draw_settings_screen() {
  auto& d = g();
  const bool webOn = web_is_enabled();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("SETTINGS", SCREEN_CENTER_X, 34, 2);
  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString(webOn ? web_ap_ssid().c_str() : "WEB DISABLED", SCREEN_CENTER_X, 56, 1);
  d.drawFastHLine(40, 68, 280, 0x2104);

  for (int i = 0; i < (int)SettingId::Count; i++) {
    SettingId id = (SettingId)i;
    const int y = 78 + i * 22;
    d.fillRoundRect(58, y, 244, 20, 4, TFT_BLACK);

    d.setTextDatum(middle_left);
    d.setTextColor(0xC618, TFT_BLACK);
    d.drawString(settingLabel(id), 68, y + 10, 1);

    d.setTextDatum(middle_right);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    // Модель длиннее остальных значений — для неё мелкий шрифт.
    d.drawString(settingValue(id), 292, y + 10, id == SettingId::Model ? 1 : 2);
  }

  d.drawFastHLine(40, 282, 280, 0x2104);
  const uint16_t btnBg = webOn ? 0x0420 : 0x4208;
  d.fillRoundRect(58, 290, 244, 30, 6, btnBg);
  d.setTextDatum(middle_center);
  d.setTextColor(webOn ? TFT_GREEN : TFT_BLACK, btnBg);
  d.drawString(webOn ? "WEB ON - TAP TO DISABLE" : "WEB OFF - TAP TO ENABLE",
               SCREEN_CENTER_X, 305, 1);

  d.setTextColor(0x8410, TFT_BLACK);
  d.drawString("SWIPE UP - BACK", SCREEN_CENTER_X, 340, 1);
  flush();
}

// Тап-зоны: на ELM — Scan/список, на Settings — только кнопка WEB.
// Дашборд и замер тапы игнорируют: навигация свайпом.
void handle_tap(int32_t x, int32_t y) {
  if (screen == Screen::Settings) {
    if (in_rect(x, y, 58, 290, 302, 320)) {
      web_set_enabled(!web_is_enabled());
      draw_settings_screen();
    }
    return;
  }
  if (screen != Screen::ElmConnect) return;

  if (in_rect(x, y, 130, 94, 230, 122)) {
    connectTarget = "";
    elm_start_scan();
    elm_screen_dirty = true;
    return;
  }
  // Подключённый адаптер показывается как информация, а не как список: тапы
  // по области списка игнорируются. Для смены устройства есть кнопка Scan.
  if (elm_is_connected()) return;
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

// Двойной тап по экрану замера: прервать заезд и вернуть автомат в ожидание
// холостых, не оставляя в истории незавершённый CSV.
void resetDragRun() {
  storage_drag_reset();
  drag_begin();
  elm_set_race_mode(elm_is_connected());
  lastDragState = drag_state();
  dragScreenDirty = true;
  storage_log("DRAG RESET");
  draw_drag_screen();
}

// Опрос CST816S. Свайп >=60 px за 1 с:
//   вверх с Dash — ELM Monitor, вниз с Dash — Settings;
//   вниз с ELM Monitor и вверх с Settings — назад на Dash;
//   влево/вправо — Dash ↔ Drag. С Drag вертикальные свайпы игнорируются.
// Диагонали — тоже.
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
      if (now - touch.startedAt < 1000 && now - touch.lastClick > 150) {
        Screen target = screen;
        bool vertical = ady >= 60 && ady > adx * 2;
        bool horizontal = adx >= 60 && adx > ady * 2 &&
                          screen != Screen::ElmConnect && screen != Screen::Settings;

        if (vertical && dy < 0 && screen == Screen::Dash) {
          target = Screen::ElmConnect;
        } else if (vertical && dy > 0 && screen == Screen::Dash) {
          target = Screen::Settings;
        } else if (vertical && dy > 0 && screen == Screen::ElmConnect) {
          target = elmReturnScreen;
        } else if (vertical && dy < 0 && screen == Screen::Settings) {
          target = Screen::Dash;
        } else if (horizontal) {
          target = otherMainScreen(screen);
        }

        if (target != screen) {
          touch.swipeHandled = true;
          touch.lastClick = now;
          SD_LOG("[UI] swipe %d,%d -> screen %d\n", (int)dx, (int)dy, (int)target);
          switchScreen(target);
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

  const bool isTap = now - touch.startedAt < 800 && adx < 24 && ady < 24;
  if (isTap && screen != Screen::Drag) touch.lastTapAt = 0;
  if (isTap && screen == Screen::Drag &&
      touch.lastTapAt && now - touch.lastTapAt <= 400) {
    // Второй тап за 400 мс — сброс замера. Третий не продолжает серию.
    touch.lastTapAt = 0;
    touch.lastClick = now;
    SD_LOGln("[UI] drag reset (double tap)");
    resetDragRun();
    return;
  }
  if (isTap) touch.lastTapAt = now;
  if (isTap && now - touch.lastClick > 250) {
    touch.lastClick = now;
    SD_LOG("[UI] tap %d,%d\n", (int)touch.x, (int)touch.y);
    handle_tap(touch.x, touch.y);
  }
}

// Пин BOOT — резервная кнопка: с модального экрана (ELM/Settings) уходит
// на дашборд, иначе переключает Основной ↔ Замер, как горизонтальный свайп.
void poll_button() {
  static bool last = true;
  bool level = digitalRead(BTN_PIN) == LOW;
  if (level && !last && millis() - touch.lastClick > 250) {
    touch.lastClick = millis();
    SD_LOGln("[UI] button press");
    bool modal = screen == Screen::ElmConnect || screen == Screen::Settings;
    Screen target = modal ? Screen::Dash : otherMainScreen(screen);
    switchScreen(target);
  }
  last = level;
}

// Единая смена экрана. Любой экран перезаписал спрайт целиком, поэтому после
// перехода принудительно инвалидируем статику и все кэши виджетов дашборда.
void switchScreen(Screen next) {
  if (screen == next) return;
  Screen prev = screen;
  if (next == Screen::ElmConnect && prev != Screen::ElmConnect) elmReturnScreen = prev;
  screen = next;

  if (prev == Screen::Drag) {
    // Уходим с экрана посреди заезда: закрываем файл, если он был открыт.
    // storage_drag_finish() ничего не сделает, если заезд не начинался.
    storage_drag_finish(0, "", true, "Cancelled");
    drag_cancel();
    elm_set_race_mode(false);
  }

  touch.lastTapAt = 0;  // новая серия тапов после смены экрана
  static_drawn = false;
  drawn.rpm = -999;
  drawn.speed_kmh = -999;
  drawn.coolant_c = -999;
  drawn.volt = -999;
  drawn.connected = !cur.connected;
  drawn_dot_phase = -1;
  dirty = true;

  if (screen == Screen::ElmConnect) {
    connectTarget = "";
    elm_screen_dirty = true;
    last_elm_draw = 0;
    // Поиск/подключение живут в elmTask и продолжаются при любой навигации:
    // открытие или закрытие экрана не должно сбрасывать состояние сканера.
    draw_elm_screen();
  } else if (screen == Screen::Drag) {
    drag_begin();
    elm_set_race_mode(elm_is_connected());
    dragScreenDirty = true;
    lastDragState = drag_state();
    draw_drag_screen();
  } else if (screen == Screen::Settings) {
    draw_settings_screen();
  } else {
    ui_show_dash();
  }
}

}  // namespace

// Выделяем спрайт и заполняем кэш отрисовки невозможными значениями, чтобы первая
// же ui_show_dash() перерисовала все виджеты, включая видимый ноль скорости.
void ui_init() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  apply_rotation();  // заставка и первый кадр уже рисуются в нужной ориентации
  if (!canvas_ok) {
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    canvas_ok = canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT) != nullptr;
  }
  if (!partialBuf) {
    partialBuf = static_cast<uint16_t*>(heap_caps_malloc(
        SCREEN_WIDTH * PARTIAL_SLICE_H * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
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
  dirtyRectCount = 0;
  needFullFlush = false;
  last_sweep_ms = millis();
  sweep_step = 0;
  sweep_done = !gSettings.bootAnim;
  if (sweep_done) {
    // Анимация отключена: сразу показываем нули, как после её завершения.
    cur.rpm = 0;
    cur.speed_kmh = 0;
    cur.coolant_c = 0;
    cur.volt = 0.0f;
    cur.connected = false;
  }
  settingsRedrawPending = false;

  latest = {};
  haveLatest = false;
  lastDragDraw = 0;
  dragScreenDirty = true;
  lastDragState = DragState::Disabled;
}

// Вызывается из BLE-задачи. Только сохраняет снимок под мьютексом; сама отрисовка
// остаётся на ядре UI, поэтому LGFX не используется из двух потоков.
void ui_set_data(const DashData& data) {
  portENTER_CRITICAL(&dataMux);
  latest = data;
  haveLatest = true;
  portEXIT_CRITICAL(&dataMux);
}

// UI забирает только самый свежий снимок. cur меняется исключительно в ui_loop(),
// поэтому отрисовка не видит «полуобновлённых» значений.
bool ui_get_data(DashData& out) {
  portENTER_CRITICAL(&dataMux);
  if (!haveLatest) {
    portEXIT_CRITICAL(&dataMux);
    return false;
  }
  out = latest;
  haveLatest = false;
  portEXIT_CRITICAL(&dataMux);
  return true;
}

// Стартовая заставка на 700 мс перед первым кадром дашборда.
void ui_show_boot() {
  auto& d = g();
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString("SmartDash", SCREEN_CENTER_X, SCREEN_CENTER_Y - 10, 4);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(gSettings.model, SCREEN_CENTER_X, SCREEN_CENTER_Y + 24, 2);
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

// Открываем ELM Monitor только для просмотра состояния. Сам поиск и подключение
// идут в фоновой задаче elmTask и не перезапускаются при смене экрана.
void ui_show_elm_connect() { switchScreen(Screen::ElmConnect); }

// Возврат на дашборд с принудительной перерисовкой спрайта.
void ui_hide_elm_connect() { switchScreen(Screen::Dash); }

// Settings — тот же модальный экран, открывается свайпом вниз с Dash.
void ui_show_settings() { switchScreen(Screen::Settings); }
void ui_hide_settings() { switchScreen(Screen::Dash); }

// Веб-форма сохранила новые значения. Обработка идёт в ui_loop(), чтобы не
// рисовать из обработчика HTTP и не гнать полный кадр на каждое поле формы.
void ui_settings_changed() { settingsRedrawPending = true; }

// Рендер по «грязным» виджетам. У каждого виджета своё сравнение с кэшем, чтобы
// при изменении только напряжения или точки статуса не перерисовывать дугу RPM.
// Готовый кадр уходит на панель не целиком, а только изменившимися кусками.
void ui_show_dash() {
  const bool freshStatic = !static_drawn;
  if (freshStatic) draw_static();

  // Сглаживание идёт каждый тик UI, а не только по новым снимкам: после резкого
  // падения оборотов стрелка плавно возвращается к цели в течение ~0.6 с.
  uint32_t rpmNow = millis();
  float targetRpm = constrain(cur.rpm, 0, gSettings.rpmMax);
  if (freshStatic || lastRpmSmoothMs == 0 || rpmNow - lastRpmSmoothMs > 1000) {
    shownRpm = targetRpm;
  } else {
    uint32_t dt = rpmNow - lastRpmSmoothMs;
    float tau = targetRpm >= shownRpm ? RPM_TAU_UP_MS : RPM_TAU_DOWN_MS;
    shownRpm += (targetRpm - shownRpm) * (1.0f - expf(-(float)dt / tau));
  }
  lastRpmSmoothMs = rpmNow;
  const int shownRpmInt = (int)lroundf(shownRpm);

  if (drawn.rpm != shownRpmInt) {
    // Sentinel означает свежую статику: дугу нужно перерисовать целиком.
    if (drawn.rpm < 0) draw_rpm(shownRpmInt);
    else update_rpm(drawn.rpm, shownRpmInt);
    drawn.rpm = shownRpmInt;
  }

  if (dirty) {
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
  }
  flushDirty();
}

// Основной тик UI. Порядок важен: сначала ввод может сменить экран, затем
// отрисовывается активный. Данные от реального ELM сразу останавливают анимацию.
void ui_loop() {
  uint32_t now = millis();
  uint32_t t = micros();

  // Настройки пришли из веба: сбрасываем кэши виджетов и статики дашборда.
  // Полный кадр нужен, потому что изменились риски/зоны дуги, а не только числа.
  if (settingsRedrawPending) {
    settingsRedrawPending = false;
    apply_rotation();
    static_drawn = false;
    drawn.rpm = -999;
    drawn.speed_kmh = -999;
    drawn.coolant_c = -999;
    drawn.volt = -999;
    drawn.connected = !cur.connected;
    drawn_dot_phase = -1;
    dirty = true;
    needFullFlush = true;
    // Выключение анимации действует сразу, включение — после перезагрузки.
    if (!gSettings.bootAnim) sweep_done = true;
    if (screen == Screen::Settings) draw_settings_screen();
    else if (screen == Screen::ElmConnect) elm_screen_dirty = true;
    else if (screen == Screen::Drag) dragScreenDirty = true;
  }
  perf_add(PERF_UI_SETUP, micros() - t);

  // Тестовый режим имитирует подключённый адаптер: стартовая анимация
  // останавливается, индикатор "ELM OK" загорается как при реальном подключении.
  bool sim = sim_is_active();
  bool connected = elm_is_connected() || sim;

  // Забираем новый снимок до обработки ввода: экран замера получит сэмпл
  // с таймштампом, а дашборд — актуальные значения. В тестовом режиме
  // источник снимков — USB Serial, реальные данные от ELM не перебивают его.
  DashData sample;
  bool haveSample;
  if (sim) {
    haveSample = sim_get_data(sample);
    DashData ignored;
    ui_get_data(ignored);  // слить снимки BLE, пока идёт тест
  } else {
    haveSample = ui_get_data(sample);
  }
  if (haveSample) {
    cur = sample;
    dirty = true;
  }

  t = micros();
  const Screen screenBeforeInput = screen;
  poll_touch();
  poll_button();
  perf_add(PERF_UI_INPUT, micros() - t);

  t = micros();
  // Снимок взят до обработки ввода. На входе в Drag он ещё с дашборда: такой
  // сэмпл мог бы сразу прервать замер (ELM LOST) или испортить интерполяцию.
  if (screen == Screen::Drag && screenBeforeInput != Screen::Drag) haveSample = false;

  // Подключившийся ELM прерывает анимацию: дальше значения приходят из OBD.
  // Отмечаем конец анимации, чтобы вернуться к частичному выводу кадра.
  if (connected && !sweep_done) sweep_done = true;

  // На экране замера стартовая анимация не нужна: там своя синтетика или ELM.
  if (screen != Screen::Drag && !connected && !sweep_done && now - last_sweep_ms >= SWEEP_INTERVAL_MS) {
    last_sweep_ms = now;

    int s = sweep_step;
    int half = SWEEP_STEPS / 2;

    // RPM: 0 -> текущий максимум шкалы -> 0.
    cur.rpm = (s <= half) ? (gSettings.rpmMax * s / half)
                          : (gSettings.rpmMax * (SWEEP_STEPS - s) / half);
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

  if ((!connected || screen == Screen::ElmConnect) && screen != Screen::Drag &&
      now - last_dot_ms >= DOT_PERIOD_MS) {
    last_dot_ms = now;
    dot_phase = (dot_phase + 1) % 3;
    dirty = true;
  }

  if (screen == Screen::Drag) {
    // Экран замера работает только с реальными снимками (ELM или тест через
    // USB Serial). Встроенной синтетики больше нет: она мешала проверять
    // настоящий автомат и создавала ложный «разгон» без подачи данных.
    if (haveSample && cur.t_ms != 0) {
      // Только новый снимок: повторная подача того же t_ms не двигает автомат
      // (время холостых не накапливается и пересечения отметок не считаются).
      const DragState before = drag_state();
      drag_feed(cur.t_ms, cur.rpm, cur.speed_kmh, cur.connected);
      // Все сэмплы от Armed до финиша идут в отдельный CSV заезда, чтобы их
      // можно было разобрать через веб; в общий ELM-журнал они больше не пишутся.
      if (before == DragState::Armed || before == DragState::Running) {
        storage_drag_sample(cur.t_ms, cur.t_tx_ms, cur.t_rx_ms, cur.rpm, cur.speed_kmh,
                            before == DragState::Armed ? "ARM" : "RUN");
      }
    }

    DragState st = drag_state();
    if (st != lastDragState) {
      if (st == DragState::Armed && lastDragState == DragState::Waiting) {
        storage_drag_begin(drag_result().target, cur.t_ms);
      }
      if (st == DragState::Finished || st == DragState::Aborted) {
        DragResult r = drag_result();
        String splits;
        for (int i = 0; i < DRAG_MAX_SPLITS; i++) {
          if (i) splits += "|";
          splits += r.has[i] ? String(r.split_cs[i]) : String("-1");
        }
        storage_drag_finish(r.total_cs, splits.c_str(), st == DragState::Aborted,
                            drag_abort_text(r.abort));
        saveDragResult();
      }
      lastDragState = st;
      dragScreenDirty = true;
    }
    if (dragScreenDirty || now - lastDragDraw >= 100) {
      lastDragDraw = now;
      dragScreenDirty = false;
      draw_drag_screen();
    }
  } else if (screen == Screen::ElmConnect) {
    // Вывод всего экрана стоит ~50 мс; не делаем его на каждый шаг точек.
    if (elm_screen_dirty || now - last_elm_draw >= 600) {
      last_elm_draw = now;
      elm_screen_dirty = false;
      draw_elm_screen();
    }
  } else if (screen == Screen::Settings) {
    // Заготовка статическая: draw_settings_screen() вызывается только при входе.
  } else {
    ui_show_dash();
  }
  perf_add(PERF_UI_DRAW, micros() - t);
}
