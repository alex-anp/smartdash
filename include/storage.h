#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>

// Тонкая обёртка над Arduino Preferences (NVS). Пара namespace/key —
// "smartdash"/"last_elm"; хранение строки в одном месте исключает дублирование.
void storage_init();
String storage_get_last_elm();
void storage_save_last_elm(const String &addr);
void storage_clear_last_elm();

#endif
