/**
 * Lab master bridge (ESP32-C6) — SoftAP always on + CAM proxy + lab job upload.
 *
 * NOT fleet_v2_master. Flash only on spare carrier for camera lab.
 * DELETE-ME pieces: MQTT desk trigger + HTTP upload to tools/cam_lab_api.
 *
 * SoftAP: Fleet-C6 / fleetc61 @ 192.168.4.1 (always on) — CAM only
 * STA (optional): join home/office Wi‑Fi so Mac keeps internet (do NOT put Mac on SoftAP)
 * CAM STA: 192.168.4.10 — GET /capture?deg=  (CAM needs SoftAP Wi‑Fi only; no USB)
 *
 * Lab job (USB or MQTT over STA internet):
 *   capture from CAM → multipart POST to Mac/ngrok cam_lab_api → ACK Serial / MQTT
 *
 * USB serial @ 115200:
 *   help | status | ping | servo <deg> | capture [deg] | job [deg] [job_id]
 *   sta <ssid> <pass> | uploadurl <url> | mqtthost <host> | mqttport <n> | mqtt
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "lte_lab.h"

#ifndef MASTER_AP_SSID
#define MASTER_AP_SSID "Fleet-C6"
#endif
#ifndef MASTER_AP_PASS
#define MASTER_AP_PASS "fleetc61"
#endif
#ifndef CAM_STA_IP
#define CAM_STA_IP "192.168.4.10"
#endif
#ifndef CAM_STA_IP_LEGACY
#define CAM_STA_IP_LEGACY "192.168.4.2"
#endif
#ifndef CAM_CAPTURE_PATH
#define CAM_CAPTURE_PATH "/capture"
#endif
#ifndef LAB_UPLOAD_URL
/* Must be public (ngrok) when uploading over EC200U LTE — LAN IPs are unreachable */
#define LAB_UPLOAD_URL "https://REPLACE.ngrok-free.app/lab/camera/upload"
#endif
#ifndef LAB_MQTT_HOST
/* Wi‑Fi MQTT fallback only; preferred path is EC200U QMT → test.mosquitto.org */
#define LAB_MQTT_HOST "test.mosquitto.org"
#endif
#ifndef LAB_MQTT_PORT
#define LAB_MQTT_PORT 1883
#endif
#ifndef LAB_JPEG_MAX
#define LAB_JPEG_MAX (96 * 1024)
#endif
#ifndef LAB_STA_SSID
#define LAB_STA_SSID ""
#endif
#ifndef LAB_STA_PASS
#define LAB_STA_PASS ""
#endif

static WebServer server(80);
static const size_t kProxyBuf = 4096;
static String s_cli_line;
static char s_cam_ip[16] = CAM_STA_IP;
static char s_upload_url[160] = LAB_UPLOAD_URL;
static char s_mqtt_host[64] = LAB_MQTT_HOST;
static uint16_t s_mqtt_port = LAB_MQTT_PORT;
static char s_sta_ssid[33] = LAB_STA_SSID;
static char s_sta_pass[65] = LAB_STA_PASS;
static bool s_job_busy = false;
/** Same key as production uplink/OTA: NVS namespace elm, key uplink_did. */
static char s_device_id[40] = "fleet-demo-001";

