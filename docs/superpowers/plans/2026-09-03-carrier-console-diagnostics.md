# Carrier Console Fleet Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Fleet Diagnostics card to Carrier Console and show host `node_id` / `schema_id` on Connected hosts cards, using existing `fleet hosts` dashboard data (no firmware changes).

**Architecture:** Pure frontend work on `tools/carrier_console/static/`. Backend already parses `node=` / `schema=` into each host object. New `renderDiagnostics()` mirrors `renderHosts()` from `hostsState` + `carrier`, plus clipboard copy of a JSON snapshot. Rename OBD section title so “Diagnostics” is unambiguous.

**Tech Stack:** Vanilla HTML/CSS/JS, FastAPI carrier console (`app.py` unchanged in v1), WebSocket/REST dashboard already in use.

**Spec:** `docs/superpowers/specs/2026-09-03-carrier-console-diagnostics-design.md`

## Global Constraints

- No new carrier firmware CLI commands.
- No new REST endpoints unless clipboard somehow requires them (v1: browser-only copy).
- `HOST_STALE_MS` stays `20000`.
- Host object fields: `node_id`, `schema_id` (already from `app.py`).
- Do not put BLE sensor health on this UI.
- Cache-bust `app.js` / `style.css` query strings when changing them.
- Do not commit unless the user explicitly asks (plan steps may stage; skip `git commit` until requested).

## File map

| File | Role |
|------|------|
| `tools/carrier_console/static/index.html` | Diagnostics card markup; rename OBD heading; bump script/css `?v=` |
| `tools/carrier_console/static/app.js` | Host card envelope line; `renderDiagnostics`; copy snapshot; wire refresh |
| `tools/carrier_console/static/style.css` | Diagnostics table / footer styles |
| `tools/carrier_console/README.md` | Document Diagnostics panel |

---

### Task 1: HTML scaffold + OBD title rename

**Files:**
- Modify: `tools/carrier_console/static/index.html`
- Modify: `tools/carrier_console/static/style.css`

**Interfaces:**
- Consumes: existing dashboard section ending at Connected hosts / Carrier KV card
- Produces: DOM ids `diagEmpty`, `diagHostTable`, `diagHostBody`, `diagZbRadio`, `diagZbReports`, `diagCopyStatus`, `diagRefreshBtn`, `diagCopyBtn`

- [ ] **Step 1: Insert Diagnostics card after the dashboard section**

In `index.html`, immediately after the closing `</section>` of `<section class="card dashboard">` (before `#provisionCard`), insert:

```html
    <section class="card" id="diagnosticsCard">
      <div class="dash-head">
        <h2>Diagnostics</h2>
        <span class="pill" id="diagPill">fleet path</span>
      </div>
      <p class="hint" style="margin-top:0">
        Zigbee hosts → carrier registry → uplink envelope (<code>node_id</code> / <code>schemaId</code>).
        Same data as <strong>Refresh hosts</strong>.
      </p>

      <h3 class="diag-sub">Hosts</h3>
      <div id="diagEmpty" class="empty-hint">Connect the carrier over USB — diagnostics refresh with hosts.</div>
      <div class="diag-table-wrap" id="diagHostTable" hidden>
        <table class="diag-table">
          <thead>
            <tr>
              <th>Device</th>
              <th>Type</th>
              <th>Node</th>
              <th>Schema</th>
              <th>Link</th>
              <th>Last report</th>
              <th>Readings</th>
            </tr>
          </thead>
          <tbody id="diagHostBody"></tbody>
        </table>
      </div>

      <h3 class="diag-sub">Carrier Zigbee</h3>
      <dl class="kv-list diag-kv">
        <div><dt>Radio</dt><dd id="diagZbRadio">—</dd></div>
        <div><dt>Reports</dt><dd id="diagZbReports">—</dd></div>
      </dl>

      <div class="dash-foot">
        <button type="button" id="diagRefreshBtn">Refresh hosts</button>
        <button type="button" id="diagCopyBtn">Copy fleet snapshot</button>
        <span class="hint" id="diagCopyStatus"></span>
      </div>
    </section>
```

