extern "C" {
#include "ul212_rs232_parse.h"
}

#include "ul212_rs232_config.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

static HardwareSerial SensorUart(1);

static char s_lineBuf[256];
static size_t s_lineLen = 0;

static void ul212ReadingZero(Ul212Reading *out)
{
  out->valid = false;
  out->height_mm = 0.0f;
  out->smooth_mm = 0.0f;
  out->temp_c = 0.0f;
  out->tilt_deg = 0;
  out->signal = 0;
  out->stamp_ms = 0;
}

static void ul212DrainUart(void)
{
  while (SensorUart.available() > 0) {
    (void)SensorUart.read();
  }
}

static void ul212ClearLineBuf(void)
{
  s_lineLen = 0;
  s_lineBuf[0] = '\0';
}

#if UL212_RS232_PROTOCOL == 14
static bool ul212TryParseXdBuffer(Ul212Reading *out)
{
  char *xd = strstr(s_lineBuf, "*XD");
  if (xd == NULL) {
    return false;
  }

  if (xd != s_lineBuf) {
    size_t remain = strlen(xd);
    memmove(s_lineBuf, xd, remain + 1);
    s_lineLen = remain;
  }

  char *hash = strchr(s_lineBuf, '#');
  if (hash == NULL) {
    return false;
  }

  size_t frameLen = (size_t)(hash - s_lineBuf + 1);
  char frame[256];
  if (frameLen >= sizeof(frame)) {
    return false;
  }

  memcpy(frame, s_lineBuf, frameLen);
  frame[frameLen] = '\0';

  if (!ul212ParseXd(frame, out)) {
    return false;
  }

  out->stamp_ms = millis();

  size_t tailLen = s_lineLen - frameLen;
  if (tailLen > 0) {
    memmove(s_lineBuf, s_lineBuf + frameLen, tailLen + 1);
    s_lineLen = tailLen;
  } else {
    ul212ClearLineBuf();
  }

  return true;
}
#endif

void ul212Rs232Begin(int rx_pin, int tx_pin, uint32_t baud)
{
  SensorUart.begin(baud, SERIAL_8N1, rx_pin, tx_pin);
  ul212ClearLineBuf();
}

Ul212Reading ul212Rs232Poll(uint32_t timeout_ms)
{
  Ul212Reading reading;
  ul212ReadingZero(&reading);

#if UL212_RS232_PROTOCOL == 14
  if (ul212TryParseXdBuffer(&reading)) {
    return reading;
  }

  if (s_lineLen == 0 || strstr(s_lineBuf, "*XD") == NULL) {
    ul212DrainUart();
    ul212ClearLineBuf();
  }
#else
  ul212DrainUart();
  ul212ClearLineBuf();
#endif

  char cmd[16];
  snprintf(cmd, sizeof(cmd), "$!RY%02u%02u\r\n",
           (unsigned)UL212_RS232_ADDRESS, (unsigned)UL212_RS232_PROTOCOL);
  SensorUart.print(cmd);
  SensorUart.flush();

  const uint32_t start_ms = millis();
  uint32_t last_rx_ms = start_ms;
  bool terminated = false;

  while ((millis() - start_ms) < timeout_ms) {
    while (SensorUart.available() > 0) {
      const int raw = SensorUart.read();
      if (raw < 0) {
        continue;
      }

      const char c = (char)raw;
      last_rx_ms = millis();

      if (s_lineLen < (sizeof(s_lineBuf) - 1)) {
        s_lineBuf[s_lineLen++] = c;
        s_lineBuf[s_lineLen] = '\0';
      }

#if UL212_RS232_PROTOCOL == 51
      if (c == '\n') {
        terminated = true;
        break;
      }
#else
      if (c == '#') {
        terminated = true;
        break;
      }
#endif
    }

    if (terminated) {
      break;
    }

#if UL212_RS232_PROTOCOL == 51
    if (s_lineLen > 0 && (millis() - last_rx_ms) > 80) {
      break;
    }
#endif

    delay(1);
  }

#if UL212_RS232_PROTOCOL == 51
  if (ul212ParseCfv(s_lineBuf, &reading)) {
    reading.stamp_ms = millis();
    ul212ClearLineBuf();
  } else {
    ul212ReadingZero(&reading);
  }
#else
  if (terminated && ul212TryParseXdBuffer(&reading)) {
    return reading;
  }

  if (terminated && strchr(s_lineBuf, '#') != NULL) {
    char *hash = strchr(s_lineBuf, '#');
    size_t consumed = (size_t)(hash - s_lineBuf + 1);
    if (consumed < s_lineLen) {
      memmove(s_lineBuf, s_lineBuf + consumed, s_lineLen - consumed + 1);
      s_lineLen -= consumed;
    } else {
      ul212ClearLineBuf();
    }
  }

  ul212ReadingZero(&reading);
#endif

  return reading;
}
