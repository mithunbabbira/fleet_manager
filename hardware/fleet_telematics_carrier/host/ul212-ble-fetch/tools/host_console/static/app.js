const $ = (id) => document.getElementById(id);
const logEl = $("log");
const ul212Re = /\[UL212\]\s+([\d.]+)\s+mm.*?signal\s+(\d+)/i;

let ws;
let connected = false;
let activePort = null;
let pollTimer = null;
let cmdChain = Promise.resolve();
const scanHits = [];
let scanning = false;
let scanWatch = null;
/** True while ROM prints "waiting for download" — stop status spam. */
let downloadMode = false;

function stripAnsi(s) {
  return s.replace(/\x1b\[[0-9;]*m/g, "");
}

function setDownloadMode(on) {
  downloadMode = on;
  const el = $("bootWarn");
  if (el) el.hidden = !on;
  if (on) stopPollTimer();
  else if (connected) startPollTimer();
}

function setHeight(mm, signal, source) {
  $("height").innerHTML = `${mm}<span>mm</span>`;
  $("meta").textContent = `signal ${signal} · ${source}`;
}

function normalizeMac(raw) {
  const hex = (raw || "").replace(/[^0-9A-Fa-f]/g, "").toUpperCase();
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g).join(":");
}

function parseRx(line) {
  line = stripAnsi(line).trim();
  if (!line) return;

  if (/waiting for download/i.test(line) || /boot:0x4\s*\(DOWNLOAD/i.test(line)) {
    setDownloadMode(true);
  }
  if (/UL212 BLE Fetch/i.test(line) || /\[ble\] connecting/i.test(line) || /\[UL212\]/.test(line)) {
    setDownloadMode(false);
  }

  const ul212 = line.match(ul212Re);
  if (ul212) {
    setHeight(ul212[1], ul212[2], "live from serial");
  }

  const statusHeight = line.match(/height_mm=([\d.]+)/);
  if (statusHeight) {
    const sig = line.match(/signal=(\d+)/);
    setHeight(statusHeight[1], sig ? sig[1] : "?", "from status");
  }

  const cfgMac = line.match(/^mac=(.+)$/);
  if (cfgMac && cfgMac[1] !== "(not set)") $("macIn").value = cfgMac[1].trim();

  const cfgId = line.match(/^device_id=(\S+)/);
  if (cfgId) $("idIn").value = cfgId[1];

  const poll = line.match(/^poll=(\d+)/);
  if (poll) $("pollIn").value = poll[1];
  const sil = line.match(/^silence=(\d+)/);
  if (sil) $("silIn").value = sil[1];

  /* Firmware: "  MAC  rssi dBm  name" — line is trimmed, so no leading spaces. */
  const scanLine = line.match(
    /^([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+(-?\d+)\s+dBm\s*(.*)$/i
  );
  if (scanLine) {
    scanHits.push({
      mac: scanLine[1].toUpperCase(),
      rssi: scanLine[2],
      name: (scanLine[3] || "").trim(),
    });
    renderScanHits();
  }
  if (line.startsWith("scanning")) {
    scanHits.length = 0;
    scanning = true;
  }
  if (line.startsWith("scan: no devices found")) {
    scanning = false;
    $("scanOut").textContent = "No UL212 sensors found. Power the sensor and scan again.";
  }
  if (line.includes("save: ok")) {
    $("scanOut").textContent = "Saved to NVS — device rebooting…";
    setTimeout(() => {
      if (connected) sendCmd("status").catch(() => {});
    }, 3000);
  }

  const ble = line.match(/ble_connected=(\w+)/);
  const zb = line.match(/zigbee_joined=(\w+)/);
  if (ble || zb) {
    const cur = $("meta").textContent;
    const sig = cur.match(/signal\s+(\S+)/);
    $("meta").textContent =
      `${sig ? `signal ${sig[1]} · ` : ""}BLE ${ble ? ble[1] : "?"} · Zigbee ${zb ? zb[1] : "?"}`;
  }
}

function renderScanHits() {
  if (!scanHits.length) return;
  scanning = false;
  $("scanOut").innerHTML =
    '<div class="scan-hint">Click a sensor to set MAC and save (NVS):</div>' +
    scanHits
      .map(
        (s) =>
          `<a href="#" data-mac="${s.mac}">${s.mac}  ${s.rssi} dBm  ${s.name || ""}</a>`
      )
      .join("");
  $("scanOut").querySelectorAll("a").forEach((a) => {
    a.onclick = (e) => {
      e.preventDefault();
      selectAndSaveMac(a.dataset.mac).catch((err) => alert(err.message));
    };
  });
}

/** Fill MAC, write NVS via save (firmware reboots itself). */
async function selectAndSaveMac(mac) {
  const norm = normalizeMac(mac);
  if (!norm) throw new Error("Invalid MAC — use 00:65:01:0A:54:B7 or 0065010A54B7");
  $("macIn").value = norm;
  const id = $("idIn").value.trim() || "ul212-001";
  $("scanOut").textContent = `Saving ${norm}…`;
  stopPollTimer();
  await sendCmd(`mac ${norm}`);
  await sendCmd(`id ${id}`);
  const poll = +$("pollIn").value;
  const sil = +$("silIn").value;
  if (poll >= 200) await sendCmd(`poll ${poll}`);
  if (sil >= 3000) await sendCmd(`silence ${sil}`);
  await sendCmd("save");
  setTimeout(() => {
    if (connected) startPollTimer();
  }, 4000);
}

function appendLog(kind, line) {
  const span = document.createElement("span");
  span.className = kind;
  span.textContent = line + "\n";
  logEl.appendChild(span);
  logEl.scrollTop = logEl.scrollHeight;
  while (logEl.childNodes.length > 400) logEl.removeChild(logEl.firstChild);
  parseRx(line);
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
  const hostSn = data.host_usb_sn || "58:E6:C5:DB:7B:D4";
  const carrierSn = data.carrier_usb_sn || "10:BD:A3:96:5A:0C";
  sel.innerHTML = "";
  for (const p of data.ports) {
    const o = document.createElement("option");
    o.value = p.device;
    const sn = p.serial_number || "";
    let tag = "";
    if (sn === hostSn) tag = " [HOST]";
    if (sn === carrierSn) tag = " [CARRIER — do not use here]";
    o.textContent = `${p.device}${tag} — ${p.description}`;
    if (sn === carrierSn) o.disabled = true;
    sel.appendChild(o);
  }
  if (activePort) {
    sel.value = activePort;
  } else if (cur) {
    sel.value = cur;
  } else {
    const preferred =
      data.ports.find((p) => p.serial_number === hostSn) ||
      data.ports.find((p) => /usbmodem1101/i.test(p.device));
    if (preferred) sel.value = preferred.device;
  }
  syncConnectUi();
}

function stopPollTimer() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

function startPollTimer() {
  stopPollTimer();
  if (downloadMode) return;
  pollTimer = setInterval(() => {
    if (connected && !scanning && !downloadMode) sendCmd("status").catch(() => {});
  }, 2000);
}

async function refreshConn() {
  const s = await api("/api/connection");
  const was = connected;
  connected = s.connected;
  activePort = s.connected ? s.port : null;
  $("linkPill").textContent = s.connected ? `connected · ${s.port}` : "disconnected";
  $("linkPill").className = "pill " + (s.connected ? "live" : "down");
  if (activePort && $("portSel").value !== activePort) {
    $("portSel").value = activePort;
  }
  syncConnectUi();
  if (connected) {
    if (!was && !downloadMode) setTimeout(() => sendCmd("status").catch(() => {}), 800);
    startPollTimer();
  } else {
    stopPollTimer();
  }
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "clear_log") {
      logEl.innerHTML = "";
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
$("scanBtn").onclick = async () => {
  scanHits.length = 0;
  scanning = true;
  $("scanOut").textContent = "Scanning ~5 s…";
  stopPollTimer();
  if (scanWatch) clearTimeout(scanWatch);
  await sendCmd("scan");
  scanWatch = setTimeout(() => {
    if (scanning && !scanHits.length) {
      scanning = false;
      $("scanOut").textContent = "No devices found. Check sensor power / range.";
    }
    if (connected) startPollTimer();
  }, 7000);
};
$("statusBtn").onclick = () => sendCmd("status");
$("rebootBtn").onclick = () => sendCmd("reboot");

$("quitBtn").onclick = async () => {
  if (
    !confirm(
      "Stop Host Console on this computer?\n\nThe ESP32 keeps running — only this web app closes."
    )
  ) {
    return;
  }
  $("quitBtn").disabled = true;
  $("quitBtn").textContent = "Closing…";
  try {
    await api("/api/shutdown", { method: "POST" });
  } catch (_) {}
  document.body.innerHTML =
    '<div class="wrap"><div class="card"><h1>Host Console closed</h1>' +
    '<p class="hint">You can close this browser tab. Restart with ' +
    "<code>python3 tools/host_console/app.py</code></p></div></div>";
};

$("applyBtn").onclick = async () => {
  const macRaw = $("macIn").value.trim();
  const norm = macRaw ? normalizeMac(macRaw) : null;
  if (macRaw && !norm) {
    alert("MAC must be 12 hex digits, e.g. 00:65:01:0A:54:B7 or 0065010A54B7");
    return;
  }
  if (norm) $("macIn").value = norm;
  const id = $("idIn").value.trim();
  const poll = +$("pollIn").value;
  const sil = +$("silIn").value;
  if (norm) await sendCmd(`mac ${norm}`);
  if (id) await sendCmd(`id ${id}`);
  if (poll >= 200) await sendCmd(`poll ${poll}`);
  if (sil >= 3000) await sendCmd(`silence ${sil}`);
  await sendCmd("save");
  $("scanOut").textContent = "Saved to NVS — device rebooting…";
};

(async () => {
  connectWs();
  await refreshPorts();
  await refreshConn();
})();
