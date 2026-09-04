# Carrier Console Diagnostics v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Parse existing `uplink` serial status into Diagnostics tiles for GPS, last HTTP result, and SD queue; poll `uplink` on dashboard refresh.

**Architecture:** Extend `carrier` JS state + `parseRxLogOnly`; add HTML KV rows; append `uplink` in `SerialBridge.refresh_dashboard`. No firmware changes.

**Tech Stack:** Carrier Console Python/JS as today.

**Spec:** `docs/superpowers/specs/2026-09-03-carrier-console-diagnostics-v2-design.md`

## Global Constraints

- Parse-only; no firmware CLI changes.
- Do not commit unless the user asks.
- Cache-bust `app.js` / `style.css` / HTML `?v=`.

## File map

| File | Role |
|------|------|
| `tools/carrier_console/app.py` | After fleet hosts, send `uplink` |
| `tools/carrier_console/static/index.html` | Carrier path KV rows |
| `tools/carrier_console/static/app.js` | Parse + render + snapshot |
| `tools/carrier_console/README.md` | Document v2 tiles |

---

### Task 1: Poll `uplink` on dashboard refresh

**Files:** Modify `tools/carrier_console/app.py` (`refresh_dashboard`)

- [ ] After successful `fleet hosts` wait, `self.send_line("uplink")` and `time.sleep(0.5)` so GPS/last/queue lines reach the WebSocket clients.
- [ ] Keep timeout behavior unchanged for fleet wait.

### Task 2: UI tiles + parse + render

**Files:** `static/index.html`, `static/app.js`, bump `?v=`

- [ ] Add under Carrier Zigbee:

```html
      <h3 class="diag-sub">Carrier path</h3>
      <dl class="kv-list diag-kv">
        <div><dt>GPS</dt><dd id="diagGps">—</dd></div>
        <div><dt>Last uplink</dt><dd id="diagUplinkLast">—</dd></div>
        <div><dt>SD queue</dt><dd id="diagSdQueue">—</dd></div>
      </dl>
```

- [ ] Extend `carrier` with `gps`, `uplinkLast`, `sdQueue` objects.
- [ ] Regexes in `parseRxLogOnly`:

```javascript
  // last: ok=yes skipped=no http=200 reason="ok" error=""
  const lastM = line.match(
    /(?:^|\s)last:\s*ok=(\w+)\s+skipped=(\w+)\s+http=(-?\d+)\s+reason="([^"]*)"\s+error="([^"]*)"/
  );
  // queue: sd=yes depth=0 bytes=0 drain_err=""
  const qM = line.match(
    /(?:^|\s)queue:\s*sd=(\w+)\s+depth=(\d+)\s+bytes=(\d+)\s+drain_err="([^"]*)"/
  );
  // gps: ok lat=.. lng=.. age_ms=..
  const gpsOk = line.match(
    /(?:^|\s)gps:\s*ok\s+lat=(-?[\d.]+)\s+lng=(-?[\d.]+)\s+age_ms=(\d+)/
  );
  const gpsNo = /(?:^|\s)gps:\s*no fix/.test(line);
  // uplink now: ESP_OK http=200 reason="..." error="..."
  const nowM = line.match(
    /uplink now:\s*\S+\s+http=(-?\d+)\s+reason="([^"]*)"\s+error="([^"]*)"/
  );
```

- [ ] `renderDiagnostics` fills `#diagGps`, `#diagUplinkLast`, `#diagSdQueue`.
- [ ] `buildFleetSnapshot` includes `carrier_path`.
- [ ] Clear path state on disconnect / reboot (`onCarrierReboot`).

### Task 3: README + manual verify

- [ ] Document Carrier path tiles.
- [ ] Verify with live `uplink` dump (or inject lines via log parse).

---

## Spec coverage

GPS / last / queue tiles, poll `uplink`, snapshot JSON, no firmware — Tasks 1–3.
