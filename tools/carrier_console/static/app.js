const $ = (id) => document.getElementById(id);
const logEl = $("log");

let ws;
let connected = false;
let activePort = null;
let provisioned = false;
let hostsTimer = null;
const profileNames = [];
const dtcCodes = { stored: [], pending: [] };

const carrier = {
  fw: null,
  deviceId: null,
  uptimeS: null,
  uptimeAt: null,
  lte: null,
  zigbee: null,
  zigbeeReports: null,
  uplink: null,
  gps: null,
  uplinkLast: null,
  sdQueue: null,
};

const HOST_STALE_MS = 20000;

let hostsState = null;
let hostsRenderTimer = null;
let connWatchTimer = null;
let reconnectBusy = false;
let lastRxAt = Date.now();
let lastUptimeS = null;
let cmdChain = Promise.resolve();

function stripAnsi(s) {
  return s.replace(/\x1b\[[0-9;]*m/g, "");
}

function setProvisionUi(yes) {
  provisioned = yes;
  const pill = $("provisionPill");
  pill.textContent = yes ? "provisioned" : "not provisioned";
  pill.className = "pill " + (yes ? "live" : "down");
  $("uplinkEn").disabled = !yes;
}

function appendLog(kind, line) {
  const span = document.createElement("span");
  span.className = kind;
  span.textContent = line + "\n";
  logEl.appendChild(span);
  logEl.scrollTop = logEl.scrollHeight;
  while (logEl.childNodes.length > 500) logEl.removeChild(logEl.firstChild);
  parseRxLogOnly(line);
}

function fmtAgo(ms) {
  if (ms == null || ms === "") return "—";
  const n = Number(ms);
  if (!Number.isFinite(n)) return String(ms);
  if (n < 5000) return "just now";
  if (n < 60000) return `${Math.round(n / 1000)}s ago`;
  return `${Math.round(n / 60000)}m ago`;
}

/** Extrapolate carrier uptime between status polls. */
function carrierUptimeMs() {
  if (carrier.uptimeS == null) return null;
  const base = carrier.uptimeS * 1000;
  if (carrier.uptimeAt == null) return base;
  return base + (Date.now() - carrier.uptimeAt);
}

/** last_seen_ms is carrier uptime at last report, not age. */
function hostReportAgeMs(lastSeenMs) {
  const nowMs = carrierUptimeMs();
  if (nowMs == null || lastSeenMs == null) return null;
  const last = Number(lastSeenMs);
  if (!Number.isFinite(last)) return null;
  return Math.max(0, nowMs - last);
}

function isHostOnline(h) {
  if (h.link === "0") return false;
  const age = hostReportAgeMs(h.last);
  if (age == null) return h.link === "1";
  return age <= HOST_STALE_MS;
}

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

function fmtReportAge(lastSeenMs) {
  const age = hostReportAgeMs(lastSeenMs);
  if (age == null) return "unknown";
  return fmtAgo(age);
}

function renderCarrierKv() {
  $("kvFw").textContent = carrier.fw || "—";
  $("kvDid").textContent = carrier.deviceId || $("uplinkDid").value || "—";
  $("kvUptime").textContent =
    carrier.uptimeS != null ? `${carrier.uptimeS}s` : "—";
  $("kvLte").textContent = carrier.lte || "—";
  const zbParts = [];
  if (carrier.zigbee) zbParts.push(carrier.zigbee);
  if (carrier.zigbeeReports != null) zbParts.push(`${carrier.zigbeeReports} reports`);
  $("kvZigbee").textContent = zbParts.length ? zbParts.join(" · ") : "—";
  $("kvUplink").textContent = carrier.uplink || "—";
  renderDiagnostics();
}

function renderHosts() {
  const pill = $("hostsPill");
  const list = $("hostsList");
  const snap = hostsState;

  if (!snap) {
    pill.textContent = "—";
    pill.className = "pill";
    renderDiagnostics();
    return;
  }

  const total = Math.max(snap.hosts.length, snap.host_count || 0);
  const online = snap.hosts.length
    ? snap.hosts.filter(isHostOnline).length
    : snap.joined || 0;
  pill.textContent = `${online} online · ${total} registered`;
  pill.className = "pill " + (online > 0 ? "live" : total > 0 ? "down" : "down");

  if (!total) {
    const waiting =
      carrier.uptimeS != null && carrier.uptimeS < 120
        ? "Carrier just rebooted — Zigbee registry is empty until the UL212 host rejoins (watch for <code>host joined</code> in the log)."
        : "No Zigbee hosts in registry. Power the UL212 host after the carrier and wait for <code>[zb] joined</code> on the host serial.";
    list.innerHTML = `<p class="empty-hint">${waiting}</p>`;
    renderDiagnostics();
    return;
  }

  if (snap.hosts.length < total) {
    list.innerHTML =
      `<p class="empty-hint">Registry reports ${total} host(s) but serial output was incomplete — click <strong>Refresh hosts</strong>.</p>`;
    renderDiagnostics();
    return;
  }

  list.innerHTML = snap.hosts
    .map((h) => {
      const online = isHostOnline(h);
      const readings = h.readings || {};
      const age = hostReportAgeMs(h.last);

      const chip = (key, label, hero) => {
        const r = readings[key];
        if (!r) return "";
        /* Keep last known values visible even when link is stale — serial log
         * already shows them; blanking the cards looked like a UI bug. */
        const val = r.value ?? "—";
        const unit = r.unit ? `<span class="ru">${r.unit}</span>` : "";
        return `<div class="reading${hero ? " hero-reading" : ""}"><span class="rk">${label}</span><span class="rv">${val}${unit}</span></div>`;
      };

      const zb = h.short_addr ? ` · Zigbee 0x${h.short_addr}` : "";
      const statusLabel = online ? "online" : age != null && age > HOST_STALE_MS ? "offline" : "link down";
      const statusClass = online ? "ok" : "bad";

      return `<article class="host-card${online ? "" : " offline"}">
        <div class="host-card-head">
          <div>
            <span class="name">${h.device_id}</span>
            <span class="type"> · ${h.host_type || "unknown"}</span>
          </div>
          <span class="host-link ${statusClass}">${statusLabel}</span>
        </div>
        <p class="host-envelope">${formatEnvelope(h)}</p>
        <div class="readings">
          ${chip("height_mm", "Fuel height", true)}
          ${chip("smooth_mm", "Smooth", false)}
          ${chip("temperature_c", "Temp", false)}
          ${chip("signal", "Signal", false)}
          ${chip("valid_echo", "Valid", false)}
          ${chip("tilt_deg", "Tilt", false)}
        </div>
        <p class="hint" style="margin:8px 0 0">Last report ${fmtReportAge(h.last)}${zb}</p>
      </article>`;
    })
    .join("");
  renderDiagnostics();
}

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

  const gpsEl = $("diagGps");
  const upEl = $("diagUplinkLast");
  const sdEl = $("diagSdQueue");
  if (gpsEl) {
    if (!carrier.gps) {
      gpsEl.textContent = "—";
    } else if (!carrier.gps.ok) {
      gpsEl.textContent = "no fix";
    } else {
      const age =
        carrier.gps.age_ms != null ? ` · age ${fmtAgo(carrier.gps.age_ms)}` : "";
      gpsEl.textContent = `ok · ${carrier.gps.lat}, ${carrier.gps.lng}${age}`;
    }
  }
  if (upEl) {
    const u = carrier.uplinkLast;
    if (!u) {
      upEl.textContent = "—";
    } else {
      const bits = [`http=${u.http}`];
      if (u.ok === true) bits.push("ok");
      if (u.ok === false) bits.push("fail");
      if (u.skipped) bits.push("skipped");
      if (u.reason) bits.push(u.reason);
      if (u.error) bits.push(`err=${u.error}`);
      upEl.textContent = bits.join(" · ");
    }
  }
  if (sdEl) {
    const q = carrier.sdQueue;
    if (!q) {
      sdEl.textContent = "—";
    } else {
      const bits = [
        q.sd_mounted ? "mounted" : "not mounted",
        `depth=${q.depth}`,
      ];
      if (q.bytes != null) bits.push(`bytes=${q.bytes}`);
      if (q.drain_err) bits.push(`drain=${q.drain_err}`);
      sdEl.textContent = bits.join(" · ");
    }
  }
}

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
    carrier_path: {
      gps: carrier.gps,
      uplink_last: carrier.uplinkLast,
      queue: carrier.sdQueue,
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
    if (status) {
      status.textContent = "Copy failed — select serial log or allow clipboard";
    }
    console.warn(e);
  }
  if (status) {
    setTimeout(() => {
      if (
        status.textContent.startsWith("Copied") ||
        status.textContent.startsWith("Copy failed")
      ) {
        status.textContent = "";
      }
    }, 2500);
  }
}

