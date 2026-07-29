# LTE Telemetry Cloud Uplink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** POST live OBD snapshots to `https://api.trafyn.info/nc-events-api/v2/messages` over Quectel EC200U HTTP AT, with SoftAP enable/interval/identity controls (NVS), uplink status on the web UI, and no misleading PID values (e.g. speed 255 when stale/missing).

**Architecture:** New `telemetry_uplink` component caches PID samples from `telemetry_bus`, builds JSON, gates on live OBD + enable flag, and calls `net_lte_http_post()`. SoftAP exposes `/api/uplink*`. Invalid/stale samples render as `-` / JSON `null`.

**Tech Stack:** ESP-IDF C, FreeRTOS, cJSON, NVS (`elm`), Quectel QHTTP*/QSSLCFG AT, existing SoftAP httpd UI.

## Global Constraints

- URL hardcoded: `https://api.trafyn.info/nc-events-api/v2/messages`
- `schemaId` hardcoded: `"1087"`
- Enable default **off**; interval default **5 s**, clamp **1–300**
- Gate: `ble_connected && elm_ready && poller_on && ≥1 fresh ok sample` (age ≤ 15 s)
- Missing/stale PIDs: JSON `null` + `*_ok: false`; SoftAP shows `-`
- Legitimate speed `0` with `ok=true` is allowed
- No PPP; no auth headers
- NVS keys in namespace `elm`: `uplink_enabled`, `uplink_interval_s`, `uplink_device_id`, `uplink_node_id`

## File map

| File | Role |
|------|------|
| `components/net_lte/include/net_lte.h` | Add `net_lte_http_post` + result struct |
| `components/net_lte/net_lte.c` | PDP ensure + HTTPS POST via QHTTP* |
| `components/telemetry_uplink/*` | Config, cache, JSON, task, last_result |
| `components/transport_http/http_api.c` | `/api/uplink` GET/POST, `/api/uplink/send` |
| `components/transport_http/static_index.html.h` | Cloud uplink card + refresh; stale live `-` |
| `components/transport_http/CMakeLists.txt` | Require `telemetry_uplink` |
| `main/app_main.c` + `main/CMakeLists.txt` | Start uplink |
| `tests/host/test_obd_codec.c` | Speed stale/invalid expectations if codec tightened |
| `tests/host/test_uplink_payload.c` | Pure payload null/`ok` rules |
| `PROJECT_CONTEXT.md` | Document uplink |

---

### Task 1: Speed/stale display + codec guard

**Files:**
- Modify: `components/obd_codec/obd_codec.c` (speed PID: require enough bytes; keep `ok` only on successful parse)
- Modify: `components/transport_http/static_index.html.h` (`setLive` treats missing/`!ok`/age>15s as `-`)
- Modify: `tests/host/test_obd_codec.c` (assert speed `410D00` → 0 ok; incomplete response → fail)

- [ ] **Step 1:** In SoftAP `setLive`, if `!sample || !sample.ok || (sample.age_ms != null && sample.age_ms > 15000)` show `-`.
- [ ] **Step 2:** Ensure codec returns false (no `ok`) when mode01 frame incomplete for speed.
- [ ] **Step 3:** Host test `test_obd_codec` still passes; add parked speed 0 case.

### Task 2: `net_lte_http_post`

**Files:**
- Modify: `components/net_lte/include/net_lte.h`
- Modify: `components/net_lte/net_lte.c`

**Produces:**
```c
typedef struct {
    int http_status;   /* 0 if unknown */
    char error[96];
} net_lte_http_result_t;

esp_err_t net_lte_http_post(const char *url, const char *body,
                           net_lte_http_result_t *out);
```

- [ ] **Step 1:** Under UART mutex: ensure PDP (`QICSGP`/`QIACT`), SSL (`QHTTPCFG`/`QSSLCFG` seclevel 0 for demo), `contenttype` application/json, `requestheader` 0.
- [ ] **Step 2:** `QHTTPURL` length + URL bytes; wait CONNECT/OK.
- [ ] **Step 3:** `QHTTPPOST=<len>,80,80`; wait CONNECT; write body; parse `+QHTTPPOST: 0,<status>,...`; 2xx → `ESP_OK`.
- [ ] **Step 4:** Stub returns `ESP_ERR_NOT_SUPPORTED` when `CONFIG_NET_LTE_ENABLE=n`.

### Task 3: `telemetry_uplink` component

**Files:**
- Create: `components/telemetry_uplink/include/telemetry_uplink.h`
- Create: `components/telemetry_uplink/telemetry_uplink.c`
- Create: `components/telemetry_uplink/uplink_payload.c` (pure builder used by device + host test)
- Create: `components/telemetry_uplink/include/uplink_payload.h`
- Create: `components/telemetry_uplink/CMakeLists.txt`

**Produces:**
```c
esp_err_t telemetry_uplink_start(void);
esp_err_t telemetry_uplink_get_config(...);
esp_err_t telemetry_uplink_set_config(...); /* NVS */
esp_err_t telemetry_uplink_get_status(...); /* config + last */
esp_err_t telemetry_uplink_send_now(void);
```

- [ ] **Step 1:** NVS load/save config defaults.
- [ ] **Step 2:** Subscribe PID samples; keep latest-by-name cache.
- [ ] **Step 3:** Interval task + `send_now`: gate → build JSON → `net_lte_http_post` → store last_result.
- [ ] **Step 4:** Host-testable `uplink_payload_build()` with null/`ok` rules (15 s freshness).

### Task 4: SoftAP REST + UI

**Files:**
- Modify: `http_api.c`, `static_index.html.h`, `CMakeLists.txt`

- [ ] **Step 1:** Register GET/POST `/api/uplink`, POST `/api/uplink/send`.
- [ ] **Step 2:** UI card: enable, interval, device_id, node_id, Save, Send now, status pills.
- [ ] **Step 3:** Poll `/api/uplink` in refresh loop.

### Task 5: Wire boot + docs + host tests

**Files:**
- Modify: `main/app_main.c`, `main/CMakeLists.txt`, `tests/host/CMakeLists.txt`, `PROJECT_CONTEXT.md`

- [ ] **Step 1:** `telemetry_uplink_start()` after telemetry + net_lte.
- [ ] **Step 2:** Add `test_uplink_payload` to host ctest.
- [ ] **Step 3:** Update PROJECT_CONTEXT uplink section.
- [ ] **Step 4:** Run host tests; build firmware if IDF available.

## Spec coverage

| Spec item | Task |
|-----------|------|
| Quectel HTTP POST | 2 |
| Enable/interval/device/node NVS | 3–4 |
| Gate + payload | 3 |
| SoftAP status + controls | 4 |
| Speed/misleading values | 1 + 3 |
| Boot wire | 5 |
