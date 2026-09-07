# Build bin & release a new version

Short checklist for `firmware_v2/master` (project `fleet_v2_master`).

## Before you change code

1. Work on a branch that has `firmware_v2/master/` (e.g. `firmware-v2` or Bitbucket `main`).
2. Use **ESP-IDF 5.2.3** only:
   ```bash
   source ~/esp/esp-idf/export.sh   # must print ESP-IDF v5.2.3
   ```
3. Know what you are changing:
   - **Fleet-wide** (same for every device): edit [`sdkconfig.defaults`](sdkconfig.defaults) — URLs, OTA interval, APN, Zigbee channel/EPAN, `device_type` for OBD/GPS.
   - **Per-device** (NVS via USB): `device_id`, `node_id`, `ota_token`, `ota_user` — do **not** bake secrets into git.
4. Uplink rules of thumb:
   - Master owns **OBD** (`device_type=obd`) and **GPS** (`device_type=gps`).
   - Zigbee hosts are dynamic — **omit** `device_type`; identity is `device_id` + `payload.host_type`.
   - Body is a **bare JSON array** (no `{"Vehicle":[...]}` unless you deliberately enable wrap).
5. Run impact / think about blast radius on LTE + uplink paths (QHTTP, queue, OTA).

## New version — steps

### 1. Bump version

Edit [`VERSION`](VERSION) only (e.g. `1.0.33` → `1.0.34`).  
This string is baked into the bin via CMake `PROJECT_VER`.

### 2. Make your code / config changes

Typical touch points:

| Change | Where |
|--------|--------|
| Telemetry / OTA URL | `sdkconfig.defaults` (`CONFIG_UPLINK_URL`, `CONFIG_OTA_CLOUD_CHECK_URL`) |
| OBD/GPS `device_type` strings | `sdkconfig.defaults` + uplink Kconfig |
| LTE / recover / GPS | `components/lte/` |
| Uplink envelopes / batch | `components/uplink/` |
| OTA check interval | `sdkconfig.defaults` (`CONFIG_OTA_CLOUD_AUTO_*`) |
| Operator notes | [`docs/MASTER.md`](MASTER.md) |

After changing `sdkconfig.defaults`, rebuild (and if an old `sdkconfig` exists locally, confirm the new `CONFIG_*` values are present — defaults apply on fresh configure).

### 3. Build the bin

```bash
cd firmware_v2/master
idf.py set-target esp32c6   # first time / after clean
idf.py build
```

Output:

```text
firmware_v2/master/build/fleet_v2_master.bin
```

### 4. Sanity-check the bin

```bash
# Version string
strings build/fleet_v2_master.bin | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head

# Uplink URL (should match sdkconfig.defaults)
strings build/fleet_v2_master.bin | grep nc-fleet-device
```

Optional host tests (from repo root):

```bash
cmake -S tests/host -B tests/host/build
cmake --build tests/host/build
ctest --test-dir tests/host/build --output-on-failure
```

### 5. Publish for OTA (lab / Trafyn)

1. Copy/rename bin, e.g. `elm327_esp32c6-<VERSION>.bin`.
2. SHA-256 + size of that file.
3. Upload with the **publish multipart** workflow (not stored in firmware).  
   Do **not** put publish auth or the multipart URL into the repo.
4. Confirm with get-latest that `latestVersion` / `updateAvailable` look right for a device still on the previous version.

### 6. Get it onto the vehicle

- Prefer **LTE OTA** (`ota check` or wait for auto-check: ~90 s after LTE, then every 30 min).
- USB flash only for recovery / first bring-up.

After reboot, confirm version on serial / `status`.

### 7. Commit & push

Commit the version bump + code/docs together. Push your branch.  
Do not commit: `sdkconfig`, `build/`, `.pio/`, secrets, or publish credentials.

---

## Things that often go wrong

| Mistake | Result |
|---------|--------|
| Bumped `VERSION` but forgot rebuild / republish | Device never sees a new OTA |
| Changed URL only in local `sdkconfig`, not `sdkconfig.defaults` | Next clean build / other machines keep old URL |
| Wrong IDF (e.g. 6.x) | Build/configure failures |
| Hardcoded `device_type=fuel` for all Zigbee hosts | Breaks dynamic host types |
| Published bin but get-latest still shows old version | Wrong file name / metadata / cache — re-check publish response |
| Flashed merged full-image as OTA payload | Use **app** bin `fleet_v2_master.bin` for Trafyn OTA |

## Quick reference

```bash
# Build
source ~/esp/esp-idf/export.sh
cd firmware_v2/master
# edit VERSION + code
idf.py build
ls -la build/fleet_v2_master.bin
```

More detail: [`MASTER.md`](MASTER.md). Repo overview: [`../../README.md`](../../README.md) (on Bitbucket main) or root README on your branch.
