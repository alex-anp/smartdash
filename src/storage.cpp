/*
 * Хранилище: NVS (адрес ELM, счётчик загрузок) и LittleFS (журнал подключений).
 *
 * Журнал пишется в файл на флэше, поэтому переживает поездку, выключение
 * питания и перезагрузку. При достижении 1 МБ текущий файл становится
 * предыдущим, и начинается новый — суммарно до ~2 МБ истории.
 * Доступ из BLE-задачи и Serial/HTTP защищён мьютексом.
 */
#include "storage.h"

#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static Preferences prefs;
static SemaphoreHandle_t logMutex = nullptr;
static bool fsReady = false;

static const char* NS = "smartdash";
static const char* KEY_LAST_ELM = "last_elm";
static const char* KEY_LAST_ELM_TYPE = "last_elm_type";
static const char* KEY_DRAG_LAST = "drag_last";
static const char* KEY_DRAG_BEST_OLD = "drag_best";  // совместимость со старой прошивкой
static const char* KEY_LOG_BOOT = "log_boot";

static const char* LOG_PATH = "/elm.log";
static const char* LOG_PREV_PATH = "/elm.prev.log";

static const uint32_t LOG_ROTATE_BYTES = 1024 * 1024;  // 1 МБ на файл
static const size_t SERIAL_TAIL_BYTES = 16 * 1024;
static uint32_t bootCount = 0;

void storage_init() {
  prefs.begin(NS, false);  // Namespace на чтение/запись
  if (!logMutex) logMutex = xSemaphoreCreateMutex();
  bootCount = prefs.getUInt(KEY_LOG_BOOT, 0) + 1;
  prefs.putUInt(KEY_LOG_BOOT, bootCount);

  // Раздел spiffs уже есть в таблице разделов default_16MB.csv (3.5 МБ).
  fsReady = LittleFS.begin(true);
}

String storage_get_last_elm() {
  return prefs.getString(KEY_LAST_ELM, "");  // Пустая строка = «ещё не подключались»
}

void storage_save_last_elm(const String &addr) {
  prefs.putString(KEY_LAST_ELM, addr);
}

int storage_get_last_elm_type() {
  uint32_t v = prefs.getUInt(KEY_LAST_ELM_TYPE, 0xFFFFFFFFu);
  // 0 = BLE_ADDR_PUBLIC, 1 = BLE_ADDR_RANDOM. Старые записи без типа
  // возвращают -1, чтобы драйвер сначала попробовал прямой connect, а при
  // неудаче перешёл к сканированию и узнал тип из advertisement.
  return v <= 1u ? (int)v : -1;
}

void storage_save_last_elm_type(uint8_t type) {
  prefs.putUInt(KEY_LAST_ELM_TYPE, type);
}

void storage_clear_last_elm() {
  prefs.remove(KEY_LAST_ELM);
  prefs.remove(KEY_LAST_ELM_TYPE);
}

String storage_get_drag_last() { return prefs.getString(KEY_DRAG_LAST, ""); }
void storage_save_drag_last(const String& line) { prefs.putString(KEY_DRAG_LAST, line); }

// Ключ лучшего результата включает цель: 0-60 и 0-100 не должны перезаписывать
// друг друга. Буфера достаточно для "drag_best_100" и завершающего '\0'.
static void dragBestKey(uint16_t target, char* key, size_t len) {
  snprintf(key, len, "drag_best_%u", (unsigned)target);
}

String storage_get_drag_best(uint16_t target) {
  char key[20];
  dragBestKey(target, key, sizeof(key));
  String line = prefs.getString(key, "");
  if (line.length()) return line;

  // Старая прошивка хранила один общий best. Переносим его только если цель
  // совпадает с запрошенной — иначе 0-60 снова стал бы рекордом 0-100.
  String old = prefs.getString(KEY_DRAG_BEST_OLD, "");
  int comma = old.indexOf(',');
  if (comma > 0 && (uint16_t)old.substring(0, comma).toInt() == target) return old;
  return "";
}

void storage_save_drag_best(uint16_t target, const String& line) {
  char key[20];
  dragBestKey(target, key, sizeof(key));
  prefs.putString(key, line);
}

// Одна строка журнала. Переносы строк из ответов ELM заменяются на '|'.
void storage_log(const char* fmt, ...) {
  if (!logMutex || !fsReady) return;
  char line[168];  // 192 с учётом префикса "bN tS.mmm "
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  for (size_t i = 0; i < sizeof(line); i++) {
    if (line[i] == '\r' || line[i] == '\n' || line[i] == '\t') line[i] = '|';
    if (line[i] == '\0') break;
  }

  xSemaphoreTake(logMutex, portMAX_DELAY);
  File f = LittleFS.open(LOG_PATH, "a");
  if (f && f.size() >= LOG_ROTATE_BYTES) {
    f.close();
    LittleFS.remove(LOG_PREV_PATH);
    LittleFS.rename(LOG_PATH, LOG_PREV_PATH);
    f = LittleFS.open(LOG_PATH, "a");
  }
  if (f) {
    const uint32_t t = millis();
    char full[192];
    snprintf(full, sizeof(full), "b%lu t%lu.%03lu %s", (unsigned long)bootCount,
             (unsigned long)(t / 1000), (unsigned long)(t % 1000), line);
    f.println(full);
    f.close();
  }
  xSemaphoreGive(logMutex);
}