- [ ] **Step 2: Rename OBD heading**

Change:

```html
        <h2>OBD / diagnostics</h2>
```

to:

```html
        <h2>OBD / DTC</h2>
```

- [ ] **Step 3: Bump asset versions**

Update:

```html
  <link rel="stylesheet" href="/static/style.css?v=3">
...
  <script src="/static/app.js?v=10"></script>
```

(Use next integers if already higher when implementing.)

- [ ] **Step 4: Add CSS for diagnostics table**

Append to `style.css`:

```css
.diag-sub {
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: .05em;
  color: var(--muted);
  margin: 16px 0 8px;
}
.diag-table-wrap { overflow-x: auto; }
.diag-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}
.diag-table th,
.diag-table td {
  text-align: left;
  padding: 8px 10px;
  border-bottom: 1px solid var(--line);
  vertical-align: top;
}
.diag-table th {
  color: var(--muted);
  font-weight: 600;
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: .04em;
}
.diag-table .mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12px; }
.diag-kv { margin-top: 4px; }
.host-envelope { color: var(--muted); font-size: 12px; margin: 0 0 8px; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
```

- [ ] **Step 5: Visual check**

Run: `cd tools/carrier_console && ./run.sh` (or existing start command), open `http://127.0.0.1:8766`, confirm Diagnostics card appears under the dashboard and OBD says “OBD / DTC”. Serial connect optional for this step.

- [ ] **Step 6: Commit only if user asked**

```bash
# skip unless user requested a commit
```

---

### Task 2: Show node / schema on host cards

**Files:**
- Modify: `tools/carrier_console/static/app.js`

**Interfaces:**
- Consumes: host objects with optional `node_id`, `schema_id` strings
- Produces: `formatEnvelope(h)` → display string; host cards include envelope line

- [ ] **Step 1: Add helpers near `isHostOnline`**

```javascript
function envelopeOrDash(v) {
  return v && String(v).trim() ? String(v).trim() : "—";
}

function formatEnvelope(h) {
  return `node=${envelopeOrDash(h.node_id)} · schema=${envelopeOrDash(h.schema_id)}`;
}

function readingsSummary(readings) {
  const r = readings || {};
  const keys = Object.keys(r);
  if (!keys.length) return "—";
  return keys
    .slice(0, 6)
    .map((k) => {
      const item = r[k];
      const unit = item.unit ? ` ${item.unit}` : "";
      return `${k}=${item.value}${unit}`;
    })
    .join(", ");
}
```

- [ ] **Step 2: Insert envelope into host card HTML**

Inside `renderHosts()`, in the returned `<article class="host-card…">` template, after `host-card-head` and before `<div class="readings">`, add:

```javascript
        <p class="host-envelope">${formatEnvelope(h)}</p>
```

Keep the existing footer `Last report …`.

- [ ] **Step 3: Manual verify with live or mocked state**

With carrier connected and a joined UL212 host, confirm each host card shows e.g. `node=node-ul212-001 · schema=1088`. With empty `node_id`/`schema_id`, confirm `—`.

- [ ] **Step 4: Commit only if user asked**

---

### Task 3: `renderDiagnostics` + wire into dashboard updates

**Files:**
- Modify: `tools/carrier_console/static/app.js`

**Interfaces:**
- Consumes: `hostsState`, `carrier`, `isHostOnline`, `hostReportAgeMs`, `fmtReportAge`, `formatEnvelope`, `readingsSummary`, `envelopeOrDash`
- Produces: `renderDiagnostics()`; called whenever hosts or carrier KV re-renders

- [ ] **Step 1: Implement `renderDiagnostics`**

