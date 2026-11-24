/*
 * SmartDash — прибор Honda Dunk AF78 на базе OBD-II
 *
 * Плата:   JC3636W518C (ESP32-S3, круглый IPS 360x360, ST77916 QSPI)
 * BT:      только BLE (у ESP32-S3 нет Bluetooth Classic)
 * ELM:     совместимый адаптер ELM327, автоподключение к последнему MAC
 * UI:      полноэкранный спрайт в PSRAM, одноразовая анимация, навигация свайпами
 * Настройки: Wi-Fi точка доступа (базовая страница, формы — в будущем)
 *
 * setup() выполняется строго по порядку: NVS -> дисплей -> UI -> AP -> BLE.
 * loop() остаётся лёгким: блокирующая логика BLE живёт в задаче elmTask
 * на core 0 (см. src/elm327.cpp).
 */

#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "elm327.h"
#include "ui.h"
#include "storage.h"
#include "web.h"

void setup() {
  // USB CDC инициализируем даже при отключённых логах: они понадобятся,
  // если включить SMARTDASH_SERIAL_DEBUG=1.
  Serial.begin(115200);
  delay(100);

  storage_init();  // Открыть NVS до первого чтения сохранённого адреса ELM
  display_init();  // ST77916 + подсветка + CST816S
  ui_init();       // Выделить спрайт и сбросить кэши отрисовки
  ui_show_boot();  // Короткая заставка перед дашбордом
  delay(700);
  web_init();      // Поднять точку доступа SmartDash-<id>
  elm_init();      // Запустить BLE-задачу на core 0

  // Автоподключение к сохранённому ELM327 или поиск любого совместимого адаптера.
  String last = storage_get_last_elm();
  if (last.length() > 0) {
    elm_connect_saved(last);
  } else {
    elm_start_scan();
  }
}

void loop() {
  ui_loop();    // Ввод, стартовая анимация, точки статуса и перерисовка виджетов
  web_loop();   // Обработать один ожидающий HTTP-запрос, если он есть
  delay(2);     // Отдать управление планировщику; опрос OBD раз в 120 мс — в elmTask
}
