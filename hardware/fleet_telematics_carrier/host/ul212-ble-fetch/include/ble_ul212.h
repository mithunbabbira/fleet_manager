#pragma once

#include <stddef.h>
#include <stdint.h>

/* BLE Modbus client for the Tenet UL212 over 0xFFE0 / 0xFFE1.
   Reverse-engineered from the vendor mobile app handshake. */

struct Ul212Reading {
  bool valid = false;
  float heightMm = 0;
  float smoothMm = 0;
  float temperatureC = 0;
  uint8_t signal = 0;
  uint8_t validSignals = 0;
  uint8_t tiltDeg = 0;
  uint8_t firmware = 0;
  uint16_t sonicSpeed5C = 0;
  uint16_t sonicSpeed55C = 0;
  uint16_t netAddress = 0;
  uint32_t baudRate = 0;
  uint16_t protocol = 0;
  uint32_t stampMs = 0;
  uint32_t count = 0;
};

struct BleStats {
  uint32_t connectAttempts = 0;
  uint32_t connectFailures = 0;
  uint32_t disconnects = 0;
  uint32_t watchdogTrips = 0;
  uint32_t crcErrors = 0;
  uint32_t noEchoReadings = 0;
  uint32_t longestGapMs = 0;
  uint32_t connectedMs = 0;
  uint32_t notAdvertising = 0;
  int lastSensorRssi = 0;
  uint32_t reconnects = 0;
};

struct BleEvent {
  uint32_t atS;
  char what[48];
};

struct BleScanEntry {
  char mac[24];
  int rssi;
  char name[32];
};

/* Call once before starting bleUl212Task. */
void bleUl212Begin();

/* Apply config and force a reconnect on next loop iteration. */
void bleUl212ApplyConfig(const char *sensorMac, uint32_t pollIntervalMs,
                         uint32_t silenceTimeoutMs);

bool bleUl212Configured();
bool bleUl212Connected();
Ul212Reading bleUl212Latest();
BleStats bleUl212Stats();
uint32_t bleUl212LastRxAgeMs();
uint32_t bleUl212ConnectedMsTotal();

size_t bleUl212CopyEvents(BleEvent *out, size_t maxEvents);

/* Active scan for UL212-like devices (service 0xFFE0). Blocks ~5 s. */
size_t bleUl212Scan(BleScanEntry *out, size_t maxEntries);

/* FreeRTOS task — pass nullptr. Runs forever. */
void bleUl212Task(void *param);

/* Time-slice helpers for BLE + Zigbee coexistence on one radio. */
void bleUl212PausePolling(bool pause);
void bleUl212PreferZigbeeAirtime(bool prefer_zigbee);
