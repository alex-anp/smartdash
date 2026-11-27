/*
 * Драйвер ELM327 по BLE.
 *
 * Особенности архитектуры:
 *  - Вся блокирующая работа с BLE (старт сканирования, подключение, discovery GATT)
 *    выполняется в отдельной задаче FreeRTOS на core 0. Цикл UI/web на core 1
 *    никогда не ждёт радио-операций.
 *  - Публичные функции только выставляют volatile-флаги запросов. Рабочая задача
 *    разбирает их в elm_loop(), поэтому вызовы из setup()/UI быстрые и повторно входимые.
 *  - AT-команды работают через неблокирующий конечный автомат: отправить -> ждать
 *    приглашения '>' или таймаута -> разобрать. Приглашение уже означает готовность,
 *    поэтому дополнительная пауза не нужна; она вырастает только если клон ответил
 *    по таймауту или без 410C, и медленно уменьшается после серии удачных ответов.
 *    Быстрые PID запрашиваются одним запросом 010C0D; температура и напряжение
 *    чередуются отдельно раз в секунду.
 *  - Ответы приходят асинхронно в notifyCB(); rxBuf защищён rxMux, а список
 *    результатов сканирования — foundMux.
 *
 * Разобранные значения передаются в UI через ui_set_data(); UI сам выполняет
 * отрисовку и никогда не обращается к BLE-объектам напрямую.
 */
#include "elm327.h"

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <vector>

#include "config.h"
#include "storage.h"
#include "ui.h"

