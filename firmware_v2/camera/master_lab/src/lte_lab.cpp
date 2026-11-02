#include "lte_lab.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HardwareSerial s_modem(1);
static bool s_ready = false;
static bool s_mqtt_up = false;
static char s_urc_acc[512];

static void modemDrain(void)
{
    while (s_modem.available()) {
        (void)s_modem.read();
    }
}

static bool atWait(char *buf, size_t buflen, uint32_t timeout_ms, const char *token)
{
    if (buf && buflen) {
        buf[0] = '\0';
    }
    size_t used = 0;
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        while (s_modem.available()) {
            char c = (char)s_modem.read();
            if (buf && used + 1 < buflen) {
                buf[used++] = c;
                buf[used] = '\0';
            }
            if (token && buf && strstr(buf, token)) {
                return true;
            }
            if (buf && (strstr(buf, "\nOK") || strstr(buf, "\rOK"))) {
                if (!token) {
                    return true;
                }
            }
            if (buf && strstr(buf, "ERROR")) {
                return false;
            }
        }
        delay(5);
        yield();
    }
    return token ? (buf && strstr(buf, token) != nullptr) : (buf && strstr(buf, "OK") != nullptr);
}

static bool atCmd(const char *cmd, char *buf, size_t buflen, uint32_t timeout_ms)
{
    modemDrain();
    s_modem.print(cmd);
    s_modem.print("\r");
    s_modem.flush();
    bool ok = atWait(buf, buflen, timeout_ms, nullptr);
    if (buf && buflen) {
        /* accept OK even if ERROR not seen */
        if (strstr(buf, "OK")) {
            return true;
        }
    }
    return ok && buf && strstr(buf, "OK");
}

bool lteLabBegin(void)
{
    s_modem.begin(115200, SERIAL_8N1, LAB_LTE_RX_GPIO, LAB_LTE_TX_GPIO);
    delay(200);
    modemDrain();
    char resp[256];
    for (int i = 0; i < 8; i++) {
        if (atCmd("AT", resp, sizeof(resp), 1000) && strstr(resp, "OK")) {
            s_ready = true;
            Serial.printf("LTE lab UART OK TX=GPIO%d RX=GPIO%d\n", LAB_LTE_TX_GPIO, LAB_LTE_RX_GPIO);
            (void)atCmd("ATE0", resp, sizeof(resp), 1000);
            (void)atCmd("AT+QHTTPSTOP", resp, sizeof(resp), 3000);
            return true;
        }
        delay(300);
    }
    Serial.println("LTE lab: no AT OK (modem power? UART16/17? SIM?)");
    s_ready = false;
    return false;
}

bool lteLabReady(void)
{
    return s_ready;
}

bool lteLabEnsurePdp(void)
{
    if (!s_ready) {
        return false;
    }
    char resp[384];
    char cmd[128];
    (void)atCmd("AT+CFUN=1", resp, sizeof(resp), 10000);
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"\",\"\",1", LAB_LTE_APN);
    (void)atCmd(cmd, resp, sizeof(resp), 5000);

    /* Wait for PS attach before QIACT. */
    for (int i = 0; i < 30; i++) {
        if (atCmd("AT+CGATT?", resp, sizeof(resp), 3000) && strstr(resp, "+CGATT: 1")) {
            break;
        }
        if (i == 0 || i == 29) {
            (void)atCmd("AT+CEREG?", resp, sizeof(resp), 3000);
            Serial.printf("LTE wait attach CEREG/CGATT: %s\n", resp);
        }
        delay(2000);
    }
    if (!atCmd("AT+CGATT?", resp, sizeof(resp), 3000) || !strstr(resp, "+CGATT: 1")) {
        Serial.println("LTE FAIL: CGATT!=1 (SIM/antenna/network?)");
        return false;
    }

    if (!atCmd("AT+QIACT=1", resp, sizeof(resp), 60000)) {
        if (strstr(resp, "ERROR")) {
            Serial.printf("LTE QIACT: %s\n", resp);
        }
    }
    if (!atCmd("AT+QIACT?", resp, sizeof(resp), 5000)) {
        return false;
    }
    Serial.printf("LTE PDP: %s\n", resp);
    return strstr(resp, "+QIACT:") != nullptr;
}

void lteLabStatus(void)
{
    Serial.printf("lte_ready=%d mqtt_up=%d apn=%s\n", (int)s_ready, (int)s_mqtt_up, LAB_LTE_APN);
    if (!s_ready) {
        return;
    }
    char resp[256];
    if (atCmd("AT+CGATT?", resp, sizeof(resp), 3000)) {
        Serial.printf("CGATT %s\n", resp);
    }
    if (atCmd("AT+QIACT?", resp, sizeof(resp), 5000)) {
        Serial.printf("QIACT %s\n", resp);
    }
    if (atCmd("AT+QMTCONN?", resp, sizeof(resp), 3000)) {
        Serial.printf("QMTCONN %s\n", resp);
    }
}

