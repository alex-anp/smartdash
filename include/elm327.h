#ifndef ELM327_H
#define ELM327_H

#include <Arduino.h>

enum class ElmStatus { Idle, Scanning, Connecting, Initializing, Connected };

struct ElmScanEntry {
  char address[24];
  char name[40];
  int8_t rssi;
  bool compatible;
};

void elm_init();
void elm_start_scan();
void elm_connect_saved(const String& addr);
void elm_connect(const String& addr);
void elm_loop();
bool elm_is_connected();

// State and discovered devices for the on-screen ELM connection view.
ElmStatus elm_status();
size_t elm_get_found(ElmScanEntry* out, size_t max);

#endif