static bool deviceIdOk(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n >= sizeof(s_device_id)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/** Read provisioned id. If NVS is empty, store the fleet default. */
static void loadOrProvisionDeviceId()
{
    Preferences prefs;
    if (!prefs.begin("elm", false)) {
        Serial.printf("device_id=%s (NVS elm open failed)\n", s_device_id);
        return;
    }
    String id = prefs.getString("uplink_did", "");
    if (deviceIdOk(id.c_str())) {
        snprintf(s_device_id, sizeof(s_device_id), "%s", id.c_str());
        Serial.printf("device_id=%s (NVS elm/uplink_did)\n", s_device_id);
    } else {
        snprintf(s_device_id, sizeof(s_device_id), "%s", "fleet-demo-001");
        prefs.putString("uplink_did", s_device_id);
        Serial.printf("device_id=%s (provisioned; elm/uplink_did was empty)\n", s_device_id);
    }
    prefs.end();
    Serial.printf("mqtt cmd topic fleet/v1/%s/camera/cmd\n", s_device_id);
}

static WiFiClient s_mqtt_wifi;
static PubSubClient s_mqtt(s_mqtt_wifi);
static uint32_t s_mqtt_last_attempt_ms = 0;

static String camBaseUrl(const char *path)
{
    String u = "http://";
    u += s_cam_ip;
    u += path;
    return u;
}

/** Probe /status on an IP; true if HTTP 200 and body looks like CAM JSON. */
static bool probeCamIp(const char *ip)
{
    HTTPClient http;
    String url = String("http://") + ip + "/status";
    if (!http.begin(url)) {
        return false;
    }
    http.setTimeout(2500);
    int code = http.GET();
    bool ok = false;
    if (code == HTTP_CODE_OK) {
        String body = http.getString();
        ok = body.indexOf("\"ready\"") >= 0;
    }
    http.end();
    return ok;
}

/** Prefer .10 (new); fall back to .2 (old lab builds). */
static bool resolveCamIp()
{
    if (probeCamIp(s_cam_ip)) {
        return true;
    }
    const char *candidates[] = {CAM_STA_IP, CAM_STA_IP_LEGACY};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        Serial.printf("probe CAM %s …\n", candidates[i]);
        if (probeCamIp(candidates[i])) {
            snprintf(s_cam_ip, sizeof(s_cam_ip), "%s", candidates[i]);
            Serial.printf("CAM resolved @ %s\n", s_cam_ip);
            return true;
        }
    }
    Serial.println("CAM resolve failed (.10 and .2)");
    return false;
}

static void printStatus()
{
    Serial.printf("ap=%s ip=%s cam=%s stations=%d\n", MASTER_AP_SSID,
                  WiFi.softAPIP().toString().c_str(), s_cam_ip,
                  WiFi.softAPgetStationNum());
    wl_status_t st = WiFi.status();
    Serial.printf("sta=%s status=%d ip=%s\n", s_sta_ssid[0] ? s_sta_ssid : "(none)", (int)st,
                  (st == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "-");
    Serial.printf("upload=%s\n", s_upload_url);
    Serial.printf("mqtt=%s:%u device=%s connected=%d\n", s_mqtt_host, (unsigned)s_mqtt_port,
                  s_device_id, (int)s_mqtt.connected());
}

/** Join home/office Wi‑Fi while keeping SoftAP for CAM. Mac must NOT join SoftAP. */
static bool startSta(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!ssid || !ssid[0]) {
        Serial.println("FAIL: empty STA ssid");
        return false;
    }
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    snprintf(s_sta_pass, sizeof(s_sta_pass), "%s", pass ? pass : "");
    Serial.printf("STA join \"%s\" …\n", s_sta_ssid);
    WiFi.begin(s_sta_ssid, s_sta_pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(200);
        Serial.print('.');
        yield();
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("FAIL: STA join (check ssid/pass)");
        return false;
    }
    Serial.printf("STA ok ip=%s gw=%s\n", WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());
    s_mqtt.disconnect();
    s_mqtt_last_attempt_ms = 0;
    return true;
}

/** GET CAM /status; print body. Returns HTTP code or <=0 on transport fail. */
static int camPing()
{
    if (!resolveCamIp()) {
        Serial.println("FAIL: CAM unreachable (power? STA firmware? SoftAP join? phone IP clash?)");
        Serial.println("HINT: disconnect phone from Fleet-C6, reset CAM, or flash CAM to .10");
        return -1;
    }
    HTTPClient http;
    String url = camBaseUrl("/status");
    Serial.printf("GET %s\n", url.c_str());
    if (!http.begin(url)) {
        Serial.println("FAIL: http begin");
        return -1;
    }
    http.setTimeout(5000);
    int code = http.GET();
    if (code <= 0) {
        Serial.println("FAIL: CAM unreachable after resolve");
        http.end();
        return code;
    }
    String body = http.getString();
    http.end();
    Serial.printf("http=%d %s", code, body.c_str());
    return code;
}

