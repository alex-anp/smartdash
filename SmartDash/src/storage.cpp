#include "storage.h"

static Preferences prefs;
static const char* NS = "smartdash";

void storage_init() {
  prefs.begin(NS, false);
}

String storage_get_last_elm() {
  return prefs.getString("last_elm", "");
}

void storage_save_last_elm(const String &addr) {
  prefs.putString("last_elm", addr);
}

void storage_clear_last_elm() {
  prefs.remove("last_elm");
}
