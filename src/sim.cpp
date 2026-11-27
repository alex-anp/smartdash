/*
 * Тестовый режим: имитация потока снимков OBD через USB Serial.
 *
 * Задача — проверить рендер дашборда на плате без подключённого адаптера.
 * Снимок уходит в UI точно таким же путём, как и данные из BLE-задачи: ui_loop()
 * забирает sim_get_data() вместо ui_get_data(). Поэтому все виджеты, дуга RPM
 * и индикатор "ELM OK" работают с тестовыми значениями так же, как с реальными.
 *
 * NimBLE и elmTask не трогаются: если адаптер подключён, его снимки в тестовом
 * режиме просто игнорируются и не перебивают картинку на экране.
 */
#include "sim.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

namespace {

bool active = false;
DashData simData;   // последние заданные значения
bool pending = false;

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void defaults() {
  simData.speed_kmh = 0;
  simData.rpm = 1500;   // холостые
  simData.coolant_c = 85;
  simData.volt = 13.8f;
}

// Отправить очередной снимок. Таймштампы такой же формы, как у ответа ELM:
// t_ms — середина интервала запрос→ответ.
void publish() {
  uint32_t t = millis();
  simData.connected = true;
  simData.t_tx_ms = t;
  simData.t_rx_ms = t + 20;
  simData.t_ms = t + 10;
  pending = true;
  Serial.printf("SIM rpm=%d spd=%d coolant=%d volt=%.1f\n",
                simData.rpm, simData.speed_kmh, simData.coolant_c, simData.volt);
}

bool lowerEq(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    ++a; ++b;
  }
  return *a == 0 && *b == 0;
}

// Первое слово во входной строке не заканчивается на NUL — сравниваем по длине.
bool verbIs(const char* s, size_t len, const char* word) {
  if (strlen(word) != len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (tolower((unsigned char)s[i]) != tolower((unsigned char)word[i])) return false;
  }
  return true;
}

// Разбор одной пары "ключ=значение". Возвращает false для неизвестного поля.
bool setField(const char* tok) {
  const char* eq = strchr(tok, '=');
  if (!eq || eq == tok) return false;

  char key[12] = {0};
  size_t n = (size_t)(eq - tok);
  if (n >= sizeof(key)) n = sizeof(key) - 1;
  for (size_t i = 0; i < n; ++i) key[i] = (char)tolower((unsigned char)tok[i]);
  const char* val = eq + 1;

  if (strcmp(key, "rpm") == 0) {
    simData.rpm = clampInt(atoi(val), 0, RPM_MAX);
  } else if (strcmp(key, "spd") == 0 || strcmp(key, "speed") == 0) {
    simData.speed_kmh = clampInt(atoi(val), 0, 99);
  } else if (strcmp(key, "coolant") == 0 || strcmp(key, "temp") == 0) {
    simData.coolant_c = clampInt(atoi(val), -1, 199);
  } else if (strcmp(key, "volt") == 0) {
    simData.volt = clampFloat((float)atof(val), 0.0f, 20.0f);
  } else {
    return false;
  }
  return true;
}

void help() {
  Serial.println("SIM: sim [rpm=3500 spd=60 coolant=88 volt=14.1] | sim 3500 60 88 14.1");
  Serial.println("SIM: sim on|off|reset|status");
}

}  // namespace

bool sim_handle_line(const char* line) {
  // Быстрая проверка без изменения входной строки: нас интересуют только
  // строки, начинающиеся с "sim". Остальное остаётся обработчику 'log'.
  const char* p = line;
  while (*p == ' ' || *p == '\t') ++p;
  const char* start = p;
  while (*p && *p != ' ' && *p != '\t') ++p;
  size_t verbLen = (size_t)(p - start);
  if (!verbIs(start, verbLen, "sim") && !verbIs(start, verbLen, "s")) {
    return false;
  }

  char buf[128];
  strncpy(buf, line, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* tok[12];
  int n = 0;
  for (char* t = strtok(buf, " \t"); t && n < (int)(sizeof(tok) / sizeof(tok[0]));
       t = strtok(NULL, " \t")) {
    tok[n++] = t;
  }

  if (n == 1 || (n == 2 && lowerEq(tok[1], "on"))) {
    active = true;
    publish();
    return true;
  }
  if (n == 2 && lowerEq(tok[1], "off")) {
    active = false;
    pending = false;
    Serial.println("SIM off");
    return true;
  }
  if (n == 2 && lowerEq(tok[1], "reset")) {
    defaults();
    active = true;
    publish();
    return true;
  }
  if (n == 2 && lowerEq(tok[1], "status")) {
    Serial.printf("SIM %s rpm=%d spd=%d coolant=%d volt=%.1f\n",
                  active ? "on" : "off", simData.rpm, simData.speed_kmh,
                  simData.coolant_c, simData.volt);
    return true;
  }
  if (n == 2 && lowerEq(tok[1], "help")) {
    help();
    return true;
  }

  // Позиционная форма: sim <rpm> <spd> <coolant> <volt>.
  if (isdigit((unsigned char)tok[1][0]) || tok[1][0] == '-') {
    if (n < 5) {
      Serial.println("SIM: need rpm spd coolant volt");
      return true;
    }
    simData.rpm = clampInt(atoi(tok[1]), 0, RPM_MAX);
    simData.speed_kmh = clampInt(atoi(tok[2]), 0, 99);
    simData.coolant_c = clampInt(atoi(tok[3]), -1, 199);
    simData.volt = clampFloat((float)atof(tok[4]), 0.0f, 20.0f);
    active = true;
    publish();
    return true;
  }

  // Именованные поля в любом порядке: sim spd=40 rpm=5000.
  int changed = 0;
  for (int i = 1; i < n; ++i) {
    if (setField(tok[i])) ++changed;
  }
  if (changed == 0) {
    help();
    return true;
  }
  active = true;
  publish();
  return true;
}

bool sim_is_active() { return active; }

bool sim_get_data(DashData& out) {
  if (!pending) return false;
  out = simData;
  pending = false;
  return true;
}
