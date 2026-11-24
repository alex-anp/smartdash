/*
 * NVS storage for the last successfully connected BLE address.
 * Written by elm327.cpp after a successful characteristic discovery,
 * read once in setup() to decide between autoconnect and a full scan.
 */
#include "storage.h"

static Preferences prefs;
static const char* NS = "smartdash";
static const char* KEY_LAST_ELM = "last_elm";

void storage_init() {
  prefs.begin(NS, false);  // Read/write namespace; committed implicitly by putString()
}

String storage_get_last_elm() {
  return prefs.getString(KEY_LAST_ELM, "");  // Empty string means "never connected"
}

void storage_save_last_elm(const String &addr) {
  prefs.putString(KEY_LAST_ELM, addr);
}

void storage_clear_last_elm() {
  prefs.remove(KEY_LAST_ELM);
}
