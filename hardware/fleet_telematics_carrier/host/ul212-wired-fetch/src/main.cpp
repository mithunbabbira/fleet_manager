#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

/*
 * UL212 Wired — auto-scan baud + parity, then poll if a setting works.
 *
 * Keep the sensor fully wired:
 *   ESP32 GPIO21 (TX) → MAX232 TTL-TX-IN
 *   MAX232 RS232-TX   → Sensor RX
 *   Sensor TX         → MAX232 RS232-RX
 *   MAX232 TTL-RX-OUT → ESP32 GPIO20 (RX)
 *   Common GND
 */

static const int PIN_RX = 20;
static const int PIN_TX = 21;
static HardwareSerial SensorSerial(1);

static const uint8_t CMD_UNLOCK[]     = {0x09, 0x06, 0x01, 0x07, 0xCC, 0x9D, 0xAC, 0x16};
static const uint8_t CMD_UNLOCK_ALT[] = {0x09, 0x06, 0x01, 0x07, 0x7D, 0x5A, 0x99, 0xD4};
static const uint8_t CMD_POLL[]       = {0x09, 0x03, 0x00, 0xFD, 0x00, 0x1C, 0xD4, 0xBB};
static const size_t  REPLY_LEN        = 4 + 28 * 2 + 2; /* 62 */

static uint32_t s_baud = 9600;
static uint32_t s_config = SERIAL_8N1;
static const char *s_configName = "8N1";
static bool s_found = false;

struct UartMode {
    uint32_t baud;
    uint32_t config;
    const char *name;
};

static const UartMode kModes[] = {
    {9600,   SERIAL_8N1, "9600 8N1"},
    {9600,   SERIAL_8E1, "9600 8E1"},
    {9600,   SERIAL_8O1, "9600 8O1"},
    {4800,   SERIAL_8N1, "4800 8N1"},
    {4800,   SERIAL_8E1, "4800 8E1"},
    {19200,  SERIAL_8N1, "19200 8N1"},
    {19200,  SERIAL_8E1, "19200 8E1"},
    {2400,   SERIAL_8N1, "2400 8N1"},
    {38400,  SERIAL_8N1, "38400 8N1"},
    {115200, SERIAL_8N1, "115200 8N1"},
};

