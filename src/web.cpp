/*
 * Wi-Fi точка доступа: веб-форма настроек, выгрузка журнала ELM и CSV заездов.
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
        ".btn.danger{background:#a33;color:#fff}"
        ".row{display:flex;gap:8px}.row>*{flex:1;width:auto;margin:8px 0;padding:10px 4px;font-size:15px}"
       "a{color:#7bd}.ok{background:#163;border-radius:6px;padding:8px 10px;margin-bottom:12px}"
       ".hint{color:#888;font-size:13px}"
        "canvas{width:100%;height:260px;display:block;background:#15151b;"
        "border:1px solid #333;border-radius:8px;margin:8px 0}"
        ".legend{font-size:13px;color:#bbb;margin:4px 0 10px}"
        ".legend i{display:inline-block;width:10px;height:10px;border-radius:2px;"
        "margin:0 4px 0 10px}.legend i:first-child{margin-left:0}"
        "table{width:100%;border-collapse:collapse;font-size:13px;margin-top:8px}"
        "th,td{padding:5px 4px;border-bottom:1px solid #333;text-align:right;"
        "white-space:nowrap}th{color:#8a8}td.ev{text-align:left;color:#bbb}"
        ".res td{background:#163;color:#fff;font-weight:bold}"
        ".tablewrap{overflow-x:auto}</style></head><body><main><h1>SmartDash</h1>";
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
  p += "<a class='btn alt' href='/drag'>DRAG LOG</a>";
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

// Каждый заезд лежит отдельным CSV. Со списка открывается страница просмотра
// с графиком и таблицей (/drag-view), скачивание и удаление — рядом в одном ряду.
String dragPage(bool cleared, bool deleted) {
  String p = pageHead("Drag log");
  if (cleared) p += "<p class='ok'>Drag logs cleared.</p>";
  if (deleted) p += "<p class='ok'>Drag run deleted.</p>";
  const size_t n = storage_drag_count();
  p += "<fieldset><legend>DRAG RUNS</legend>";
  p += "<p class='hint'>Stored: ";
  p += String(n);
  p += " runs (last 20 kept)</p>";
  p += "</fieldset>";

  if (!n) {
    p += "<p class='hint'>No drag runs yet.</p>";
  } else {
    for (size_t i = 0; i < n; i++) {
      const String name = storage_drag_name(i);
      const String idx = String(i);
      p += "<fieldset><legend>RUN ";
      p += String(i + 1);
      p += "</legend><p>";
      p += htmlEscape(name);
      p += "</p><p class='hint'>";
      p += String(storage_drag_size(i));
      p += " bytes</p><div class='row'>";
      p += "<a class='btn' href='/drag-view?n=" + idx + "'>VIEW</a>";
      p += "<a class='btn alt' href='/drag-csv?n=" + idx +
           "&amp;dl=1'>DOWNLOAD</a>";
      p += "<a class='btn danger' href='/drag-delete?n=" + idx + "&amp;name=" +
           name + "' onclick=\"return confirm('Delete this run?')\">DELETE</a>";
      p += "</div></fieldset>";
    }
  }

  p += "<div class='row'>";
  p += "<a class='btn alt' href='/drag?clear=1' "
       "onclick=\"return confirm('Clear all drag logs?')\">CLEAR ALL</a>";
  p += "<a class='btn alt' href='/'>MAIN</a></div>";
  return p + pageTail();
}

// Просмотр одного заезда: CSV читается браузером с /drag-csv, по нему рисуется
// график скорости и оборотов, ниже — таблица сэмплов с интерполяцией.
String dragViewPage(size_t index) {
  String p = pageHead("Drag run");
  p += "<fieldset><legend>RUN " + String(index + 1) +
       "</legend><p class='hint' id='meta'></p></fieldset>";
  p += "<div id='msg' class='hint'></div>";
  p += "<canvas id='chart'></canvas>";
  p += "<div class='legend'><i style='background:#4dd'></i>speed, km/h"
       "<i style='background:#fa5'></i>rpm</div>";
  p += "<p class='hint'>Decimals are estimates: OBD speed is integer km/h; "
       "the fractional part comes from smoothed rpm inside each 1 km/h bin, "
       "then interpolated to 50 ms.</p>";
  p += "<div id='summary' class='hint'></div>";
  p += "<div class='tablewrap'><table id='samples'></table></div>";
  p += "<div class='row'>";
  p += "<a class='btn alt' href='/drag'>BACK</a>";
  p += "<a class='btn' href='/drag-csv?n=" + String(index) +
       "&amp;dl=1'>DOWNLOAD CSV</a></div>";
  p += "<script>";
  p += "const DRAG_INDEX = " + String(index) + ";\n";
  p += R"JS(
(function(){
  const msg = document.getElementById('msg');
  fetch('/drag-csv?n=' + DRAG_INDEX).then(function(r){
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.text();
  }).then(function(text){
    const all = [];
    text.split(/\r?\n/).forEach(function(line){
      if (!line.trim()) return;
      if (line.charAt(0) === '#') {
        document.getElementById('meta').textContent = line.slice(1).trim();
        return;
      }
      const c = line.split(',');
      if (c.length < 6 || c[0] === 't_ms') return;
      all.push(c);
    });
    const isResult = function(c){ return c[5].indexOf('RESULT') === 0 || c[5].indexOf('ABORT') === 0; };
    const samples = all.filter(function(c){ return !isResult(c); });
    const result = all.filter(isResult).pop() || null;
    let first = -1;
    for (let i = 0; i < samples.length; i++) {
      if (Number(samples[i][4]) > 0) { first = i; break; }
    }
    // Один сэмпл до начала движения — точка отсчёта; остальные нули (включая
    // на остановке) не показываем.
    const rows = [];
    if (first >= 0) {
      if (first > 0) rows.push(samples[first - 1]);
      for (let i = first; i < samples.length; i++) {
        if (Number(samples[i][4]) > 0) rows.push(samples[i]);
      }
    }
    if (!rows.length) { msg.textContent = 'No moving samples in this run.'; return; }
    // OBD отдаёт целые км/ч. Дробную часть оцениваем по оборотам внутри
    // интервала с одинаковой скоростью. Джиттер оборотов (~±1.5%) больше,
    // чем шаг оборотов между соседними км/ч, поэтому деление на разность
    // соседних групп давало выбросы до 100 км/ч. Сглаживаем обороты, берём
    // наклон по окну ±10 сэмплов и ограничиваем дробную часть диапазоном [0,1).
    const raw = rows.map(function(c){
      return {t:Number(c[0]), r:Number(c[3]), s:Number(c[4])};
    });
    const n = raw.length;
    const smoothRpm = [];
    for (let i = 0; i < n; i++) {
      const a = Math.max(0, i - 4), b = Math.min(n - 1, i + 4);
      let sum = 0;
      for (let k = a; k <= b; k++) sum += raw[k].r;
      smoothRpm.push(sum / (b - a + 1));
    }
    const groups = [];
    raw.forEach(function(p, i){
      if (i === 0 || p.s !== raw[i - 1].s) {
        groups.push({s:p.s, sum:0, n:0, rpm:0});
      }
      groups[groups.length - 1].sum += smoothRpm[i];
      groups[groups.length - 1].n++;
    });
    groups.forEach(function(g){ g.rpm = g.sum / g.n; });
    const bySpeed = {};
    groups.forEach(function(g){ bySpeed[g.s] = g; });

    // Резервный наклон: регрессия оборотов по целой скорости всем заездом.
    let sx = 0, sy = 0, sxx = 0, sxy = 0;
    groups.forEach(function(g){
      sx += g.s; sy += g.rpm; sxx += g.s * g.s; sxy += g.s * g.rpm;
    });
    const gn = groups.length;
    const denom = gn * sxx - sx * sx;
    const globalSlope = Math.abs(denom) > 1e-6 ? (gn * sxy - sx * sy) / denom : 1;

    const est = [];
    raw.forEach(function(p, i){
      const a = Math.max(0, i - 10), b = Math.min(n - 1, i + 10);
      const ds = raw[b].s - raw[a].s;
      const slope = Math.abs(ds) >= 3 ? (smoothRpm[b] - smoothRpm[a]) / ds : globalSlope;
      const g = bySpeed[p.s];
      const d = Math.abs(slope) > 1e-6 ? slope : globalSlope;
      // OBD округляет вниз: истинная скорость лежит в [v, v+1).
      const frac = 0.5 + (smoothRpm[i] - g.rpm) / d;
      est.push(p.s + Math.max(0, Math.min(0.999, frac)));
    });
    // Небольшое сглаживание убирает ступеньки на границах целых интервалов.
    // Обороты на графике тоже берём сглаженными.
    raw.forEach(function(p, i){
      const a = Math.max(0, i - 2), b = Math.min(n - 1, i + 2);
      let sum = 0;
      for (let k = a; k <= b; k++) sum += est[k];
      p.s = sum / (b - a + 1);
      p.r = smoothRpm[i];
    });
    const t0 = raw[0].t;
    const tEnd = raw[n - 1].t;
    const series = [];
    let j = 0;
    for (let t = t0; t <= tEnd; t += 50) {
      while (j < n - 1 && raw[j + 1].t < t) j++;
      const a = raw[j], b = raw[j + 1] || a;
      const f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 0;
      series.push({t:t, s:a.s + (b.s - a.s) * f, r:a.r + (b.r - a.r) * f});
    }
    draw(series);
    fillTable(series, result);
  }).catch(function(e){ msg.textContent = 'Load error: ' + e.message; });

  function draw(series){
    const cv = document.getElementById('chart');
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth || 360, h = 260;
    cv.width = Math.round(w * dpr);
    cv.height = Math.round(h * dpr);
    const ctx = cv.getContext('2d');
    ctx.scale(dpr, dpr);
    ctx.fillStyle = '#15151b';
    ctx.fillRect(0, 0, w, h);

    const padL = 38, padR = 38, padT = 14, padB = 26;
    const x0 = padL, x1 = w - padR, y0 = h - padB, y1 = padT;
    const t0 = series[0].t, tEnd = series[series.length - 1].t;
    let maxS = 10, maxR = 1000;
    series.forEach(function(p){
      maxS = Math.max(maxS, p.s);
      maxR = Math.max(maxR, p.r);
    });
    maxS = Math.ceil((maxS + 5) / 10) * 10;
    maxR = Math.ceil((maxR + 500) / 1000) * 1000;

    const X = function(t){ return x0 + (t - t0) / Math.max(1, tEnd - t0) * (x1 - x0); };
    const YS = function(v){ return y0 - v / maxS * (y0 - y1); };
    const YR = function(v){ return y0 - v / maxR * (y0 - y1); };

    ctx.strokeStyle = '#2a2a32';
    ctx.lineWidth = 1;
    ctx.font = '11px system-ui';
    ctx.fillStyle = '#888';
    for (let i = 0; i <= 4; i++) {
      const y = y1 + (y0 - y1) * i / 4;
      ctx.beginPath(); ctx.moveTo(x0, y); ctx.lineTo(x1, y); ctx.stroke();
      ctx.textAlign = 'right';
      ctx.fillText(String(Math.round(maxS * (4 - i) / 4)), x0 - 4, y + 4);
      ctx.textAlign = 'left';
      ctx.fillText(String(Math.round(maxR * (4 - i) / 4)), x1 + 4, y + 4);
    }
    // Шкала времени: 5 подписей через равные интервалы, а не одна общая длительность.
    for (let i = 0; i <= 5; i++) {
      const x = x0 + (x1 - x0) * i / 5;
      ctx.beginPath(); ctx.moveTo(x, y1); ctx.lineTo(x, y0); ctx.stroke();
      const sec = (tEnd - t0) * i / 5 / 1000;
      ctx.textAlign = 'center';
      ctx.fillText(sec.toFixed(1) + 's', x, y0 + 14);
    }

    function plot(key, Y, color){
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      let first = true;
      series.forEach(function(p){
        const x = X(p.t), y = Y(p[key]);
        if (first) { ctx.moveTo(x, y); first = false; } else { ctx.lineTo(x, y); }
      });
      ctx.stroke();
    }
    plot('s', YS, '#4dd');
    plot('r', YR, '#fa5');
  }

  function fillTable(series, result){
    const table = document.getElementById('samples');
    const t0 = series[0].t;
    let maxSpd = -1, maxT = t0;
    series.forEach(function(p){
      if (p.s > maxSpd) { maxSpd = p.s; maxT = p.t; }
    });
    const data = series.map(function(p){
      return [String(Math.round(p.r)), p.s.toFixed(1), ((p.t - t0) / 1000).toFixed(2)];
    });
    if (result) {
      // В итоговой строке — не время финиша, а момент максимальной скорости.
      data.push(['', maxSpd >= 0 ? maxSpd.toFixed(1) : '', ((maxT - t0) / 1000).toFixed(2)]);
      document.getElementById('summary').textContent =
        (result[5] || 'RESULT') + ': max ' + (maxSpd >= 0 ? maxSpd.toFixed(1) : '-') +
        ' km/h at ' + ((maxT - t0) / 1000).toFixed(2) + ' s';
    }
    const cols = ['RPM', 'SPEED', 'TIME, s'];
    const head = document.createElement('thead');
    const hr = document.createElement('tr');
    cols.forEach(function(key){
      const th = document.createElement('th');
      th.textContent = key;
      hr.appendChild(th);
    });
    head.appendChild(hr);
    table.appendChild(head);
    const body = document.createElement('tbody');
    data.forEach(function(row, i){
      const tr = document.createElement('tr');
      if (result && i === data.length - 1) tr.className = 'res';
      row.forEach(function(v){
        const td = document.createElement('td');
        td.textContent = v;
        tr.appendChild(td);
      });
      body.appendChild(tr);
    });
    table.appendChild(body);
  }
})();
)JS";
  p += "</script>";
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

  // Панель ST77916 поддерживает только кратные 90°, остальные значения
  // settings_normalize() округляет до ближайшего.
  p += "<fieldset><legend>Display</legend>";
  p += "<label><span>ROTATION</span></label><select name='rotation'>";
  static const int rotations[] = {0, 90, 180, 270};
  for (int r : rotations) {
    p += "<option value='";
    p += String(r);
    p += "'";
    if (gSettings.rotation == r) p += " selected";
    p += ">";
    p += String(r);
    p += " deg</option>";
  }
  p += "</select></fieldset>";

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
  p += "<a class='btn alt' href='/drag'>DRAG LOG</a>";
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
  parseInt("rotation", next.rotation);
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

  // Подробные данные заездов: по одному CSV на заезд в /drag.
  server.on("/drag", []() {
    if (server.hasArg("clear")) {
      storage_drag_clear();
      server.sendHeader("Location", "/drag?cleared=1");
      server.send(303, "text/plain", "OK");
      return;
    }
    server.send(200, "text/html; charset=utf-8",
                dragPage(server.hasArg("cleared"), server.hasArg("deleted")));
  });

  server.on("/drag-view", []() {
    const int index = server.arg("n").toInt();
    if (index < 0 || (size_t)index >= storage_drag_count()) {
      server.send(404, "text/plain; charset=utf-8", "Drag run not found");
      return;
    }
    server.send(200, "text/html; charset=utf-8", dragViewPage((size_t)index));
  });

  // Удаление одного заезда: name берём из списка и сверяем с индексом, чтобы
  // не удалить другой файл, если список сдвинулся после открытия страницы.
  server.on("/drag-delete", []() {
    const int index = server.arg("n").toInt();
    const String name = server.arg("name");
    if (index < 0 || (size_t)index >= storage_drag_count() ||
        name != storage_drag_name((size_t)index)) {
      server.send(404, "text/plain; charset=utf-8", "Drag run not found");
      return;
    }
    storage_drag_remove(name);
    server.sendHeader("Location", "/drag?deleted=1");
    server.send(303, "text/plain", "OK");
  });

  server.on("/drag-csv", []() {
    const int index = server.arg("n").toInt();
    const size_t count = storage_drag_count();
    if (index < 0 || (size_t)index >= count) {
      server.send(404, "text/plain; charset=utf-8", "Drag run not found");
      return;
    }
    const String name = storage_drag_name((size_t)index);
    if (server.hasArg("dl")) {
      server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/csv; charset=utf-8", "");
    const size_t size = storage_drag_size((size_t)index);
    char buf[512];
    size_t pos = 0;
    while (pos < size) {
      size_t want = sizeof(buf);
      if (want > size - pos) want = size - pos;
      const size_t got = storage_drag_read((size_t)index, buf, want, pos);
      if (!got) break;
      server.sendContent(buf, got);
      pos += got;
    }
    server.sendContent("");
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