/** GET CAM /servo?deg=N. Returns HTTP code. */
static int camServo(int deg)
{
    if (deg < 0) {
        deg = 0;
    }
    if (deg > 180) {
        deg = 180;
    }
    if (!resolveCamIp()) {
        Serial.println("FAIL: CAM unreachable");
        return -1;
    }
    HTTPClient http;
    String url = camBaseUrl("/servo");
    url += "?deg=";
    url += String(deg);
    Serial.printf("GET %s\n", url.c_str());
    if (!http.begin(url)) {
        Serial.println("FAIL: http begin");
        return -1;
    }
    http.setTimeout(10000);
    int code = http.GET();
    if (code <= 0) {
        Serial.println("FAIL: CAM unreachable");
        http.end();
        return code;
    }
    String body = http.getString();
    http.end();
    Serial.printf("http=%d %s", code, body.c_str());
    return code;
}

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Stream base64 to Serial (lab pull of JPEG when Mac is not on SoftAP). */
static void b64Emit(const uint8_t *data, size_t len, uint8_t *rem, size_t *rem_len)
{
    uint8_t tmp[3];
    size_t ti = 0;
    for (size_t i = 0; i < *rem_len; i++) {
        tmp[ti++] = rem[i];
    }
    for (size_t i = 0; i < len; i++) {
        tmp[ti++] = data[i];
        if (ti == 3) {
            Serial.write(kB64[(tmp[0] >> 2) & 63]);
            Serial.write(kB64[((tmp[0] & 3) << 4) | (tmp[1] >> 4)]);
            Serial.write(kB64[((tmp[1] & 15) << 2) | (tmp[2] >> 6)]);
            Serial.write(kB64[tmp[2] & 63]);
            ti = 0;
        }
    }
    *rem_len = ti;
    for (size_t i = 0; i < ti; i++) {
        rem[i] = tmp[i];
    }
}

static void b64Flush(uint8_t *rem, size_t rem_len)
{
    if (rem_len == 0) {
        return;
    }
    uint8_t tmp[3] = {rem[0], (uint8_t)(rem_len > 1 ? rem[1] : 0), 0};
    Serial.write(kB64[(tmp[0] >> 2) & 63]);
    Serial.write(kB64[((tmp[0] & 3) << 4) | (tmp[1] >> 4)]);
    if (rem_len == 1) {
        Serial.write('=');
        Serial.write('=');
    } else {
        Serial.write(kB64[((tmp[1] & 15) << 2)]);
        Serial.write('=');
    }
}

/**
 * Fetch JPEG from CAM.
 * @p out SoftAP HTTP client (proxy), or null.
 * @p dump_b64 if true (and out null), emit BEGIN_JPEG…END_JPEG base64 on Serial.
 * @p heap_out if non-null, malloc JPEG into *heap_out (caller frees); size in *heap_len.
 * Returns bytes received, or -1 on failure.
 */