```javascript
function renderDiagnostics() {
  const empty = $("diagEmpty");
  const table = $("diagHostTable");
  const body = $("diagHostBody");
  const pill = $("diagPill");
  if (!empty || !table || !body) return;

  const snap = hostsState;
  if (!snap) {
    empty.hidden = false;
    empty.textContent =
      "Connect the carrier over USB — diagnostics refresh with hosts.";
    table.hidden = true;
    body.innerHTML = "";
    if (pill) {
      pill.textContent = "fleet path";
      pill.className = "pill";
    }
  } else if (!snap.hosts.length) {
    empty.hidden = false;
    empty.innerHTML =
      carrier.uptimeS != null && carrier.uptimeS < 120
        ? "Carrier just rebooted — wait for host rejoin (see Connected hosts)."
        : "No Zigbee hosts in registry.";
    table.hidden = true;
    body.innerHTML = "";
    if (pill) {
      pill.textContent = "0 hosts";
      pill.className = "pill down";
    }
  } else {
    empty.hidden = true;
    table.hidden = false;
    const online = snap.hosts.filter(isHostOnline).length;
    if (pill) {
      pill.textContent = `${online} online · ${snap.hosts.length} listed`;
      pill.className = "pill " + (online > 0 ? "live" : "down");
    }
    body.innerHTML = snap.hosts
      .map((h) => {
        const onlineH = isHostOnline(h);
        const age = hostReportAgeMs(h.last);
        const statusLabel = onlineH
          ? "online"
          : age != null && age > HOST_STALE_MS
            ? "offline"
            : "link down";
        const statusClass = onlineH ? "ok" : "bad";
        return `<tr>
          <td>${h.device_id || "—"}</td>
          <td>${h.host_type || "—"}</td>
          <td class="mono">${envelopeOrDash(h.node_id)}</td>
          <td class="mono">${envelopeOrDash(h.schema_id)}</td>
          <td><span class="host-link ${statusClass}">${statusLabel}</span></td>
          <td>${fmtReportAge(h.last)}</td>
          <td class="mono">${readingsSummary(h.readings)}</td>
        </tr>`;
      })
      .join("");
  }

  const zbRadio = $("diagZbRadio");
  const zbReports = $("diagZbReports");
  if (zbRadio) zbRadio.textContent = carrier.zigbee || "—";
  if (zbReports) {
    zbReports.textContent =
      carrier.zigbeeReports != null ? String(carrier.zigbeeReports) : "—";
  }
}
```

- [ ] **Step 2: Call `renderDiagnostics` from existing update paths**

After every `renderHosts()` call that should refresh diagnostics, also call `renderDiagnostics()`. Minimum set:

1. End of `renderHosts()` — add `renderDiagnostics();` as the last line (simplest: one place).
2. In `onCarrierReboot` after `renderCarrierKv()` — ensure diagnostics clears (covered if `renderHosts` calls it).
3. End of `renderCarrierKv()` — call `renderDiagnostics()` so Zigbee radio/report count updates even when hosts unchanged.

Preferred: call from end of `renderHosts()` **and** end of `renderCarrierKv()`.

- [ ] **Step 3: Init on load**

Near bottom where `renderHosts()` is already invoked, ensure:

```javascript
renderHosts();
renderDiagnostics();
```

(If `renderHosts` already calls `renderDiagnostics`, a single `renderHosts()` is enough.)

- [ ] **Step 4: Manual verify**

Connect carrier, refresh hosts: Diagnostics table rows match Connected hosts; Carrier Zigbee Reports matches Carrier column; disconnect clears/empties appropriately.

- [ ] **Step 5: Commit only if user asked**

---

### Task 4: Diagnostics actions (Refresh + Copy snapshot)

**Files:**
- Modify: `tools/carrier_console/static/app.js`

**Interfaces:**
- Consumes: existing `refreshDashboard()` used by `#fleetHostsRefresh`
- Produces: `buildFleetSnapshot()`, `#diagRefreshBtn` / `#diagCopyBtn` handlers

- [ ] **Step 1: Add snapshot builder**

