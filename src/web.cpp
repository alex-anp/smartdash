/*
 * Wi-Fi точка доступа для будущих настроек и выгрузки журнала ELM.
 *
 * К SSID добавляется суффикс из efuse MAC, чтобы несколько приборов рядом не
 * конфликтовали по имени. Журнал отдаётся порциями из LittleFS, чтобы не
 * держать в памяти мегабайты и не блокировать запись новых событий.
 */
#include "web.h"
#include "config.h"
#include "storage.h"
#include "elm327.h"

#include <WiFi.h>
#include <WebServer.h>

static WebServer server(80);
static const size_t HTTP_TAIL_BYTES = 32 * 1024;

void web_init() {
  // Суффикс для конкретного устройства, например SmartDash-A1B2C3D4.
  String apName = String(WIFI_AP_SSID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str(), WIFI_AP_PASS);

  server.on("/", []() {
    server.send(200, "text/html",
                "<html><body><h1>SmartDash</h1><p>Config page coming soon</p>"
                "<p><a href=\"/elm-log\">ELM log (tail)</a> · "
                "<a href=\"/elm-log?all=1\">full log</a> · "
                "<a href=\"/elm-log?clear=1\">clear</a></p></body></html>");
  });

  // Журнал подключений сохраняется во флэше и переживает перезагрузку.
  // ?all=1 — весь файл, ?clear=1 — очистить после копирования.
  server.on("/elm-log", []() {
    if (server.hasArg("clear")) {
      storage_clear_log();
      server.send(200, "text/plain", "ELM log cleared\n");
      return;
    }
    const size_t size = storage_log_size();
    size_t start = 0;
    if (!server.hasArg("all") && size > HTTP_TAIL_BYTES) start = size - HTTP_TAIL_BYTES;

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/plain; charset=utf-8", "");
    char buf[512];
    size_t pos = start;
    while (pos < size) {
      size_t want = sizeof(buf);
      if (want > size - pos) want = size - pos;
      size_t n = storage_read_log(buf, want, pos);
      if (!n) break;
      server.sendContent(buf, n);
      pos += n;
    }
    server.sendContent("");
  });

  server.begin();
}

void web_loop() {
  server.handleClient();
}
