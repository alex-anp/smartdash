/*
 * Wi-Fi точка доступа: веб-форма настроек и выгрузка журнала ELM.
 *
 * Суффикс SSID берётся из efuse, чтобы несколько приборов рядом не конфликтовали
 * по имени. Форма /settings редактирует ту же структуру gSettings, что и рендер:
 * после сохранения вызывается ui_settings_changed(), и UI перерисовывает статику.
 * Captive portal: DNS отвечает адресом AP на любые имена, а системные проверки
 * интернета (/generate_204, /hotspot-detect.html и т.п.) редиректят на форму —
 * телефон сам открывает страницу после подключения к сети.
 * Журнал отдаётся порциями из LittleFS, чтобы не держать в памяти мегабайты
 * и не блокировать запись новых событий.
 */
#include "web.h"
#include "config.h"
#include "elm327.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <stdlib.h>

static WebServer server(80);
static DNSServer dnsServer;  // AsyncUDP внутри, processNextRequest() не нужен
static bool webRunning = false;
static const size_t HTTP_TAIL_BYTES = 32 * 1024;

String web_ap_ssid() {
  // Суффикс для конкретного устройства, например SmartDash-A1B2C3D4.
  return String(WIFI_AP_SSID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

namespace {

// Captive portal: Location обязательно абсолютным — запросы проверок приходят
// на host вроде connectivitycheck.gstatic.com, а не на IP прибора. Открываем
// корневую страницу, оттуда уже есть переходы в настройки и журнал.
void redirect_to_root() {
  String url = "http://" + WiFi.softAPIP().toString() + "/";
  server.sendHeader("Location", url, true);
  server.send(302, "text/plain", "");
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    switch (s[i]) {
      case '&': out += "&#38;"; break;
      case '<': out += "&#60;"; break;
      case '>': out += "&#62;"; break;
      case '"': out += "&#34;"; break;
      case '\'': out += "&#39;"; break;
      default: out += s[i];
    }
  }
  return out;
}

String numField(const char* key, const char* label, const String& value,
                const String& min, const String& max, const char* step,
                const char* unit) {
  String h = "<label><span>";
  h += label;
  h += " <em>(";
  h += unit;
  h += ")</em></label><input type='number' name='";
  h += key;
  h += "' value='";
  h += htmlEscape(value);
  h += "' min='";
  h += min;
  h += "' max='";
  h += max;
  h += "' step='";
  h += step;
  h += "' inputmode='numeric' required>";
  return h;
}

// Общий каркас для всех страниц, чтобы корень, настройки и журнал выглядели одинаково.
String pageHead(const String& title) {
  String p = "<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>SmartDash - ";
  p += htmlEscape(title);
  p += "</title><style>"
       "body{background:#101014;color:#eee;font:16px/1.4 system-ui,sans-serif;margin:0;padding:16px}"
       "main{max-width:420px;margin:0 auto}h1{font-size:20px;margin:0 0 12px}"
       "fieldset{border:1px solid #333;border-radius:8px;margin:0 0 14px;padding:8px 12px 12px}"
       "legend{color:#6c6;padding:0 6px}label{display:block;margin-top:10px;color:#bbb;font-size:14px}"
       "em{color:#888;font-style:normal}"
       "input,select{box-sizing:border-box;width:100%;font-size:16px;padding:10px;margin-top:4px;"
       "border:1px solid #444;border-radius:6px;background:#1c1c22;color:#fff}"
       "button,.btn{box-sizing:border-box;display:block;width:100%;padding:12px;font-size:17px;"
       "border:0;border-radius:8px;background:#2a8;color:#000;font-weight:bold;"
       "text-align:center;text-decoration:none;margin:8px 0}"
       ".btn.alt{background:#28323c;color:#dce}"
       "a{color:#7bd}.ok{background:#163;border-radius:6px;padding:8px 10px;margin-bottom:12px}"
       ".hint{color:#888;font-size:13px}</style></head><body><main><h1>SmartDash</h1>";
  if (title.length()) {
    p += "<p class='hint'>";
    p += htmlEscape(title);
    p += "</p>";
  }
  return p;
}

String pageTail() { return "</main></body></html>"; }

// Корень — точка входа после подключения к AP.
String rootPage() {
  String p = pageHead("Main");
  p += "<fieldset><legend>DEVICE</legend>";
  p += "<p>AP: ";
  p += htmlEscape(web_ap_ssid());
  p += "</p><p class='hint'>Wi-Fi is on only while WEB is enabled on the device.</p>";
  p += "</fieldset>";
  p += "<a class='btn' href='/settings'>SETTINGS</a>";
  p += "<a class='btn alt' href='/log'>ELM LOG</a>";
  return p + pageTail();
}

// Журнал — отдельная страница; /elm-log остаётся текстовым эндпоинтом.
String logPage(bool cleared) {
  String p = pageHead("ELM log");
  if (cleared) p += "<p class='ok'>Log cleared.</p>";
  p += "<fieldset><legend>ELM LOG</legend>";
  p += "<p class='hint'>Stored: ";
  p += String(storage_log_size());
  p += " bytes (rotation at 1 MB)</p>";
  p += "<p class='hint'>Text view shows the last 32 KB by default.</p>";
  p += "</fieldset>";
  p += "<a class='btn' href='/elm-log'>VIEW TAIL</a>";
  p += "<a class='btn' href='/elm-log?all=1'>VIEW FULL LOG</a>";
  p += "<a class='btn alt' href='/elm-log?clear=1' "
       "onclick=\"return confirm('Clear ELM log?')\">CLEAR LOG</a>";
  p += "<a class='btn alt' href='/'>MAIN</a>";
  return p + pageTail();
}

String settingsPage(bool saved) {
  String p = pageHead("Settings");
  if (saved) p += "<p class='ok'>Saved.</p>";

  p += "<form method='post' action='/settings'>";
  p += "<fieldset><legend>Tachometer</legend>";
  p += numField("rpm_max", "RPM MAX", String(gSettings.rpmMax), "6000", "16000", "1000", "rpm");
  p += numField("rpm_redline", "REDLINE", String(gSettings.rpmRedline), "4000",
                String(gSettings.rpmMax - SETTINGS_RPM_ZONE_SPAN), "500", "rpm");
  p += "</fieldset>";

  p += "<fieldset><legend>Temperature</legend>";
  p += numField("cool_lo", "COOL LOW", String(gSettings.coolantLow), "40", "130", "5", "C");
  p += numField("cool_hi", "COOL HIGH", String(gSettings.coolantHigh), "50", "140", "5", "C");
  p += "</fieldset>";

  p += "<fieldset><legend>Voltage</legend>";
  p += numField("volt_lo", "VOLT LOW", String(gSettings.voltLow, 1), "9", "13", "0.5", "V");
  p += numField("volt_mid", "VOLT MID", String(gSettings.voltMid, 1), "9.5", "14", "0.5", "V");
  p += numField("volt_hi", "VOLT HIGH", String(gSettings.voltHigh, 1), "10", "18", "0.5", "V");
  p += "</fieldset>";

  p += "<fieldset><legend>Other</legend>";
  p += "<label><span>BOOT ANIM</span></label><select name='boot_anim'>";
  p += "<option value='1'";
  if (gSettings.bootAnim) p += " selected";
  p += ">ON</option><option value='0'";
  if (!gSettings.bootAnim) p += " selected";
  p += ">OFF</option></select>";
  p += "<label><span>MODEL</span></label><input type='text' name='model' maxlength='";
  p += String(SETTINGS_MODEL_LEN - 1);
  p += "' value='";
  p += htmlEscape(gSettings.model);
  p += "'>";
  p += "</fieldset>";

  p += "<button type='submit'>SAVE</button></form>";
  p += "<p class='hint'>Limits are clamped automatically (e.g. redline keeps "
       "2000 rpm before the scale maximum).</p>";
  p += "<a class='btn alt' href='/'>MAIN</a>";
  p += "<a class='btn alt' href='/log'>ELM LOG</a>";
  return p + pageTail();
}

void handle_settings_post() {
  Settings next = gSettings;

  auto parseInt = [](const char* key, int& dst) {
    if (!server.hasArg(key)) return;
    String v = server.arg(key);
    v.trim();
    if (!v.length()) return;
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    if (*end == '\0') dst = (int)n;
  };
  auto parseFloat = [](const char* key, float& dst) {
    if (!server.hasArg(key)) return;
    String v = server.arg(key);
    v.trim();
    if (!v.length()) return;
    char* end = nullptr;
    float f = strtof(v.c_str(), &end);
    if (*end == '\0') dst = f;
  };

  parseInt("rpm_max", next.rpmMax);
  parseInt("rpm_redline", next.rpmRedline);
  parseInt("cool_lo", next.coolantLow);
  parseInt("cool_hi", next.coolantHigh);
  parseFloat("volt_lo", next.voltLow);
  parseFloat("volt_mid", next.voltMid);
  parseFloat("volt_hi", next.voltHigh);
  if (server.hasArg("boot_anim")) next.bootAnim = server.arg("boot_anim") == "1";
  if (server.hasArg("model")) {
    String model = server.arg("model");
    model.trim();
    snprintf(next.model, sizeof(next.model), "%s", model.c_str());
  }

  // Нормализация и запись — только здесь, в UI-задаче. elmTask настройки не читает.
  gSettings = next;
  settings_normalize();
  settings_save();
  ui_settings_changed();

  // PRG: обновление страницы не повторяет сохранение.
  server.sendHeader("Location", "/settings?saved=1");
  server.send(303, "text/plain", "OK");
}

}  // namespace

void web_init() {
  // Маршруты регистрируются один раз; при выключении AP сервер только stop()/begin().
  server.on("/", []() {
    server.send(200, "text/html; charset=utf-8", rootPage());
  });

  server.on("/settings", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", settingsPage(server.hasArg("saved")));
  });
  server.on("/settings", HTTP_POST, handle_settings_post);

  // Журнал — отдельная страница в том же стиле; ?cleared=1 — подтверждение очистки.
  server.on("/log", []() {
    server.send(200, "text/html; charset=utf-8", logPage(server.hasArg("cleared")));
  });

  // Журнал подключений сохраняется во флэше и переживает перезагрузку.
  // ?all=1 — весь файл, ?clear=1 — очистить и вернуться на страницу журнала.
  server.on("/elm-log", []() {
    if (server.hasArg("clear")) {
      storage_clear_log();
      server.sendHeader("Location", "/log?cleared=1");
      server.send(303, "text/plain", "OK");
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

  // Проверки «есть ли интернет» у Android/Apple/Windows. На каждый такой запрос
  // отвечаем редиректом на корень — ОС показывает вкладку captive portal.
  server.on("/generate_204", redirect_to_root);
  server.on("/gen_204", redirect_to_root);
  server.on("/hotspot-detect.html", redirect_to_root);
  server.on("/library/test/success.html", redirect_to_root);
  server.on("/connecttest.txt", redirect_to_root);
  server.on("/ncsi.txt", redirect_to_root);
  server.on("/fwlink", redirect_to_root);
  server.on("/redirect", redirect_to_root);
  // Всё остальное (favicon, неизвестные probing-пути) тоже ведёт на корень.
  server.onNotFound(redirect_to_root);

  // Применить состояние, сохранённое с прошлого включения. По умолчанию выключено.
  web_set_enabled(gSettings.webEnabled);
}

void web_set_enabled(bool enabled) {
  if (enabled != webRunning) {
    if (enabled) {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(web_ap_ssid().c_str(), WIFI_AP_PASS);
      // "*" — catch-all: любые домены из проверок интернета резолвятся в IP AP.
      // Обработка пакетов идёт в AsyncUDP сама, web_loop() её не опрашивает.
      dnsServer.start(53, "*", WiFi.softAPIP());
      server.begin();
    } else {
      server.stop();
      dnsServer.stop();
      WiFi.softAPdisconnect(true);  // true — выключить радио, не только сам AP
    }
    webRunning = enabled;
  }
  if (gSettings.webEnabled != enabled) {
    gSettings.webEnabled = enabled;
    settings_save();
  }
}

bool web_is_enabled() { return webRunning; }

void web_loop() {
  if (!webRunning) return;
  server.handleClient();
}
