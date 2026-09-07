# Carrier Console — Fleet Diagnostics panel (design)

**Date:** 2026-09-03  
**Status:** Approved (Approach A) — awaiting implementation plan  
**Surface:** `tools/carrier_console/` (master / carrier web UI on port 8766)  
**Related:** [Host-owned Zigbee uplink envelope](./2026-09-03-host-owned-zigbee-envelope-design.md)

## Goal

Operators monitoring the **master carrier** need a single place on the Carrier Console to see whether the Zigbee fleet path is healthy: hosts joined, envelope fields (`node_id` / `schemaId`), link freshness, and a compact carrier Zigbee snapshot — without leaving the dashboard or grepping the serial log.

## Non-goals (v1)

- No new carrier firmware CLI commands.
- No BLE sensor health on this UI (that stays on Host Console).
- No full LTE / OTA / GPS / SD-queue diagnostics panel (existing cards remain; expand later).
- No separate Diagnostics route/tab.
- No `fleet ingest` loopback button in v1.

## Current state (gap)

| Area | Backend (`app.py`) | UI today |
|------|--------------------|----------|
| Host `device_id`, type, link, readings | Parsed | Shown on host cards |
| Host `node_id`, `schema_id` | Parsed into dashboard JSON | **Not rendered** on cards |
| Report age / stale (20s) | `last` + carrier uptime | Shown as “Last report …” |
| Zigbee report count | Parsed into carrier KV | Shown under Carrier column |
| Dedicated diagnostics | — | OBD DTC only (“OBD / diagnostics”) |
| Envelope for uplink | Available after HELLO | Invisible unless serial log watched |

## Approach

**Approach A (approved):** Keep the Connected hosts dashboard. Add a **Diagnostics** card below it. Fix host cards to show `node` + `schema`. All data from existing serial (`fleet hosts`, status/metrics lines) already polled by the console.

## UI layout

```
[ Connection ]
[ Vehicle link hero ]
[ Connected hosts | Carrier KV ]   ← existing dashboard
[ Diagnostics ]                    ← NEW card
[ Device identity ]
[ Cloud uplink | LTE | OBD … ]
[ Serial log ]
```

### Connected hosts (enhance)

For each host card, under the name/type line (or in the footer hint), show:

- `node=<node_id>` (or `node=—` if empty)
- `schema=<schema_id>` (or `schema=—` if empty)

Keep existing fuel reading chips and last-report age. Stale threshold stays **20 000 ms** (`HOST_STALE_MS`), matching firmware link timeout.

### Diagnostics card

**Title:** Diagnostics  
**Subtitle / hint:** Fleet path (Zigbee hosts → carrier registry → uplink envelope). Refresh uses the same poll as Connected hosts.

**Section 1 — Per-host table (or stacked mini-rows)**

| Column | Source |
|--------|--------|
| Device | `device_id` |
| Type | `host_type` |
| Node | `node_id` |
| Schema | `schema_id` |
| Link | `link` + online/offline derived from age |
| Last report | age from `last` vs carrier uptime |
| Readings | short summary: e.g. `height_mm=39.1` (+ unit if present); omit empty keys |

If zero hosts: same empty guidance as the hosts list (reboot / wait for join).

**Section 2 — Carrier Zigbee snapshot**

Reuse fields already on the Carrier column where possible:

- Zigbee radio / PAN string (if present in carrier state)
- Report count (`zigbee_reports`)
- Optional: last serial RX age from bridge (if already exposed to the page)

Do not invent last short-addr unless already in host or carrier state.

**Section 3 — Actions**

| Action | Behavior |
|--------|----------|
| Refresh hosts | Same as existing `fleetHostsRefresh` / poll (`status` + `fleet hosts`) |
| Copy fleet snapshot | Copy JSON of current hosts dashboard (+ carrier zigbee fields) to clipboard; toast or brief status on success/fail |

Rename the existing OBD section title from “OBD / diagnostics” to **OBD** (or “OBD / DTC”) so “Diagnostics” is unambiguous.

## Data flow

```
Carrier serial ──► SerialBridge parsers ──► dashboard JSON
                                              │
                         WebSocket / REST ◄────┘
                                              │
                    app.js renderHosts + renderDiagnostics
```

v1 changes:

1. **Frontend only** for envelope display and Diagnostics card (data already in `hostsState`).
2. **Copy snapshot** builds JSON in the browser from `hostsState` + `carrier` (no new API).
3. Backend change only if clipboard payload needs a dedicated endpoint — **not required** for v1.

## Empty / edge states

| State | UI |
|-------|-----|
| Disconnected | Diagnostics shows “Connect carrier USB” |
| Connected, 0 hosts | Empty hint (rejoin guidance) |
| Host without `node`/`schema` | Show `—`; optional muted note “pre-envelope host / legacy HELLO” |
| Stale link | Offline styling consistent with host cards |
| Incomplete `fleet hosts` parse | Same incomplete-serial message as hosts list |

## Success criteria

1. Operator can see `node_id` and `schema_id` without opening the serial log.
2. Diagnostics card answers: who joined, is link fresh, what envelope would uplink use.
3. Refresh and copy work without new firmware.
4. Host Console / BLE monitoring unchanged.

## Out of scope follow-ups (document only)

- GPS fix tile, last uplink HTTP status, SD queue depth.
- `fleet ingest` loopback control.
- Parsing richer `[zb]` / uplink HTTP lines into structured carrier state.
- Host Console parity for BLE diagnostics cross-link.

## Testing (manual)

1. Connect carrier with joined UL212 host → Diagnostics shows node/schema matching `fleet hosts`.
2. Power-cycle host → age grows / offline; after rejoin, fields update.
3. Copy fleet snapshot → paste JSON includes hosts + envelope fields.
4. Disconnect serial → Diagnostics idle state; reconnect restores data.
5. Legacy host line without `node=`/`schema=` → dashes, no UI crash.

## Implementation notes

- Files: `tools/carrier_console/static/index.html`, `static/app.js`, `static/style.css`; optionally `README.md`.
- Prefer small CSS additions matching existing `.card` / `.kv-list` / `.host-card` patterns.
- Do not change `HOST_LINE_RE` unless a real parse bug appears; groups 4–5 already carry node/schema.
