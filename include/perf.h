#ifndef PERF_H
#define PERF_H

#include <stdint.h>

// Измеряемые участки. PERF_PERIOD — интервал между входами в loop(), то есть
// реальный такт основной задачи вместе с задержками и возможными просадками.
enum PerfLane : uint8_t {
  PERF_UI = 0,
  PERF_UI_SETUP,   // разбор флагов настроек перед вводом
  PERF_UI_INPUT,   // тач и кнопка
  PERF_UI_DRAW,    // подготовка и вывод кадра
  PERF_WEB,
  PERF_SERIAL,
  PERF_ELM,        // elm_loop() в задаче на core 0
  PERF_LOG,        // запись сводки в LittleFS (стоимость прошлого отчёта)
  PERF_PERIOD,
  PERF_LANES
};

// Добавить длительность участка в микросекундах. Вызывается из обеих задач,
// поэтому накопление идёт под коротким спинлоком.
void perf_add(uint8_t lane, uint32_t us);

// Вызвать в начале loop(): измерить интервал между тактами и раз в PERF_REPORT_MS
// сбросить накопленную статистику в журнал.
void perf_cycle(uint32_t nowUs);

#endif
