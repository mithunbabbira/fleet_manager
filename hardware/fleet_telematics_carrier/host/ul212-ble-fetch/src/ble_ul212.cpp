#include "ble_ul212.h"

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <esp_coexist.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <string.h>

static BLEUUID serviceUUID((uint16_t)0xFFE0);
static BLEUUID charFfe1((uint16_t)0xFFE1);
static BLEUUID charFfe2((uint16_t)0xFFE2);

/* Vendor app (UNI__9933E72): primary unlock CC9D…; alt unlock 7D5A… also present. */
static const uint8_t CMD_UNLOCK[] = {0x09, 0x06, 0x01, 0x07, 0xCC, 0x9D, 0xAC, 0x16};
static const uint8_t CMD_UNLOCK_ALT[] = {0x09, 0x06, 0x01, 0x07, 0x7D, 0x5A, 0x99, 0xD4};
static const uint8_t CMD_POLL[] = {0x09, 0x03, 0x00, 0xFD, 0x00, 0x1C, 0xD4, 0xBB};
static const size_t REPLY_LEN = 4 + 28 * 2 + 2;
/* Soft re-auth before tearing down GATT (reduces reconnect thrash in production). */
static const uint8_t SOFT_RECOVER_MAX = 2;

static BLEClient *pClient = nullptr;
static BLERemoteCharacteristic *pWriteChar = nullptr;
static BLERemoteCharacteristic *pNotifyChar = nullptr;
static volatile bool linkUp = false;

static char targetMac[18] = "";
static uint32_t pollIntervalMs = 1000;
static uint32_t silenceTimeoutMs = 10000;
static volatile bool configDirty = false;

static uint8_t rxBuf[128];
static size_t rxLen = 0;
static uint32_t rxLastMs = 0;
static uint32_t lastRxMs = 0;
static uint32_t lastConnectMs = 0;
static volatile bool pollPaused = false;

static Ul212Reading latest;
static BleStats stats;

static const size_t EVENT_MAX = 24;
static BleEvent events[EVENT_MAX];
static size_t eventHead = 0;
static size_t eventCount = 0;

static SemaphoreHandle_t stateMux;

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint16_t modbusCrc(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

static void logEvent(const char *fmt, ...) {
  BleEvent &e = events[eventHead];
  e.atS = millis() / 1000;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.what, sizeof(e.what), fmt, ap);
  va_end(ap);
  eventHead = (eventHead + 1) % EVENT_MAX;
  if (eventCount < EVENT_MAX) eventCount++;
  Serial.printf("[ble] %s\n", e.what);
}

static void closeUptimeWindow() {
  if (lastConnectMs) {
    stats.connectedMs += millis() - lastConnectMs;
    lastConnectMs = 0;
  }
}

