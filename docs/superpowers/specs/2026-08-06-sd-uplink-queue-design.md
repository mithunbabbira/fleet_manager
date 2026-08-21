# SD-backed uplink queue (batch drain) — Design

Device: ESP32-C6 + MCP2515 (soft-SPI 21/22/23/20) + microSD (SPI2 4/5/6 CS18) + EC200U LTE (UART GPIO16 TX / GPIO17 RX)  
Date: 2026-08-06

## Goal

Do not drop telemetry when LTE/API fails. Enqueue every constructed uplink snapshot on SD; remove records only after a successful batch HTTPS POST.

## Flow

1. Producer (tick): build payload JSON → append one NDJSON line to SD queue.
2. Consumer (drain task): read up to N oldest lines → POST batch body → on HTTP 2xx advance head; on failure leave queue and back off.
3. Queue full: drop oldest then append.
4. SD missing: fall back to live single POST (legacy path); do not block CAN/LTE.

## Batch API (backend)

URL unchanged: `https://api.trafyn.info/nc-events-api/v2/messages`

```json
{
  "schemaId": "1087",
  "events": [
    { "payload": { /* same fields as single POST payload */ }, "queued_at_ms": 123 },
    { "payload": { /* ... */ }, "queued_at_ms": 456 }
  ]
}
```

- Cap: max 10 events or ~12 KiB body.
- Success: HTTP 2xx → dequeue that many records.

## On-disk layout

- Mount point: `/sdcard`
- Queue: `/sdcard/uplinkq.dat` (one JSON object per line; 8.3 FatFS name)
- Meta: `/sdcard/uplinkq.met` — `head_offset` (byte offset of first unread line), `count` (approx pending lines)
- Max queue bytes: configurable (default 16 MiB)

## SPI sharing

- MCP2515: soft-SPI on GPIO **21/22/23/20** (ESP32-C6 has only one GPSPI)
- microSD: hardware SPI2 on GPIO **4/5/6/18**
- Separate wires — no shared SCK/MOSI/MISO
- Optional PCB: 10 kΩ pull-ups on both CS to 3.3 V
- Boot: drive CS idle-HIGH before init

## SoftAP / serial

Status includes: `sd_mounted`, `queue_depth`, `queue_bytes`, last drain error/phase.