static int camCapture(int deg, bool have_deg, WiFiClient *out, bool dump_b64, uint8_t **heap_out,
                      size_t *heap_len)
{
    if (heap_out) {
        *heap_out = nullptr;
    }
    if (heap_len) {
        *heap_len = 0;
    }
    if (!resolveCamIp()) {
        Serial.println("FAIL: CAM unreachable");
        return -1;
    }
    HTTPClient http;
    String url = camBaseUrl(CAM_CAPTURE_PATH);
    if (have_deg) {
        if (deg < 0) {
            deg = 0;
        }
        if (deg > 180) {
            deg = 180;
        }
        url += "?deg=";
        url += String(deg);
    }
    Serial.printf("GET %s\n", url.c_str());

    if (!http.begin(url)) {
        Serial.println("FAIL: http begin");
        return -1;
    }
    http.setTimeout(45000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("FAIL: capture http=%d\n", code);
        http.end();
        return -1;
    }

    int len = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("FAIL: no stream");
        http.end();
        return -1;
    }

    uint8_t *heap = nullptr;
    size_t heap_cap = 0;
    size_t heap_used = 0;
    if (heap_out) {
        heap_cap = (len > 0 && len <= LAB_JPEG_MAX) ? (size_t)len : (size_t)LAB_JPEG_MAX;
        heap = (uint8_t *)malloc(heap_cap);
        if (!heap) {
            Serial.println("FAIL: malloc JPEG buffer");
            http.end();
            return -1;
        }
    }

    if (out) {
        if (len > 0) {
            server.setContentLength((size_t)len);
        }
        if (have_deg) {
            server.sendHeader("X-Servo-Deg", String(deg));
        }
        server.send(200, "image/jpeg", "");
    }

    if (dump_b64) {
        Serial.println("BEGIN_JPEG");
    }

    uint8_t buf[kProxyBuf];
    uint8_t rem[2];
    size_t rem_len = 0;
    size_t total = 0;
    uint8_t first2[2] = {0, 0};
    uint8_t last2[2] = {0, 0};
    bool have_first = false;
    unsigned long idle_ms = millis();
    while (http.connected() && (len > 0 || len == -1)) {
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - idle_ms > 15000) {
                Serial.println("FAIL: stream idle timeout");
                break;
            }
            delay(1);
            yield();
            continue;
        }
        idle_ms = millis();
        size_t want = avail;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        if (len > 0 && want > (size_t)len) {
            want = (size_t)len;
        }
        int n = stream->readBytes(buf, want);
        if (n <= 0) {
            break;
        }
        if (!have_first && n >= 1) {
            first2[0] = buf[0];
            if (n >= 2) {
                first2[1] = buf[1];
                have_first = true;
            }
        } else if (!have_first && total == 1) {
            first2[1] = buf[0];
            have_first = true;
        }
        if (n == 1) {
            last2[0] = last2[1];
            last2[1] = buf[0];
        } else {
            last2[0] = buf[n - 2];
            last2[1] = buf[n - 1];
        }
        if (out) {
            out->write(buf, (size_t)n);
        }
        if (dump_b64) {
            b64Emit(buf, (size_t)n, rem, &rem_len);
        }
        if (heap) {
            if (heap_used + (size_t)n > heap_cap) {
                size_t next = heap_cap * 2;
                if (next < heap_used + (size_t)n) {
                    next = heap_used + (size_t)n;
                }
                if (next > (size_t)LAB_JPEG_MAX) {
                    Serial.println("FAIL: JPEG too large for lab buffer");
                    free(heap);
                    http.end();
                    return -1;
                }
                uint8_t *grown = (uint8_t *)realloc(heap, next);
                if (!grown) {
                    Serial.println("FAIL: realloc JPEG buffer");
                    free(heap);
                    http.end();
                    return -1;
                }
                heap = grown;
                heap_cap = next;
            }
            memcpy(heap + heap_used, buf, (size_t)n);
            heap_used += (size_t)n;
        }
        total += (size_t)n;
        if (len > 0) {
            len -= n;
            if (len <= 0) {
                break;
            }
        }
    }
    http.end();

    if (dump_b64) {
        b64Flush(rem, rem_len);
        Serial.println();
        Serial.println("END_JPEG");
    }

    if (total == 0) {
        Serial.println("FAIL: 0 bytes");
        if (heap) {
            free(heap);
        }
        return -1;
    }

    bool soi = have_first && first2[0] == 0xFF && first2[1] == 0xD8;
    bool eoi = last2[0] == 0xFF && last2[1] == 0xD9;
    Serial.printf("OK capture bytes=%u stations=%d jpeg_soi=%d jpeg_eoi=%d\n",
                  (unsigned)total, WiFi.softAPgetStationNum(), (int)soi, (int)eoi);
    if (!soi) {
        Serial.println("FAIL: not JPEG (missing FFD8)");
        if (heap) {
            free(heap);
        }
        return -1;
    }
    if (heap_out) {
        *heap_out = heap;
        *heap_len = heap_used;
    }
    return (int)total;
}

static void mqttPublishAck(const char *job_id, const char *status, const char *detail)
{
    char topic[96];
    snprintf(topic, sizeof(topic), "fleet/v1/%s/camera/ack", s_device_id);
    char body[256];
    snprintf(body, sizeof(body),
             "{\"job_id\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\",\"device_id\":\"%s\"}",
             job_id ? job_id : "", status ? status : "", detail ? detail : "", s_device_id);
    if (lteLabReady()) {
        (void)lteLabMqttPublishAck(s_device_id, job_id, status, detail);
    }
    if (s_mqtt.connected()) {
        s_mqtt.publish(topic, body);
    }
    Serial.printf("ACK %s\n", body);
}

/** Build multipart body; caller frees. */
static uint8_t *buildMultipart(const char *job_id, int deg, const uint8_t *jpeg, size_t jpeg_len,
                               const char *boundary, size_t *out_len)
{
    String head;
    head.reserve(512);
    head += "--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"job_id\"\r\n\r\n";
    head += job_id;
    head += "\r\n--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"device_id\"\r\n\r\n";
    head += s_device_id;
    head += "\r\n--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"deg\"\r\n\r\n";
    head += String(deg);
    head += "\r\n--";
    head += boundary;
    head += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    head += job_id;
    head += ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--";
    tail += boundary;
    tail += "--\r\n";
    size_t total = head.length() + jpeg_len + tail.length();
    uint8_t *body = (uint8_t *)malloc(total);
    if (!body) {
        return nullptr;
    }
    memcpy(body, head.c_str(), head.length());
    memcpy(body + head.length(), jpeg, jpeg_len);
    memcpy(body + head.length() + jpeg_len, tail.c_str(), tail.length());
    *out_len = total;
    return body;
}

