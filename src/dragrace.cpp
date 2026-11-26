/*
 * Конечный автомат замера разгона с места.
 *
 * Вход — сэмплы из BLE-задачи (или синтетика в демо-режиме) с таймштампом
 * millis(). Все пороги собраны здесь, чтобы UI занимался только отрисовкой.
 * Пересечение каждой отметки 10 км/ч ищется интерполяцией между соседними
 * замерами: OBD отдаёт скорость с шагом 1 км/ч и опрашивается ~раз в 120 мс,
 * поэтому без интерполяции таблица была бы «ступеньками».
 */
#include "dragrace.h"

#include <Arduino.h>

namespace {

constexpr int IDLE_RPM_MIN = 1500;
constexpr int IDLE_RPM_MAX = 2500;
constexpr uint32_t IDLE_HOLD_MS = 800;     // сколько держать холостые перед-arm
constexpr int RPM_JUMP = 600;              // рост оборотов от холостых
constexpr int MIN_LAUNCH_SPEED = 3;        // защита от «газовки» на месте; старт по скорости
constexpr uint32_t RUN_TIMEOUT_MS = 60000; // защита от вечного замера
constexpr int STALL_RPM = 1000;            // ниже — считаем, что мотор заглох

DragState state = DragState::Disabled;
DragResult result;

uint16_t target = DRAG_TARGET_DEFAULT;
uint32_t idleSince = 0;
int idleRpm = 0;
uint32_t launchT = 0;
uint32_t launchSpeedT = 0;   // интерполированный момент пересечения скорости старта
bool launchSpeedSeen = false;

bool havePrev = false;
uint32_t prevT = 0;
int prevRpm = 0;
int prevSpeed = 0;

int savedSplits = 0;

// Линейная интерполяция момента, когда значение пересекло порог между двумя
// замерами. Предполагает, что prevValue < threshold <= curValue.
uint32_t crossTime(uint32_t t0, int v0, uint32_t t1, int v1, int threshold) {
  if (v1 <= v0) return t1;
  float part = (float)(threshold - v0) / (float)(v1 - v0);
  if (part < 0.0f) part = 0.0f;
  if (part > 1.0f) part = 1.0f;
  return t0 + (uint32_t)((float)(t1 - t0) * part);
}

void abortRun(DragAbort reason) {
  if (state == DragState::Running) {
    result.total_cs = (millis() - launchT) / 10;
  }
  result.abort = reason;
  state = DragState::Aborted;
}

}  // namespace

void drag_begin(uint16_t t) {
  state = DragState::Waiting;
  target = constrain(t, (uint16_t)20, (uint16_t)100);
  result = DragResult{};
  result.target = target;
  idleSince = 0;
  idleRpm = 0;
  launchT = 0;
  launchSpeedT = 0;
  launchSpeedSeen = false;
  havePrev = false;
  savedSplits = 0;
}

void drag_cancel() {
  state = DragState::Disabled;
  havePrev = false;
}

bool drag_active() {
  return state == DragState::Waiting || state == DragState::Armed || state == DragState::Running;
}

