/**
 * Lab ESP32-CAM — stills only (no MJPEG / no continuous stream).
 *
 * Modes (platformio.ini envs):
 *   softap_lab  — CAM is SoftAP Fleet-CAM @ 192.168.4.1
 *   sta_master  — CAM joins master SoftAP Fleet-C6 as STA @ 192.168.4.10
 *
 * Servo (SG90 / MG996R): GET /capture?deg=90 → move → settle → JPEG
 *   Last angle is kept (PWM stays on) and stored in NVS so it survives reboot.
 *
 * Reboot after curl is usually a brownout (UXGA + Wi‑Fi + servo on weak 5 V).
 * This build uses HD stills, lower TX power, and brownout detector off for lab.
 * STA IP is .10 so SoftAP DHCP can give .2 to a phone without clashing.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <esp_camera.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include "camera_pins.h"

#ifndef CAM_WIFI_SSID
#define CAM_WIFI_SSID "Fleet-CAM"
#endif
#ifndef CAM_WIFI_PASS
#define CAM_WIFI_PASS "fleetcams"
#endif
#ifndef CAM_SERVO_PIN
#define CAM_SERVO_PIN 13
#endif
/* Base damp time after the horn arrives (vibration / lens settle). */
#ifndef CAM_SERVO_SETTLE_MS
#define CAM_SERVO_SETTLE_MS 1000
#endif
/* Extra wait proportional to travel — MG996R ~0.17 s/60° ≈ 3 ms/deg; use margin. */
#ifndef CAM_SERVO_MS_PER_DEG
#define CAM_SERVO_MS_PER_DEG 8
#endif
#ifndef CAM_SERVO_MIN_US
#define CAM_SERVO_MIN_US 500
#endif
#ifndef CAM_SERVO_MAX_US
#define CAM_SERVO_MAX_US 2400
#endif
#ifndef CAM_STA_IP_A
#define CAM_STA_IP_A 192
#endif
#ifndef CAM_STA_IP_B
#define CAM_STA_IP_B 168
#endif
#ifndef CAM_STA_IP_C
#define CAM_STA_IP_C 4
#endif
#ifndef CAM_STA_IP_D
#define CAM_STA_IP_D 10
#endif

/* HD = good quality without UXGA peak current that browns out FTDI-powered CAMs. */
static const framesize_t kFrameSize = FRAMESIZE_HD; /* 1280x720 */
static const int kJpegQuality = 12;

static WebServer server(80);
static Preferences prefs;
static Servo s_servo;
static int s_servo_deg = 90;
static bool s_servo_ok = false;

static int clampDeg(int deg)
{
    if (deg < 0) {
        return 0;
    }
    if (deg > 180) {
        return 180;
    }
    return deg;
}

static void saveServoDeg(int deg)
{
    s_servo_deg = clampDeg(deg);
    prefs.putUChar("deg", (uint8_t)s_servo_deg);
}

static void loadServoDeg()
{
    s_servo_deg = clampDeg((int)prefs.getUChar("deg", 90));
}

static bool parseDegParam(int *out_deg)
{
    if (!server.hasArg("deg")) {
        return false;
    }
    *out_deg = clampDeg(server.arg("deg").toInt());
    return true;
}

static void holdServo()
{
    if (s_servo_ok) {
        s_servo.write(s_servo_deg);
    }
}

static void settleMs(uint32_t ms)
{
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) {
        delay(20);
        yield();
        holdServo();
    }
}

static void moveServo(int deg)
{
    deg = clampDeg(deg);
    const int from = s_servo_deg;
    const int delta = abs(deg - from);
    if (!s_servo_ok) {
        Serial.println("servo not attached — skip move");
        saveServoDeg(deg);
        return;
    }
    s_servo.write(deg);
    saveServoDeg(deg);
    /* Wait for travel to finish, then damp — capture must see a still frame. */
    uint32_t settle = 0;
    if (delta > 0) {
        settle = (uint32_t)CAM_SERVO_SETTLE_MS + (uint32_t)delta * (uint32_t)CAM_SERVO_MS_PER_DEG;
    }
    Serial.printf("servo -> %d deg (from %d, d=%d), settle %u ms then hold\n", deg, from,
                  delta, (unsigned)settle);
    if (settle > 0) {
        settleMs(settle);
    }
    holdServo();
}

