#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>

// Thin wrapper around Arduino Preferences (NVS). The namespace/key pair is
// "smartdash"/"last_elm"; keeping it here avoids duplicating string literals.
void storage_init();
String storage_get_last_elm();
void storage_save_last_elm(const String &addr);
void storage_clear_last_elm();

#endif