// t_ms — millis() в момент получения сэмпла, rpm/speed — значения OBD,
// connected=false означает потерю ELM во время замера.
void drag_feed(uint32_t t_ms, int rpm, int speed_kmh, bool connected) {
  if (state == DragState::Disabled || state == DragState::Finished || state == DragState::Aborted) return;

  if (!connected) {
    abortRun(DragAbort::LinkLost);
    return;
  }

  if (rpm < 0) rpm = 0;
  if (speed_kmh < 0) speed_kmh = 0;

  if (state == DragState::Waiting) {
    // Холостые должны быть устойчивыми: один случайный сэмпл 1800 об/мин
    // не должен открывать замер.
    bool idle = (rpm >= IDLE_RPM_MIN && rpm <= IDLE_RPM_MAX && speed_kmh <= 1);
    if (idle) {
      if (idleSince == 0) idleSince = t_ms;
      if ((uint32_t)(t_ms - idleSince) >= IDLE_HOLD_MS) {
        idleRpm = rpm;
        state = DragState::Armed;
      }
    } else {
      idleSince = 0;
    }
  } else if (state == DragState::Armed) {
    int threshold = idleRpm + RPM_JUMP;
    // Рост оборотов может быть просто газовкой на месте. Поэтому скачок оборотов
    // лишь подтверждает старт, а отсчёт идёт от пересечения скорости 3 км/ч —
    // как в анализе AURA, где старт ищут по ускорению, а не по оборотам.
    if (!launchSpeedSeen && speed_kmh >= MIN_LAUNCH_SPEED) {
      launchSpeedT = (havePrev && prevSpeed < MIN_LAUNCH_SPEED && speed_kmh > prevSpeed)
                         ? crossTime(prevT, prevSpeed, t_ms, speed_kmh, MIN_LAUNCH_SPEED)
                         : t_ms;
      launchSpeedSeen = true;
    }
    if (launchSpeedSeen && rpm >= threshold) {
      launchT = launchSpeedT;
      state = DragState::Running;
    }
  } else if (state == DragState::Running) {
    // За один сэмпл могли проскочить сразу несколько отметок — фиксируем все.
    while (savedSplits < DRAG_MAX_SPLITS) {
      int k = (savedSplits + 1) * DRAG_SPLIT_STEP;
      if (k > target) break;
      if (!havePrev || prevSpeed >= k || speed_kmh < k || speed_kmh <= prevSpeed) break;

      uint32_t crossed = crossTime(prevT, prevSpeed, t_ms, speed_kmh, k);
      uint32_t cs = crossed >= launchT ? (crossed - launchT) / 10 : 0;
      result.split_cs[savedSplits] = cs;
      result.delta_cs[savedSplits] = savedSplits == 0 ? cs : cs - result.split_cs[savedSplits - 1];
      result.has[savedSplits] = true;
      savedSplits++;
    }

    int targetIdx = target / DRAG_SPLIT_STEP - 1;
    if (targetIdx >= 0 && targetIdx < DRAG_MAX_SPLITS && result.has[targetIdx]) {
      result.total_cs = result.split_cs[targetIdx];
      state = DragState::Finished;
    } else if ((uint32_t)(t_ms - launchT) > RUN_TIMEOUT_MS) {
      abortRun(DragAbort::Timeout);
    } else if (rpm < STALL_RPM && (uint32_t)(t_ms - launchT) > 500) {
      abortRun(DragAbort::Stall);
    }
  }

  prevT = t_ms;
  prevRpm = rpm;
  prevSpeed = speed_kmh;
  havePrev = true;
}

DragState drag_state() { return state; }

uint32_t drag_elapsed_cs() {
  if (state != DragState::Running) return 0;
  return (millis() - launchT) / 10;
}

DragResult drag_result() { return result; }

String drag_serialize(const DragResult& r) {
  String s = String(r.target) + "," + String(r.total_cs) + ",";
  for (int i = 0; i < DRAG_MAX_SPLITS; i++) {
    if (i > 0) s += "|";
    s += r.has[i] ? String(r.split_cs[i]) : String("-1");
  }
  return s;
}

uint32_t drag_serialized_total(const String& s) {
  int comma = s.indexOf(',');
  if (comma < 0) return UINT32_MAX;
  int end = s.indexOf(',', comma + 1);
  String v = end < 0 ? s.substring(comma + 1) : s.substring(comma + 1, end);
  return (uint32_t)v.toInt();
}

const char* drag_abort_text(DragAbort a) {
  switch (a) {
    case DragAbort::Timeout:  return "TIMEOUT";
    case DragAbort::LinkLost: return "ELM LOST";
    case DragAbort::Stall:    return "STALL";
    default:                  return "ABORT";
  }
}
