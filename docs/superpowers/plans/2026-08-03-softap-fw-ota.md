# SoftAP Firmware OTA — Implementation Plan

> **For agentic workers:** Implement task-by-task. SoftAP first; LTE later.

**Goal:** Add modular `fw_ota` (A/B flash, SHA-256, rollback) and SoftAP `GET/POST /api/ota`.

**Architecture:** `fw_ota` owns `esp_ota_*` + hash + pending-verify. `http_api` streams POST body into `fw_ota_write`. Enable bootloader app rollback in `sdkconfig.defaults`.

**Tech Stack:** ESP-IDF 5.2 `app_update`, `mbedtls` SHA-256, SoftAP `esp_http_server`.

## Global Constraints

- Raw POST body; headers `X-Firmware-Size`, `X-Firmware-Sha256` required.
- Lab health gate: mark valid after SoftAP/httpd up (CAN not required).
- Max image size = OTA partition size (`0x1C0000`).
- No LTE in this plan.

---

### Task 1: `fw_ota` component + rollback Kconfig

**Files:**
- Create: `components/fw_ota/include/fw_ota.h`
- Create: `components/fw_ota/fw_ota.c`
- Create: `components/fw_ota/CMakeLists.txt`
- Modify: `sdkconfig.defaults` — enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
- Modify: `main/CMakeLists.txt`, `main/app_main.c` — init + confirm
- Modify: `components/sys_runtime/sys_runtime.c` — ota stub delegates or leave unused

**Produces:**
- `esp_err_t fw_ota_init(void);`
- `esp_err_t fw_ota_confirm_after_boot(void);` — mark valid if pending
- `esp_err_t fw_ota_begin(size_t expected_size, const char *sha256_hex);`
- `esp_err_t fw_ota_write(const void *data, size_t len);`
- `esp_err_t fw_ota_abort(void);`
- `esp_err_t fw_ota_end_and_reboot(void);`
- `esp_err_t fw_ota_get_status(fw_ota_status_t *out);`

- [ ] Implement component, enable rollback, call `fw_ota_init` then `fw_ota_confirm_after_boot` after SoftAP starts
- [ ] Build succeeds

---

### Task 2: SoftAP `/api/ota`

**Files:**
- Modify: `components/transport_http/http_api.c`
- Modify: `components/transport_http/CMakeLists.txt` — REQUIRES `fw_ota`
- Modify: `components/transport_http/transport_http.c` — raise `recv_wait_timeout` for ~1 MiB upload; bump `HTTP_MAX_URI_HANDLERS` if needed

- [ ] `GET /api/ota` JSON status
- [ ] `POST /api/ota` stream into fw_ota; on success reboot
- [ ] Flash + curl smoke from SoftAP client

---

### Task 3: Document curl test

**Files:**
- Modify: `docs/superpowers/specs/2026-08-03-softap-fw-ota-design.md` — add curl example

- [ ] Document lab test steps