function applyDashboard(data) {
  if (!data) return;

  const h = data.hosts;
  if (h) {
    hostsState = {
      host_count: h.host_count || 0,
      joined: h.joined || 0,
      hosts: h.hosts || [],
    };
    renderHosts();
    if (h.updated_at) {
      $("hostsUpdated").textContent = `Updated ${new Date(
        h.updated_at * 1000
      ).toLocaleTimeString()}`;
    }
  }

  const c = data.carrier;
  if (!c) return;

  if (c.uptime_s != null) {
    noteUptime(c.uptime_s);
    carrier.uptimeS = c.uptime_s;
    carrier.uptimeAt = Date.now();
  }
  if (c.zigbee_reports != null) carrier.zigbeeReports = c.zigbee_reports;
  if (c.fw) carrier.fw = c.fw;
  if (c.provisioned != null) setProvisionUi(c.provisioned);
  if (c.can_ready) {
    const ok = c.can_ready === "yes";
    $("canStatus").textContent = ok ? "CAN ready" : "No CAN link";
    $("canStatus").style.color = ok ? "var(--ok)" : "var(--bad)";
  }
  if (c.can_meta) {
    $("canMeta").textContent =
      `protocol ${c.can_meta.protocol} · poller ${c.can_meta.poller} · profile ${c.can_meta.profile}`;
    const pname = c.can_meta.profile;
    if (pname && pname !== "(none)") {
      ensureProfileOption(pname);
      $("profileSel").value = pname;
      $("profileList").textContent = "Active: " + pname;
    }
  }
  renderCarrierKv();
  if (hostsState) renderHosts();
}