/** Multipart POST JPEG — prefer EC200U QHTTP (LTE), else Wi‑Fi HTTPClient. */
static bool uploadJpeg(const char *job_id, int deg, const uint8_t *jpeg, size_t jpeg_len)
{
    if (!jpeg || jpeg_len < 100 || !job_id) {
        return false;
    }
    if (strstr(s_upload_url, "REPLACE.ngrok")) {
        Serial.println("FAIL: set uploadurl to ngrok HTTPS URL first (LTE cannot reach LAN)");
        return false;
    }
    const char *boundary = "FleetCamLabBoundary7a3f";
    size_t body_len = 0;
    uint8_t *body = buildMultipart(job_id, deg, jpeg, jpeg_len, boundary, &body_len);
    if (!body) {
        Serial.println("FAIL: malloc multipart");
        return false;
    }

    bool ok = false;
    if (lteLabReady()) {
        Serial.printf("LTE QHTTP POST %s job_id=%s bytes=%u\n", s_upload_url, job_id,
                      (unsigned)body_len);
        ok = lteLabHttpPostMultipart(s_upload_url, body, body_len, boundary);
        Serial.printf("LTE upload %s\n", ok ? "OK" : "FAIL");
    }

    if (!ok && WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        Serial.printf("WiFi POST %s job_id=%s bytes=%u\n", s_upload_url, job_id, (unsigned)body_len);
        if (http.begin(s_upload_url)) {
            http.setTimeout(60000);
            String ctype = "multipart/form-data; boundary=";
            ctype += boundary;
            http.addHeader("Content-Type", ctype);
            int code = http.POST(body, body_len);
            String resp = http.getString();
            http.end();
            Serial.printf("upload http=%d %s\n", code, resp.c_str());
            ok = code == HTTP_CODE_OK && resp.indexOf("\"ok\":true") >= 0;
        }
    }
    free(body);
    return ok;
}

static void makeJobId(char *out, size_t out_len)
{
    snprintf(out, out_len, "job-%lu", (unsigned long)(millis() & 0xfffffffUL));
}

/** Shared lab job: capture (±deg) → upload → MQTT/Serial ACK. */
static void runCamJob(const char *job_id, int deg, bool have_deg)
{
    if (s_job_busy) {
        Serial.println("FAIL: job already running");
        mqttPublishAck(job_id, "fail", "busy");
        return;
    }
    s_job_busy = true;
    char jid[48];
    if (job_id && job_id[0]) {
        snprintf(jid, sizeof(jid), "%s", job_id);
    } else {
        makeJobId(jid, sizeof(jid));
    }
    Serial.printf("JOB start id=%s deg=%d have_deg=%d\n", jid, deg, (int)have_deg);
    mqttPublishAck(jid, "started", "capture");

    uint8_t *jpeg = nullptr;
    size_t jpeg_len = 0;
    int n = camCapture(deg, have_deg, nullptr, false, &jpeg, &jpeg_len);
    if (n < 0 || !jpeg) {
        mqttPublishAck(jid, "fail", "capture");
        s_job_busy = false;
        return;
    }
    bool up = uploadJpeg(jid, have_deg ? deg : -1, jpeg, jpeg_len);
    free(jpeg);
    if (up) {
        mqttPublishAck(jid, "uploaded", "ok");
        Serial.printf("JOB ok id=%s\n", jid);
    } else {
        mqttPublishAck(jid, "fail", "upload");
        Serial.printf("JOB FAIL upload id=%s (is Mac on SoftAP + server.py running?)\n", jid);
    }
    s_job_busy = false;
}

static void handleRoot()
{
    server.send(200, "text/plain",
                "Fleet CAM master lab (SoftAP always on)\n"
                "USB: help | job [deg] [job_id] | uploadurl | mqtthost | mqtt\n"
                "GET /api/cam/capture?deg=90\n"
                "GET /api/cam/ping\n");
}

