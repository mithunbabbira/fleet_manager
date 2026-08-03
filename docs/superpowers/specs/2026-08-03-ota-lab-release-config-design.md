# Lab OTA release config (temporary)

**Goal:** Flip offered firmware without `publish.sh` folder copies.

**File:** `tools/ota_dev_server/release.json` (re-read every request).

```json
{
  "version": "1.0.4-lab",
  "bin": "../../build/elm327_esp32c6.bin",
  "channel": "stable"
}
```

| Field | Meaning |
|-------|---------|
| `version` | Manifest `version` (change this to offer an update vs device) |
| `bin` | Path to `.bin` (relative to `tools/ota_dev_server/` or absolute) |
| `channel` | Echoed in manifest (default `stable`) |

**Serving**

- `GET /firmware/manifest` → JSON from config + live `sha256`/`size` of `bin`
- Bin URL always: `{base}/firmware/active/elm327_esp32c6.bin`
- If `release.json` missing/invalid → fall back to existing `firmware/<ver>/` latest
- Keep `?scenario=bad_sha|truncate|gone`

**Out of scope:** real CM/Git API, multi-channel maps, auth.
