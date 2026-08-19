# Trafyn Firmware-Check OTA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace lab GET `/firmware/manifest` (ngrok) and SoftAP `.bin` upload with Trafyn POST `get-latest-device-firmware` over LTE, then stream the presigned S3 URL through existing `fw_ota`.

**Architecture:** A host-testable parser maps the frozen Trafyn envelope onto `{version, url, sha256, size}`. `fw_ota_lte_run` POSTs that API (optional auth headers), then reuses `net_lte_http_get_stream` + `fw_ota_begin/write/end`. SoftAP keeps stats and `device_id`; ngrok and local upload go away.

**Tech Stack:** ESP-IDF 5.2, cJSON, Quectel QHTTP POST/GET, NVS `elm` namespace, host C tests under `tests/host/`.

## Global Constraints

- POST body keys stay `input.deviceId`, `manufacturer`, `deviceType`, `currentVersion` — never rename.
- Response envelope stays `success` / `status` / `errorMessage` / `data`; `data` includes `latestVersion`, `presignedUrl`, `updateAvailable`, `deviceId`, plus **`sha256`** and **`size`**.
- `manufacturer` = `Espressif Systems`; `deviceType` = `fleet monitor`.
- `currentVersion` = substring of running app version before first `-` (`1.0.4-lab` → `1.0.4`); if empty, send the full string.
- `deviceId` = NVS `uplink_did` / SoftAP `device_id` (default `fleet-demo-001`).
- Auth headers omitted when Kconfig strings are empty; later fill `FW_OTA_LTE_AUTH_HEADER` and `FW_OTA_LTE_SYSTEM_USER_ID` without changing JSON.
- Auto-check: wait ~90 s for LTE, every 24 h, always `force=false`.
- Do not flash without 64-char hex `sha256` and `size > 0`.
- NVS key `ota_manif` stays (optional URL override); do not send `channel` to Trafyn.
- No ngrok URLs in Kconfig, sdkconfig.defaults, or SoftAP.

## File map

| File | Responsibility |
|---|---|
| `components/fw_ota_lte/fw_ota_lte_parse.c` + `include/fw_ota_lte_parse.h` | Version strip + Trafyn JSON → check result (no ESP-IDF) |
| `tests/host/test_fw_ota_lte_parse.c` | Host tests for parse/strip |
| `components/net_lte/include/net_lte.h` + `net_lte.c` | POST that returns body; optional request headers |
| `components/fw_ota_lte/fw_ota_lte.c` + `Kconfig` + `include/fw_ota_lte.h` | POST check, map, existing flash path, status fields |
| `components/transport_http/http_api.c` + `static_index.html.h` | Remove upload; stats UI |
| `components/transport_serial/transport_serial.c` | `ota url` = firmware-check URL; drop query-append docs |
| `sdkconfig.defaults` | Trafyn QA URL, no ngrok |
| `tools/ota_dev_server/` | Delete lab GET server |

---

### Task 1: Host-testable Trafyn parse + version strip

**Files:**
- Create: `components/fw_ota_lte/include/fw_ota_lte_parse.h`
- Create: `components/fw_ota_lte/fw_ota_lte_parse.c`
- Create: `tests/host/test_fw_ota_lte_parse.c`
- Modify: `components/fw_ota_lte/CMakeLists.txt`
- Modify: `tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: cJSON
- Produces: `fw_ota_strip_version()`, `fw_ota_parse_check_json()`

- [ ] **Step 1: Write the header**

```c
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    FW_OTA_CHECK_UPDATE = 0,
    FW_OTA_CHECK_NO_UPDATE,
    FW_OTA_CHECK_FAIL,
} fw_ota_check_kind_t;

typedef struct {
    fw_ota_check_kind_t kind;
    char error[96];
    char latest_version[40];
    char presigned_url[1024];
    char sha256[65];
    size_t size;
    bool update_available;
} fw_ota_check_result_t;

void fw_ota_strip_version(const char *in, char *out, size_t out_len);
int fw_ota_parse_check_json(const char *json, const char *stripped_current,
                            fw_ota_check_result_t *out);
