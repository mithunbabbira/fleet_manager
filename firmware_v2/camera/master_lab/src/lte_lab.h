/**
 * DELETE ME — lab-only EC200U AT helpers for master_lab (not fleet_v2_master).
 * UART: ESP TX GPIO16 → modem RX, ESP RX GPIO17 ← modem TX (same as production).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef LAB_LTE_TX_GPIO
#define LAB_LTE_TX_GPIO 16
#endif
#ifndef LAB_LTE_RX_GPIO
#define LAB_LTE_RX_GPIO 17
#endif
#ifndef LAB_LTE_APN
#define LAB_LTE_APN "airtelgprs.com"
#endif
#ifndef LAB_LTE_MQTT_HOST
#define LAB_LTE_MQTT_HOST "test.mosquitto.org"
#endif
#ifndef LAB_LTE_MQTT_PORT
#define LAB_LTE_MQTT_PORT 1883
#endif

bool lteLabBegin(void);
bool lteLabReady(void);
bool lteLabEnsurePdp(void);
/** Print brief modem / PDP / MQTT state on Serial. */
void lteLabStatus(void);

/**
 * HTTPS/HTTP POST multipart body already built (same as Wi‑Fi uploadJpeg).
 * Uses Quectel QHTTP. url must be reachable over LTE (ngrok / public host).
 */
bool lteLabHttpPostMultipart(const char *url, const uint8_t *body, size_t body_len,
                             const char *boundary);

/** Open/connect/subscribe cmd topic. Idempotent. */
bool lteLabMqttStart(const char *device_id);
/** Poll UART for +QMTRECV; if cmd JSON found, fill job_id/deg and return true. */
bool lteLabMqttPollJob(char *job_id, size_t job_len, int *deg, bool *have_deg);
bool lteLabMqttPublishAck(const char *device_id, const char *job_id, const char *status,
                          const char *detail);
