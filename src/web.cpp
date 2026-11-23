#include "web.h"
#include "config.h"
#include "storage.h"
#include "elm327.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);

void web_init() {
  String apName = String(WIFI_AP_SSID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), WIFI_AP_PASS);
  
  // Basic routes placeholder
  server.on("/", []() {
    server.send(200, "text/html", "<html><body><h1>SmartDash</h1><p>Config page coming soon</p></body></html>");
  });
  
  server.begin();
}

void web_loop() {
  server.handleClient();
}