namespace {

// Тайминги. Сканирование короткое, чтобы отклик ощущался быстрым; таймауты команд
// запасные, поскольку дешёвые клоны ELM могут делать паузу перед ответом.
// Для ATZ и 0100 в handleInit() задан ещё больший таймаут.
constexpr uint32_t SCAN_TIME_MS = 4000;
constexpr uint32_t CMD_TIMEOUT_MS = 1200;
// '>' в конце ответа уже говорит, что ELM принял команду и готов к следующей.
// Старые 120 мс добавлялись к времени ответа и были главным тормозом опроса.
// Стартуем без паузы; дешёвые клоны иногда выдают '>' раньше, чем сбрасывают
// приёмник команд, — тогда ответ приходит без 410C или по таймауту, и пауза
// автоматически удваивается. После длинной серии удачных ответов она снова
// уменьшается, чтобы не залипать на избыточной задержке после помехи.
constexpr uint32_t POLL_GAP_MS = 0;
constexpr uint32_t POLL_GAP_STEP_MS = 8;
constexpr uint32_t POLL_GAP_MAX_MS = 120;
constexpr uint8_t POLL_GOOD_TO_RELAX = 12;  // ~2 с удачных ответов перед уменьшением паузы
constexpr uint32_t SLOW_POLL_MS = 1000;     // 0105 и ATRV по очереди: каждый раз в 2 с
constexpr uint8_t MULTI_FAIL_LIMIT = 3;     // Столько ответов без 41 0C — и уходим на раздельные PID
constexpr uint32_t CONNECT_TIMEOUT_MS = 6000;
constexpr uint32_t STEADY_MS = 300000;      // 5 мин быстрых повторов после старта/потери связи
constexpr uint32_t PAUSE_MIN = 2000;        // Быстрый повтор в окне STEADY_MS
constexpr uint32_t PAUSE_MAX = 60000;       // Долгая стоянка: 2→4→8→…→60 с
constexpr size_t MAX_FOUND = 8;             // Максимум строк в списке на экране

// Объекты GATT принадлежат и используются только BLE-задачей. advDevice — копия
// найденного устройства в куче: сканер освобождает свой результат после onResult.
NimBLEAdvertisedDevice* advDevice = nullptr;
String pendingName;                 // имя устройства, выбранного сканером
int8_t pendingRssi = -127;           // и его уровень сигнала в момент выбора
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* chrWrite = nullptr;
NimBLERemoteCharacteristic* chrNotify = nullptr;

// Флаги для обмена между задачами: пишутся API или колбэками, читаются в elm_loop().
volatile bool doConnect = false;
volatile bool connecting = false;
volatile bool connected = false;
volatile bool needScanRestart = false;
volatile bool requestScanStart = false;
uint32_t connectStartedAt = 0;
uint32_t retryAt = 0;               // когда можно делать следующую попытку
uint32_t reconnectGap = PAUSE_MIN;  // текущая пауза между неудачными попытками
uint32_t steadyUntil = 0;           // до этого момента повторяем часто
bool directConnectNext = false;     // следующая попытка — прямой connect к сохранённому MAC
bool requestDirectConnect = false;  // запрос к рабочей задаче (аналог requestScanStart)
String pendingAddr;                 // Адрес, к которому подключаемся; при успехе сохраняется в NVS
volatile ElmStatus status = ElmStatus::Idle;
volatile bool raceMode = false;  // экран замера: опрашиваем только обороты и скорость

// Общий снимок, который читает draw_elm_screen() на ядре UI.
ElmScanEntry found[MAX_FOUND];
size_t foundCount = 0;
ElmConnectedInfo linked;  // данные активного соединения, тот же мьютекс, что и found
portMUX_TYPE foundMux = portMUX_INITIALIZER_UNLOCKED;  // Защищает found/foundCount/linked

// notifyCB() добавляет байты в BLE-задаче; readCmdResult() их там же и читает,
// но мьютекс явно фиксирует границу на случай отладки и логирования.
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

void setStatus(ElmStatus s) { status = s; }

void clearFound() {
  portENTER_CRITICAL(&foundMux);
  foundCount = 0;
  portEXIT_CRITICAL(&foundMux);
}

// Запоминаем, к чему подключаемся. Вызывается до начала подключения, поэтому
// при неудаче эти данные просто не попадут на экран (elm_get_connected()
// проверяет initState == INIT_DONE).
void rememberLink(const String& addr, uint8_t type, bool typeKnown, const String& name, int8_t rssi) {
  portENTER_CRITICAL(&foundMux);
  snprintf(linked.address, sizeof(linked.address), "%s", addr.c_str());
  snprintf(linked.name, sizeof(linked.name), "%s", name.length() ? name.c_str() : "<unknown>");
  linked.proto[0] = 0;
  linked.rssi = rssi;
  linked.addrType = type;
  linked.typeKnown = typeKnown;
  linked.sinceMs = millis();
  portEXIT_CRITICAL(&foundMux);
}

// Добавить или обновить результат сканирования. Если список фиксированного размера
// заполнен, заменяем запись с самым слабым RSSI, чтобы ближайшие адаптеры оставались видимыми.
// Массив всегда отсортирован по убыванию RSSI: сильнейший сигнал первый.
void recordFound(const NimBLEAdvertisedDevice* d, bool compatible) {
  const String addr = String(d->getAddress().toString().c_str());
  if (!addr.length()) return;
  std::string name = d->haveName() ? d->getName() : "";
  if (name.empty()) name = "<unknown>";

  portENTER_CRITICAL(&foundMux);
  int idx = -1;
  for (size_t i = 0; i < foundCount; i++) {
    if (strcmp(found[i].address, addr.c_str()) == 0) {
      idx = (int)i;
      break;
    }
  }
  if (idx < 0) {
    if (foundCount < MAX_FOUND) {
      idx = (int)foundCount++;
    } else {
      // При переполнении списка оставляем самые сильные сигналы.
      int weakest = 0;
      for (size_t i = 1; i < foundCount; i++) {
        if (found[i].rssi < found[weakest].rssi) weakest = (int)i;
      }
      idx = weakest;
    }
  }
  snprintf(found[idx].address, sizeof(found[idx].address), "%s", addr.c_str());
  snprintf(found[idx].name, sizeof(found[idx].name), "%s", name.c_str());
  found[idx].rssi = d->getRSSI();
  found[idx].compatible = compatible;

  // Переставляем запись в нужное место: вверх при усилении сигнала,
  // вниз при ослаблении. Остальные записи уже отсортированы.
  while (idx > 0 && found[idx].rssi > found[idx - 1].rssi) {
    ElmScanEntry tmp = found[idx - 1];
    found[idx - 1] = found[idx];
    found[idx] = tmp;
    idx--;
  }
  while (idx + 1 < (int)foundCount && found[idx].rssi < found[idx + 1].rssi) {
    ElmScanEntry tmp = found[idx + 1];
    found[idx + 1] = found[idx];
    found[idx] = tmp;
    idx++;
  }
  portEXIT_CRITICAL(&foundMux);
}

String targetAddr;
uint8_t targetType = BLE_ADDR_PUBLIC;       // тип адреса сохранённого адаптера
bool targetTypeKnown = false;               // false = старая запись NVS без типа
uint8_t pendingAddrType = BLE_ADDR_PUBLIC;  // тип адреса текущей попытки
String rxBuf;

// Если сохранённый MAC не отвечает (например, дешёвый адаптер сменил случайный
// адрес), после нескольких неудач возвращаемся к поиску любого совместимого ELM.
uint8_t targetFailCount = 0;
void noteTargetFailure() {
  if (targetAddr.length() == 0) return;
  storage_log("TARGET fail %u/3 addr=%s", targetFailCount + 1, targetAddr.c_str());
  if (++targetFailCount >= 3) {
    targetFailCount = 0;
    storage_log("TARGET fallback: scan any ELM");
    targetAddr = "";
    targetTypeKnown = false;
    directConnectNext = false;
  }
}

// Единая точка планирования повтора. После потери выбранного адаптера или в
// первые 5 минут после старта повторяем через 2 с; дальше пауза удваивается
// до 60 с, чтобы долгий простой не жег батарею и не мешал телефону.
void scheduleRetry(bool immediate, bool directNext) {
  const uint32_t now = millis();
  if (immediate) {
    reconnectGap = PAUSE_MIN;
    retryAt = now;
  } else {
    if (now >= steadyUntil && reconnectGap < PAUSE_MAX) {
      reconnectGap *= 2;
      if (reconnectGap > PAUSE_MAX) reconnectGap = PAUSE_MAX;
    }
    retryAt = now + (now < steadyUntil ? PAUSE_MIN : reconnectGap);
  }
  directConnectNext = directNext && targetAddr.length() > 0;
  needScanRestart = true;
  storage_log("RETRY in %u ms direct=%d", immediate ? 0u : (uint32_t)(retryAt - now),
              directConnectNext ? 1 : 0);
}

DashData dash;
uint32_t lastPoll = 0;
uint32_t pollGap = POLL_GAP_MS;       // адаптивная пауза: растёт только после сбоев
uint8_t pollGoodStreak = 0;           // подряд удачных ответов перед уменьшением паузы
uint32_t lastSlowPoll = 0;            // когда последний раз отправляли медленный запрос
bool slowVoltageNext = false;         // false = следующий медленный запрос 0105, true = ATRV
bool useMultiPid = true;              // 010C0D даёт обороты и скорость за один ответ
uint8_t multiFails = 0;               // ответы без 41 0C — сигнал к откату на раздельные запросы
uint8_t pollLogBudget = 0;  // первые несколько ответов опроса пишем в журнал

enum CmdState { CMD_IDLE, CMD_WAIT };
CmdState cmdState = CMD_IDLE;
String cmdPending;
uint32_t cmdSentAt = 0;
uint32_t cmdTimeout = CMD_TIMEOUT_MS;
uint8_t cmdPidStep = 0;

// Таймштамп ставим в BLE-задаче. Для ответа OBD берём середину интервала
// запрос→ответ: значение EFI относится к моменту обработки команды, а не к моменту,
// когда парсер дочитал последний байт. Для служебных снимков tx/rx остаются нулями.
void pushData(const DashData& d, bool fromCmd = false) {
  DashData snap = d;
  const uint32_t now = millis();
  if (fromCmd && cmdSentAt != 0) {
    snap.t_tx_ms = cmdSentAt;
    snap.t_rx_ms = now;
    // Безопасная середина даже при переполнении millis(): разница < 2^31.
    snap.t_ms = snap.t_tx_ms + (uint32_t)(now - snap.t_tx_ms) / 2;
  } else {
    snap.t_tx_ms = 0;
    snap.t_rx_ms = 0;
    snap.t_ms = now;
  }
  ui_set_data(snap);
}

// Пауза между запросами нужна только адаптерам, которые выдают '>' раньше, чем
// готовы принять следующую команду. Такой ответ приходит по таймауту или без
// нужного PID — тогда быстро уходим на большую задержку. Длинная серия чистых
// ответов, наоборот, позволяет снова её уменьшить.
void notePollResult(bool ok) {
  const uint32_t prev = pollGap;
  if (ok) {
    if (pollGap > 0 && ++pollGoodStreak >= POLL_GOOD_TO_RELAX) {
      pollGoodStreak = 0;
      pollGap /= 2;
    }
  } else {
    pollGoodStreak = 0;
    pollGap = min(max(pollGap * 2, (uint32_t)POLL_GAP_STEP_MS), (uint32_t)POLL_GAP_MAX_MS);
  }
  if (pollGap != prev) storage_log("POLL gap=%u", pollGap);
}

// Ответы ELM — куски обычного ASCII с завершающим приглашением '>'. Добавляем их
// в rxBuf, а конечный автомат команд решает, когда ответ считается полным.
void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  portENTER_CRITICAL(&rxMux);
  for (size_t i = 0; i < len; i++) rxBuf += (char)data[i];
  portEXIT_CRITICAL(&rxMux);
}

