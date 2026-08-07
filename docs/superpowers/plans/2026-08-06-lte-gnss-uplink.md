# LTE GNSS lat/lng on OBD uplink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cache EC200U GNSS in `net_lte` and embed `gps_ok` / `lat` / `lng` inside the Trafyn schema 1087 `payload` on every OBD uplink (omit lat/lng when no fix).

**Architecture:** Background GNSS refresh under the existing modem UART mutex writes a RAM cache. `telemetry_uplink` copies that cache when building the snapshot JSON — never blocks produce on a live GPS AT. Same omit-null policy as stale PIDs.

**Tech Stack:** ESP-IDF, Quectel EC200U `AT+QGPS` / `AT+QGPSLOC`, existing `uplink_payload` builder, host C tests under `tests/host/`.

**Spec:** `docs/superpowers/specs/2026-08-06-lte-gnss-uplink-design.md`

## Global Constraints

- JSON keys inside `payload` only: `gps_ok`, and when fix exists `lat`, `lng` (separate numbers).
- No fix: `gps_ok: false`; omit `lat`/`lng` (never JSON null).
- OBD uplink never waits for a GPS fix.
- GNSS AT shares `s_uart_mutex`; skip refresh while `fw_ota_lte_is_busy()`.
- Max GPS age before treating as no-fix: 120 s (Kconfig).
- Refresh interval default: 10 s (Kconfig).

## File map

| File | Role |
|------|------|
| `components/net_lte/include/net_lte.h` | `net_lte_gps_t`, `net_lte_gps_get()` |
| `components/net_lte/net_lte.c` | QGPS enable + refresh task + cache |
| `components/net_lte/Kconfig` | GPS enable, interval, max age |
| `components/telemetry_uplink/include/uplink_payload.h` | GPS fields on `uplink_snapshot_t` |
| `components/telemetry_uplink/uplink_payload.c` | Emit GPS keys in payload JSON |
| `components/telemetry_uplink/telemetry_uplink.c` | Fill snapshot from `net_lte_gps_get()` |
| `tests/host/test_uplink_payload.c` | Assert GPS emit/omit |
| `components/transport_serial/transport_serial.c` | Optional: show GPS on `uplink` / `lte` status |

---

### Task 1: Payload builder — GPS fields inside `payload`

**Files:**
- Modify: `components/telemetry_uplink/include/uplink_payload.h`
- Modify: `components/telemetry_uplink/uplink_payload.c`
- Modify: `tests/host/test_uplink_payload.c`
- Test: host `test_uplink_payload`

**Interfaces:**
- Produces: `uplink_snapshot_t` gains `bool gps_ok; double lat; double lng;` (always set `gps_ok`; lat/lng only meaningful when true)
- Consumes: existing `uplink_payload_build_payload()`

- [ ] **Step 1: Extend snapshot struct**

In `uplink_payload.h`, add after voltage PID fields:

```c
    bool gps_ok;
    double lat;
    double lng;
```

- [ ] **Step 2: Write failing host assertions**

Append to `tests/host/test_uplink_payload.c` before the final PASS printf:

```c
    snap.gps_ok = true;
    snap.lat = 12.9716;
    snap.lng = 77.5946;
    n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"gps_ok\":true") != NULL);
    assert(strstr(buf, "\"lat\":12.9716") != NULL || strstr(buf, "\"lat\":12.971") != NULL);
    assert(strstr(buf, "\"lng\":77.5946") != NULL || strstr(buf, "\"lng\":77.594") != NULL);
    /* Must be inside payload, not next to schemaId only */
    assert(strstr(buf, "\"payload\":{") != NULL);

    snap.gps_ok = false;
    n = uplink_payload_build(&snap, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "\"gps_ok\":false") != NULL);
    assert(strstr(buf, "\"lat\":") == NULL);
    assert(strstr(buf, "\"lng\":") == NULL);
```

- [ ] **Step 3: Run host test — expect FAIL**

```bash
cmake -S tests/host -B build-host-tests && cmake --build build-host-tests --target test_uplink_payload
./build-host-tests/test_uplink_payload
```

Expected: assert fail (no `gps_ok` in JSON yet).