static void handleStatus()
{
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"role\":\"master_lab\",\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"cam_ip\":\"%s\",\"stations\":%d,\"softap\":\"always_on\","
             "\"device_id\":\"%s\",\"mqtt\":%s,\"upload\":\"%s\",\"stream\":false}\n",
             MASTER_AP_SSID, WiFi.softAPIP().toString().c_str(), s_cam_ip,
             WiFi.softAPgetStationNum(), s_device_id,
             s_mqtt.connected() ? "true" : "false", s_upload_url);
    server.send(200, "application/json", buf);
}

static void handleCamPing()
{
    if (!resolveCamIp()) {
        server.send(502, "text/plain", "CAM unreachable\n");
        return;
    }
    HTTPClient http;
    String url = camBaseUrl("/status");
    if (!http.begin(url)) {
        server.send(502, "text/plain", "http begin failed\n");
        return;
    }
    http.setTimeout(5000);
    int code = http.GET();
    if (code <= 0) {
        server.send(502, "text/plain", "CAM unreachable\n");
        http.end();
        return;
    }
    String body = http.getString();
    http.end();
    server.send(code, "application/json", body);
}

static void handleCamServo()
{
    if (!server.hasArg("deg")) {
        server.send(400, "text/plain", "missing deg=0..180\n");
        return;
    }
    if (!resolveCamIp()) {
        server.send(502, "text/plain", "CAM unreachable\n");
        return;
    }
    int deg = server.arg("deg").toInt();
    HTTPClient http;
    String url = camBaseUrl("/servo");
    url += "?deg=";
    url += String(deg);
    if (!http.begin(url)) {
        server.send(502, "text/plain", "http begin failed\n");
        return;
    }
    http.setTimeout(10000);
    int code = http.GET();
    if (code <= 0) {
        server.send(502, "text/plain", "CAM unreachable\n");
        http.end();
        return;
    }
    String body = http.getString();
    http.end();
    server.send(code, "application/json", body);
}

static void handleCamCapture()
{
    bool have_deg = server.hasArg("deg");
    int deg = have_deg ? server.arg("deg").toInt() : 0;
    WiFiClient client = server.client();
    int n = camCapture(deg, have_deg, &client, false, nullptr, nullptr);
    if (n < 0 && !client.connected()) {
        return;
    }
    if (n < 0) {
        server.send(502, "text/plain", "CAM capture failed\n");
    }
}

/** Parse {"job_id":"...","deg":90} — minimal, lab-only. */
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

static void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
    char msg[192];
    unsigned int n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
    memcpy(msg, payload, n);
    msg[n] = '\0';
    Serial.printf("MQTT %s => %s\n", topic, msg);

    char job_id[48];
    int deg = 90;
    bool have_deg = false;
    if (!parseJobJson(msg, job_id, sizeof(job_id), &deg, &have_deg)) {
        Serial.println("FAIL: bad MQTT job JSON");
        mqttPublishAck("?", "fail", "bad_json");
        return;
    }
    if (!job_id[0]) {
        makeJobId(job_id, sizeof(job_id));
    }
    runCamJob(job_id, deg, true);
}

static void mqttEnsureConnected()
{
    /* Prefer LTE MQTT; Wi‑Fi PubSubClient is desk fallback only. */
    if (lteLabReady()) {
        return;
    }
    if (s_mqtt.connected()) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    uint32_t now = millis();
    if (now - s_mqtt_last_attempt_ms < 5000) {
        return;
    }
    s_mqtt_last_attempt_ms = now;
    s_mqtt.setServer(s_mqtt_host, s_mqtt_port);
    s_mqtt.setCallback(onMqttMessage);
    char client_id[40];
    snprintf(client_id, sizeof(client_id), "%s-%lu", s_device_id, (unsigned long)(now & 0xffff));
    Serial.printf("MQTT connect %s:%u as %s …\n", s_mqtt_host, (unsigned)s_mqtt_port, client_id);
    if (!s_mqtt.connect(client_id)) {
        Serial.printf("MQTT connect fail rc=%d (STA up? broker reachable?)\n", s_mqtt.state());
        return;
    }
    char topic[96];
    snprintf(topic, sizeof(topic), "fleet/v1/%s/camera/cmd", s_device_id);
    s_mqtt.subscribe(topic);
    Serial.printf("MQTT subscribed %s\n", topic);
}