// Только эвристика: она определяет, какое устройство подсвечивать на экране ELM
// и кого выбрать, если конкретный адрес не был задан явно.
bool isElmName(const std::string& name) {
  String n = String(name.c_str());
  n.toUpperCase();
  return n.indexOf("OBD") >= 0 || n.indexOf("ELM") >= 0 || n.indexOf("V-LINK") >= 0 ||
         n.indexOf("VLINK") >= 0 || n.indexOf("ICAR") >= 0 || n.indexOf("VEEPEAK") >= 0 ||
         n.indexOf("OBDLINK") >= 0 || n.indexOf("VGATE") >= 0 || n.indexOf("KONNWEI") >= 0 ||
         n.indexOf("CARISTA") >= 0 || n.indexOf("MEATPI") >= 0;
}

// Многие BLE-адаптеры ELM327 передают один из этих UUID UART; проверка ловит
// модули с пустым или безликим широковещательным именем.
bool looksLikeElm(const NimBLEAdvertisedDevice* d) {
  if (d->haveName() && isElmName(d->getName())) return true;
  if (d->haveServiceUUID()) {
    for (int i = 0; i < d->getServiceUUIDCount(); i++) {
      String us = d->getServiceUUID(i).toString().c_str();
      us.toUpperCase();
      if (us.indexOf("FFF0") >= 0 || us.indexOf("FFE0") >= 0 || us.indexOf("18F0") >= 0 ||
          us.indexOf("6E400001") >= 0) return true;
    }
  }
  return false;
}

// Колбэки сканирования выполняются внутри NimBLE. Копируем подходящее устройство
// и поднимаем doConnect; само подключение позже делает рабочая задача.
class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    String addr = d->getAddress().toString().c_str();
    std::string name = d->haveName() ? d->getName() : "";
    bool compatible = looksLikeElm(d);
    recordFound(d, compatible);

    bool match = false;
    if (targetAddr.length() > 0 && addr.equalsIgnoreCase(targetAddr)) match = true;
    if (!match && targetAddr.length() == 0 && compatible) match = true;
    if (match) {
      SD_LOG("[ELM] match %s %s\n", addr.c_str(), name.c_str());
      storage_log("MATCH addr=%s name=%s rssi=%d target=%s", addr.c_str(),
                  name.c_str(), d->getRSSI(),
                  targetAddr.length() ? "saved" : "any");
      NimBLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new NimBLEAdvertisedDevice(*d);
      pendingName = name.c_str();
      pendingRssi = d->getRSSI();
      doConnect = true;
    }
  }

  void onScanEnd(const NimBLEScanResults&, int) override {
    if (!connected && !connecting && !doConnect) {
      storage_log("SCAN end: no match");
      scheduleRetry(false, false);
    }
  }
};

// Колбэк отключения должен сразу сбросить кэшированные характеристики и сообщить
// UI; иначе после перезагрузки устройства были бы использованы устаревшие дескрипторы.
class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override {
    SD_LOGln("[ELM] connected");
    storage_log("GATT onConnect");
  }
  void onDisconnect(NimBLEClient*, int reason) override {
    SD_LOGln("[ELM] disconnected");
    storage_log("GATT onDisconnect reason=%d", reason);
    // Потеря во время поездки — норма (отключилось питание адаптера): это не
    // «не тот MAC», а значит не считаем неудачей и не уходим в scan any.
    const bool wasConnected = connected;
    chrWrite = nullptr;
    chrNotify = nullptr;
    connected = false;
    connecting = false;
    setStatus(ElmStatus::Idle);
    cmdState = CMD_IDLE;
    dash.connected = false;
    pushData(dash);
    if (wasConnected) {
      targetFailCount = 0;
      steadyUntil = millis() + STEADY_MS;
      scheduleRetry(true, true);  // сразу тот же MAC, без сканирования
    } else {
      noteTargetFailure();
      scheduleRetry(false, false);
    }
  }
};

ClientCB clientCB;

// Короткий UUID без префикса 0x — удобно сравнивать и писать в журнал.
String charUuid(NimBLERemoteCharacteristic* ch) {
  String u = ch->getUUID().toString().c_str();
  u.toUpperCase();
  u.replace("0X", "");
  return u;
}

bool canWriteChar(NimBLERemoteCharacteristic* ch) {
  return ch && (ch->canWrite() || ch->canWriteNoResponse());
}

bool canNotifyChar(NimBLERemoteCharacteristic* ch) {
  return ch && (ch->canNotify() || ch->canIndicate());
}

bool useChars(NimBLERemoteCharacteristic* w, NimBLERemoteCharacteristic* n) {
  if (!canWriteChar(w) || !canNotifyChar(n)) return false;
  chrWrite = w;
  chrNotify = n;
  return true;
}

// Чем больше баллов, тем вероятнее, что это UART-характеристика ELM.
// Стандартные GAP/Device Info (2Axx) не подходят: в логе уже было
// write=0x2a00 (Device Name), из-за чего AT-команды не доходили до адаптера.
int uartScore(const String& u, bool write) {
  // Vgate vLinker / iCar Pro: сервис 18F0, notify=2AF0, write=2AF1.
  // Это единственные 2Axx, которые могут быть UART; 2A00 (Device Name)
  // по-прежнему исключён, из-за него и появился этот запрет.
  if (u == "2AF0" || u == "2AF1") {
    if (write) return u == "2AF1" ? 95 : 70;
    return u == "2AF0" ? 95 : 70;
  }
  if (u.startsWith("2A")) return -1000;
  if (write) {
    if (u.indexOf("FFF1") >= 0 || u.indexOf("FFE1") >= 0) return 100;
    if (u.indexOf("6E400002") >= 0) return 90;
    if (u.indexOf("FFF2") >= 0 || u.indexOf("FFE2") >= 0) return 80;
  } else {
    if (u.indexOf("FFF2") >= 0 || u.indexOf("FFE1") >= 0) return 100;
    if (u.indexOf("6E400003") >= 0) return 90;
    if (u.indexOf("FFF1") >= 0 || u.indexOf("FFE2") >= 0) return 80;
  }
  if (u.startsWith("FFF") || u.startsWith("FFE")) return 40;
  return 0;
}

