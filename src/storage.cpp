/*
 * Хранилище NVS для адреса последнего успешно подключённого BLE-устройства.
 * elm327.cpp записывает его после удачного поиска характеристик,
 * а setup() читает один раз, чтобы выбрать автоподключение или полный поиск.
 */
#include "storage.h"

static Preferences prefs;
static const char* NS = "smartdash";
static const char* KEY_LAST_ELM = "last_elm";

void storage_init() {
  prefs.begin(NS, false);  // Namespace на чтение/запись; putString() фиксирует изменение
}

String storage_get_last_elm() {
  return prefs.getString(KEY_LAST_ELM, "");  // Пустая строка = «ещё не подключались»
}

void storage_save_last_elm(const String &addr) {
  prefs.putString(KEY_LAST_ELM, addr);
}

void storage_clear_last_elm() {
  prefs.remove(KEY_LAST_ELM);
}