static bool initServo()
{
    /* Camera XCLK uses LEDC timer 0 / channel 0 — leave those alone. */
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    s_servo.setPeriodHertz(50);
    /* Clear any stale LEDC claim on the pin (common after soft reboot). */
    s_servo.detach();
    delay(20);
    if (!s_servo.attach(CAM_SERVO_PIN, CAM_SERVO_MIN_US, CAM_SERVO_MAX_US)) {
        Serial.printf("servo attach failed on GPIO%d\n", CAM_SERVO_PIN);
        return false;
    }
    /* Restore last commanded angle — do not snap back to 90 on boot. */
    s_servo.write(s_servo_deg);
    settleMs(300);
    Serial.printf("servo ready GPIO%d holding %d deg (%d-%dus)\n", CAM_SERVO_PIN,
                  s_servo_deg, CAM_SERVO_MIN_US, CAM_SERVO_MAX_US);
    return true;
}

static bool initCamera()
{
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM;
    cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM;
    cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM;
    cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM;
    cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM;
    cfg.pin_href = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn = PWDN_GPIO_NUM;
    cfg.pin_reset = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = kFrameSize;
    cfg.jpeg_quality = kJpegQuality;
    cfg.fb_count = 1;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("camera init failed: 0x%x — retry SVGA\n", (unsigned)err);
        cfg.frame_size = FRAMESIZE_SVGA;
        err = esp_camera_init(&cfg);
        if (err != ESP_OK) {
            Serial.printf("camera init failed again: 0x%x\n", (unsigned)err);
            return false;
        }
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s, 0);
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
        s->set_sharpness(s, 1);
        s->set_whitebal(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_awb_gain(s, 1);
        s->set_aec2(s, 1);
    }
    return true;
}

static void handleRoot()
{
    server.send(200, "text/plain",
                "Fleet CAM lab\n"
                "GET /status\n"
                "GET /servo?deg=90     -> move + HOLD angle\n"
                "GET /capture          -> one JPEG (keeps angle)\n"
                "GET /capture?deg=90   -> move, HOLD, then JPEG\n");
}

static void handleStatus()
{
    IPAddress ip =
#ifdef CAM_STA_MODE
        WiFi.localIP();
#else
        WiFi.softAPIP();
#endif
    char buf[300];
    snprintf(buf, sizeof(buf),
             "{\"ready\":true,\"ssid\":\"%s\",\"ip\":\"%s\",\"mode\":\"%s\","
             "\"stream\":false,\"quality\":%d,\"servo_ok\":%s,\"servo_pin\":%d,"
             "\"servo_deg\":%d,\"hold\":true}\n",
             CAM_WIFI_SSID, ip.toString().c_str(),
#ifdef CAM_STA_MODE
             "sta",
#else
             "softap",
#endif
             kJpegQuality, s_servo_ok ? "true" : "false", CAM_SERVO_PIN, s_servo_deg);
    server.send(200, "application/json", buf);
}

static void handleServo()
{
    int deg = s_servo_deg;
    if (!parseDegParam(&deg)) {
        server.send(400, "text/plain", "missing deg=0..180\n");
        return;
    }
    moveServo(deg);
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"servo_deg\":%d,\"hold\":true}\n",
             s_servo_deg);
    server.send(200, "application/json", buf);
}

static void handleCapture()
{
    int deg = s_servo_deg;
    if (parseDegParam(&deg)) {
        moveServo(deg);
    } else {
        holdServo();
    }

    camera_fb_t *warm = esp_camera_fb_get();
    if (warm) {
        esp_camera_fb_return(warm);
    }
    holdServo();

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb || !fb->buf || fb->len == 0) {
        if (fb) {
            esp_camera_fb_return(fb);
        }
        holdServo();
        server.send(503, "text/plain", "capture failed\n");
        return;
    }

    const size_t len = fb->len;
    server.setContentLength(len);
    server.sendHeader("X-Servo-Deg", String(s_servo_deg));
    server.send(200, "image/jpeg", "");
    WiFiClient client = server.client();

    /* Chunked write — one giant write + SoftAP TX often browns out the CAM. */
    const uint8_t *p = fb->buf;
    size_t left = len;
    while (left > 0 && client.connected()) {
        size_t n = left > 1024 ? 1024 : left;
        size_t w = client.write(p, n);
        if (w == 0) {
            delay(1);
            yield();
            continue;
        }
        p += w;
        left -= w;
        yield();
    }
    esp_camera_fb_return(fb);
    holdServo();
    Serial.printf("capture ok len=%u deg=%d (holding)\n", (unsigned)len, s_servo_deg);
}