// Ищем характеристики с записью и уведомлениями. Важный момент из проекта
// obd-ble-serial: у OBDLink CX и множества клонов FFF1 = Notify, FFF2 = Write,
// то есть направление противоположно «очевидному». Сначала делаем полный
// discovery GATT, затем пробуем именно этот профиль, потом остальные.
bool pickChars(NimBLEClient* c) {
  chrWrite = nullptr;
  chrNotify = nullptr;

  struct Cand {
    NimBLERemoteCharacteristic* ch;
    String uuid;
    String svc;
    bool write;
    bool notify;
  };
  std::vector<Cand> cands;
  // Полный discovery обязателен до getService(): часть адаптеров не отдаёт
  // сервисы по одиночному запросу сразу после connection event. Заодно пишем
  // таблицу GATT в журнал — пригодится для диагностики.
  for (auto* svc : c->getServices(true)) {
    String su = svc->getUUID().toString().c_str();
    su.toUpperCase();
    su.replace("0X", "");
    for (auto* ch : svc->getCharacteristics(true)) {
      const String u = charUuid(ch);
      const bool w = canWriteChar(ch);
      const bool n = canNotifyChar(ch);
      storage_log("GATT %s/%s w=%d n=%d", su.c_str(), u.c_str(), w ? 1 : 0, n ? 1 : 0);
      if (w || n) cands.push_back({ch, u, su, w, n});
    }
  }

  // Профиль из ESP32-S3-Touch-OBD2: у Vgate vLinker / iCar Pro BLE сервис 18F0,
  // запись идёт в 2AF1, уведомления приходят от 2AF0. Пробуем его первым.
  if (!chrWrite || !chrNotify) {
    NimBLERemoteService* vg = c->getService("18F0");
    if (!vg) vg = c->getService("18f0");
    if (vg) {
      NimBLERemoteCharacteristic* a0 = vg->getCharacteristic("2AF0");
      if (!a0) a0 = vg->getCharacteristic("2af0");
      NimBLERemoteCharacteristic* a1 = vg->getCharacteristic("2AF1");
      if (!a1) a1 = vg->getCharacteristic("2af1");
      if (useChars(a1, a0)) storage_log("PROFILE 18F0 write=2AF1 notify=2AF0");
    }
  }

  if (!chrWrite || !chrNotify) {
    NimBLERemoteService* s = c->getService("FFF0");
    if (!s) s = c->getService("fff0");
    if (s) {
      NimBLERemoteCharacteristic* f1 = s->getCharacteristic("FFF1");
      if (!f1) f1 = s->getCharacteristic("fff1");
      NimBLERemoteCharacteristic* f2 = s->getCharacteristic("FFF2");
      if (!f2) f2 = s->getCharacteristic("fff2");

      // Проверенный профиль: FFF1 = уведомления, FFF2 = запись.
      if (!useChars(f2, f1)) {
        // У части клонов направление обратное: FFF1 = запись, FFF2 = уведомления.
        if (!useChars(f1, f2)) {
          // Одна характеристика на оба направления.
          if (!useChars(f1, f1)) useChars(f2, f2);
        }
      }
    }
  }

  if (!chrWrite || !chrNotify) {
    NimBLERemoteService* ffe = c->getService("FFE0");
    if (!ffe) ffe = c->getService("ffe0");
    if (ffe) {
      NimBLERemoteCharacteristic* e1 = ffe->getCharacteristic("FFE1");
      if (!e1) e1 = ffe->getCharacteristic("ffe1");
      NimBLERemoteCharacteristic* e2 = ffe->getCharacteristic("FFE2");
      if (!e2) e2 = ffe->getCharacteristic("ffe2");
      if (!useChars(e1, e1) && !useChars(e2, e2)) {
        if (!useChars(e1, e2)) useChars(e2, e1);
      }
    }
  }

  if (!chrWrite || !chrNotify) {
    // Nordic UART: в RX-характеристику пишем, TX-характеристика уведомляет.
    NimBLERemoteService* nus = c->getService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    if (nus) {
      NimBLERemoteCharacteristic* w = nus->getCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
      NimBLERemoteCharacteristic* n = nus->getCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
      useChars(w, n);
    }
  }

  if (!chrWrite || !chrNotify) {
    // Крайняя мера: выбираем лучшую пару среди всех подходящих характеристик.
    int best = -1;
    for (const auto& a : cands) {
      if (!a.write) continue;
      for (const auto& b : cands) {
        if (!b.notify) continue;
        int sc = uartScore(a.uuid, true) + uartScore(b.uuid, false);
        if (a.svc == b.svc) sc += 20;
        if (a.ch == b.ch) sc += 5;
        if (sc > best) {
          best = sc;
          chrWrite = a.ch;
          chrNotify = b.ch;
        }
      }
    }
  }

  bool subscribed = false;
  if (chrNotify) subscribed = chrNotify->subscribe(chrNotify->canNotify(), notifyCB);
  SD_LOG("[ELM] write=%s notify=%s\n", chrWrite ? chrWrite->getUUID().toString().c_str() : "-",
                 chrNotify ? chrNotify->getUUID().toString().c_str() : "-");
  storage_log("CHARS write=%s notify=%s sub=%d", chrWrite ? charUuid(chrWrite).c_str() : "-",
              chrNotify ? charUuid(chrNotify).c_str() : "-", subscribed ? 1 : 0);
  // Без канала уведомлений адаптер не вернёт ни одного ответа на AT-команду,
  // поэтому такой «половинчатый» профиль не считаем пригодным.
  return chrWrite != nullptr && chrNotify != nullptr && subscribed;
}

// Неблокирующий запрос AT/OBD: очищаем RX-буфер, пишем "CMD\r", запускаем таймаут.
// Одновременно может быть только одна команда (cmdState == CMD_IDLE).
bool sendCmdAsync(const String& cmd, uint32_t timeoutMs = CMD_TIMEOUT_MS) {
  if (!client || !client->isConnected() || !chrWrite || cmdState != CMD_IDLE) return false;
  portENTER_CRITICAL(&rxMux);
  rxBuf = "";
  rxBuf.reserve(128);  // ответы короткие: заранее выделяем место, без реаллокаций на каждый байт
  portEXIT_CRITICAL(&rxMux);
  String out = cmd + "\r";
  // WRITE_REQ можно запрашивать только у характеристики с флагом WRITE.
  // У клонов с FFF1 = WRITE_NO_RSP старый код (!canWrite()) запрашивал
  // неподдерживаемый WRITE_REQ и все AT-команды падали.
  bool response = chrWrite->canWrite();
  bool ok = chrWrite->writeValue((uint8_t*)out.c_str(), out.length(), response);
  if (!ok) return false;
  cmdPending = cmd;
  cmdSentAt = millis();
  cmdTimeout = timeoutMs;
  cmdState = CMD_WAIT;
  return true;
}

// Ответ готов, когда пришло приглашение ELM '>' или истёк срок ожидания.
// false означает «вызови позже», а не ошибку.
bool readCmdResult(String& resp) {
  if (cmdState != CMD_WAIT) return false;
  String tmp;
  bool ready = false;
  portENTER_CRITICAL(&rxMux);
  if (rxBuf.indexOf('>') >= 0 || (millis() - cmdSentAt) > cmdTimeout) {
    tmp = rxBuf;
    ready = true;
  }
  portEXIT_CRITICAL(&rxMux);
  if (!ready) return false;
  resp = tmp;
  resp.replace('\r', '\n');
  cmdState = CMD_IDLE;
  return true;
}