```javascript
function buildFleetSnapshot() {
  return {
    captured_at: new Date().toISOString(),
    hosts: hostsState
      ? {
          host_count: hostsState.host_count,
          joined: hostsState.joined,
          hosts: hostsState.hosts,
        }
      : null,
    carrier_zigbee: {
      radio: carrier.zigbee || null,
      reports: carrier.zigbeeReports != null ? carrier.zigbeeReports : null,
      uptime_s: carrier.uptimeS != null ? carrier.uptimeS : null,
    },
  };
}

async function copyFleetSnapshot() {
  const status = $("diagCopyStatus");
  const payload = JSON.stringify(buildFleetSnapshot(), null, 2);
  try {
    await navigator.clipboard.writeText(payload);
    if (status) status.textContent = "Copied to clipboard";
  } catch (e) {
    if (status) status.textContent = "Copy failed — select serial log or allow clipboard";
    console.warn(e);
  }
  if (status) {
    setTimeout(() => {
      if (status.textContent.startsWith("Copied") || status.textContent.startsWith("Copy failed")) {
        status.textContent = "";
      }
    }, 2500);
  }
}
```

- [ ] **Step 2: Wire buttons** (near `$("fleetHostsRefresh").onclick`)

```javascript
$("diagRefreshBtn").onclick = () => refreshDashboard().catch((e) => alert(e.message));
$("diagCopyBtn").onclick = () => copyFleetSnapshot().catch((e) => alert(e.message));
```

Guard with null checks if elements missing.

- [ ] **Step 3: Manual verify**

Click **Copy fleet snapshot**, paste into an editor: JSON includes `hosts[].node_id`, `hosts[].schema_id`, and `carrier_zigbee`. Click **Refresh hosts** on Diagnostics; hostsUpdated / table update like the dashboard button.

- [ ] **Step 4: Commit only if user asked**

---

### Task 5: README + acceptance pass

**Files:**
- Modify: `tools/carrier_console/README.md`

**Interfaces:**
- Consumes: shipped UI behavior from Tasks 1–4
- Produces: operator-facing docs for Diagnostics

- [ ] **Step 1: Update README Connected hosts / features section**

Add a short subsection:

```markdown
### Diagnostics

The **Diagnostics** card (below Connected hosts) shows per-host uplink envelope fields (`node_id`, `schema_id`), link freshness, a readings summary, and carrier Zigbee radio/report counts. Use **Copy fleet snapshot** to clipboard JSON for support. Data comes from the same `fleet hosts` poll as the dashboard (no extra firmware commands). Host cards also show `node=` / `schema=` under the device name.
```

Fix any README claim that hosts go offline after **5s** if it still says that — UI/firmware stale is **20s**.

- [ ] **Step 2: Full acceptance checklist**

| # | Check | Pass? |
|---|--------|-------|
| 1 | Joined UL212: Diagnostics shows matching `node` / `schema` | |
| 2 | Host cards show same envelope line | |
| 3 | Stale host: offline styling in both places | |
| 4 | Copy snapshot JSON includes envelope + zigbee reports | |
| 5 | Refresh on Diagnostics updates table | |
| 6 | OBD section titled “OBD / DTC” | |
| 7 | No firmware / `app.py` changes required for happy path | |

- [ ] **Step 3: Commit only if user asked**

---

## Spec coverage (self-review)

| Spec requirement | Task |
|------------------|------|
| Host cards show node + schema | Task 2 |
| Diagnostics per-host table | Task 3 |
| Carrier Zigbee snapshot | Task 3 |
| Refresh hosts action | Task 4 |
| Copy fleet snapshot | Task 4 |
| Rename OBD / diagnostics | Task 1 |
| Empty / edge states | Task 3 |
| README | Task 5 |
| No new firmware / APIs | Global + Tasks 1–4 |
| Follow-ups (GPS, ingest, …) | Out of plan (spec non-goals) |

## Placeholder scan

No TBD / “similar to Task N” gaps. Commit steps intentionally gated on user request.