#ifdef CAM_STA_MODE
/** Join SoftAP with static .10 (DHCP on SoftAP typically hands out .2 to phones). */
static bool startStaOnce(uint32_t timeout_ms)
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    IPAddress local(CAM_STA_IP_A, CAM_STA_IP_B, CAM_STA_IP_C, CAM_STA_IP_D);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    if (!WiFi.config(local, gateway, subnet)) {
        Serial.println("WiFi.config static IP failed");
    }

    Serial.printf("STA joining \"%s\" as %u.%u.%u.%u …\n", CAM_WIFI_SSID,
                  (unsigned)CAM_STA_IP_A, (unsigned)CAM_STA_IP_B, (unsigned)CAM_STA_IP_C,
                  (unsigned)CAM_STA_IP_D);
    WiFi.disconnect(false, false);
    delay(50);
    WiFi.begin(CAM_WIFI_SSID, CAM_WIFI_PASS);

    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(250);
        Serial.print('.');
        holdServo();
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("STA join FAILED — will retry (is master SoftAP up?)");
        return false;
    }
    Serial.printf("STA ok ip=%s gw=%s rssi=%d\n", WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    return true;
}

/** Never halt on Wi‑Fi — SoftAP may come up later (C6 reflash / power order). */
static void startSta()
{
    uint32_t attempt = 0;
    while (!startStaOnce(20000)) {
        attempt++;
        Serial.printf("STA retry #%u in 3s\n", (unsigned)attempt);
        WiFi.disconnect(true, false);
        delay(3000);
        holdServo();
    }
}
#else
static void startSoftAp()
{
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.softAP(CAM_WIFI_SSID, CAM_WIFI_PASS);
    delay(100);
    Serial.printf("SoftAP \"%s\" ip=%s\n", CAM_WIFI_SSID,
                  WiFi.softAPIP().toString().c_str());
}
#endif

void setup()
{
    /* Lab: FTDI 5 V often can't sustain UXGA + Wi‑Fi + servo → brownout reboot. */
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("fleet_v2 camera lab — stills + servo HOLD");

    prefs.begin("camservo", false);
    loadServoDeg();
    Serial.printf("restored servo_deg=%d from NVS\n", s_servo_deg);

    if (!initCamera()) {
        Serial.println("HALT: camera init failed (check 5V PSRAM board + wiring)");
        while (true) {
            delay(1000);
        }
    }
    Serial.println("camera ready (HD)");

    s_servo_ok = initServo();
    if (!s_servo_ok) {
        Serial.println("WARN: servo attach failed — /capture still works without deg move");
    }

#ifdef CAM_STA_MODE
    startSta();
#else
    startSoftAp();
#endif

    server.on("/", HTTP_GET, handleRoot);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/servo", HTTP_GET, handleServo);
    server.on("/capture", HTTP_GET, handleCapture);
    server.begin();

#ifdef CAM_STA_MODE
    Serial.printf("HTTP up — GET http://%u.%u.%u.%u/capture?deg=90\n",
                  (unsigned)CAM_STA_IP_A, (unsigned)CAM_STA_IP_B, (unsigned)CAM_STA_IP_C,
                  (unsigned)CAM_STA_IP_D);
#else
    Serial.println("HTTP up — curl -o shot.jpg 'http://192.168.4.1/capture?deg=90'");
#endif
}

void loop()
{
    server.handleClient();
    /* Refresh PWM so SG90 keeps holding under light load. */
    static uint32_t last_hold;
    if (s_servo_ok && millis() - last_hold > 2000) {
        last_hold = millis();
        holdServo();
    }
#ifdef CAM_STA_MODE
    if (WiFi.status() != WL_CONNECTED) {
        static uint32_t last;
        if (millis() - last > 8000) {
            last = millis();
            Serial.println("WiFi lost — full STA rejoin");
            (void)startStaOnce(15000);
        }
    }
#endif
}
