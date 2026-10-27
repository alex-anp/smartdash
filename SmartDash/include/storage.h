#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>

void storage_init();
String storage_get_last_elm();
void storage_save_last_elm(const String &addr);
void storage_clear_last_elm();

#endif
