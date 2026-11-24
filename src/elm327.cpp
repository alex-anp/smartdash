#include "elm327.h"

#include <NimBLEDevice.h>
#include <string.h>
#include <vector>

#include "config.h"
#include "storage.h"
#include "ui.h"

namespace {

constexpr uint32_t SCAN_TIME_MS = 4000;
constexpr uint32_t CMD_TIMEOUT_MS = 1200;
constexpr uint32_t POLL_GAP_MS = 120;
constexpr uint32_t RECONNECT_GAP_MS = 3000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 4000;
constexpr size_t MAX_FOUND = 8;

NimBLEAdvertisedDevice* advDevice = nullptr;
NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* chrWrite = nullptr;
NimBLERemoteCharacteristic* chrNotify = nullptr;

bool doConnect = false;
bool connecting = false;
bool connected = false;
bool needScanRestart = false;
uint32_t lastScanRestart = 0;
uint32_t connectStartedAt = 0;
String pendingAddr;
ElmStatus status = ElmStatus::Idle;

ElmScanEntry found[MAX_FOUND];
size_t foundCount = 0;
portMUX_TYPE foundMux = portMUX_INITIALIZER_UNLOCKED;

void setStatus(ElmStatus s) { status = s; }

void clearFound() {
  portENTER_CRITICAL(&foundMux);
  foundCount = 0;
  portEXIT_CRITICAL(&foundMux);
}

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
      // Keep the strongest signals when the list is full.
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
  portEXIT_CRITICAL(&foundMux);
}

String targetAddr;
String rxBuf;

DashData dash;
uint32_t lastPoll = 0;

enum CmdState { CMD_IDLE, CMD_WAIT };
CmdState cmdState = CMD_IDLE;
String cmdPending;
uint32_t cmdSentAt = 0;
uint32_t cmdTimeout = CMD_TIMEOUT_MS;
uint8_t cmdPidStep = 0;

void pushData(const DashData& d) { ui_set_data(d); }

void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len; i++) rxBuf += (char)data[i];
}

bool isElmName(const std::string& name) {
  String n = String(name.c_str());
  n.toUpperCase();
  return n.indexOf("OBD") >= 0 || n.indexOf("ELM") >= 0 || n.indexOf("V-LINK") >= 0 ||
         n.indexOf("VEEPEAK") >= 0 || n.indexOf("OBDLINK") >= 0 || n.indexOf("VGATE") >= 0;
}

bool looksLikeElm(const NimBLEAdvertisedDevice* d) {
  if (d->haveName() && isElmName(d->getName())) return true;
  if (d->haveServiceUUID()) {
    for (int i = 0; i < d->getServiceUUIDCount(); i++) {
      String us = d->getServiceUUID(i).toString().c_str();
      us.toUpperCase();
      if (us.indexOf("FFF0") >= 0 || us.indexOf("FFE0") >= 0 || us.indexOf("6E400001") >= 0) return true;
    }
  }
  return false;
}

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
      Serial.printf("[ELM] match %s %s\n", addr.c_str(), name.c_str());
      NimBLEDevice::getScan()->stop();
      if (advDevice) delete advDevice;
      advDevice = new NimBLEAdvertisedDevice(*d);
      doConnect = true;
    }
  }

  void onScanEnd(const NimBLEScanResults&, int) override {
    if (!connected && !connecting && !doConnect) needScanRestart = true;
  }
};

class ClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) override { Serial.println("[ELM] connected"); }
  void onDisconnect(NimBLEClient*, int) override {
    Serial.println("[ELM] disconnected");
    chrWrite = nullptr;
    chrNotify = nullptr;
    connected = false;
    connecting = false;
    setStatus(ElmStatus::Idle);
    cmdState = CMD_IDLE;
    dash.connected = false;
    pushData(dash);
    needScanRestart = true;
    lastScanRestart = 0;
  }
};

ClientCB clientCB;

