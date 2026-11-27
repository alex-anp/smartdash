#ifndef DRAGRACE_H
#define DRAGRACE_H

#include <Arduino.h>

// Логика замера разгона с места. Модуль не знает про дисплей и BLE: он получает
// сэмплы (время, обороты, скорость) и ведёт конечный автомат. UI только рисует
// состояние и результат, а elm327.cpp в режиме замера отдаёт обороты и скорость
// одним запросом 010C0D. Старт — по пересечению скорости 3 км/ч после скачка
// оборотов; t_ms сэмпла — середина интервала запрос→ответ. Заезд завершается
// и по сбросу оборотов: в результате остаётся последняя достигнутая отметка
// (её скорость становится target), а не обязательно выбранная заранее цель.

constexpr int DRAG_SPLIT_STEP = 10;   // фиксация каждые 10 км/ч
constexpr int DRAG_MAX_SPLITS = 10;   // отметки 10..100 км/ч
constexpr uint16_t DRAG_TARGET_DEFAULT = 100;

enum class DragState {
  Disabled,  // замер выключен
  Waiting,   // включён, ждём устойчивые холостые
  Armed,     // холостые есть, ждём резкого роста оборотов
  Running,   // таймер идёт
  Finished,  // доехали до цели
  Aborted    // прервано (таймаут, потеря связи, падение оборотов)
};

enum class DragAbort { None, Timeout, LinkLost, Stall };

struct DragResult {
  uint16_t target = DRAG_TARGET_DEFAULT;
  bool has[DRAG_MAX_SPLITS] = {false};
  uint32_t split_cs[DRAG_MAX_SPLITS] = {0};  // время отметки, ss.cc в сотых
  uint32_t delta_cs[DRAG_MAX_SPLITS] = {0};  // прирост относительно прошлой отметки
  uint32_t total_cs = 0;                     // время до целевой скорости
  DragAbort abort = DragAbort::None;
};

void drag_begin(uint16_t target = DRAG_TARGET_DEFAULT);  // сброс и ожидание холостых
void drag_cancel();                                      // выключить автомат
bool drag_active();                                      // Waiting/Armed/Running
void drag_feed(uint32_t t_ms, int rpm, int speed_kmh, bool connected);

DragState drag_state();
uint32_t drag_elapsed_cs();  // текущее время замера (только в Running)
DragResult drag_result();

// Сериализация результата для NVS: "target,total_cs,cs0|cs1|...".
String drag_serialize(const DragResult& r);
uint32_t drag_serialized_total(const String& s);
const char* drag_abort_text(DragAbort a);

#endif