static bool httpSetUrl(const char *url)
{
    char resp[256];
    char cmd[64];
    size_t ulen = strlen(url);
    snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,80", (unsigned)ulen);
    modemDrain();
    s_modem.print(cmd);
    s_modem.print("\r");
    s_modem.flush();
    if (!atWait(resp, sizeof(resp), 10000, "CONNECT")) {
        Serial.printf("QHTTPURL no CONNECT: %s\n", resp);
        return false;
    }
    s_modem.write((const uint8_t *)url, ulen);
    s_modem.flush();
    if (!atWait(resp, sizeof(resp), 30000, nullptr) || !strstr(resp, "OK")) {
        Serial.printf("QHTTPURL fail: %s\n", resp);
        return false;
    }
    return true;
}

bool lteLabHttpPostMultipart(const char *url, const uint8_t *body, size_t body_len,
                             const char *boundary)
{
    if (!s_ready || !url || !body || body_len == 0) {
        return false;
    }
    if (!lteLabEnsurePdp()) {
        Serial.println("LTE PDP fail");
        return false;
    }
    char resp[512];
    (void)atCmd("AT+QHTTPCFG=\"contextid\",1", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QHTTPCFG=\"sslctxid\",1", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QSSLCFG=\"sslversion\",1,4", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QSSLCFG=\"seclevel\",1,0", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QSSLCFG=\"sni\",1,1", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QHTTPCFG=\"requestheader\",1", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QHTTPCFG=\"responseheader\",0", resp, sizeof(resp), 3000);

    if (!httpSetUrl(url)) {
        return false;
    }

    /* Build raw HTTP/1.1 request with multipart body (binary-safe). */
    const char *hostp = strstr(url, "://");
    hostp = hostp ? hostp + 3 : url;
    const char *path = strchr(hostp, '/');
    char host[128];
    char pathbuf[160];
    if (!path) {
        snprintf(host, sizeof(host), "%s", hostp);
        snprintf(pathbuf, sizeof(pathbuf), "/");
    } else {
        size_t hlen = (size_t)(path - hostp);
        if (hlen >= sizeof(host)) {
            hlen = sizeof(host) - 1;
        }
        memcpy(host, hostp, hlen);
        host[hlen] = '\0';
        /* strip :port from host for Host header */
        char *colon = strchr(host, ':');
        if (colon) {
            *colon = '\0';
        }
        snprintf(pathbuf, sizeof(pathbuf), "%s", path);
    }

    char hdr[384];
    int hdr_n = snprintf(hdr, sizeof(hdr),
                         "POST %s HTTP/1.1\r\n"
                         "Host: %s\r\n"
                         "Content-Type: multipart/form-data; boundary=%s\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         pathbuf, host, boundary, (unsigned)body_len);
    if (hdr_n <= 0 || (size_t)hdr_n >= sizeof(hdr)) {
        return false;
    }
    size_t total = (size_t)hdr_n + body_len;
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%u,120,120", (unsigned)total);
    modemDrain();
    s_modem.print(cmd);
    s_modem.print("\r");
    s_modem.flush();
    if (!atWait(resp, sizeof(resp), 30000, "CONNECT")) {
        Serial.printf("QHTTPPOST no CONNECT: %s\n", resp);
        return false;
    }
    s_modem.write((const uint8_t *)hdr, (size_t)hdr_n);
    size_t off = 0;
    while (off < body_len) {
        size_t chunk = body_len - off;
        if (chunk > 1024) {
            chunk = 1024;
        }
        s_modem.write(body + off, chunk);
        off += chunk;
        yield();
    }
    s_modem.flush();

    /* Wait +QHTTPPOST: err,status,len */
    if (!atWait(resp, sizeof(resp), 180000, "+QHTTPPOST:")) {
        Serial.printf("QHTTPPOST no URC: %s\n", resp);
        (void)atCmd("AT+QHTTPSTOP", resp, sizeof(resp), 3000);
        return false;
    }
    Serial.printf("LTE upload URC: %s\n", resp);
    int err = -1, status = 0;
    const char *p = strstr(resp, "+QHTTPPOST:");
    if (p) {
        sscanf(p, "+QHTTPPOST: %d,%d", &err, &status);
    }
    (void)atCmd("AT+QHTTPSTOP", resp, sizeof(resp), 3000);
    return err == 0 && status >= 200 && status < 300;
}

bool lteLabMqttStart(const char *device_id)
{
    if (!s_ready || !device_id) {
        return false;
    }
    if (!lteLabEnsurePdp()) {
        return false;
    }
    char resp[384];
    char cmd[192];
    (void)atCmd("AT+QMTCFG=\"recv/mode\",0,0,1", resp, sizeof(resp), 3000);
    (void)atCmd("AT+QMTDISC=0", resp, sizeof(resp), 10000);
    delay(300);
    snprintf(cmd, sizeof(cmd), "AT+QMTOPEN=0,\"%s\",%d", LAB_LTE_MQTT_HOST, LAB_LTE_MQTT_PORT);
    if (!atCmd(cmd, resp, sizeof(resp), 60000)) {
        Serial.printf("QMTOPEN fail: %s\n", resp);
        /* may already be open */
    }
    delay(500);
    atWait(resp, sizeof(resp), 5000, "+QMTOPEN:");
    Serial.printf("QMTOPEN: %s\n", resp);

    snprintf(cmd, sizeof(cmd), "AT+QMTCONN=0,\"%s\"", device_id);
    if (!atCmd(cmd, resp, sizeof(resp), 60000)) {
        Serial.printf("QMTCONN fail: %s\n", resp);
    }
    atWait(resp, sizeof(resp), 10000, "+QMTCONN:");
    Serial.printf("QMTCONN: %s\n", resp);

    snprintf(cmd, sizeof(cmd), "AT+QMTSUB=0,1,\"fleet/v1/%s/camera/cmd\",1", device_id);
    (void)atCmd(cmd, resp, sizeof(resp), 30000);
    if (!strstr(resp, "+QMTSUB:")) {
        atWait(resp, sizeof(resp), 15000, "+QMTSUB:");
    }
    Serial.printf("QMTSUB: %s\n", resp);
    if (!strstr(resp, "+QMTSUB:")) {
        Serial.println("QMTSUB fail");
        return false;
    }
    s_mqtt_up = true;
    s_urc_acc[0] = '\0';
    return true;
}

static bool parseJobJson(const char *json, char *job_id, size_t job_len, int *deg, bool *have_deg)
{
    *have_deg = false;
    *deg = 90;
    if (job_id && job_len) {
        job_id[0] = '\0';
    }
    if (!json) {
        return false;
    }
    const char *p = strstr(json, "\"job_id\"");
    if (p) {
        p = strchr(p + 8, ':');
        if (p) {
            p++;
            while (*p && isspace((unsigned char)*p)) {
                p++;
            }
            if (*p == '"') {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i + 1 < job_len) {
                    char c = *p++;
                    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') {
                        job_id[i++] = c;
                    }
                }
                job_id[i] = '\0';
            }
        }
    }
    p = strstr(json, "\"deg\"");
    if (p) {
        p = strchr(p + 5, ':');
        if (p) {
            *deg = atoi(p + 1);
            *have_deg = true;
            if (*deg < 0) {
                *deg = 0;
            }
            if (*deg > 180) {
                *deg = 180;
            }
        }
    }
    return job_id[0] != '\0' || *have_deg;
}