static void decodeTelemetry(const uint8_t *e, size_t len) {
  if (len < 30) return;

  Ul212Reading r;
  r.valid = true;
  r.heightMm = be16(e + 8) / 10.0f;
  r.smoothMm = be16(e + 10) / 10.0f;
  r.temperatureC = (be16(e + 12) - 400) / 10.0f;
  r.signal = e[6];
  r.validSignals = e[7];
  r.tiltDeg = e[14];
  r.firmware = e[4];
  r.sonicSpeed5C = be16(e + 18);
  r.sonicSpeed55C = be16(e + 20);
  r.netAddress = be16(e + 22);
  r.baudRate = (uint32_t)be16(e + 26) * 100;
  r.protocol = be16(e + 28);
  r.stampMs = millis();
  r.count = latest.count + 1;

  uint32_t gap = r.stampMs - lastRxMs;
  if (lastRxMs && gap > stats.longestGapMs) stats.longestGapMs = gap;
  if (r.signal == 0 || r.validSignals == 0) stats.noEchoReadings++;

  if (xSemaphoreTake(stateMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    latest = r;
    xSemaphoreGive(stateMux);
  }
  lastRxMs = r.stampMs;

  Serial.printf("[UL212] %.1f mm (smooth %.1f) | signal %u | valid %u | tilt %u deg | %.1f C%s\n",
                r.heightMm, r.smoothMm, r.signal, r.validSignals, r.tiltDeg,
                r.temperatureC,
                (r.signal == 0 || r.validSignals == 0) ? "  [no echo]" : "");
}

static void handleFrame() {
  if (rxLen < REPLY_LEN) return;

  uint16_t crc = modbusCrc(rxBuf, REPLY_LEN - 2);
  uint16_t got = (uint16_t)(rxBuf[REPLY_LEN - 1] << 8 | rxBuf[REPLY_LEN - 2]);
  if (crc != got) {
    stats.crcErrors++;
    rxLen = 0;
    return;
  }
  decodeTelemetry(rxBuf, REPLY_LEN);
  rxLen = 0;
}

static void notifyCallback(BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) {
  uint32_t now = millis();
  if (now - rxLastMs > 500) rxLen = 0;
  rxLastMs = now;

  if (len >= 2 && data[0] == 0x09 && data[1] == 0x03) rxLen = 0;

  if (rxLen + len > sizeof(rxBuf)) rxLen = 0;
  memcpy(rxBuf + rxLen, data, len);
  rxLen += len;
  handleFrame();
}

class ClientCb : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {
    linkUp = true;
    logEvent("connected");
  }
  void onDisconnect(BLEClient *) override {
    bool was = lastConnectMs != 0;
    closeUptimeWindow();
    linkUp = false;
    if (was) {
      stats.disconnects++;
      logEvent("disconnected (#%lu)", (unsigned long)stats.disconnects);
    }
  }
};

static ClientCb clientCb;

static bool charWritable(BLERemoteCharacteristic *ch) {
  if (!ch) return false;
  return ch->canWrite() || ch->canWriteNoResponse();
}

static bool charNotifiable(BLERemoteCharacteristic *ch) {
  if (!ch) return false;
  return ch->canNotify() || ch->canIndicate();
}

static void sendCmd(const uint8_t *cmd, size_t len) {
  if (!pWriteChar) return;

  // Match vendor app: try write-without-response first, then with-response.
  if (pWriteChar->canWriteNoResponse()) {
    pWriteChar->writeValue((uint8_t *)cmd, len, false);
    return;
  }
  if (pWriteChar->canWrite()) {
    pWriteChar->writeValue((uint8_t *)cmd, len, true);
    return;
  }
  pWriteChar->writeValue((uint8_t *)cmd, len, false);
}

static bool enableNotify(BLERemoteCharacteristic *notifyCh) {
  if (!notifyCh || !charNotifiable(notifyCh)) return false;

  notifyCh->registerForNotify(notifyCallback);
  BLERemoteDescriptor *cccd = notifyCh->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (cccd) {
    uint8_t on[2] = {0x01, 0x00};
    cccd->writeValue(on, 2, true);
  }
  return true;
}

static void releaseClient() {
  closeUptimeWindow();
  pWriteChar = nullptr;
  pNotifyChar = nullptr;
  if (pClient) {
    if (pClient->isConnected()) pClient->disconnect();
    delay(100);
    delete pClient;
    pClient = nullptr;
  }
  linkUp = false;
}

static bool pickEndpoint(BLERemoteService *svc) {
  BLERemoteCharacteristic *ffe1 = svc->getCharacteristic(charFfe1);
  BLERemoteCharacteristic *ffe2 = svc->getCharacteristic(charFfe2);

  BLERemoteCharacteristic *notifyCh = nullptr;
  BLERemoteCharacteristic *writeCh = nullptr;

  if (ffe1 && ffe2) {
    // Vendor Be(): notify=FFE1; write=FFE2 if writable else FFE1.
    notifyCh = charNotifiable(ffe1) ? ffe1 : nullptr;
    writeCh = charWritable(ffe2) ? ffe2 : nullptr;
    if (!writeCh && charWritable(ffe1)) writeCh = ffe1;
    if (!notifyCh && charNotifiable(ffe2)) notifyCh = ffe2;
  } else {
    BLERemoteCharacteristic *only = ffe1 ? ffe1 : ffe2;
    if (!only) return false;
    notifyCh = charNotifiable(only) ? only : nullptr;
    writeCh = charWritable(only) ? only : nullptr;
  }

  if (!writeCh || !notifyCh) {
    Serial.println("[ble] missing writable/notify characteristics");
    return false;
  }

  pWriteChar = writeCh;
  pNotifyChar = notifyCh;
  Serial.printf("[ble] endpoint write=%s notify=%s same=%s\n",
                pWriteChar->getUUID().toString().c_str(),
                pNotifyChar->getUUID().toString().c_str(),
                pWriteChar == pNotifyChar ? "yes" : "no");
  return true;
}