static void printHelp()
{
    Serial.println("USB CLI (lab — DELETE ME with tools/cam_lab_api):");
    Serial.println("  help                 this text");
    Serial.println("  status               SoftAP + STA + upload + mqtt");
    Serial.println("  ping                 GET CAM /status");
    Serial.println("  servo <0..180>       move + hold");
    Serial.println("  capture [deg]        snap + validate JPEG");
    Serial.println("  captureb64 [deg]     snap + dump base64");
    Serial.println("  job [deg] [id]       capture → POST cam_lab_api");
    Serial.println("  lte                  EC200U AT/PDP status");
    Serial.println("  ltemqtt              EC200U MQTT subscribe (preferred)");
    Serial.println("  uploadurl <url>      ngrok HTTPS URL (required for LTE upload)");
    Serial.println("  sta <ssid> <pass>    optional Wi‑Fi STA fallback");
    Serial.println("  mqtthost / mqttport / mqtt   Wi‑Fi MQTT fallback");
    Serial.println("  resolve              re-probe CAM .10/.2");
}

static void handleCliLine(String line)
{
    line.trim();
    if (line.isEmpty()) {
        return;
    }

    String lower = line;
    lower.toLowerCase();

    if (lower == "help" || lower == "?") {
        printHelp();
        return;
    }
    if (lower == "status") {
        printStatus();
        return;
    }
    if (lower == "ping") {
        camPing();
        return;
    }
    if (lower == "resolve") {
        (void)resolveCamIp();
        printStatus();
        return;
    }
    if (lower == "lte") {
        if (!lteLabReady()) {
            (void)lteLabBegin();
        }
        (void)lteLabEnsurePdp();
        lteLabStatus();
        return;
    }
    if (lower == "ltemqtt") {
        if (!lteLabReady()) {
            (void)lteLabBegin();
        }
        bool ok = lteLabMqttStart(s_device_id);
        Serial.printf("ltemqtt %s\n", ok ? "OK" : "FAIL");
        return;
    }
    if (lower == "mqtt") {
        s_mqtt.disconnect();
        s_mqtt_last_attempt_ms = 0;
        mqttEnsureConnected();
        return;
    }
    if (lower.startsWith("servo ")) {
        int deg = lower.substring(6).toInt();
        camServo(deg);
        return;
    }
    if (lower == "capture") {
        (void)camCapture(0, false, nullptr, false, nullptr, nullptr);
        return;
    }
    if (lower.startsWith("capture ")) {
        int deg = lower.substring(8).toInt();
        (void)camCapture(deg, true, nullptr, false, nullptr, nullptr);
        return;
    }
    if (lower == "captureb64") {
        (void)camCapture(0, false, nullptr, true, nullptr, nullptr);
        return;
    }
    if (lower.startsWith("captureb64 ")) {
        int deg = lower.substring(11).toInt();
        (void)camCapture(deg, true, nullptr, true, nullptr, nullptr);
        return;
    }
    if (lower.startsWith("uploadurl ")) {
        String u = line.substring(10);
        u.trim();
        if (u.length() < 8 || u.length() >= (int)sizeof(s_upload_url)) {
            Serial.println("FAIL: bad uploadurl");
            return;
        }
        snprintf(s_upload_url, sizeof(s_upload_url), "%s", u.c_str());
        Serial.printf("uploadurl=%s\n", s_upload_url);
        return;
    }
    if (lower.startsWith("mqtthost ")) {
        String h = line.substring(9);
        h.trim();
        if (h.length() < 3 || h.length() >= (int)sizeof(s_mqtt_host)) {
            Serial.println("FAIL: bad mqtthost");
            return;
        }
        snprintf(s_mqtt_host, sizeof(s_mqtt_host), "%s", h.c_str());
        s_mqtt.disconnect();
        s_mqtt_last_attempt_ms = 0;
        Serial.printf("mqtthost=%s\n", s_mqtt_host);
        return;
    }
    if (lower.startsWith("mqttport ")) {
        int p = lower.substring(9).toInt();
        if (p <= 0 || p > 65535) {
            Serial.println("FAIL: bad mqttport");
            return;
        }
        s_mqtt_port = (uint16_t)p;
        s_mqtt.disconnect();
        s_mqtt_last_attempt_ms = 0;
        Serial.printf("mqttport=%u\n", (unsigned)s_mqtt_port);
        return;
    }
    if (lower.startsWith("sta ")) {
        /* sta <ssid> <pass> — pass may contain spaces only if ssid is one token */
        String rest = line.substring(4);
        rest.trim();
        int sp = rest.indexOf(' ');
        if (sp <= 0) {
            Serial.println("FAIL: usage sta <ssid> <pass>");
            return;
        }
        String ssid = rest.substring(0, sp);
        String pass = rest.substring(sp + 1);
        ssid.trim();
        pass.trim();
        (void)startSta(ssid.c_str(), pass.c_str(), 20000);
        return;
    }
    if (lower == "job") {
        runCamJob(nullptr, 90, true);
        return;
    }
    if (lower.startsWith("job ")) {
        /* job <deg> [job_id] — job_id keeps original case from line */
        String rest = line.substring(4);
        rest.trim();
        int sp = rest.indexOf(' ');
        int deg;
        String jid;
        if (sp < 0) {
            deg = rest.toInt();
        } else {
            deg = rest.substring(0, sp).toInt();
            jid = rest.substring(sp + 1);
            jid.trim();
        }
        if (deg < 0) {
            deg = 0;
        }
        if (deg > 180) {
            deg = 180;
        }
        runCamJob(jid.length() ? jid.c_str() : nullptr, deg, true);
        return;
    }
    Serial.printf("unknown: %s  (type help)\n", line.c_str());
}

