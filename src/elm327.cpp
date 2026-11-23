#include "elm327.h"
#include "config.h"
#include "storage.h"
#include "ui.h"

// Will use BluetoothSerial for Classic, and BLE Serial for LE
// Support both as requested

void elm_init() {
  // TODO: init BT Classic + BLE
}

void elm_start_scan() {
  // TODO: scan for ELM327 (Classic + BLE), show on touch UI
  ui_show_search();
}

void elm_connect_saved(const String &addr) {
  // TODO: try connect to saved address
  (void)addr;
}

void elm_connect(const String &addr) {
  // TODO: connect and init AT sequence: ATZ, ATE0, ATL0, ATH0, ATSP0, etc
  storage_save_last_elm(addr);
  (void)addr;
}

void elm_loop() {
  // TODO: periodic polling of 010D (speed), 010C (RPM), 0105 (coolant) or fallbacks, ATRV
}

bool elm_is_connected() {
  return false;
}