- [ ] **Step 4: Emit GPS in `uplink_payload_build_payload`**

Before the final `,"source":"esp32_obd"}` append in `uplink_payload.c`, add:

```c
    if (appendf(out, out_len, &off, ",\"gps_ok\":%s",
                snap->gps_ok ? "true" : "false") != 0) {
        return -1;
    }
    if (snap->gps_ok) {
        if (appendf(out, out_len, &off, ",\"lat\":%.7f,\"lng\":%.7f",
                    snap->lat, snap->lng) != 0) {
            return -1;
        }
    }
```

- [ ] **Step 5: Run host test — expect PASS**

```bash
./build-host-tests/test_uplink_payload
```

Expected: `test_uplink_payload: PASS`

- [ ] **Step 6: Commit**

```bash
git add components/telemetry_uplink/include/uplink_payload.h \
        components/telemetry_uplink/uplink_payload.c \
        tests/host/test_uplink_payload.c
git commit -m "$(cat <<'EOF'
feat(uplink): embed gps_ok/lat/lng inside telemetry payload JSON

EOF
)"
```

---

### Task 2: `net_lte` GNSS cache + refresh

**Files:**
- Modify: `components/net_lte/include/net_lte.h`
- Modify: `components/net_lte/net_lte.c` (enabled path and `#else` stubs)
- Modify: `components/net_lte/Kconfig`

**Interfaces:**
- Produces: `net_lte_gps_t`, `esp_err_t net_lte_gps_get(net_lte_gps_t *out)`
- Consumes: existing `s_uart_mutex`, AT helpers, `fw_ota_lte_is_busy()` (add REQUIRES if needed)

- [ ] **Step 1: Kconfig**

Add under `components/net_lte/Kconfig` (depends on `NET_LTE_ENABLE`):

```
config NET_LTE_GPS_ENABLE
    bool "Enable EC200U GNSS for uplink lat/lng"
    default y
    depends on NET_LTE_ENABLE

config NET_LTE_GPS_REFRESH_S
    int "GNSS refresh interval (seconds)"
    default 10
    range 5 120
    depends on NET_LTE_GPS_ENABLE

config NET_LTE_GPS_MAX_AGE_S
    int "Max GNSS fix age before gps_ok=false (seconds)"
    default 120
    range 30 600
    depends on NET_LTE_GPS_ENABLE
```

- [ ] **Step 2: Public API in `net_lte.h`**

```c
typedef struct {
    bool gps_ok;
    double lat;
    double lng;
    uint32_t age_ms;
} net_lte_gps_t;

/** Copy latest GNSS cache. gps_ok false if never fixed or older than max age. */
esp_err_t net_lte_gps_get(net_lte_gps_t *out);
```

- [ ] **Step 3: Cache + parse + task in `net_lte.c` (enabled build)**

Add static cache under mutex protection (or copy under `s_uart_mutex` / a small GPS mutex):

```c
static bool s_gps_ok;
static double s_gps_lat, s_gps_lng;
static int64_t s_gps_fix_ms; /* esp_timer ms when last good fix stored */
```

Quectel locate (typical): after `AT+QGPS=1` success, poll `AT+QGPSLOC=2` (decimal degrees). Parse `+QGPSLOC: ...` lat/lng fields (verify against EC200U AT manual for your FW — adjust field indices if needed).

On parse success: set `s_gps_ok=true`, store lat/lng, stamp `s_gps_fix_ms = esp_timer_get_time()/1000`.

`net_lte_gps_get`:
- If `!s_gps_ok` or age > `CONFIG_NET_LTE_GPS_MAX_AGE_S * 1000` → `out->gps_ok=false`, lat/lng 0.
- Else copy lat/lng and age.

Start a low-priority task after bring-up AT OK (from `bringup_task` success path or `net_lte_start`):

```c
static void gps_task(void *arg) {
    (void)arg;
    // wait until uart ready
    // AT+QGPS=1 once (ignore already-on errors)
    for (;;) {
        if (!fw_ota_lte_is_busy()) {
            // take mutex, AT+QGPSLOC=2, parse, update cache
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NET_LTE_GPS_REFRESH_S * 1000));
    }
}
```