static void printHex(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) Serial.printf("%02X ", d[i]);
    Serial.println();
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint16_t modbusCrc(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static bool isEcho(const uint8_t *cmd, size_t cmdLen, const uint8_t *rx, size_t rxLen) {
    return rxLen == cmdLen && memcmp(cmd, rx, cmdLen) == 0;
}

static size_t drain(uint8_t *buf, size_t maxLen, uint32_t ms) {
    size_t n = 0;
    uint32_t last = millis();
    uint32_t start = last;
    while ((millis() - start) < ms && n < maxLen) {
        if (SensorSerial.available()) {
            buf[n++] = SensorSerial.read();
            last = millis();
        } else if (n > 0 && (millis() - last) > 20) {
            break;
        } else {
            delay(1);
        }
    }
    return n;
}

static size_t sendAndRead(const uint8_t *cmd, size_t cmdLen, uint8_t *resp, size_t maxResp,
                          uint32_t waitMs) {
    while (SensorSerial.available()) SensorSerial.read();
    SensorSerial.write(cmd, cmdLen);
    SensorSerial.flush();
    return drain(resp, maxResp, waitMs);
}

static bool looksLikePollReply(const uint8_t *d, size_t n) {
    if (n < 5) return false;
    /* Find 09 03 header even if a few garbage bytes precede it. */
    for (size_t i = 0; i + 5 <= n; i++) {
        if (d[i] == 0x09 && d[i + 1] == 0x03) {
            uint8_t byteCount = d[i + 2];
            size_t need = (size_t)3 + byteCount + 2;
            if (i + need > n) return false;
            uint16_t crc = modbusCrc(d + i, need - 2);
            uint16_t got = (uint16_t)(d[i + need - 1] << 8 | d[i + need - 2]);
            return crc == got;
        }
    }
    return false;
}

static void decodeIfPossible(const uint8_t *d, size_t n) {
    for (size_t i = 0; i + REPLY_LEN <= n; i++) {
        if (d[i] != 0x09 || d[i + 1] != 0x03) continue;
        uint16_t crc = modbusCrc(d + i, REPLY_LEN - 2);
        uint16_t got = (uint16_t)(d[i + REPLY_LEN - 1] << 8 | d[i + REPLY_LEN - 2]);
        if (crc != got) continue;
        const uint8_t *e = d + i;
        float heightMm = be16(e + 8) / 10.0f;
        float smoothMm = be16(e + 10) / 10.0f;
        float tempC = (be16(e + 12) - 400) / 10.0f;
        Serial.printf("[UL212] height=%.1f mm  smooth=%.1f mm  temp=%.1f C  "
                      "signal=%u  tilt=%u deg\n",
                      heightMm, smoothMm, tempC, e[6], e[14]);
        return;
    }
}

static void openUart(uint32_t baud, uint32_t config) {
    SensorSerial.end();
    delay(50);
    SensorSerial.begin(baud, config, PIN_RX, PIN_TX);
    delay(80);
    while (SensorSerial.available()) SensorSerial.read();
}

static int scoreReply(const uint8_t *cmd, size_t cmdLen, const uint8_t *rx, size_t n) {
    if (n == 0) return 0;
    if (isEcho(cmd, cmdLen, rx, n)) return 1; /* local loopback, not the sensor */
    if (looksLikePollReply(rx, n)) return 100;
    if (n >= 20) return 40;
    if (n > cmdLen) return 25;
    return 10; /* noise / short junk */
}

static bool tryMode(const UartMode &mode) {
    Serial.printf("\n--- Trying %s ---\n", mode.name);
    openUart(mode.baud, mode.config);

    uint8_t resp[160];
    size_t n;

    n = sendAndRead(CMD_UNLOCK, sizeof(CMD_UNLOCK), resp, sizeof(resp), 400);
    Serial.printf("  unlock: %zu bytes", n);
    if (n) {
        Serial.print(" → ");
        printHex(resp, n);
    } else {
        Serial.println();
    }
    delay(80);

    if (n == 0 || isEcho(CMD_UNLOCK, sizeof(CMD_UNLOCK), resp, n)) {
        n = sendAndRead(CMD_UNLOCK_ALT, sizeof(CMD_UNLOCK_ALT), resp, sizeof(resp), 400);
        Serial.printf("  unlock-alt: %zu bytes", n);
        if (n) {
            Serial.print(" → ");
            printHex(resp, n);
        } else {
            Serial.println();
        }
        delay(80);
    }

    int best = 0;
    for (int i = 0; i < 2; i++) {
        n = sendAndRead(CMD_POLL, sizeof(CMD_POLL), resp, sizeof(resp), 600);
        Serial.printf("  poll: %zu bytes", n);
        if (n) {
            Serial.print(" → ");
            printHex(resp, n);
        } else {
            Serial.println();
        }
        int s = scoreReply(CMD_POLL, sizeof(CMD_POLL), resp, n);
        if (s > best) best = s;
        if (looksLikePollReply(resp, n)) {
            Serial.println("  *** VALID MODBUS REPLY ***");
            decodeIfPossible(resp, n);
            return true;
        }
        delay(50);
    }

    if (best == 1) Serial.println("  (echo of our own TX — not the sensor)");
    else if (best == 0) Serial.println("  (silence)");
    else Serial.println("  (bytes but not a valid Modbus frame)");
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n==========================================");
    Serial.println("  UL212 Wired — baud + parity scan");
    Serial.println("==========================================");
    Serial.printf("  UART1 TX=GPIO%d  RX=GPIO%d\n", PIN_TX, PIN_RX);
    Serial.println("  Leave the sensor wired. Scanning now...\n");

    for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++) {
        if (tryMode(kModes[i])) {
            s_found = true;
            s_baud = kModes[i].baud;
            s_config = kModes[i].config;
            s_configName = kModes[i].name;
            break;
        }
    }

    if (s_found) {
        Serial.printf("\nUsing %s for continuous poll.\n\n", s_configName);
        openUart(s_baud, s_config);
    } else {
        Serial.println("\nNo valid Modbus frame at any setting.");
        Serial.println("Falling back to 9600 8N1 so we can keep watching RX.\n");
        s_baud = 9600;
        s_config = SERIAL_8N1;
        s_configName = "9600 8N1";
        openUart(s_baud, s_config);
    }
}

void loop() {
    uint8_t resp[160];
    size_t n = sendAndRead(CMD_POLL, sizeof(CMD_POLL), resp, sizeof(resp), 600);

    Serial.printf("[%s] %zu bytes", s_configName, n);
    if (n) {
        Serial.print(" → ");
        printHex(resp, n);
        if (looksLikePollReply(resp, n)) decodeIfPossible(resp, n);
        else if (isEcho(CMD_POLL, sizeof(CMD_POLL), resp, n))
            Serial.println("  (echo of TX)");
    } else {
        Serial.println();
    }

    delay(1500);
}