static size_t fileSize(const char* path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, "r");
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

// Логическая длина журнала: сначала старый файл, затем текущий.
size_t storage_log_size() {
  if (!logMutex || !fsReady) return 0;
  xSemaphoreTake(logMutex, portMAX_DELAY);
  const size_t n = fileSize(LOG_PREV_PATH) + fileSize(LOG_PATH);
  xSemaphoreGive(logMutex);
  return n;
}

// Порционное чтение с переходом через границу двух файлов.
size_t storage_read_log(char* buf, size_t len, size_t offset) {
  if (!logMutex || !fsReady || !buf || !len) return 0;
  xSemaphoreTake(logMutex, portMAX_DELAY);
  const size_t prevSize = fileSize(LOG_PREV_PATH);
  const size_t curSize = fileSize(LOG_PATH);
  const char* path = nullptr;
  size_t inFileOffset = 0;
  size_t available = 0;
  if (offset < prevSize) {
    path = LOG_PREV_PATH;
    inFileOffset = offset;
    available = prevSize - offset;
  } else if (offset < prevSize + curSize) {
    path = LOG_PATH;
    inFileOffset = offset - prevSize;
    available = curSize - inFileOffset;
  }

  size_t n = 0;
  if (path && available) {
    File f = LittleFS.open(path, "r");
    if (f && f.seek(inFileOffset, SeekSet)) {
      if (len > available) len = available;
      n = f.read((uint8_t*)buf, len);
    }
    if (f) f.close();
  }
  xSemaphoreGive(logMutex);
  return n;
}

void storage_clear_log() {
  if (!logMutex || !fsReady) return;
  xSemaphoreTake(logMutex, portMAX_DELAY);
  LittleFS.remove(LOG_PATH);
  LittleFS.remove(LOG_PREV_PATH);
  xSemaphoreGive(logMutex);
}

void storage_dump_log_serial(bool all) {
  const size_t size = storage_log_size();
  if (!size) {
    Serial.println("ELM log: empty or LittleFS unavailable");
    return;
  }
  size_t start = 0;
  if (!all && size > SERIAL_TAIL_BYTES) {
    start = size - SERIAL_TAIL_BYTES;
    char probe[256];
    size_t pos = start;
    bool aligned = false;
    while (pos < size) {
      size_t n = storage_read_log(probe, sizeof(probe), pos);
      if (!n) break;
      for (size_t i = 0; i < n; i++) {
        if (probe[i] == '\n') {
          start = pos + i + 1;  // начать со следующей полной строки
          aligned = true;
          break;
        }
      }
      if (aligned) break;
      pos += n;
    }
  }

  Serial.printf("\n=== ELM connection log (%s), %u bytes ===\n", all ? "all" : "tail",
                (unsigned int)(size - start));
  char buf[256];
  size_t pos = start;
  while (pos < size) {
    size_t want = sizeof(buf);
    if (want > size - pos) want = size - pos;
    size_t n = storage_read_log(buf, want, pos);
    if (!n) break;
    Serial.write((uint8_t*)buf, n);
    pos += n;
  }
  Serial.println("\n=== end of log ===");
  Serial.println("Commands: log - tail, log all - full, clear - reset, help - hint");
}

// Неблокирующий разбор команд из USB Serial: verb [arg].
void storage_serial_loop() {
  static char line[32];
  static size_t len = 0;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len == 0) continue;
      line[len] = '\0';
      len = 0;

      char verb[16] = {0};
      char arg[8] = {0};
      size_t vi = 0, ai = 0;
      bool inArg = false;
      for (char* p = line; *p; ++p) {
        if (*p == ' ' || *p == '\t') {
          if (vi) inArg = true;
          continue;
        }
        if (!inArg) {
          if (vi + 1 < sizeof(verb)) verb[vi++] = (char)tolower((unsigned char)*p);
        } else if (ai + 1 < sizeof(arg)) {
          arg[ai++] = (char)tolower((unsigned char)*p);
        }
      }

      if (strcmp(verb, "log") == 0 || strcmp(verb, "l") == 0) {
        storage_dump_log_serial(strcmp(arg, "all") == 0);
      } else if (strcmp(verb, "clear") == 0 || strcmp(verb, "c") == 0) {
        storage_clear_log();
        Serial.println("ELM log cleared");
      } else if (strcmp(verb, "help") == 0 || strcmp(verb, "h") == 0) {
        Serial.println("Commands: log - tail, log all - full, clear - reset");
      } else {
        Serial.println("Unknown command. Type 'help'.");
      }
    } else if (len + 1 < sizeof(line)) {
      line[len++] = c;
    } else {
      len = 0;  // слишком длинная строка — сбрасываем
    }
  }
}
