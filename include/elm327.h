#ifndef ELM327_H
#define ELM327_H

#include <Arduino.h>

// Public façade over the BLE worker task. All stateful BLE work happens inside
// src/elm327.cpp on core 0; these functions are safe to call from the UI loop.
enum class ElmStatus {
  Idle,          // No active connection attempt
  Scanning,      // BLE scan in progress
  Connecting,    // Async connect issued, waiting for peer
  Initializing,  // Connected, sending AT init sequence
  Connected      // Init done, OBD polling active
};

// One row of the ELM Connect screen. Copied out under a mutex by elm_get_found().
struct ElmScanEntry {
  char address[24];  // BLE MAC as "AA:BB:CC:DD:EE:FF"
  char name[40];     // Advertised name or "<unknown>"
  int8_t rssi;       // Signal strength in dBm
  bool compatible;   // Heuristic match by name / service UUID
};

void elm_init();                            // Init NimBLE and spawn the worker task
void elm_start_scan();                      // Disconnect current peer and rescan all devices
void elm_connect_saved(const String& addr); // Scan specifically for the NVS-saved address
void elm_connect(const String& addr);       // Save address, then scan/connect to it
bool elm_is_connected();                    // True only after AT init finished

// State and discovered devices for the on-screen ELM connection view.
ElmStatus elm_status();
size_t elm_get_found(ElmScanEntry* out, size_t max);

#endif