async function fetchDashboard() {
  const data = await api("/api/dashboard");
  applyDashboard(data);
}

function onCarrierReboot() {
  hostsState = null;
  carrier.zigbee = null;
  carrier.zigbeeReports = null;
  carrier.gps = null;
  carrier.uplinkLast = null;
  carrier.sdQueue = null;
  renderHosts();
  renderCarrierKv();
  if (connected) {
    setTimeout(() => refreshDashboard().catch(() => {}), 2000);
  }
}

function noteUptime(uptimeS) {
  const u = Number(uptimeS);
  if (!Number.isFinite(u)) return;
  if (lastUptimeS != null && u < lastUptimeS - 3) {
    onCarrierReboot();
  }
  lastUptimeS = u;
}

function parseRxLogOnly(raw) {
  let line = stripAnsi(raw).trim();
  if (!line) return;
  line = line.replace(/^(?:obd>\s*)+/g, "").trim();
  if (!line) return;

  const kv = line.match(/^([a-z_]+)=(.*)$/);
  if (kv) {
    const k = kv[1];
    const v = kv[2].trim();
    if (k === "provisioned") setProvisionUi(v === "yes");
    if (k === "uplink_enabled") $("uplinkEn").checked = v === "yes";
    if (k === "uplink_interval") $("uplinkIv").value = v;
    if (k === "uplink_device_id") {
      $("uplinkDid").value = v;
      carrier.deviceId = v;
    }
    if (k === "uplink_node_id") $("uplinkNid").value = v;
    if (k === "uplink_url") $("uplinkUrl").value = v;
    if (k === "uplink_schema") $("uplinkSchema").value = v;
    if (k === "ota_force") $("otaForce").checked = v === "on";
    if (k === "ota_url") $("otaUrl").value = v;
    if (k === "allow_unsafe") $("unsafeEn").checked = v === "true";
    if (k === "profile" && v !== "(none)") {
      ensureProfileOption(v);
      $("profileSel").value = v;
    }
    if (k === "vin") $("dtcOut").textContent = "VIN: " + v;
  }

  const can = line.match(/^can_ready=(\w+)/);
  if (can) {
    const ok = can[1] === "yes";
    $("canStatus").textContent = ok ? "CAN ready" : "No CAN link";
    $("canStatus").style.color = ok ? "var(--ok)" : "var(--bad)";
  }
  const st = line.match(/^can_ready=\w+ protocol=(\S+) poller=(\w+) profile=(\S+)/);
  if (st) {
    $("canMeta").textContent = `protocol ${st[1]} · poller ${st[2]} · profile ${st[3]}`;
    if (st[3] && st[3] !== "(none)") {
      ensureProfileOption(st[3]);
      $("profileSel").value = st[3];
      $("profileList").textContent = "Active: " + st[3];
    }
  }

  const metrics = line.match(/^metrics=(\{.*\})$/);
  if (metrics) {
    try {
      const m = JSON.parse(metrics[1]);
      if (m.uptime_s != null) noteUptime(m.uptime_s);
    } catch (_) {}
  }

  const zbJoin = line.match(/host joined 0x([0-9a-f]+)/i);
  if (zbJoin) {
    carrier.zigbee = `host joined 0x${zbJoin[1].toUpperCase()}`;
    renderCarrierKv();
  }

  const lte = line.match(/^lte:\s*(.+)$/);
  if (lte) {
    const parts = lte[1].split(/\s+/);
    const flags = [];
    for (const p of parts) {
      const m = p.match(/^(uart_ok|sim|reg|attached)=(\w+)/);
      if (m) flags.push(`${m[1]} ${m[2]}`);
      const csq = p.match(/^csq=(\d+)/);
      if (csq) flags.push(`csq ${csq[1]}`);
    }
    carrier.lte = flags.join(" · ") || lte[1].slice(0, 80);
    renderCarrierKv();
  }

  const otaFw = line.match(/^ota:\s*fw=(\S+)/);
  if (otaFw) {
    carrier.fw = otaFw[1];
    renderCarrierKv();
  }

  const uplinkHdr = line.match(
    /^uplink:\s*enabled=(\w+)\s+interval=(\d+)s?\s+device_id=(\S+)\s+node_id=(\S+)/
  );
  if (uplinkHdr) {
    $("uplinkEn").checked = uplinkHdr[1] === "yes";
    $("uplinkIv").value = uplinkHdr[2];
    if (uplinkHdr[3] && uplinkHdr[3] !== "(none)") {
      $("uplinkDid").value = uplinkHdr[3];
      carrier.deviceId = uplinkHdr[3];
    }
    if (uplinkHdr[4]) $("uplinkNid").value = uplinkHdr[4];
    carrier.uplink =
      uplinkHdr[1] === "yes"
        ? `on · every ${uplinkHdr[2]}s`
        : "off";
    renderCarrierKv();
  }

  const uplinkUrlLine = line.match(/^\s*url=(\S+)\s+schemaId=(\S+)/);
  if (uplinkUrlLine) {
    if (uplinkUrlLine[1]) $("uplinkUrl").value = uplinkUrlLine[1];
    if (uplinkUrlLine[2]) $("uplinkSchema").value = uplinkUrlLine[2];
  }

  const uplinkFail = line.match(/uplink:.*POST fail/);
  const uplinkOk = line.match(/uplink: enqueued/);
  if (uplinkFail) carrier.uplink = "POST failing (see log)";
  if (uplinkOk) carrier.uplink = "queueing batches";
  if (uplinkFail || uplinkOk) renderCarrierKv();

  let pathDirty = false;
  const lastM = line.match(
    /(?:^|\s)last:\s*ok=(\w+)\s+skipped=(\w+)\s+http=(-?\d+)\s+reason="([^"]*)"\s+error="([^"]*)"/
  );
  if (lastM) {
    carrier.uplinkLast = {
      ok: lastM[1] === "yes",
      skipped: lastM[2] === "yes",
      http: Number(lastM[3]),
      reason: lastM[4] || "",
      error: lastM[5] || "",
    };
    pathDirty = true;
  }
  const nowM = line.match(
    /uplink now:\s*\S+\s+http=(-?\d+)\s+reason="([^"]*)"\s+error="([^"]*)"/
  );
  if (nowM) {
    carrier.uplinkLast = {
      ok: Number(nowM[1]) >= 200 && Number(nowM[1]) < 300,
      skipped: false,
      http: Number(nowM[1]),
      reason: nowM[2] || "",
      error: nowM[3] || "",
    };
    pathDirty = true;
  }
  const qM = line.match(
    /(?:^|\s)queue:\s*sd=(\w+)\s+depth=(\d+)\s+bytes=(\d+)\s+drain_err="([^"]*)"/
  );
  if (qM) {
    carrier.sdQueue = {
      sd_mounted: qM[1] === "yes",
      depth: Number(qM[2]),
      bytes: Number(qM[3]),
      drain_err: qM[4] || "",
    };
    pathDirty = true;
  }
  const qtestM = line.match(
    /uplink qtest:.*\bsd=(\w+)\s+depth=(\d+)/
  );
  if (qtestM) {
    carrier.sdQueue = {
      sd_mounted: qtestM[1] === "yes",
      depth: Number(qtestM[2]),
      bytes: carrier.sdQueue?.bytes ?? null,
      drain_err: carrier.sdQueue?.drain_err || "",
    };
    pathDirty = true;
  }
  const gpsOk = line.match(
    /(?:^|\s)gps:\s*ok\s+lat=(-?[\d.]+)\s+lng=(-?[\d.]+)\s+age_ms=(\d+)/
  );
  if (gpsOk) {
    carrier.gps = {
      ok: true,
      lat: gpsOk[1],
      lng: gpsOk[2],
      age_ms: Number(gpsOk[3]),
    };
    pathDirty = true;
  } else if (/(?:^|\s)gps:\s*no fix/.test(line)) {
    carrier.gps = { ok: false, lat: null, lng: null, age_ms: null };
    pathDirty = true;
  }
  if (pathDirty) renderDiagnostics();

  const zbTlv = line.match(/TLV\s+\d+\s+B\s+from\s+0x([0-9a-f]+)/i);
  if (zbTlv) {
    carrier.zigbee = `last frame 0x${zbTlv[1].toUpperCase()}`;
    if (hostsState && hostsState.hosts.length === 1) {
      hostsState.hosts[0].short_addr = zbTlv[1].toUpperCase();
      renderHosts();
    }
    renderCarrierKv();
  }

  const profLine = line.match(/^\s+(\S+)(\s+\*)?$/);
  if (profLine && line.startsWith("  ") && !line.includes("=")) {
    const name = profLine[1];
    if (name && !name.startsWith("(")) {
      ensureProfileOption(name);
      profileNames.push(name);
    }
  }
  const profHdr = line.match(/^profiles \(\d+\), active=(\S+)/);
  if (profHdr) {
    $("profileList").textContent = "Active: " + profHdr[1];
    ensureProfileOption(profHdr[1]);
    $("profileSel").value = profHdr[1];
  }

  const dtcCnt = line.match(/^dtc_(stored|pending)_count=(\d+)/);
  if (dtcCnt) dtcCodes[dtcCnt[1]] = [];
  const dtcCode = line.match(/^dtc_(stored|pending)_(\d+)=(\S+)/);
  if (dtcCode) {
    dtcCodes[dtcCode[1]][+dtcCode[2]] = dtcCode[3];
    renderDtc();
  }
}

