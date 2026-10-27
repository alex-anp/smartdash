#ifndef ELM327_H
#define ELM327_H

#include <Arduino.h>

void elm_init();
void elm_start_scan();
void elm_connect_saved(const String &addr);
void elm_connect(const String &addr);
void elm_loop();
bool elm_is_connected();

#endif