static bool connectAndUnlock(const char *mac) {
  releaseClient();

  pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(&clientCb);

  stats.connectAttempts++;
  Serial.printf("[ble] connecting to %s ...\n", mac);
  if (!pClient->connect(BLEAddress(mac))) {
    stats.connectFailures++;
    logEvent("connect failed (#%lu)", (unsigned long)stats.connectFailures);
    releaseClient();
    return false;
  }

  delay(500);

  BLERemoteService *svc = pClient->getService(serviceUUID);
  if (!svc || !pickEndpoint(svc)) {
    releaseClient();
    return false;
  }

  delay(200);

  const bool sameChar = (pWriteChar == pNotifyChar);
  const bool unlockBeforeNotify =
      sameChar && pWriteChar->canWrite() && !pWriteChar->canWriteNoResponse();

  if (unlockBeforeNotify) {
    sendCmd(CMD_UNLOCK, sizeof(CMD_UNLOCK));
    delay(400);
    if (!enableNotify(pNotifyChar)) {
      releaseClient();
      return false;
    }
  } else {
    if (!enableNotify(pNotifyChar)) {
      releaseClient();
      return false;
    }
    delay(400);
    sendCmd(CMD_UNLOCK, sizeof(CMD_UNLOCK));
  }

  delay(300);
  rxLen = 0;
  lastRxMs = millis();
  lastConnectMs = lastRxMs;
  stats.reconnects++;
  sendCmd(CMD_POLL, sizeof(CMD_POLL));
  return true;
}

/** Re-unlock + poll without tearing down GATT. Returns false if link already dead. */
static bool softRecover() {
  if (!bleUl212Connected() || !pWriteChar) return false;
  logEvent("soft recover (unlock+poll)");
  sendCmd(CMD_UNLOCK, sizeof(CMD_UNLOCK));
  delay(250);
  /* If primary unlock never produced frames, try vendor alternate once. */
  if (!latest.valid) {
    sendCmd(CMD_UNLOCK_ALT, sizeof(CMD_UNLOCK_ALT));
    delay(250);
  }
  rxLen = 0;
  sendCmd(CMD_POLL, sizeof(CMD_POLL));
  return true;
}

void bleUl212Begin() {
  stateMux = xSemaphoreCreateMutex();
  esp_coex_preference_set(ESP_COEX_PREFER_BT);
  BLEDevice::init("UL212_BLE_FETCH");
  BLEDevice::setMTU(512);
}

void bleUl212PausePolling(bool pause) { pollPaused = pause; }

void bleUl212PreferZigbeeAirtime(bool prefer_zigbee) {
  /* On ESP32-C6, IEEE802.15.4 (Zigbee) shares the WiFi coex side. */
  esp_coex_preference_set(prefer_zigbee ? ESP_COEX_PREFER_WIFI : ESP_COEX_PREFER_BT);
}

void bleUl212ApplyConfig(const char *sensorMac, uint32_t pollMs, uint32_t silenceMs) {
  if (sensorMac) {
    strncpy(targetMac, sensorMac, sizeof(targetMac) - 1);
    targetMac[sizeof(targetMac) - 1] = 0;
  }
  if (pollMs >= 200) pollIntervalMs = pollMs;
  if (silenceMs >= 3000) silenceTimeoutMs = silenceMs;
  configDirty = true;
}

bool bleUl212Configured() { return targetMac[0] != '\0'; }

bool bleUl212Connected() { return linkUp && pClient && pClient->isConnected(); }

Ul212Reading bleUl212Latest() {
  Ul212Reading r{};
  if (xSemaphoreTake(stateMux, pdMS_TO_TICKS(50)) == pdTRUE) {
    r = latest;
    xSemaphoreGive(stateMux);
  }
  return r;
}