function renderDtc() {
  const s = dtcCodes.stored.filter(Boolean);
  const p = dtcCodes.pending.filter(Boolean);
  $("dtcOut").textContent =
    `Stored (${s.length}): ${s.join(", ") || "none"} · Pending (${p.length}): ${p.join(", ") || "none"}`;
}

function ensureProfileOption(name) {
  const sel = $("profileSel");
  for (const o of sel.options) {
    if (o.value === name) return;
  }
  const o = document.createElement("option");
  o.value = name;
  o.textContent = name;
  sel.appendChild(o);
}

async function api(path, opts) {
  const r = await fetch(path, opts);
  if (!r.ok) {
    const t = await r.text();
    throw new Error(t || r.statusText);
  }
  return r.json();
}

function syncConnectUi() {
  const sel = $("portSel").value;
  const btn = $("connectBtn");
  btn.disabled = !sel;
  if (connected && activePort && sel === activePort) {
    btn.textContent = "Reconnect";
  } else {
    btn.textContent = "Connect";
  }
  $("disconnectBtn").disabled = !connected;
}

async function refreshPorts() {
  const data = await api("/api/ports");
  const sel = $("portSel");
  const cur = sel.value;
  const carrierSn = data.carrier_usb_sn || "10:BD:A3:96:5A:0C";
  const hostSn = data.host_usb_sn || "58:E6:C5:DB:7B:D4";
  sel.innerHTML = "";
  for (const p of data.ports) {
    const o = document.createElement("option");
    o.value = p.device;
    const sn = p.serial_number || "";
    let tag = "";
    if (sn === carrierSn) tag = " [CARRIER]";
    if (sn === hostSn) tag = " [HOST — do not use here]";
    o.textContent = `${p.device}${tag} — ${p.description}`;
    if (sn === hostSn) o.disabled = true;
    sel.appendChild(o);
  }
  if (activePort) {
    sel.value = activePort;
  } else if (cur) {
    sel.value = cur;
  } else {
    const preferred =
      data.ports.find((p) => p.serial_number === carrierSn) ||
      data.ports.find((p) => /usbmodem1201/i.test(p.device));
    if (preferred) sel.value = preferred.device;
  }
  syncConnectUi();
}

