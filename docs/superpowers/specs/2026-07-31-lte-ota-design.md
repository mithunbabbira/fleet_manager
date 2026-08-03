# Fleet OTA over LTE — Design & Step Plan

Date: 2026-07-31  
Repo: `63idealabs/fleet-telematics-node`  
Branch context: post BLE hard-delete (`feature/mcp2515-can`)  
Current app size: ~**1000 KiB** (~0.98 MiB) after BLE trim (was ~1.27 MiB)

> This document is the **how and why** plan for internet firmware updates.
> Implementation comes later. A full `idf.py fullclean` rebuild is **not** required to read this plan.

---

## 1. Goal

Update truck/car devices in the field over **cellular (EC200U LTE)** without:

- Sending a technician with USB
- Bricking a remote unit on a failed download
- Writing to the OBD/CAN bus (updates are MCU-only)

Success = device runs new firmware after reboot, or **automatically rolls back** to the previous good image.

---

## 2. Why this architecture (not alternatives)

| Approach | Why we choose / reject |
|----------|-------------------------|
| **A/B flash OTA (chosen)** | Industry standard on ESP-IDF. Two full apps; download into inactive slot; reboot; rollback if unhealthy. No SD required. |
| Factory + single `ota_0` (today) | Weak for fleets: awkward second update + weaker rollback story. |
| SD card as boot media | Vibration/failure risk on trucks; SD is optional **staging only**, not boot. |
| SoftAP-only update | Fine for lab; trucks need LTE. SoftAP can remain a **tech fallback**. |
| Stream over BLE | BLE removed; not relevant. |

---

## 3. Flash layout — what and why

### Today (temporary)

```
nvs | otadata | phy_init | factory (~1.75 MiB) | ota_0 (~1.75 MiB)
```

### Target for fleet OTA

```
nvs | otadata | phy_init | ota_0 (~1.75–1.9 MiB) | ota_1 (~1.75–1.9 MiB)
```

| Partition | Contents | Why |
|-----------|----------|-----|
| **`nvs`** | Config: APN, uplink enable, device_id, CAN protocol, OTA URL/channel | Survives app updates |
| **`otadata`** | “Boot slot A or B” + rollback state | ESP-IDF chooses which app runs |
| **`phy_init`** | RF calibration | Standard Espressif |
| **`ota_0`** | **Full firmware** (CAN + LTE + SoftAP + OTA client) | Slot A |
| **`ota_1`** | **Full firmware** (same kind of image, different version) | Slot B |

**Why two full apps, not an “updater partition”:**  
Whichever slot is running already includes the OTA client. There is no tiny dedicated updater. That avoids a second incomplete image that can’t recover the device.

**Why ~1.75 MiB slots still work:**  
App is now ~1.0 MiB → ~0.75 MiB headroom per slot for growth (features, logs, future stacks). BLE trim was done **first** so OTA slots stay comfortable on 4 MB flash.

**Optional later:** tiny `factory` recovery image — only if you move to 8 MB flash or shrink SoftAP out of factory.

---

## 4. End-to-end update flow (how)

```
┌──────────────┐     HTTPS GET / version check      ┌─────────────────┐
│  Fleet cloud │ ◄────────────────────────────────── │  Device (LTE)   │
│  (bin + JSON)│ ──────────────────────────────────► │  running slot A │
└──────────────┘     firmware.bin + sha256 + sig     └────────┬────────┘
                                                              │
                     write chunks into inactive slot B         │
                     esp_ota_begin / write / end               ▼
                                                     ┌─────────────────┐
                                                     │  Flash ota_1    │
                                                     │  (inactive)     │
                                                     └────────┬────────┘
                                                              │ verify hash/sig
                                                              │ set otadata → B
                                                              ▼
                                                           reboot
                                                              │
                     health check (CAN + LTE + uplink)         ▼
                     mark valid ──► stay on B
                     fail ─────────► rollback to A
```

---

## 5. Step-by-step plan (how + why)

### Phase 0 — Prerequisites (mostly done)

| Step | How | Why |
|------|-----|-----|
| 0.1 Trim BLE/ELM | Hard-delete NimBLE path (Tasks 1–5 done) | Free ~250 KiB so dual slots fit with margin |
| 0.2 Keep SoftAP for now | Field debug + local OTA fallback | Don’t remove Wi‑Fi until LTE OTA is proven |
| 0.3 Confirm size | Note ~1.0 MiB bin (Task 6 verify later) | Prove headroom before changing partitions |

---

### Phase 1 — Partition table A/B

| Step | How | Why |
|------|-----|-----|
| 1.1 Replace `factory`+`ota_0` with `ota_0`+`ota_1` | Edit `partitions.csv`; sizes equal | Enable true dual-bank rollback |
| 1.2 First flash both slots or flash once + copy | Production: flash same image to active; or factory-flash both | Device must boot from an OTA slot, not a dead factory |
| 1.3 Smoke boot | `idf.py flash` after table change | Catch offset mistakes before OTA code |

**Risk:** Wrong offsets brick USB recovery until serial download. Mitigate with careful CSV math and one lab device first.

---

### Phase 2 — Local OTA first (SoftAP or USB), no LTE yet