int hexByte(const String& s, int idx) {
  if (idx + 1 >= (int)s.length()) return -1;
  char buf[3] = {s[idx], s[idx + 1], 0};
  char* end = nullptr;
  long v = strtol(buf, &end, 16);
  if (!end || end == buf) return -1;
  return (int)v;
}

// В ответе OBD могут быть пробелы, CRLF и эхо команды. Оставляем только hex-цифры;
// для одного запроса с несколькими PID строку уплотняем один раз и ищем в ней
// каждый pattern (0x40 | service, pid) по отдельности.
void compactHex(const String& resp, String& compact) {
  compact = "";
  compact.reserve(resp.length());
  for (size_t i = 0; i < resp.length(); i++) {
    char c = resp[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
      compact += (char)toupper(c);
  }
}

bool findPid(const String& compact, uint8_t service, uint8_t pid, std::vector<int>& bytes) {
  bytes.clear();
  char pat[6];  // 4 hex-цифры + '\0'; 6 убирает предупреждение format-truncation
  snprintf(pat, sizeof(pat), "%02X%02X", service + 0x40, pid);
  int pos = compact.indexOf(pat);
  if (pos < 0) return false;
  pos += 4;
  while (pos + 1 < (int)compact.length() && bytes.size() < 8) {
    int b = hexByte(compact, pos);
    if (b < 0) break;
    bytes.push_back(b);
    pos += 2;
  }
  return !bytes.empty();
}

bool parsePid(const String& resp, uint8_t service, uint8_t pid, std::vector<int>& bytes) {
  String compact;
  compactHex(resp, compact);
  return findPid(compact, service, pid, bytes);
}

// Разбор ответа с двумя быстрыми PID. В пакетном ответе нет разделителей, поэтому
// идём последовательно: после заголовка 41 <PID> пропускаем известное число байт
// этого PID. Иначе байты оборотов, случайно равные 41 0D (~4187 об/мин),
// принимались бы за заголовок скорости.
struct FastPids {
  bool hasRpm = false;
  bool hasSpeed = false;
  int rpmRaw = 0;
  int speed = 0;
};

FastPids parseFastPids(const String& resp) {
  String compact;
  compactHex(resp, compact);
  FastPids out;
  int pos = 0;
  while (pos + 3 < (int)compact.length()) {
    const int h = compact.indexOf("41", pos);
    if (h < 0) break;
    const int pid = hexByte(compact, h + 2);
    int nbytes = 0;
    if (pid == 0x0C) nbytes = 2;
    else if (pid == 0x0D) nbytes = 1;
    if (nbytes == 0 || h + 4 + 2 * nbytes > (int)compact.length()) {
      pos = h + 2;  // не наш PID (например, часть CAN-адреса) — ищем дальше
      continue;
    }
    const int b0 = hexByte(compact, h + 4);
    if (pid == 0x0C) {
      const int b1 = hexByte(compact, h + 6);
      if (b0 >= 0 && b1 >= 0) {
        out.rpmRaw = b0 * 256 + b1;
        out.hasRpm = true;
      }
    } else if (b0 >= 0) {
      out.speed = b0;
      out.hasSpeed = true;
    }
    pos = h + 4 + 2 * nbytes;
  }
  return out;
}

// Конечный автомат инициализации. Избегает блокирующего ожидания после GATT-подключения:
// за такт отправляется следующая команда или забирается предыдущий ответ.
enum InitState { INIT_IDLE, INIT_SEND, INIT_WAIT, INIT_DONE, INIT_FAIL };
volatile int initState = INIT_IDLE;
enum InitPhase { INIT_AT, INIT_PROTO, INIT_PID };
InitPhase initPhase = INIT_AT;
int initIdx = 0;

// ATZ — сброс; отключаем эхо, переводы строк, пробелы и заголовки.
const char* initCmds[] = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0"};
constexpr int INIT_N = sizeof(initCmds) / sizeof(initCmds[0]);

// По данным телефонного приложения мотоцикл работает по ISO 14230-4 KWP.
// ATSP0 (авто) у дешёвых клонов ELM327 может искать протокол дольше, чем
// ждёт прошивка, поэтому сначала пробуем явные KWP-варианты, затем авто.
const char* protoCmds[] = {"ATSP5", "ATSP4", "ATSP0"};  // fast init / slow init / auto
constexpr uint32_t protoPidTimeout[] = {6000, 6000, 8000};
constexpr int PROTO_N = 3;
uint8_t protoIdx = 0;

// Человекочитаемое название протокола для экрана подключённого ELM.
const char* protoLabel(int idx) {
  if (idx == 0) return "KWP fast (ATSP5)";
  if (idx == 1) return "KWP slow (ATSP4)";
  return "Auto (ATSP0)";
}

void startInit() {
  initState = INIT_SEND;
  initPhase = INIT_AT;
  initIdx = 0;
  protoIdx = 0;
  pollLogBudget = 0;
  pollGap = POLL_GAP_MS;
  pollGoodStreak = 0;
  lastSlowPoll = millis();
  slowVoltageNext = false;
  useMultiPid = true;
  multiFails = 0;
  cmdState = CMD_IDLE;
  setStatus(ElmStatus::Initializing);
  storage_log("INIT start addr=%s", pendingAddr.c_str());
}

bool respHasError(const String& s) {
  String u = s;
  u.toUpperCase();
  return u.indexOf("ERROR") >= 0 || u.indexOf("STOPPED") >= 0 || u.indexOf("NO DATA") >= 0;
}

// 0100 считается удачным только если пришел ответ сервиса 41 — иначе ELM
// не нашёл ECU на выбранном протоколе, и нужно попробовать следующий.
bool respHasPid(const String& s) { return s.indexOf("41") >= 0 && !respHasError(s); }

// Продвинуть автомат AT-инициализации на один шаг. Ошибка на любом протоколе
// не обрывает поиск сразу: перебираются ATSP5 -> ATSP4 -> ATSP0.
void handleInit() {
  if (initState != INIT_SEND && initState != INIT_WAIT) return;

  if (initState == INIT_SEND) {
    const char* cmd = nullptr;
    uint32_t tmo = CMD_TIMEOUT_MS;
    switch (initPhase) {
      case INIT_AT:
        cmd = initCmds[initIdx];
        tmo = (initIdx == 0) ? 2500 : 1200;  // ATZ отвечает дольше
        break;
      case INIT_PROTO:
        cmd = protoCmds[protoIdx];
        tmo = 1500;
        break;
      case INIT_PID:
        cmd = "0100";
        tmo = protoPidTimeout[protoIdx];
        break;
    }
    if (sendCmdAsync(cmd, tmo)) initState = INIT_WAIT;
    else initState = INIT_FAIL;
    return;
  }

  String resp;
  if (!readCmdResult(resp)) return;
  const char* sent = initPhase == INIT_AT ? initCmds[initIdx]
                     : initPhase == INIT_PROTO ? protoCmds[protoIdx]
                                              : "0100";
  (void)sent;  // при выключенных логах аргумент SD_LOG не используется
  SD_LOG("[ELM] %s -> %s\n", sent, resp.c_str());
  storage_log("AT %s -> %s", sent, resp.c_str());

  if (initPhase == INIT_AT) {
    if (respHasError(resp)) {
      initState = INIT_FAIL;
      return;
    }
    if (++initIdx >= INIT_N) initPhase = INIT_PROTO;
    initState = INIT_SEND;
    return;
  }

  if (initPhase == INIT_PROTO) {
    if (respHasError(resp)) {
      storage_log("PROTO %s rejected", protoCmds[protoIdx]);
      if (++protoIdx >= PROTO_N) {
        initState = INIT_FAIL;
        return;
      }
    } else {
      initPhase = INIT_PID;
    }
    initState = INIT_SEND;
    return;
  }

  if (respHasPid(resp)) {
    initState = INIT_DONE;
    connected = true;
    targetFailCount = 0;
    directConnectNext = true;  // следующая потеря связи: сразу к этому MAC
    pollLogBudget = 8;
    // Первый после инициализации запрос — быстрый 010C0D; медленные подождут 1 с.
    lastSlowPoll = millis();
    pollGap = POLL_GAP_MS;
    pollGoodStreak = 0;
    // Сохраняем только подтверждённый адаптер и тип его адреса — он нужен
    // для прямого reconnect без сканирования после перезагрузки.
    storage_save_last_elm(pendingAddr);
    storage_save_last_elm_type(pendingAddrType);
    // Данные для экрана active-соединения: протокол выбран после 0100, имя и
    // адрес уже запомнены на этапе подключения.
    String nameToSave;
    portENTER_CRITICAL(&foundMux);
    if (!linked.address[0]) {
      snprintf(linked.address, sizeof(linked.address), "%s", pendingAddr.c_str());
    }
    snprintf(linked.proto, sizeof(linked.proto), "%s", protoLabel(protoIdx));
    if (!linked.name[0]) snprintf(linked.name, sizeof(linked.name), "<unknown>");
    linked.sinceMs = millis();
    nameToSave = linked.name;
    portEXIT_CRITICAL(&foundMux);
    if (nameToSave.length() && nameToSave != "<unknown>") storage_save_last_elm_name(nameToSave);
    setStatus(ElmStatus::Connected);
    dash.connected = true;
    pushData(dash);
    SD_LOG("[ELM] init done, proto %s\n", protoCmds[protoIdx]);
    storage_log("INIT done proto=%s addr=%s", protoCmds[protoIdx], pendingAddr.c_str());
    return;
  }

  storage_log("PROTO %s no PID in 0100 -> next", protoCmds[protoIdx]);
  if (++protoIdx >= PROTO_N) {
    initState = INIT_FAIL;
    return;
  }
  initPhase = INIT_PROTO;
  initState = INIT_SEND;
}

void finishConnect() {
  if (!client) return;
  if (!pickChars(client)) {
    SD_LOGln("[ELM] no usable chars");
    storage_log("CHARS fail: no write/notify pair");
    client->disconnect();
    connecting = false;
    setStatus(ElmStatus::Idle);
    noteTargetFailure();
    scheduleRetry(false, false);
    return;
  }
  connecting = false;
  // Адрес сохраняется только после подтверждённого 0100 (см. handleInit).
  startInit();
}

// Общий путь асинхронного подключения: и по найденному advertisement, и напрямую
// к сохранённому MAC. connect(..., true) включает exchangeMTU и не блокирует UI.
bool startConnect(const NimBLEAddress& addr, const String& addrText, uint8_t type, const char* how) {
  connecting = true;
  setStatus(ElmStatus::Connecting);
  initState = INIT_IDLE;
  cmdState = CMD_IDLE;

  pendingAddr = addrText;
  pendingAddrType = type;
  storage_log("CONNECT %s addr=%s type=%d", how, pendingAddr.c_str(), (int)type);

  client = NimBLEDevice::getClientByPeerAddress(addr);
  if (!client) client = NimBLEDevice::getDisconnectedClient();
  if (!client) {
    if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS) {
      connecting = false;
      setStatus(ElmStatus::Idle);
      storage_log("CONNECT no free client");
      scheduleRetry(false, false);
      return false;
    }
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCB, false);
    // 24..40 = 30..50 мс, supervision timeout 200 = 2 с: надёжнее для клонов,
    // чем прежние 15 мс / 510 мс.
    client->setConnectionParams(24, 40, 0, 200);
    // Внимание: аргумент — МИЛЛИСЕКУНДЫ. Прежнее значение 3 означало 3 мс,
    // из-за чего попытка подключения обрывалась почти сразу.
    client->setConnectTimeout(5000);
  }

  if (!client->isConnected()) {
    // Асинхронное подключение возвращается сразу; блокирующее connect() могло
    // остановить главный цикл на секунды и заморозить UI.
    if (!client->connect(addr, true, true, true)) {
      SD_LOGln("[ELM] connect failed");
      storage_log("CONNECT rejected by NimBLE addr=%s", pendingAddr.c_str());
      connecting = false;
      setStatus(ElmStatus::Idle);
      noteTargetFailure();
      scheduleRetry(false, false);
      return false;
    }
    connectStartedAt = millis();
    return true;  // завершение обрабатывается в elm_loop()
  }

  finishConnect();
  return true;
}

