#ifndef ELM327_H
#define ELM327_H

#include <Arduino.h>

// Публичный фасад над BLE-задачей. Вся работа с состоянием BLE происходит
// внутри src/elm327.cpp на core 0, поэтому эти функции безопасно вызывать
// из цикла UI.
enum class ElmStatus {
  Idle,          // Нет активной попытки подключения
  Scanning,      // Идёт сканирование BLE
  Connecting,    // Асинхронное подключение отправлено, ждём ответа
  Initializing,  // Подключены, отправляем AT-команды инициализации
  Connected      // Инициализация завершена, идёт опрос OBD
};

// Одна строка экрана ELM Connect. Копируется с мьютексом через elm_get_found().
struct ElmScanEntry {
  char address[24];  // BLE MAC в виде "AA:BB:CC:DD:EE:FF"
  char name[40];     // Широковещательное имя или "<unknown>"
  int8_t rssi;       // Уровень сигнала в dBm
  bool compatible;   // Эвристическое совпадение по имени / UUID сервиса
};

void elm_init();                            // Инициализировать NimBLE и запустить задачу
void elm_start_scan();                      // Отключиться и заново искать все устройства
void elm_connect_saved(const String& addr); // Искать адрес, сохранённый в NVS
void elm_connect(const String& addr);       // Сохранить адрес и подключиться к нему
bool elm_is_connected();                    // true только после завершения AT-инициализации
void elm_set_race_mode(bool enabled);       // режим замена: только 010C0D с таймштампом

// Состояние и найденные устройства для экрана подключения ELM.
ElmStatus elm_status();
size_t elm_get_found(ElmScanEntry* out, size_t max);

#endif
