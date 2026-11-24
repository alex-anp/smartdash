/*
 * Wi-Fi точка доступа для будущих настроек.
 *
 * К SSID добавляется суффикс из efuse MAC, чтобы несколько приборов рядом не
 * конфликтовали по имени. Текущий обработчик «/» — пока заглушка: рабочие
 * параметры (пределы RPM, единицы, цвета) по-прежнему задаются в include/config.h.
 */
#include "web.h"
#include "config.h"
#include "storage.h"
#include "elm327.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);

void web_init() {
  // Суффикс для конкретного устройства, например SmartDash-A1B2C3D4.
  String apName = String(WIFI_AP_SSID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), WIFI_AP_PASS);

  // Пока заглушка базового маршрута. Ответ должен оставаться коротким: handleClient()
  // выполняется в том же цикле, что и UI, и не должен задерживать шаги анимации по 100 мс.
  server.on("/", []() {
    server.send(200, "text/html", "<html><body><h1>SmartDash</h1><p>Config page coming soon</p></body></html>");
  });

  server.begin();
}

void web_loop() {
  server.handleClient();
}