bool connectToAdv() {
  if (!advDevice) return false;
  NimBLEAddress addr = advDevice->getAddress();
  String addrText = String(addr.toString().c_str());
  String name = pendingName;
  if (!name.length() && advDevice->haveName()) name = advDevice->getName().c_str();
  rememberLink(addrText, addr.getType(), true, name, pendingRssi);
  return startConnect(addr, addrText, addr.getType(), "adv");
}

// Прямое подключение без сканирования: адрес и тип адреса сохранены после
// предыдущего успешного 0100. Если тип неизвестен (старая запись), пробуем
// public; неудача всё равно приведёт к сканированию, где тип станет известен.
bool connectToSavedAddr() {
  if (targetAddr.length() == 0) return false;
  NimBLEAddress addr(targetAddr.c_str(), targetType);
  // Advertisement в этой сессии не было — имя берём из NVS, RSSI неизвестен.
  rememberLink(targetAddr, targetType, targetTypeKnown, storage_get_last_elm_name(), -127);
  return startConnect(addr, targetAddr, targetType, "direct");
}

// ATRV возвращает что-то вроде "12.6V\r>". Берём первое десятичное число и
// отбрасываем неправдоподобные значения (глубокий разряд / всплески зарядки).
bool parseVoltage(const String& resp, float& volt) {
  String r = resp;
  r.replace("V", ""); r.replace("v", ""); r.replace(" ", "");
  char buf[24] = {0};
  int j = 0;
  for (size_t i = 0; i < r.length() && j < 23; i++) {
    char c = r[i];
    if ((c >= '0' && c <= '9') || c == '.') buf[j++] = c;
    else if (j > 0 && buf[j - 1] != '.') break;
  }
  const float v = atof(buf);
  if (v <= 5.0f || v >= 20.0f) return false;
  volt = v;
  return true;
}

