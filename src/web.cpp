/*
 * Wi-Fi access point used for future configuration.
 *
 * SSID is suffixed with the chip efuse MAC so several dashboards can be powered
 * nearby without name collisions. The current root handler is only a placeholder:
 * runtime values (RPM limits, units, colors) still live in include/config.h.
 */
#include "web.h"
#include "config.h"
#include "storage.h"
#include "elm327.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);

void web_init() {
  // Per-device suffix, e.g. SmartDash-A1B2C3D4.
  String apName = String(WIFI_AP_SSID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), WIFI_AP_PASS);
  
  // Basic route placeholder. Keep responses small: handleClient() runs in the
  // same loop as the UI and must not delay the 100 ms startup animation steps.
  server.on("/", []() {
    server.send(200, "text/html", "<html><body><h1>SmartDash</h1><p>Config page coming soon</p></body></html>");
  });
  
  server.begin();
}

void web_loop() {
  server.handleClient();
}
