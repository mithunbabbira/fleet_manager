#pragma once

#include "fleet_manifest_catalog.h"
#include "fleet_tlv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLEET_REGISTRY_MAX_HOSTS 16
#define FLEET_REGISTRY_MAX_READINGS FLEET_MANIFEST_MAX_READINGS

/** No REPORT/HELLO within this window → host marked offline in snapshot.
 *  BLE+Zigbee coexistence can leave multi-second gaps; 5s was too tight. */
#ifndef FLEET_HOST_STALE_MS
#define FLEET_HOST_STALE_MS 20000
#endif

typedef struct {
    char key[FLEET_MANIFEST_KEY_MAX];
    char unit[FLEET_MANIFEST_UNIT_MAX];
    uint16_t tlv_id;
    double value;
    bool valid;
    uint64_t ts_ms;
} fleet_registry_reading_t;

typedef struct {
    char device_id[FLEET_DEVICE_ID_MAX];
    char node_id[FLEET_NODE_ID_MAX];
    char schema_id[FLEET_SCHEMA_ID_MAX];
    char host_type[FLEET_MANIFEST_TYPE_NAME_MAX];
    uint16_t host_type_id;
    uint16_t short_addr;
    bool link_ok;
    uint64_t last_seen_ms;
    uint8_t reading_count;
    fleet_registry_reading_t readings[FLEET_REGISTRY_MAX_READINGS];
} fleet_registry_host_t;

typedef struct {
    uint16_t pan_id;
    uint8_t channel;
    uint32_t joined_count;
    uint8_t host_count;
    fleet_registry_host_t hosts[FLEET_REGISTRY_MAX_HOSTS];
} fleet_registry_snapshot_t;

void host_registry_init(void);

int host_registry_ingest_frame(const uint8_t *frame, size_t frame_len, uint64_t now_ms,
                               uint16_t short_addr);

int host_registry_ingest_frame_ex(const uint8_t *frame, size_t frame_len, uint64_t now_ms,
                                  uint16_t short_addr, bool publish_to_bus);

const fleet_manifest_entry_t *host_registry_find_manifest(uint16_t host_type_id);

bool host_registry_snapshot(fleet_registry_snapshot_t *out);

/** Recompute link_ok / joined_count (also run automatically on snapshot in firmware). */
void host_registry_refresh_links(uint64_t now_ms);

int host_registry_to_telemetry(const fleet_registry_host_t *host,
                               void *telemetry_out /* telemetry_host_report_t * */);

#ifdef __cplusplus
}
#endif