function stopHostsTimer() {
  if (hostsTimer) {
    clearInterval(hostsTimer);
    hostsTimer = null;
  }
  if (hostsRenderTimer) {
    clearInterval(hostsRenderTimer);
    hostsRenderTimer = null;
  }
  if (connWatchTimer) {
    clearInterval(connWatchTimer);
    connWatchTimer = null;
  }
}

function startHostsTimer() {
  stopHostsTimer();
  hostsTimer = setInterval(() => {
    if (!connected) return;
    pollCarrierDashboard().catch(() => {});
  }, 5000);
  hostsRenderTimer = setInterval(() => {
    if (hostsState) renderHosts();
  }, 1000);
  connWatchTimer = setInterval(() => {
    watchConnection().catch(() => {});
  }, 3000);
}

async function watchConnection() {
  const s = await api("/api/connection");
  if (!s.connected && connected) {
    connected = false;
    activePort = activePort || s.port;
    $("linkPill").textContent = "disconnected (serial lost)";
    $("linkPill").className = "pill down";
    syncConnectUi();
    stopHostsTimer();
    hostsState = null;
    carrier.gps = null;
    carrier.uplinkLast = null;
    carrier.sdQueue = null;
    renderHosts();
    // Do NOT auto-reopen USB — that puts ESP32-C6 into ROM download mode after
    // host/carrier power cycles and mixes old log buffers across devices.
    return;
  }
}

