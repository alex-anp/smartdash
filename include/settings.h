#ifndef SETTINGS_H
#define SETTINGS_H

#include "config.h"

// Пользовательские настройки. Значения по умолчанию берутся из config.h,
// сохранённые — из NVS namespace "smartdash" (ключи cfg_*).
// Изменения делает только UI-задача; elmTask эти поля не трогает.
// webEnabled переключается тапом на экране Settings и применяется web_set_enabled().

constexpr int SETTINGS_MODEL_LEN = 20;
constexpr int SETTINGS_RPM_ZONE_SPAN = 2000;  // красная зона начинается через 2000 об/мин после redline

struct Settings {
  int rpmMax;           // верхняя граница шкалы тахометра
  int rpmRedline;       // граница зелёной/оранжевой зоны
  int coolantLow;       // ниже — голубой, до coolantHigh — зелёный
  int coolantHigh;      // выше — красный
  float voltLow;        // ниже — красный
  float voltMid;        // ниже mid — оранжевый
  float voltHigh;       // до high — зелёный, выше — красный
  bool bootAnim;        // однократная стартовая анимация
  bool webEnabled;      // держать Wi-Fi AP / веб-интерфейс включённым
  char model[SETTINGS_MODEL_LEN];  // подпись на заставке
};

extern Settings gSettings;

void settings_init();          // Загрузить из NVS, применить границы и дефолты
void settings_save();          // Записать текущие значения в NVS
void settings_normalize();     // Привести значения к шагам и сохранить взаимные ограничения

#endif