// Круговой опрос OBD. Быстрая пара (обороты + скорость) запрашивается одним
// запросом 010C0D и не ждёт медленных параметров. 0105 и ATRV чередуются раз в
// SLOW_POLL_MS, то есть каждый параметр обновляется раз в 2 с. Раньше все четыре
// PID шли по очереди с паузой 120 мс, поэтому обороты и скорость менялись раз в
// 1–3 с. Если адаптер не понимает несколько PID в одном запросе, после
// MULTI_FAIL_LIMIT неудач включается старый round-robin.
void handlePoll() {
  if (!connected || initState != INIT_DONE) return;
  static const char* legacyCmds[] = {"010C", "010D", "0105", "ATRV"};
  uint32_t now = millis();

  // Режим замера. Два быстрых PID одним запросом; температура и напряжение
  // на это время не опрашиваются, чтобы не терять сэмплы разгона.
  if (raceMode) {
    if (cmdState == CMD_WAIT) {
      String resp;
      if (!readCmdResult(resp)) return;
      const bool fresh = resp.indexOf('>') >= 0;
      const FastPids p = parseFastPids(resp);
      notePollResult(fresh && p.hasRpm);
      if (p.hasRpm) dash.rpm = p.rpmRaw / 4;
      if (p.hasSpeed) dash.speed_kmh = p.speed;
      dash.connected = true;
      // При таймауте '>' нет — это не свежий ответ, середину интервала не используем.
      pushData(dash, fresh);
      lastPoll = now;
      return;
    }
    if (now - lastPoll < pollGap) return;
    if (!sendCmdAsync("010C0D", 900)) lastPoll = now;
    return;
  }

  if (cmdState == CMD_WAIT) {
    String resp;
    if (!readCmdResult(resp)) return;
    const bool fresh = resp.indexOf('>') >= 0;
    // Признак неготовности адаптера — таймаут или отсутствие нужного PID.
    // Для медленных запросов проверяем только таймаут: NO DATA у 0105 нормальна.
    bool good = fresh;
    if (pollLogBudget > 0) {
      storage_log("POLL %s -> %s", cmdPending.c_str(), resp.c_str());
      pollLogBudget--;
    }

    std::vector<int> bytes;
    if (useMultiPid) {
      // Быстрая пара: температура и напряжение сюда уже не попадают.
      if (cmdPending == "010C0D") {
        const FastPids p = parseFastPids(resp);
        if (p.hasRpm) dash.rpm = p.rpmRaw / 4;
        if (p.hasSpeed) dash.speed_kmh = p.speed;
        notePollResult(good && p.hasRpm);
        // Откат на раздельные PID — только когда пауза уже максимальна: иначе
        // missing 410C может быть просто признаком слишком короткой паузы.
        if (fresh && !p.hasRpm && pollGap >= POLL_GAP_MAX_MS) {
          if (++multiFails >= MULTI_FAIL_LIMIT) {
            useMultiPid = false;
            cmdPidStep = 0;
            storage_log("POLL multi-PID unsupported -> legacy 010C/010D/0105/ATRV");
          }
        } else if (p.hasRpm) {
          multiFails = 0;
        }
      } else if (cmdPending == "0105") {
        if (parsePid(resp, 0x01, 0x05, bytes) && bytes.size() >= 1) dash.coolant_c = bytes[0] - 40;
      } else if (cmdPending == "ATRV") {
        float v;
        if (parseVoltage(resp, v)) dash.volt = v;
      }
    } else {
      // Резервный путь для адаптеров без поддержки нескольких PID.
      switch (cmdPidStep % 4) {
        case 0:
          if (parsePid(resp, 0x01, 0x0C, bytes) && bytes.size() >= 2)
            dash.rpm = (bytes[0] * 256 + bytes[1]) / 4;
          break;
        case 1:
          if (parsePid(resp, 0x01, 0x0D, bytes) && bytes.size() >= 1) dash.speed_kmh = bytes[0];
          break;
        case 2:
          if (parsePid(resp, 0x01, 0x05, bytes) && bytes.size() >= 1) dash.coolant_c = bytes[0] - 40;
          break;
        case 3: {
          float v;
          if (parseVoltage(resp, v)) dash.volt = v;
          break;
        }
      }
      cmdPidStep++;
    }
    if (cmdPending != "010C0D") notePollResult(good);

    dash.connected = true;
    pushData(dash, fresh);
    lastPoll = now;
    return;
  }

  if (now - lastPoll < pollGap) return;
  const char* cmd;
  if (useMultiPid) {
    // Медленные параметры не вытесняют быстрые: между ними хотя бы секунда.
    // Температура и напряжение чередуются, поэтому каждый приходит раз в 2 с.
    if (now - lastSlowPoll >= SLOW_POLL_MS) {
      cmd = slowVoltageNext ? "ATRV" : "0105";
      slowVoltageNext = !slowVoltageNext;
      lastSlowPoll = now;
    } else {
      cmd = "010C0D";
    }
  } else {
    cmd = legacyCmds[cmdPidStep % 4];
  }
  if (!sendCmdAsync(cmd, 900)) lastPoll = now;  // чтобы не было частого повтора
}

}  // namespace

void elmTask(void*);

void elm_init() {
  // Имя NimBLE видят другие телефоны; низкая мощность передатчика экономит ток
  // и уменьшает помехи другим приборам рядом.
  NimBLEDevice::init("SmartDash");
  NimBLEDevice::setPower(9);
  setStatus(ElmStatus::Idle);
  clearFound();
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);

  // Вся работа с BLE вынесена с ядра UI: вызовы сканирования/подключения могут
  // блокироваться на сотни миллисекунд и раньше замораживали тач и анимацию.
  xTaskCreatePinnedToCore(elmTask, "elm", 6144, nullptr, 1, nullptr, 0);
}

// Публичный запрос: отключиться от текущего устройства, очистить значения дашборда
// и попросить рабочую задачу начать новое 4-секундное сканирование.
// Здесь не вызывается ни один радио-API.
void elm_start_scan() {
  storage_log("SCAN start target=%s", targetAddr.length() ? targetAddr.c_str() : "any");
  // Сначала сбрасываем цель, чтобы колбэк onDisconnect не посчитал это
  // неудачей подключения к сохранённому MAC.
  targetAddr = "";
  targetFailCount = 0;
  doConnect = false;
  connected = false;
  connecting = false;
  initState = INIT_IDLE;
  cmdState = CMD_IDLE;
  dash = {};
  dash.connected = false;
  pushData(dash);
  needScanRestart = false;
  retryAt = 0;
  reconnectGap = PAUSE_MIN;
  steadyUntil = millis() + STEADY_MS;
  directConnectNext = false;
  requestDirectConnect = false;
  clearFound();
  pendingName = "";
  pendingRssi = -127;
  setStatus(ElmStatus::Scanning);
  if (client && client->isConnected()) client->disconnect();
  SD_LOGln("[ELM] scan start");
  requestScanStart = true;  // блокирующий start() выполняет BLE-задача
}