static void pollCli()
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            handleCliLine(s_cli_line);
            s_cli_line = "";
            continue;
        }
        if (s_cli_line.length() < 160) {
            s_cli_line += c;
        }
    }
}

static void startSoftAp()
{
    /* SoftAP for CAM only — internet via EC200U LTE (not Wi‑Fi STA). */
    WiFi.mode(WIFI_AP);
    WiFi.softAP(MASTER_AP_SSID, MASTER_AP_PASS, /*channel=*/1, /*ssid_hidden=*/0,
                /*max_connection=*/4);
    delay(150);
    Serial.printf("SoftAP ALWAYS ON \"%s\" / \"%s\" ip=%s\n", MASTER_AP_SSID, MASTER_AP_PASS,
                  WiFi.softAPIP().toString().c_str());
    Serial.printf("CAM preferred %s (legacy fallback %s) — CAM USB not required\n", CAM_STA_IP,
                  CAM_STA_IP_LEGACY);
    Serial.printf("Lab upload default %s\n", s_upload_url);
    Serial.printf("Lab MQTT default %s:%u topic fleet/v1/%s/camera/cmd\n", s_mqtt_host,
                  (unsigned)s_mqtt_port, s_device_id);
    Serial.println("HINT: Prefer LTE — USB: lte | ltemqtt | uploadurl https://…ngrok…/lab/camera/upload");
}

void setup()
{
    Serial.begin(115200);
    delay(400);
    loadOrProvisionDeviceId();
    Serial.println();
    Serial.println("=== fleet camera master_lab (spare C6 only) ===");
    Serial.println("Does NOT run OBD/GPS/Zigbee/uplink — lab SoftAP bridge only.");
    Serial.println("DELETE ME later: MQTT trigger + cam_lab_api upload.");

    startSoftAp();

    Serial.println("Starting EC200U lab LTE…");
    if (lteLabBegin()) {
        (void)lteLabEnsurePdp();
        (void)lteLabMqttStart(s_device_id);
    }

    s_mqtt.setBufferSize(256);
    s_mqtt.setSocketTimeout(5);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/api/cam/ping", HTTP_GET, handleCamPing);
    server.on("/api/cam/servo", HTTP_GET, handleCamServo);
    server.on("/api/cam/capture", HTTP_GET, handleCamCapture);
    server.begin();

    printHelp();
    Serial.println("Waiting for CAM on SoftAP… then: ping | job 90");
}

void loop()
{
    server.handleClient();
    pollCli();

    char job_id[48];
    int deg = 90;
    bool have_deg = false;
    if (!s_job_busy && lteLabMqttPollJob(job_id, sizeof(job_id), &deg, &have_deg)) {
        if (!job_id[0]) {
            makeJobId(job_id, sizeof(job_id));
        }
        runCamJob(job_id, deg, true);
    }

    mqttEnsureConnected();
    s_mqtt.loop();
}