async function autoReconnect() {
  /* Intentionally disabled. Manual Connect only. */
}

let pollBusy = false;

async function pollCarrierDashboard() {
  if (pollBusy) return;
  pollBusy = true;
  try {
    await fetchDashboard();
  } finally {
    pollBusy = false;
  }
}

async function refreshDashboard() {
  if (!connected) return;
  await api("/api/dashboard/refresh", { method: "POST" });
  await fetchDashboard();
}

async function refreshConn() {
  const s = await api("/api/connection");
  const was = connected;
  connected = s.connected;
  activePort = s.connected ? s.port : activePort;
  $("linkPill").textContent = s.connected
    ? `connected · ${s.port}`
    : "disconnected";
  $("linkPill").className = "pill " + (s.connected ? "live" : "down");
  if (activePort && $("portSel").value !== activePort) {
    $("portSel").value = activePort;
  }
  syncConnectUi();
  if (connected) {
    lastRxAt = Date.now();
    if (!was) {
      lastUptimeS = null;
      setTimeout(() => refreshDashboard().catch(() => {}), 1200);
      setTimeout(() => fetchDashboard().catch(() => {}), 400);
      /* profiles list fills Active profile dropdown (PID set in NVS). */
      setTimeout(() => sendCmd("profiles").catch(() => {}), 1800);
    }
    startHostsTimer();
  } else {
    stopHostsTimer();
    if (!reconnectBusy) {
      hostsState = null;
      renderHosts();
    }
  }
}