```

`fw_ota_parse_check_json` returns 0 on a filled `out` (including FAIL/NO_UPDATE kinds). Non-zero only if `json`/`out` is NULL.

- [ ] **Step 2: Write the failing host test**

`tests/host/test_fw_ota_lte_parse.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fw_ota_lte_parse.h"

int main(void)
{
    char v[32];
    fw_ota_strip_version("1.0.4-lab", v, sizeof(v));
    assert(strcmp(v, "1.0.4") == 0);
    fw_ota_strip_version("1.0.4", v, sizeof(v));
    assert(strcmp(v, "1.0.4") == 0);
    fw_ota_strip_version("-lab", v, sizeof(v));
    assert(strcmp(v, "-lab") == 0); /* empty prefix → full string */

    const char *ok =
        "{\"success\":true,\"status\":200,\"errorMessage\":null,"
        "\"data\":{\"latestVersion\":\"1.0.7\","
        "\"presignedUrl\":\"https://s3.example/fw.bin\","
        "\"updateAvailable\":true,\"deviceId\":\"fleet-demo-001\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1109248}}";
    fw_ota_check_result_t r;
    assert(fw_ota_parse_check_json(ok, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_UPDATE);
    assert(strcmp(r.latest_version, "1.0.7") == 0);
    assert(r.size == 1109248);
    assert(r.update_available);

    const char *none =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.4\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":false,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1}}";
    assert(fw_ota_parse_check_json(none, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_NO_UPDATE);

    const char *same =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.4\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1}}";
    assert(fw_ota_parse_check_json(same, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_NO_UPDATE);

    const char *fail = "{\"success\":false,\"errorMessage\":\"nope\"}";
    assert(fw_ota_parse_check_json(fail, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_FAIL);

    const char *nosha =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.9\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,\"size\":1}}";
    assert(fw_ota_parse_check_json(nosha, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_FAIL);

    printf("test_fw_ota_lte_parse: ok\n");
    return 0;
}
```

- [ ] **Step 3: Run test to verify it fails to link**

```bash
cmake -S tests/host -B tests/host/build && cmake --build tests/host/build --target test_fw_ota_lte_parse
```

Expected: target missing or undefined symbols.

- [ ] **Step 4: Implement parse + CMake**

`fw_ota_lte_parse.c`:

- `fw_ota_strip_version`: copy `in` to `out`; if `in` contains `-` and the prefix length is > 0, copy only prefix; else copy full `in`.
- `fw_ota_parse_check_json`: `cJSON_Parse`; if fail, try first `{`. If `success` is not true → FAIL (`errorMessage` or `"success_false"`). Get `data` object. Read `updateAvailable` bool, `latestVersion` string, `presignedUrl` string, `sha256` string, `size` number. If `updateAvailable` is false **or** `latestVersion` equals `stripped_current` **or** `presignedUrl` empty → NO_UPDATE. If `sha256` is not exactly 64 `isxdigit` chars or `size <= 0` → FAIL (`"missing_sha_size"`). Else UPDATE and copy fields. `size` may be JSON number (use `valuedouble`).

`fw_ota_lte/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "fw_ota_lte.c" "fw_ota_lte_parse.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fw_ota net_lte nvs_flash json freertos log esp_app_format)
```

`tests/host/CMakeLists.txt` add (requires `IDF_PATH`):

```cmake
add_executable(test_fw_ota_lte_parse
    test_fw_ota_lte_parse.c
    ${CMAKE_SOURCE_DIR}/../../components/fw_ota_lte/fw_ota_lte_parse.c
    $ENV{IDF_PATH}/components/json/cJSON/cJSON.c
)
target_include_directories(test_fw_ota_lte_parse PRIVATE
    ${CMAKE_SOURCE_DIR}/../../components/fw_ota_lte/include
    $ENV{IDF_PATH}/components/json/cJSON
)
add_test(NAME test_fw_ota_lte_parse COMMAND test_fw_ota_lte_parse)
```

- [ ] **Step 5: Run host tests**

```bash
cmake -S tests/host -B tests/host/build && cmake --build tests/host/build && ctest --test-dir tests/host/build --output-on-failure
```

Expected: `test_fw_ota_lte_parse: ok` and all tests PASS.

- [ ] **Step 6: Commit**

```bash
git add components/fw_ota_lte/include/fw_ota_lte_parse.h \
  components/fw_ota_lte/fw_ota_lte_parse.c \
  components/fw_ota_lte/CMakeLists.txt \
  tests/host/test_fw_ota_lte_parse.c tests/host/CMakeLists.txt
git commit -m "feat(fw_ota_lte): parse Trafyn firmware-check JSON"
```

---

### Task 2: LTE POST that returns the body and optional headers

**Files:**
- Modify: `components/net_lte/include/net_lte.h`
- Modify: `components/net_lte/net_lte.c` (`net_lte_http_post` ~727–876; reuse `http_read_body_stream_locked` / `http_get_buf_cb` from GET)

**Interfaces:**
- Consumes: existing QHTTP POST + `http_read_body_stream_locked`
- Produces:

```c
typedef struct {
    const char *authorization;   /* NULL or "" → omit header */
    const char *system_user_id;  /* NULL or "" → omit header */
} net_lte_http_req_headers_t;

esp_err_t net_lte_http_post_recv(const char *url, const char *body,
                                 const net_lte_http_req_headers_t *hdr,
                                 char *resp_buf, size_t resp_buf_len, size_t *resp_len,
                                 net_lte_http_result_t *out);
```

Keep `net_lte_http_post(url, body, out)` as a wrapper that calls `post_recv` with `hdr=NULL`, `resp_buf=NULL` so telemetry uplink does not change.

- [ ] **Step 1: Add the typedef + `net_lte_http_post_recv` prototype to `net_lte.h` immediately after `net_lte_http_post`.**

- [ ] **Step 2: Implement `post_recv`**

Behavior:

1. Same PDP/SSL setup as current `net_lte_http_post`.
2. Let `use_hdr = hdr && ((hdr->authorization && hdr->authorization[0]) || (hdr->system_user_id && hdr->system_user_id[0]))`.
3. If `!use_hdr`: `AT+QHTTPCFG="requestheader",0` and send raw JSON body after CONNECT (today’s path). `AT+QHTTPCFG="contenttype",4`.
4. If `use_hdr`: `AT+QHTTPCFG="requestheader",1`. After CONNECT, send a single payload:

```
POST <path_and_query> HTTP/1.1\r\n
Host: <host>\r\n
Content-Type: application/json\r\n
Content-Length: <json_len>\r\n
Authorization: <authorization>\r\n   // only if non-empty
x-nc-system-user-id: <id>\r\n        // only if non-empty
\r\n
<json>
```

Parse `host` and `path_and_query` from `url` (strip `https://`). `AT+QHTTPPOST=<payload_len>,80,80` uses that full payload length, not JSON-only length.

5. Parse `+QHTTPPOST: err,status,rlen` as today. Set `out->http_status`. Treat HTTP 2xx as transport success even if `success:false` in JSON (caller parses).
6. If `resp_buf` non-NULL: `http_read_body_stream_locked` into `resp_buf` (same as GET). Require `resp_buf_len >= 2`. NUL-terminate. Set `*resp_len`.
7. If `resp_buf` is NULL: keep today’s short `AT+QHTTPREAD=80` drain so uplink stays unchanged.

`net_lte_http_post` becomes:

```c
esp_err_t net_lte_http_post(const char *url, const char *body, net_lte_http_result_t *out)
{
    return net_lte_http_post_recv(url, body, NULL, NULL, 0, NULL, out);
}
```

Presigned GET stays `net_lte_http_get_stream` with no extra headers.

- [ ] **Step 3: Build firmware (compile gate)**

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

Expected: build succeeds. Uplink still calls `net_lte_http_post`.

- [ ] **Step 4: Commit**

```bash
git add components/net_lte/include/net_lte.h components/net_lte/net_lte.c
git commit -m "feat(net_lte): POST recv body and optional auth headers"
```

---

### Task 3: `fw_ota_lte_run` uses Trafyn POST

**Files:**
- Modify: `components/fw_ota_lte/Kconfig`
- Modify: `components/fw_ota_lte/include/fw_ota_lte.h`
- Modify: `components/fw_ota_lte/fw_ota_lte.c`
- Modify: `sdkconfig.defaults`
- Modify: `sdkconfig` (`CONFIG_FW_OTA_LTE_*` keys after next `idf.py build`)

**Interfaces:**
- Consumes: `fw_ota_parse_check_json`, `fw_ota_strip_version`, `net_lte_http_post_recv`, existing `fw_ota_*` + `net_lte_http_get_stream`
- Produces: Trafyn check then same flash path; status includes `update_available`

- [ ] **Step 1: Kconfig**

Replace `FW_OTA_LTE_DEFAULT_MANIFEST_URL` with:

```
config FW_OTA_LTE_CHECK_URL
    string "Firmware-check POST URL"
    default "https://api.trafyn.info/workflow-engine/realm/1/user/1/v1/execution/service/runWithNoAuth/get-latest-device-firmware?refreshCache=true"

config FW_OTA_LTE_AUTH_HEADER
    string "Optional Authorization header value (empty = omit)"
    default ""

config FW_OTA_LTE_SYSTEM_USER_ID
    string "Optional x-nc-system-user-id (empty = omit)"
    default ""

config FW_OTA_LTE_MANUFACTURER
    string "Trafyn manufacturer"
    default "Espressif Systems"

config FW_OTA_LTE_DEVICE_TYPE
    string "Trafyn deviceType"
    default "fleet monitor"
```

Help text: auto-check POSTs this URL; NVS `ota_manif` overrides if non-empty. Rename AUTO_CHECK help from “GET the manifest” to “POST firmware-check”.

`sdkconfig.defaults`: delete ngrok lines; set

```
CONFIG_FW_OTA_LTE_CHECK_URL="https://api.trafyn.info/workflow-engine/realm/1/user/1/v1/execution/service/runWithNoAuth/get-latest-device-firmware?refreshCache=true"
```

Do not put a Bearer token in the repo.

- [ ] **Step 2: Status struct**

In `fw_ota_lte_status_t` add:

```c
bool update_available;
char current_version[24]; /* stripped value sent in POST */
```

Keep `manifest_url` / `manifest_version` field names (NVS/UI compatibility). `manifest_version` stores `latestVersion`. Bump `fw_ota_lte_config_t.manifest_url` to **256** bytes (Trafyn URL is ~140 chars).

`apply_default_url` copies `CONFIG_FW_OTA_LTE_CHECK_URL` (fallback `#define` if Kconfig missing).

- [ ] **Step 3: Replace `fw_ota_lte_run` GET-manifest with POST**

Delete `build_manifest_url` (do not append `?device_id=&channel=`).

Algorithm:

```c
char stripped[24];
const esp_app_desc_t *app = esp_app_get_description();
fw_ota_strip_version(app && app->version[0] ? app->version : "", stripped, sizeof(stripped));

cJSON *body = cJSON_CreateObject();
cJSON *input = cJSON_AddObjectToObject(body, "input");
cJSON_AddStringToObject(input, "deviceId", cfg.device_id[0] ? cfg.device_id : "fleet-demo-001");
cJSON_AddStringToObject(input, "manufacturer", CONFIG_FW_OTA_LTE_MANUFACTURER);
cJSON_AddStringToObject(input, "deviceType", CONFIG_FW_OTA_LTE_DEVICE_TYPE);
cJSON_AddStringToObject(input, "currentVersion", stripped[0] ? stripped : "0.0.0");
char *payload = cJSON_PrintUnformatted(body);
cJSON_Delete(body);

net_lte_http_req_headers_t hdr = {
    .authorization = CONFIG_FW_OTA_LTE_AUTH_HEADER,
    .system_user_id = CONFIG_FW_OTA_LTE_SYSTEM_USER_ID,
};
char *resp = malloc(4096);
size_t got = 0;
err = net_lte_http_post_recv(cfg.manifest_url, payload, &hdr, resp, 4096, &got, &hr);
free(payload);
```

If POST fails → phase `failed`, `hr.error`. Parse with `fw_ota_parse_check_json(resp, stripped, &parsed)` (skip to first `{` already inside parse). Store `s_st.http_status`, `s_st.update_available`, `s_st.current_version`, `s_st.manifest_version = parsed.latest_version`.

- FAIL → `FW_OTA_LTE_FAILED`, `parsed.error`
- NO_UPDATE → `FW_OTA_LTE_NO_UPDATE`
- UPDATE: if `!cfg.force` and `s_applied` equals `parsed.latest_version` → `NO_UPDATE`. Else `fw_ota_begin(parsed.size, parsed.sha256)` → `net_lte_http_get_stream(parsed.presigned_url, stream_write_cb, …)` immediately → on success `save_applied_version(parsed.latest_version)` → `fw_ota_end_and_reboot()`. On stream fail: `fw_ota_abort()`, `failed`.

Empty check URL → `failed` `"check_url empty"`.

`#ifndef CONFIG_FW_OTA_LTE_MANUFACTURER` defaults `"Espressif Systems"` / `"fleet monitor"` so host-unrelated builds don’t break.

- [ ] **Step 4: `idf.py build`**

Expected: compile OK. Strings in map: Trafyn host present, ngrok default absent (`grep -r ngrok-free sdkconfig sdkconfig.defaults components/fw_ota_lte` empty).

- [ ] **Step 5: Commit**

```bash
git add components/fw_ota_lte sdkconfig.defaults sdkconfig
git commit -m "feat(fw_ota_lte): POST Trafyn firmware-check then stream bin"
```

---

### Task 4: SoftAP stats only — remove local upload and ngrok UI

**Files:**
- Modify: `components/transport_http/http_api.c` (GET/POST `/api/ota`, `/api/ota/lte`)
- Modify: `components/transport_http/static_index.html.h`
- Modify: `components/transport_serial/transport_serial.c` (`cmd_ota`)

**Interfaces:**
- Consumes: extended `fw_ota_lte_get_status` / `get_config`
- Produces: SoftAP without file upload; LTE card shows Trafyn stats; `device_id` + force + Run remain

- [ ] **Step 1: HTTP API**

`GET /api/ota`: keep partition/version/state (no upload).  
`POST /api/ota`: return **405** JSON `{"error":"upload_disabled","message":"firmware updates are LTE-only"}` — delete `api_ota_post` flash logic (the `fw_ota_begin` receive loop). Unregister POST or keep handler as 405.

`GET /api/ota/lte`: keep existing fields; add `firmware_check_url` (same string as `manifest_url`), `update_available`, `current_version`. Do not require channel in UI.

`POST /api/ota/lte`: accept **only** `device_id` and `force`. Ignore `manifest_url` and `channel` if present (do not save URL from SoftAP — URL is Kconfig/NVS serial override only). Spec: no ngrok URL field.

`POST /api/ota/lte/run`: unchanged (`fw_ota_lte_start_background`).

- [ ] **Step 2: SoftAP HTML**

In `static_index.html.h`:

- Firmware OTA card: remove `<input id="otaFile">`, upload button, progress, `sha256Hex`, `prepareOtaFile`, `uploadOta`. Keep `refreshOta()` pills (booted slot, fw version, state).
- LTE OTA card: remove `#otaManifUrl`, `#otaChan`, `otaManifestBase()` ngrok default. Keep `#otaDid`, `#otaForce`, Save (device_id+force), Run LTE OTA, Refresh. Status pills: running fw, stripped `current_version`, phase, `manifest_version` labeled **latest**, `update_available`, HTTP, downloaded, error, read-only check URL from `o.firmware_check_url || o.manifest_url`.
- Hint text: “Updates come from Trafyn over LTE. Edit device_id if needed.”

- [ ] **Step 3: Serial**

Keep `ota status|run|force on|off|url <url>`. Change `ota url` help: persists firmware-check POST URL; **do not** strip `?refreshCache=true` (today `ota_strip_query` would destroy the Trafyn query). **Remove `ota_strip_query` on this path** — save the URL as typed.

Print `current_version` and `update_available` in `cmd_ota_status`.

- [ ] **Step 4: `idf.py build`**

Expected: success. `static_index.html.h` has no `ngrok-free` and no `otaFile`.

- [ ] **Step 5: Commit**

```bash
git add components/transport_http/http_api.c \
  components/transport_http/static_index.html.h \
  components/transport_serial/transport_serial.c
git commit -m "feat(ota): SoftAP stats only; drop local bin upload"
```

---

### Task 5: Remove lab GET server and ngrok leftovers

**Files:**
- Delete: `tools/ota_dev_server/` (entire tree)
- Modify: `README.md`, `PROJECT_CONTEXT.md`, `docs/superpowers/specs/2026-08-03-ota-backend-api-requirements.md` (point to 2026-08-19 spec; lab GET is historical)
- Modify: `main/app_main.c` comment if it still says GPIO17 TX / GET manifest
- Grep: `ngrok`, `/firmware/manifest`, `ota_dev_server`

- [ ] **Step 1: Delete lab server**

```bash
git rm -r tools/ota_dev_server
```

- [ ] **Step 2: Docs**

Root README / PROJECT_CONTEXT: LTE OTA is Trafyn POST; SoftAP does not upload bins. Link `docs/superpowers/specs/2026-08-19-trafyn-firmware-ota-design.md`.

At top of `2026-08-03-ota-backend-api-requirements.md` add: **Superseded for device behavior** by `2026-08-19-trafyn-firmware-ota-design.md` (keep file as history).

- [ ] **Step 3: Grep gate**

```bash
rg -n "ngrok-free|/firmware/manifest|ota_dev_server" --glob '!docs/superpowers/specs/2026-08-03*' --glob '!docs/superpowers/plans/2026-08-03*' --glob '!docs/superpowers/specs/2026-08-19*' --glob '!docs/superpowers/plans/2026-08-19*'
```

Expected: no hits in firmware/Kconfig/UI. Historical 2026-08-03 docs may still mention ngrok.

- [ ] **Step 4: Commit**

```bash
git add -A README.md PROJECT_CONTEXT.md docs/superpowers/specs/2026-08-03-ota-backend-api-requirements.md main/app_main.c
git commit -m "chore: remove ngrok OTA lab server and leftover GET-manifest copy"
```

---

### Task 6: Device bench (QA Trafyn)

No code. Confirm Trafyn `data` includes `sha256` and `size` first.

- [ ] **Step 1:** Flash current image; SoftAP shows no file picker; `lte` still works.
- [ ] **Step 2:** `ota run` (or wait auto-check). If `updateAvailable:false` → phase `no_update`.
- [ ] **Step 3:** When QA has a newer `latestVersion` + hash + live URL → download, reboot other slot, `fw_ota` confirm.
- [ ] **Step 4:** SoftAP shows phase/latest/HTTP/error; `device_id` save still works.

---

## Spec coverage

| Spec item | Task |
|---|---|
| POST Trafyn body + strip version | 1, 3 |
| Parse success/data/updateAvailable/sha256/size | 1, 3 |
| Stream presigned GET + fw_ota | 3 |
| Optional auth headers | 2, 3 |
| Kconfig URL QA vs prod | 3 |
| NVS ota_manif override | 3 (keep key); serial url Task 4 |
| Remove SoftAP upload | 4 |
| Remove GET manifest / ngrok / ota_dev_server | 3, 5 |
| SoftAP stats + device_id | 4 |
| Auto-check timing unchanged | 3 (no change to wait/interval) |
| Failures table | 1 kinds + 3 run() |

## Type names (locked)

`fw_ota_check_kind_t`, `fw_ota_check_result_t`, `fw_ota_strip_version`, `fw_ota_parse_check_json`, `net_lte_http_req_headers_t`, `net_lte_http_post_recv`.