BleStats bleUl212Stats() { return stats; }

uint32_t bleUl212LastRxAgeMs() {
  return lastRxMs ? millis() - lastRxMs : 0;
}

uint32_t bleUl212ConnectedMsTotal() {
  uint32_t t = stats.connectedMs;
  if (lastConnectMs) t += millis() - lastConnectMs;
  return t;
}

size_t bleUl212CopyEvents(BleEvent *out, size_t maxEvents) {
  size_t n = eventCount < maxEvents ? eventCount : maxEvents;
  size_t start = (eventCount < EVENT_MAX) ? 0 : eventHead;
  for (size_t i = 0; i < n; i++) out[i] = events[(start + i) % EVENT_MAX];
  return n;
}

size_t bleUl212Scan(BleScanEntry *out, size_t maxEntries) {
  releaseClient();
  delay(200);

  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  BLEScanResults *found = scan->start(5, false);

  size_t n = 0;
  for (int i = 0; found && i < found->getCount() && n < maxEntries; i++) {
    BLEAdvertisedDevice d = found->getDevice(i);
    if (!d.isAdvertisingService(serviceUUID)) continue;
    BleScanEntry &e = out[n++];
    strncpy(e.mac, d.getAddress().toString().c_str(), sizeof(e.mac) - 1);
    e.rssi = d.getRSSI();
    strncpy(e.name, d.getName().c_str(), sizeof(e.name) - 1);
  }
  scan->clearResults();
  return n;
}

void bleUl212Task(void *) {
  static const uint32_t BACKOFF_MIN_MS = 2000, BACKOFF_MAX_MS = 30000;
  uint32_t backoffMs = BACKOFF_MIN_MS;
  uint8_t softRecoverLeft = SOFT_RECOVER_MAX;
  uint32_t softGraceUntilMs = 0;

  for (;;) {
    if (configDirty) {
      configDirty = false;
      releaseClient();
      softRecoverLeft = SOFT_RECOVER_MAX;
      softGraceUntilMs = 0;
    }

    if (!bleUl212Configured()) {
      Serial.println("[ble] no sensor MAC configured — run scan / mac / save on serial");
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (!bleUl212Connected()) {
      linkUp = false;
      if (!connectAndUnlock(targetMac)) {
        vTaskDelay(pdMS_TO_TICKS(backoffMs));
        backoffMs = min<uint32_t>(backoffMs * 2, BACKOFF_MAX_MS);
        continue;
      }
      backoffMs = BACKOFF_MIN_MS;
      softRecoverLeft = SOFT_RECOVER_MAX;
      softGraceUntilMs = 0;
    }

    const uint32_t now = millis();
    if (lastRxMs && now - lastRxMs > silenceTimeoutMs) {
      if (now < softGraceUntilMs) {
        sendCmd(CMD_POLL, sizeof(CMD_POLL));
        vTaskDelay(pdMS_TO_TICKS(pollIntervalMs));
        continue;
      }
      stats.watchdogTrips++;
      if (softRecoverLeft > 0 && softRecover()) {
        softRecoverLeft--;
        softGraceUntilMs = now + max<uint32_t>(pollIntervalMs * 3, 3000);
        logEvent("silent %lus — soft recover (%u left)",
                 (unsigned long)((now - lastRxMs) / 1000),
                 (unsigned)softRecoverLeft);
        vTaskDelay(pdMS_TO_TICKS(pollIntervalMs));
        continue;
      }
      logEvent("silent %lus — full reconnect", (unsigned long)((now - lastRxMs) / 1000));
      releaseClient();
      softRecoverLeft = SOFT_RECOVER_MAX;
      softGraceUntilMs = 0;
      vTaskDelay(pdMS_TO_TICKS(800));
      continue;
    }

    if (latest.valid) softRecoverLeft = SOFT_RECOVER_MAX;

    if (pollPaused) {
      /* Zigbee air-slice: keep GATT up, skip Modbus polls so RF is free. */
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    sendCmd(CMD_POLL, sizeof(CMD_POLL));
    vTaskDelay(pdMS_TO_TICKS(pollIntervalMs));
  }
}