| Step | How | Why |
|------|-----|-----|
| 2.1 Add `esp_ota` writer module | `esp_ota_begin` on **inactive** slot → write → `esp_ota_end` → `esp_ota_set_boot_partition` | Prove flash path without cellular flakiness |
| 2.2 SoftAP upload endpoint | POST multipart `.bin` to SoftAP (or serial xmodem later) | Lab/tech can update without cloud |
| 2.3 SHA-256 check before boot switch | Hash file vs expected digest | Corrupt upload must not boot |
| 2.4 Rollback window | New app calls `esp_ota_mark_app_valid_cancel_rollback()` only after healthy | Bad image auto-reverts |
| 2.5 Health gate | Healthy = `can_obd_is_ready` OR “no bus expected” flag + LTE UART OK + no panic loop | Don’t confirm an app that can’t talk OBD/LTE |

**Why local before LTE:** Separates “flash/rollback bugs” from “modem/HTTP bugs”.

---

### Phase 3 — Cloud contract (version + artifact)

| Step | How | Why |
|------|-----|-----|
| 3.1 Version identity | Embed `fw_version` / `git describe` in app; report in uplink JSON | Cloud knows what each truck runs |
| 3.2 Manifest API | e.g. `GET /devices/{id}/firmware` → `{version, url, sha256, size, channel}` | Device doesn’t guess URLs |
| 3.3 Staged channels | `canary` / `stable` in NVS | Roll out 1 truck → 10 → fleet |
| 3.4 Host signed `.bin` | HTTPS object store + digest (later: ECDSA signature) | Integrity + authenticity |

**Why a manifest:** Lets you change CDN URLs, pause rollouts, and force pins without rebuilding devices.

---

### Phase 4 — LTE download (EC200U)

| Step | How | Why |
|------|-----|-----|
| 4.1 Version poll on interval | Background task (e.g. every N hours or after uplink) | Don’t block OBD polling forever |
| 4.2 Pause uplink/poller during flash write | Mutex / flags already similar to UART AT mutex | Avoid modem contention + CAN gaps during critical section |
| 4.3 Download via Quectel HTTP | Prefer `QHTTPGET` streaming chunks into `esp_ota_write`; fallback: modem FS then UART read | Reuses proven QHTTP path (same stack as Trafyn POST) |
| 4.4 Resume policy | On fail: abort OTA handle, keep old boot slot, retry with backoff | Power loss mid-download must not switch boot |
| 4.5 Optional SD staging | Download full file to SD, then flash | Only if cellular drops constantly; not required for v1 |

**Why not PPP/`esp_https_ota` first:** You already have QHTTP working without PPP. Adding PPP is a large parallel project. Stream-via-QHTTP matches current modem code.

**Why pause OBD briefly:** Writing flash + long AT sessions can starve the system; better a controlled pause than random failures.

---

### Phase 5 — Verify, reboot, confirm

| Step | How | Why |
|------|-----|-----|
| 5.1 Verify size + SHA-256 (and signature if present) | After download, before `set_boot` | Refuse garbage |
| 5.2 Set boot to new slot + reboot | `esp_ota_set_boot_partition` + `esp_restart` | Activate new image |
| 5.3 Pending-verify state | ESP-IDF anti-rollback / pending verify | Automatic rollback if app never confirms |
| 5.4 Post-boot health | CAN probe + LTE AT + one uplink attempt | Real truck conditions |
| 5.5 Confirm or die | `esp_ota_mark_app_valid_cancel_rollback()` or reboot into old | Fleet stays recoverable |
| 5.6 Telemetry | Uplink `fw_version`, `ota_state` (idle/downloading/pending/failed) | Ops visibility |

---

### Phase 6 — Fleet operations & safety

| Step | How | Why |
|------|-----|-----|
| 6.1 Update only when safe | e.g. ignition off / poller quiet / battery OK (if sensed) | Avoid mid-drive reboot if possible |
| 6.2 Rate limits | Max 1 attempt / day unless forced | Protect data plan + flash wear |
| 6.3 Force / abort API | Cloud can set `force_update` or `block_ota` in manifest | Incident control |
| 6.4 Read-only OBD unchanged | OTA never sends Mode 04/08; still `cmd_policy` on OBD | Vehicle safety separate from MCU update |
| 6.5 Lab checklist | SoftAP OTA → LTE OTA on bench → one canary truck → fleet | Don’t skip stages |

---

## 6. Suggested implementation order (when you code)

1. **Partition CSV A/B** + flash/boot smoke  
2. **Local SoftAP OTA** + hash + rollback  
3. **Manifest + version in uplink**  
4. **LTE QHTTP GET → OTA write**  
5. **Health confirm + ops fields**  
6. (Optional) signature, SD staging, drop SoftAP for more flash  

Defer: `idf.py fullclean` / size report until you want Task 6 closed — **not blocking this plan**.

---

## 7. What we are *not* doing in OTA

- Updating ECUs / flashing vehicle firmware  
- Changing CAN bitrate remotely without a safe profile (separate feature)  
- Relying on SD as the only copy of the image  
- Booting from an “updater-only” partition  

---

## 8. Success criteria

- Lab: SoftAP OTA A→B→A round-trip with intentional bad image → rollback  
- Lab: LTE OTA downloads ~1 MiB image, verifies, boots, confirms  
- Field canary: one truck updates overnight; uplink shows new `fw_version`  
- Failure case: yank power mid-download → still boots previous app  

---

## 9. Relation to BLE trim

BLE delete was **Phase 0**: make the binary small enough that dual OTA slots on 4 MB flash are safe. SoftAP remains for Phase 2 local OTA and field debug; trimming Wi‑Fi is a later optional size pass after LTE OTA works.