function clearLog() {
  logEl.innerHTML = "";
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onmessage = (ev) => {
    lastRxAt = Date.now();
    const msg = JSON.parse(ev.data);
    if (msg.type === "dashboard") {
      applyDashboard(msg.data);
      return;
    }
    if (msg.type === "clear_log") {
      clearLog();
      return;
    }
    if (msg.type === "serial_lost") {
      appendLog("sys", `Serial lost: ${msg.reason || "device reset"}`);
      refreshConn().catch(() => {});
      return;
    }
    if (msg.type === "tx") appendLog("tx", "→ " + msg.line);
    else if (msg.type === "rx") appendLog("rx", msg.line);
    else appendLog("sys", msg.line);
  };
  ws.onclose = () => setTimeout(connectWs, 1500);
}

async function sendCmd(line) {
  const run = async () => {
    await api("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ line }),
    });
  };
  cmdChain = cmdChain.then(run, run);
  return cmdChain;
}

async function connect() {
  const port = $("portSel").value;
  if (!port) return alert("Pick a serial port");
  const s = await api("/api/connection");
  if (s.connected) {
    await api("/api/disconnect", { method: "POST" });
    await new Promise((r) => setTimeout(r, 350));
  }
  await api("/api/connect", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ port }),
  });
  await refreshConn();
}

async function disconnect() {
  await api("/api/disconnect", { method: "POST" });
  await refreshConn();
}