bool pickChars(NimBLEClient* c) {
  chrWrite = nullptr;
  chrNotify = nullptr;

  NimBLERemoteService* s = c->getService("FFF0");
  if (!s) s = c->getService("fff0");
  if (s) {
    NimBLERemoteCharacteristic* w = s->getCharacteristic("FFF1");
    if (!w) w = s->getCharacteristic("fff1");
    NimBLERemoteCharacteristic* n = s->getCharacteristic("FFF2");
    if (!n) n = s->getCharacteristic("fff2");
    if (w && (w->canWrite() || w->canWriteNoResponse())) chrWrite = w;
    if (n && (n->canNotify() || n->canIndicate())) chrNotify = n;
  }
  if (!chrWrite || !chrNotify) {
    NimBLERemoteService* nus = c->getService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    if (nus) {
      NimBLERemoteCharacteristic* w = nus->getCharacteristic("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
      NimBLERemoteCharacteristic* n = nus->getCharacteristic("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
      if (w) chrWrite = w;
      if (n) chrNotify = n;
    }
  }
  if (!chrWrite || !chrNotify) {
        for (auto* svc : c->getServices(true)) {
      for (auto* ch : svc->getCharacteristics(true)) {
        if (!chrNotify && (ch->canNotify() || ch->canIndicate())) chrNotify = ch;
        if (!chrWrite && (ch->canWrite() || ch->canWriteNoResponse())) chrWrite = ch;
      }
    }
  }

  if (chrNotify && (chrNotify->canNotify() || chrNotify->canIndicate())) {
    chrNotify->subscribe(chrNotify->canNotify(), notifyCB);
  }
  Serial.printf("[ELM] write=%s notify=%s\n", chrWrite ? chrWrite->getUUID().toString().c_str() : "-",
                chrNotify ? chrNotify->getUUID().toString().c_str() : "-");
  return chrWrite != nullptr;
}

bool sendCmdAsync(const String& cmd, uint32_t timeoutMs = CMD_TIMEOUT_MS) {
  if (!client || !client->isConnected() || !chrWrite || cmdState != CMD_IDLE) return false;
  rxBuf = "";
  String out = cmd + "\r";
  bool ok = chrWrite->writeValue((uint8_t*)out.c_str(), out.length(), !chrWrite->canWrite());
  if (!ok) return false;
  cmdPending = cmd;
  cmdSentAt = millis();
  cmdTimeout = timeoutMs;
  cmdState = CMD_WAIT;
  return true;
}

bool readCmdResult(String& resp) {
  if (cmdState != CMD_WAIT) return false;
  if (rxBuf.indexOf('>') >= 0 || (millis() - cmdSentAt) > cmdTimeout) {
    resp = rxBuf;
    resp.replace('\r', '\n');
    cmdState = CMD_IDLE;
    return true;
  }
  return false;
}

int hexByte(const String& s, int idx) {
  if (idx + 1 >= (int)s.length()) return -1;
  char buf[3] = {s[idx], s[idx + 1], 0};
  char* end = nullptr;
  long v = strtol(buf, &end, 16);
  if (!end || end == buf) return -1;
  return (int)v;
}

bool parsePid(const String& resp, uint8_t service, uint8_t pid, std::vector<int>& bytes) {
  bytes.clear();
  String compact;
  compact.reserve(resp.length());
  for (size_t i = 0; i < resp.length(); i++) {
    char c = resp[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
      compact += (char)toupper(c);
  }
  char pat[5];
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

enum InitState { INIT_IDLE, INIT_SEND, INIT_WAIT, INIT_DONE, INIT_FAIL };
InitState initState = INIT_IDLE;
int initIdx = 0;
const char* initCmds[] = {"ATZ", "ATE0", "ATL0", "ATS0", "ATH0", "ATSP0", "0100"};
constexpr int INIT_N = 7;

void startInit() {
  initState = INIT_SEND;
  initIdx = 0;
  cmdState = CMD_IDLE;
  setStatus(ElmStatus::Initializing);
}

void handleInit() {
  if (initState == INIT_IDLE || initState == INIT_DONE || initState == INIT_FAIL) return;
  if (initState == INIT_SEND) {
    uint32_t tmo = (strcmp(initCmds[initIdx], "ATZ") == 0 || strcmp(initCmds[initIdx], "0100") == 0) ? 2500 : 1200;
    if (sendCmdAsync(initCmds[initIdx], tmo)) initState = INIT_WAIT;
    else initState = INIT_FAIL;
    return;
  }
  if (initState == INIT_WAIT) {
    String resp;
    if (!readCmdResult(resp)) return;
    Serial.printf("[ELM] %s -> %s\n", initCmds[initIdx], resp.c_str());
    initIdx++;
    if (initIdx >= INIT_N) {
      initState = INIT_DONE;
      connected = true;
      setStatus(ElmStatus::Connected);
      dash.connected = true;
      pushData(dash);
      Serial.println("[ELM] init done");
    } else {
      initState = INIT_SEND;
    }
  }
}

void finishConnect() {
  if (!client) return;
  if (!pickChars(client)) {
    Serial.println("[ELM] no usable chars");
    client->disconnect();
    connecting = false;
    setStatus(ElmStatus::Idle);
    needScanRestart = true;
    return;
  }
  storage_save_last_elm(pendingAddr);
  connecting = false;
  startInit();
}

bool connectToAdv() {
  if (!advDevice) return false;
  connecting = true;
  setStatus(ElmStatus::Connecting);
  initState = INIT_IDLE;
  cmdState = CMD_IDLE;

  NimBLEAddress addr = advDevice->getAddress();
  pendingAddr = String(addr.toString().c_str());
  client = NimBLEDevice::getClientByPeerAddress(addr);
  if (!client) client = NimBLEDevice::getDisconnectedClient();
  if (!client) {
    if (NimBLEDevice::getCreatedClientCount() >= NIMBLE_MAX_CONNECTIONS) {
      connecting = false;
      setStatus(ElmStatus::Idle);
      return false;
    }
    client = NimBLEDevice::createClient();
    client->setClientCallbacks(&clientCB, false);
    client->setConnectionParams(12, 12, 0, 51);
    client->setConnectTimeout(3);
  }

  if (!client->isConnected()) {
    // Async connect returns immediately; blocking connect() could stall the
    // main loop for seconds and freeze the demo UI.
    if (!client->connect(addr, true, true, true)) {
      Serial.println("[ELM] connect failed");
      connecting = false;
      setStatus(ElmStatus::Idle);
      needScanRestart = true;
      return false;
    }
    connectStartedAt = millis();
    return true;  // completion handled in elm_loop()
  }

  finishConnect();
  return true;
}

void handlePoll() {
  if (!connected || initState != INIT_DONE) return;
  uint32_t now = millis();
  if (cmdState == CMD_WAIT) {
    String resp;
    if (!readCmdResult(resp)) return;
    std::vector<int> bytes;
    switch (cmdPidStep % 4) {
      case 0:
        if (parsePid(resp, 0x01, 0x0C, bytes) && bytes.size() >= 2) dash.rpm = ((bytes[0] * 256) + bytes[1]) / 4;
        break;
      case 1:
        if (parsePid(resp, 0x01, 0x0D, bytes) && bytes.size() >= 1) dash.speed_kmh = bytes[0];
        break;
      case 2:
        if (parsePid(resp, 0x01, 0x05, bytes) && bytes.size() >= 1) dash.coolant_c = bytes[0] - 40;
        break;
      case 3: {
        String r = resp;
        r.replace("V", ""); r.replace("v", ""); r.replace(" ", "");
        char buf[24] = {0}; int j = 0;
        for (size_t i = 0; i < r.length() && j < 23; i++) {
          char c = r[i];
          if ((c >= '0' && c <= '9') || c == '.') { buf[j++] = c; }
          else if (j > 0 && buf[j - 1] != '.') break;
        }
        float v = atof(buf);
        if (v > 5.0f && v < 20.0f) dash.volt = v;
        break;
      }
    }
    dash.connected = true;
    pushData(dash);
    cmdPidStep++;
    lastPoll = now;
    return;
  }

  if (now - lastPoll < POLL_GAP_MS) return;
  const char* cmds[] = {"010C", "010D", "0105", "ATRV"};
  uint32_t tmos[] = {900, 900, 900, 900};
  uint8_t s = cmdPidStep % 4;
  if (!sendCmdAsync(cmds[s], tmos[s])) {
    lastPoll = now; // avoid tight retry
  }
}

}  // namespace

void elm_init() {
  NimBLEDevice::init("SmartDash");
  NimBLEDevice::setPower(9);
  setStatus(ElmStatus::Idle);
  clearFound();
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);
  scan->setInterval(45);
  scan->setWindow(15);
}

void elm_start_scan() {
  if (client && client->isConnected()) client->disconnect();
  targetAddr = "";
  doConnect = false;
  connected = false;
  connecting = false;
  initState = INIT_IDLE;
  cmdState = CMD_IDLE;
  dash = {};
  dash.connected = false;
  pushData(dash);
  needScanRestart = false;
  clearFound();
  setStatus(ElmStatus::Scanning);
  Serial.println("[ELM] scan start");
  NimBLEDevice::getScan()->start(SCAN_TIME_MS, false);
}

void elm_connect_saved(const String& addr) {
  targetAddr = addr;
  doConnect = false;
  connected = false;
  connecting = false;
  initState = INIT_IDLE;
  cmdState = CMD_IDLE;
  dash = {};
  pushData(dash);
  needScanRestart = false;
  clearFound();
  setStatus(ElmStatus::Scanning);
  Serial.printf("[ELM] scan for %s\n", addr.c_str());
  NimBLEDevice::getScan()->start(SCAN_TIME_MS, false);
}

void elm_connect(const String& addr) {
  targetAddr = addr;
  storage_save_last_elm(addr);
  doConnect = false;
  needScanRestart = false;
  setStatus(ElmStatus::Scanning);
  Serial.printf("[ELM] connect to %s\n", addr.c_str());
  NimBLEDevice::getScan()->start(SCAN_TIME_MS, false);
}

void elm_loop() {
  if (doConnect) {
    doConnect = false;
    connectToAdv();
  }

  if (connecting && client) {
    if (client->isConnected()) {
      Serial.println("[ELM] async connect done");
      finishConnect();
    } else if (millis() - connectStartedAt > CONNECT_TIMEOUT_MS) {
      Serial.println("[ELM] async connect timeout");
      client->cancelConnect();
      connecting = false;
      setStatus(ElmStatus::Idle);
      needScanRestart = true;
    }
  }

  if (initState != INIT_IDLE && initState != INIT_DONE && initState != INIT_FAIL) {
    handleInit();
    if (initState == INIT_FAIL) {
      if (client && client->isConnected()) client->disconnect();
      setStatus(ElmStatus::Idle);
      needScanRestart = true;
    }
  }

  if (connected) handlePoll();

  if (needScanRestart && !connected && !connecting && !doConnect) {
    uint32_t now = millis();
    if (now - lastScanRestart > RECONNECT_GAP_MS) {
      lastScanRestart = now;
      needScanRestart = false;
      setStatus(ElmStatus::Scanning);
      Serial.println("[ELM] restart scan");
      NimBLEDevice::getScan()->start(SCAN_TIME_MS, false);
    }
  }
}

bool elm_is_connected() { return connected && initState == INIT_DONE; }

ElmStatus elm_status() { return status; }

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