Stub `net_lte_gps_get` in `#else` (`CONFIG_NET_LTE_ENABLE=n`) returning `gps_ok=false`.

Wire `fw_ota_lte` into `net_lte` CMake `REQUIRES` / `PRIV_REQUIRES` only if needed for `fw_ota_lte_is_busy`; alternatively skip OTA check and only rely on UART mutex (acceptable if mutex already serializes). Prefer OTA skip per spec.

- [ ] **Step 4: Build firmware**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

```bash
git add components/net_lte/
git commit -m "$(cat <<'EOF'
feat(net_lte): cache EC200U GNSS fix for telemetry uplink

EOF
)"
```

---

### Task 3: Wire GPS into uplink produce path

**Files:**
- Modify: `components/telemetry_uplink/telemetry_uplink.c` (`build_snapshot` / `produce_once`)

**Interfaces:**
- Consumes: `net_lte_gps_get()`
- Produces: snapshot GPS fields filled before `uplink_payload_build_payload`

- [ ] **Step 1: Fill GPS when building snapshot**

In `build_snapshot` (or immediately after it in `produce_once` before JSON build):

```c
    net_lte_gps_t gps;
    memset(&gps, 0, sizeof(gps));
    if (net_lte_gps_get(&gps) != ESP_OK) {
        gps.gps_ok = false;
    }
    snap->gps_ok = gps.gps_ok;
    snap->lat = gps.lat;
    snap->lng = gps.lng;
```

Ensure `memset` of snapshot zeroes GPS when get fails.

- [ ] **Step 2: Build + flash smoke**

```bash
idf.py build flash -p /dev/cu.usbmodem1201
```

Serial: `uplink` status still works. With no antenna expect produce still enqueues OBD; payload has `gps_ok:false` (confirm via lab log or temporary ESP_LOGI of JSON length/snippet).

- [ ] **Step 3: Commit**

```bash
git add components/telemetry_uplink/telemetry_uplink.c
git commit -m "$(cat <<'EOF'
feat(uplink): stamp cached GNSS into OBD snapshot payload

EOF
)"
```

---

### Task 4: Serial visibility + device check

**Files:**
- Modify: `components/transport_serial/transport_serial.c` (`cmd_uplink` and/or `cmd_lte`)

- [ ] **Step 1: Print GPS on `uplink` status**

```c
    net_lte_gps_t gps;
    if (net_lte_gps_get(&gps) == ESP_OK) {
        if (gps.gps_ok) {
            printf("        gps: ok lat=%.6f lng=%.6f age_ms=%u\n",
                   gps.lat, gps.lng, (unsigned)gps.age_ms);
        } else {
            printf("        gps: no fix\n");
        }
    }
```

Add `net_lte` to serial component REQUIRES if not already present.

- [ ] **Step 2: Device verification**

1. Indoor / no GNSS antenna: `uplink` → `gps: no fix`; queue/produce still OK.
2. Outdoor with antenna (when available): wait up to a few minutes cold start → `gps: ok lat=... lng=...`.
3. Confirm OTA busy path does not deadlock (start OTA if practical; otherwise code review mutex).

- [ ] **Step 3: Update PROJECT_CONTEXT.md one-liner under LTE uplink** mentioning GPS fields in payload.

- [ ] **Step 4: Commit**

```bash
git add components/transport_serial/ PROJECT_CONTEXT.md
git commit -m "$(cat <<'EOF'
feat: show GNSS cache on serial uplink status

EOF
)"
```

---

## Spec coverage check

| Spec item | Task |
|-----------|------|
| Cache GNSS in net_lte | Task 2 |
| lat/lng/gps_ok inside payload | Task 1 + 3 |
| Omit lat/lng when no fix | Task 1 |
| Don’t block OBD on GPS | Task 2 + 3 |
| Skip refresh during LTE OTA | Task 2 |
| Host unit test | Task 1 |
| Device test indoor/outdoor | Task 4 |

## Placeholder scan

None intentional — QGPSLOC field indices must be verified against the EC200U AT manual for the module FW on the bench; adjust parser in Task 2 if the URC layout differs.