$("refreshBtn").onclick = () => refreshPorts().catch((e) => alert(e.message));
$("portSel").onchange = syncConnectUi;
$("connectBtn").onclick = () => connect().catch((e) => alert(e.message));
$("disconnectBtn").onclick = () => disconnect().catch((e) => alert(e.message));
$("statusBtn").onclick = () => {
  sendCmd("status");
  sendCmd("ota status");
};
$("profilesRefresh").onclick = () => sendCmd("profiles");
$("profileApply").onclick = () => {
  const n = $("profileSel").value;
  if (n) sendCmd(`profile ${n}`);
};
$("obdSend").onclick = () => {
  const c = $("obdCmd").value.trim();
  if (c) sendCmd(`cmd ${c}`);
};
$("vinBtn").onclick = () => sendCmd("vin");
$("dtcRead").onclick = () => {
  dtcCodes.stored = [];
  dtcCodes.pending = [];
  sendCmd("dtc read");
};
$("dtcClear").onclick = () => {
  if (!confirm("Clear stored DTCs (Mode 04)? Requires unsafe mode.")) return;
  sendCmd("dtc clear");
};
$("lteStatus").onclick = () => sendCmd("lte");
$("lteReconnect").onclick = () => sendCmd("lte reconnect");
$("lteTest").onclick = () => sendCmd("lte test");
$("uplinkNow").onclick = () => {
  if (!provisioned) return alert("Save device identity first");
  sendCmd("uplink now");
};
$("uplinkQtest").onclick = () => {
  if (!provisioned) return alert("Save device identity first");
  sendCmd("uplink qtest");
};
$("otaStatus").onclick = () => sendCmd("ota status");
$("otaRun").onclick = () => {
  if (!provisioned) return alert("Save device identity first");
  sendCmd("ota run");
};
$("fleetHosts").onclick = () =>
  api("/api/dashboard/refresh", { method: "POST" })
    .then(fetchDashboard)
    .catch((e) => alert(e.message));
$("fleetHostsRefresh").onclick = () => refreshDashboard().catch((e) => alert(e.message));
const diagRefreshBtn = $("diagRefreshBtn");
if (diagRefreshBtn) {
  diagRefreshBtn.onclick = () => refreshDashboard().catch((e) => alert(e.message));
}
const diagCopyBtn = $("diagCopyBtn");
if (diagCopyBtn) {
  diagCopyBtn.onclick = () => copyFleetSnapshot().catch((e) => alert(e.message));
}

$("provisionBtn").onclick = async () => {
  const did = $("uplinkDid").value.trim();
  const nid = $("uplinkNid").value.trim();
  if (!did || !nid) return alert("Device ID and Node ID are required");
  await sendCmd(`provision ${did} ${nid}`);
  await sendCmd("ota status");
};

$("uplinkApply").onclick = async () => {
  if (!provisioned) return alert("Save device identity first");
  const url = $("uplinkUrl").value.trim();
  const schema = $("uplinkSchema").value.trim();
  const iv = +$("uplinkIv").value;
  if (url) await sendCmd(`uplink url ${url}`);
  if (schema) await sendCmd(`uplink schema ${schema}`);
  if (iv >= 5) await sendCmd(`uplink interval ${iv}`);
  await sendCmd($("uplinkEn").checked ? "uplink on" : "uplink off");
  await sendCmd("ota status");
};

$("otaApply").onclick = async () => {
  await sendCmd(`ota force ${$("otaForce").checked ? "on" : "off"}`);
  const url = $("otaUrl").value.trim();
  if (url) await sendCmd(`ota url ${url}`);
  await sendCmd("ota status");
};

$("unsafeApply").onclick = async () => {
  await sendCmd($("unsafeEn").checked ? "unsafe on" : "unsafe off");
};

$("quitBtn").onclick = async () => {
  if (!confirm("Stop Carrier Console on this computer?\n\nThe ESP32 keeps running.")) return;
  $("quitBtn").disabled = true;
  try {
    await api("/api/shutdown", { method: "POST" });
  } catch (_) {}
  document.body.innerHTML =
    '<div class="wrap"><div class="card"><h1>Carrier Console closed</h1>' +
    '<p class="hint">Restart with <code>python3 tools/carrier_console/app.py</code></p></div></div>';
};

setProvisionUi(false);
renderCarrierKv();
renderHosts();

(async () => {
  connectWs();
  await refreshPorts();
  await refreshConn();
})();
