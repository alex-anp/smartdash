/*
 * Счётчики длительности основных циклов.
 *
 * Замер намеренно дешёвый: micros() в начале/конце участка и накопление трёх
 * чисел под спинлоком. В журнал сводка пишется не на каждом такте, а раз в
 * PERF_REPORT_MS, чтобы sampling не стал сам источником просадок. Строки PERF
 * показывают среднее и максимум за интервал; max и PERF_PERIOD помогают найти
 * редкие долгие просадки, которые по среднему не видны.
 */
#include "perf.h"

#include <Arduino.h>
#include <string.h>

#include "config.h"
#include "storage.h"

namespace {

struct Stat {
  uint32_t count;
  uint32_t totalUs;
  uint32_t maxUs;
};

portMUX_TYPE perfMux = portMUX_INITIALIZER_UNLOCKED;
Stat stats[PERF_LANES];
uint32_t lastCycleUs = 0;
uint32_t lastReportMs = 0;

uint32_t avgUs(const Stat& s) { return s.count ? s.totalUs / s.count : 0; }

void report() {
  Stat snap[PERF_LANES];
  portENTER_CRITICAL(&perfMux);
  memcpy(snap, stats, sizeof(snap));
  memset(stats, 0, sizeof(stats));
  portEXIT_CRITICAL(&perfMux);

  // Сначала UI: его просадки виднее всего на экране.
  storage_log("PERF ui n=%u avg=%u max=%u | setup=%u/%u input=%u/%u draw=%u/%u us",
              snap[PERF_UI].count, avgUs(snap[PERF_UI]), snap[PERF_UI].maxUs,
              avgUs(snap[PERF_UI_SETUP]), snap[PERF_UI_SETUP].maxUs,
              avgUs(snap[PERF_UI_INPUT]), snap[PERF_UI_INPUT].maxUs,
              avgUs(snap[PERF_UI_DRAW]), snap[PERF_UI_DRAW].maxUs);
  storage_log("PERF elm n=%u avg=%u max=%u | web=%u/%u serial=%u/%u log=%u/%u period=%u us",
              snap[PERF_ELM].count, avgUs(snap[PERF_ELM]), snap[PERF_ELM].maxUs,
              avgUs(snap[PERF_WEB]), snap[PERF_WEB].maxUs,
              avgUs(snap[PERF_SERIAL]), snap[PERF_SERIAL].maxUs,
              avgUs(snap[PERF_LOG]), snap[PERF_LOG].maxUs,
              snap[PERF_PERIOD].maxUs);
}

}  // namespace

void perf_add(uint8_t lane, uint32_t us) {
  if (lane >= PERF_LANES) return;
  portENTER_CRITICAL(&perfMux);
  Stat& s = stats[lane];
  s.count++;
  s.totalUs += us;
  if (us > s.maxUs) s.maxUs = us;
  portEXIT_CRITICAL(&perfMux);
}

void perf_cycle(uint32_t nowUs) {
  if (lastCycleUs != 0) {
    const uint32_t dt = nowUs - lastCycleUs;  // micros() не перескакивает за отчётный период
    perf_add(PERF_PERIOD, dt);
  }
  lastCycleUs = nowUs;

  const uint32_t nowMs = millis();
  if (lastReportMs == 0) lastReportMs = nowMs;
  if (nowMs - lastReportMs >= PERF_REPORT_MS) {
    lastReportMs = nowMs;
    const uint32_t t = micros();
    report();
    perf_add(PERF_LOG, micros() - t);  // в следующей сводке: цена этой записи
  }
}