bool lteLabMqttPollJob(char *job_id, size_t job_len, int *deg, bool *have_deg)
{
    if (!s_ready || !s_mqtt_up) {
        return false;
    }
    while (s_modem.available()) {
        char c = (char)s_modem.read();
        size_t n = strlen(s_urc_acc);
        if (n + 1 < sizeof(s_urc_acc)) {
            s_urc_acc[n] = c;
            s_urc_acc[n + 1] = '\0';
        } else {
            memmove(s_urc_acc, s_urc_acc + 64, sizeof(s_urc_acc) - 64);
            s_urc_acc[sizeof(s_urc_acc) - 65] = '\0';
        }
    }
    char *p = strstr(s_urc_acc, "+QMTRECV:");
    if (!p) {
        return false;
    }
    /* payload often after last quote-comma or raw JSON */
    char *json = strchr(p, '{');
    if (!json) {
        /* keep buffer if incomplete */
        if (strlen(p) > 400) {
            s_urc_acc[0] = '\0';
        }
        return false;
    }
    char *end = strchr(json, '}');
    if (!end) {
        return false;
    }
    char tmp[192];
    size_t len = (size_t)(end - json + 1);
    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    memcpy(tmp, json, len);
    tmp[len] = '\0';
    /* consume URC */
    memmove(s_urc_acc, end + 1, strlen(end + 1) + 1);
    Serial.printf("LTE MQTT job: %s\n", tmp);
    return parseJobJson(tmp, job_id, job_len, deg, have_deg);
}

bool lteLabMqttPublishAck(const char *device_id, const char *job_id, const char *status,
                          const char *detail)
{
    if (!s_ready || !s_mqtt_up || !device_id) {
        return false;
    }
    char topic[96];
    char body[192];
    snprintf(topic, sizeof(topic), "fleet/v1/%s/camera/ack", device_id);
    snprintf(body, sizeof(body),
             "{\"job_id\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\",\"device_id\":\"%s\"}",
             job_id ? job_id : "", status ? status : "", detail ? detail : "", device_id);
    char cmd[128];
    char resp[256];
    snprintf(cmd, sizeof(cmd), "AT+QMTPUBEX=0,0,0,0,\"%s\",%u", topic, (unsigned)strlen(body));
    modemDrain();
    s_modem.print(cmd);
    s_modem.print("\r");
    s_modem.flush();
    if (!atWait(resp, sizeof(resp), 10000, ">")) {
        /* some FW use CONNECT */
        if (!strstr(resp, ">") && !strstr(resp, "CONNECT")) {
            Serial.printf("QMTPUBEX no prompt: %s\n", resp);
            return false;
        }
    }
    s_modem.print(body);
    s_modem.flush();
    return atWait(resp, sizeof(resp), 15000, nullptr);
}