// Стартовый путь: сначала подключаемся к сохранённому MAC напрямую, без 4-секундного
// сканирования. При неудаче рабочая задача сама перейдёт к сканированию.
void elm_connect_saved(const String& addr) {
  targetAddr = addr;
  const int type = storage_get_last_elm_type();
  targetTypeKnown = type >= 0;
  targetType = targetTypeKnown ? (uint8_t)type : BLE_ADDR_PUBLIC;
  targetFailCount = 0;
  doConnect = false;
  connected = false;
  connecting = false;
  initState = INIT_IDLE;
  cmdState = CMD_IDLE;
  dash = {};
  pushData(dash);
  needScanRestart = false;
  retryAt = 0;
  reconnectGap = PAUSE_MIN;
  steadyUntil = millis() + STEADY_MS;
  requestScanStart = false;
  requestDirectConnect = false;
  directConnectNext = true;
  clearFound();
  setStatus(ElmStatus::Connecting);
  SD_LOG("[ELM] direct connect to %s\n", addr.c_str());
  storage_log("AUTO direct addr=%s type=%d known=%d", addr.c_str(), (int)targetType,
              targetTypeKnown ? 1 : 0);
  requestDirectConnect = true;  // BLE-задача создаст NimBLEAddress и подключится
}

// Пользовательский путь: тап по строке на экране ELM перезапускает поиск с этим
// MAC в качестве цели. В NVS адрес попадёт только после успешной инициализации.
void elm_connect(const String& addr) {
  targetAddr = addr;
  targetFailCount = 0;
  doConnect = false;
  needScanRestart = false;
  retryAt = 0;
  steadyUntil = millis() + STEADY_MS;
  requestDirectConnect = false;
  directConnectNext = false;
  setStatus(ElmStatus::Scanning);
  SD_LOG("[ELM] connect to %s\n", addr.c_str());
  storage_log("TAP target=%s", addr.c_str());
  requestScanStart = true;
}

// Такт рабочей задачи, вызываемый elmTask каждые 2 мс. Запросы обрабатываются
// по приоритету: сканирование -> подключение -> AT-инициализация -> опрос OBD -> пауза переподключения.
void elm_loop() {
  if (requestScanStart) {
    requestScanStart = false;
    // Этот вызов может блокироваться на сотни миллисекунд; он выполняется только
    // в BLE-задаче, поэтому цикл UI/тача его не дожидается.
    NimBLEDevice::getScan()->start(SCAN_TIME_MS, false);
  }

  if (doConnect) {
    doConnect = false;
    connectToAdv();
  }

  if (requestDirectConnect) {
    requestDirectConnect = false;
    if (targetAddr.length()) {
      directConnectNext = false;  // одна прямая попытка, затем сканирование
      connectToSavedAddr();
    }
  }

  if (connecting && client) {
    if (client->isConnected()) {
      SD_LOGln("[ELM] async connect done");
      storage_log("GATT connected addr=%s", pendingAddr.c_str());
      finishConnect();
    } else if (millis() - connectStartedAt > CONNECT_TIMEOUT_MS) {
      SD_LOGln("[ELM] async connect timeout");
      storage_log("CONNECT timeout addr=%s", pendingAddr.c_str());
      client->cancelConnect();
      connecting = false;
      setStatus(ElmStatus::Idle);
      noteTargetFailure();
      scheduleRetry(false, false);
    }
  }

  if (initState != INIT_IDLE && initState != INIT_DONE && initState != INIT_FAIL) {
    handleInit();
    if (initState == INIT_FAIL) {
      storage_log("INIT fail addr=%s", pendingAddr.c_str());
      if (client && client->isConnected()) client->disconnect();
      setStatus(ElmStatus::Idle);
      noteTargetFailure();
      scheduleRetry(false, false);
    }
  }

  if (connected) handlePoll();

  if (needScanRestart && !connected && !connecting && !doConnect && !requestDirectConnect) {
    const uint32_t now = millis();
    if ((int32_t)(now - retryAt) >= 0) {
      needScanRestart = false;
      if (directConnectNext && targetAddr.length()) {
        directConnectNext = false;
        requestDirectConnect = true;
      } else {
        setStatus(ElmStatus::Scanning);
        SD_LOGln("[ELM] restart scan");
        NimBLEDevice::getScan()->start(SCAN_TIME_MS, false);
      }
    }
  }
}

// Стека 6 КБ достаточно для колбэков NimBLE и разбора String. Приоритет 1 на core 0
// поддерживает автомат радио, не конкурируя с ядром UI.
void elmTask(void*) {
  for (;;) {
    elm_loop();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// «Подключено» означает и поднятое GATT-соединение, и завершённую AT-инициализацию; тогда начинается опрос OBD.
bool elm_is_connected() { return connected && initState == INIT_DONE; }

// Переход в режим замера и обратно. Сбрасываем автомат команд, чтобы ответ
// предыдущего запроса не был разобран уже как ответ 010C0D (и наоборот).
void elm_set_race_mode(bool enabled) {
  if (enabled == raceMode) return;
  raceMode = enabled;
  portENTER_CRITICAL(&rxMux);
  rxBuf = "";
  rxBuf.reserve(128);
  portEXIT_CRITICAL(&rxMux);
  cmdState = CMD_IDLE;
  cmdPidStep = 0;
  lastPoll = 0;
  storage_log("RACE %s", enabled ? "on" : "off");
}

ElmStatus elm_status() { return status; }

// Снимок для отрисовки. Копируем под foundMux, чтобы UI не обходил массив,
// который одновременно могут менять колбэки сканирования.
size_t elm_get_found(ElmScanEntry* out, size_t max) {
  if (!out || max == 0) return 0;
  portENTER_CRITICAL(&foundMux);
  size_t n = foundCount < max ? foundCount : max;
  for (size_t i = 0; i < n; i++) {
    memcpy(&out[i], &found[i], sizeof(ElmScanEntry));
  }
  portEXIT_CRITICAL(&foundMux);
  return n;
}

// Информация показывается только после завершённой AT-инициализации: во время
// поиска/подключения экран продолжает выводить список найденных устройств.
bool elm_get_connected(ElmConnectedInfo& out) {
  if (!connected || initState != INIT_DONE) return false;
  portENTER_CRITICAL(&foundMux);
  out = linked;
  portEXIT_CRITICAL(&foundMux);
  return true;
}
