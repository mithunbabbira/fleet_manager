#include "host_registry.h"
#include "telemetry_bus.h"

#ifndef HOST_REGISTRY_HOST_TEST
#include "esp_timer.h"
#endif

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static fleet_registry_snapshot_t s_registry;

static fleet_manifest_value_type_t tlv_to_manifest_type(fleet_value_type_t t)
{
    switch (t) {
    case FLEET_VAL_FLOAT:
        return FLEET_MANIFEST_FLOAT;
    case FLEET_VAL_UINT8:
        return FLEET_MANIFEST_UINT8;
    case FLEET_VAL_UINT16:
        return FLEET_MANIFEST_UINT16;
    case FLEET_VAL_INT32:
        return FLEET_MANIFEST_INT32;
    case FLEET_VAL_STRING:
        return FLEET_MANIFEST_STRING;
    default:
        return FLEET_MANIFEST_FLOAT;
    }
}

static double tlv_to_double(const fleet_tlv_value_t *v)
{
    switch (v->type) {
    case FLEET_VAL_FLOAT:
        return (double)v->value.f32;
    case FLEET_VAL_UINT8:
        return (double)v->value.u8;
    case FLEET_VAL_UINT16:
        return (double)v->value.u16;
    case FLEET_VAL_INT32:
        return (double)v->value.i32;
    default:
        return 0.0;
    }
}

void host_registry_init(void)
{
    memset(&s_registry, 0, sizeof(s_registry));
}

const fleet_manifest_entry_t *host_registry_find_manifest(uint16_t host_type_id)
{
    return fleet_manifest_find(host_type_id);
}

static void seed_readings_from_manifest(fleet_registry_host_t *h,
                                        const fleet_manifest_entry_t *manifest)
{
    if (!manifest) {
        return;
    }
    strncpy(h->host_type, manifest->host_type, sizeof(h->host_type) - 1);
    h->reading_count = manifest->reading_count;
    for (uint8_t r = 0; r < manifest->reading_count; r++) {
        strncpy(h->readings[r].key, manifest->readings[r].key, sizeof(h->readings[r].key) - 1);
        strncpy(h->readings[r].unit, manifest->readings[r].unit,
                sizeof(h->readings[r].unit) - 1);
        h->readings[r].tlv_id = manifest->readings[r].tlv_id;
        h->readings[r].valid = false;
        h->readings[r].value = 0.0;
        h->readings[r].ts_ms = 0;
    }
}

static fleet_registry_host_t *find_or_alloc_host(const char *device_id, uint16_t host_type_id,
                                                 const fleet_manifest_entry_t *manifest)
{
    for (uint8_t i = 0; i < s_registry.host_count; i++) {
        if (strcmp(s_registry.hosts[i].device_id, device_id) == 0) {
            return &s_registry.hosts[i];
        }
    }
    fleet_registry_host_t *h;
    if (s_registry.host_count >= FLEET_REGISTRY_MAX_HOSTS) {
        /* Table full: recycle the least-recently-seen slot so a replaced or
         * re-provisioned host can never be locked out until reboot. */
        h = &s_registry.hosts[0];
        for (uint8_t i = 1; i < s_registry.host_count; i++) {
            if (s_registry.hosts[i].last_seen_ms < h->last_seen_ms) {
                h = &s_registry.hosts[i];
            }
        }
    } else {
        h = &s_registry.hosts[s_registry.host_count++];
    }
    memset(h, 0, sizeof(*h));
    strncpy(h->device_id, device_id, sizeof(h->device_id) - 1);
    h->host_type_id = host_type_id;
    seed_readings_from_manifest(h, manifest);
    return h;
}

static fleet_manifest_value_type_t parse_type_token(const char *tok)
{
    if (!tok || !tok[0]) {
        return FLEET_MANIFEST_FLOAT;
    }
    if (strcmp(tok, "f") == 0 || strcmp(tok, "float") == 0) {
        return FLEET_MANIFEST_FLOAT;
    }
    if (strcmp(tok, "u8") == 0 || strcmp(tok, "uint8") == 0) {
        return FLEET_MANIFEST_UINT8;
    }
    if (strcmp(tok, "u16") == 0 || strcmp(tok, "uint16") == 0) {
        return FLEET_MANIFEST_UINT16;
    }
    if (strcmp(tok, "i32") == 0 || strcmp(tok, "int32") == 0) {
        return FLEET_MANIFEST_INT32;
    }
    if (strcmp(tok, "s") == 0 || strcmp(tok, "string") == 0) {
        return FLEET_MANIFEST_STRING;
    }
    return FLEET_MANIFEST_FLOAT;
}

/**
 * Parse HELLO metric map: `tlv_id:key:unit:type;...`
 * Type letters: f, u8, u16, i32, s. Unit may be empty.
 */
static void apply_metric_map(fleet_registry_host_t *host, const char *map)
{
    if (!host || !map || !map[0]) {
        return;
    }

    char buf[FLEET_METRIC_MAP_MAX];
    strncpy(buf, map, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint8_t count = 0;
    char *save = NULL;
    for (char *entry = strtok_r(buf, ";", &save); entry != NULL;
         entry = strtok_r(NULL, ";", &save)) {
        if (count >= FLEET_REGISTRY_MAX_READINGS) {
            break;
        }
        char *p = entry;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            continue;
        }

        char *c1 = strchr(p, ':');
        if (!c1) {
            continue;
        }
        *c1 = '\0';
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) {
            continue;
        }
        *c2 = '\0';
        char *c3 = strchr(c2 + 1, ':');
        if (!c3) {
            continue;
        }
        *c3 = '\0';

        long tlv = strtol(p, NULL, 10);
        if (tlv <= 0 || tlv > 255 || tlv == FLEET_TLV_METRIC_MAP) {
            continue;
        }
        const char *key = c1 + 1;
        const char *unit = c2 + 1;
        const char *type_tok = c3 + 1;
        if (!key[0]) {
            continue;
        }
        (void)parse_type_token(type_tok);

        fleet_registry_reading_t *slot = &host->readings[count++];
        memset(slot, 0, sizeof(*slot));
        slot->tlv_id = (uint16_t)tlv;
        strncpy(slot->key, key, sizeof(slot->key) - 1);
        strncpy(slot->unit, unit, sizeof(slot->unit) - 1);
    }

    if (count > 0) {
        host->reading_count = count;
        for (uint8_t i = count; i < FLEET_REGISTRY_MAX_READINGS; i++) {
            memset(&host->readings[i], 0, sizeof(host->readings[i]));
        }
    }
}

static void apply_reading(fleet_registry_host_t *host, const fleet_manifest_entry_t *manifest,
                          const fleet_tlv_value_t *tlv, uint64_t ts_ms)
{
    if (tlv->tlv_id == FLEET_TLV_METRIC_MAP) {
        if (tlv->type == FLEET_VAL_STRING && tlv->valid) {
            apply_metric_map(host, tlv->value.str);
        }
        return;
    }

    for (uint8_t i = 0; i < host->reading_count; i++) {
        if (host->readings[i].tlv_id != 0) {
            if (host->readings[i].tlv_id != tlv->tlv_id) {
                continue;
            }
        } else if (manifest && i < manifest->reading_count) {
            if (manifest->readings[i].tlv_id != tlv->tlv_id) {
                continue;
            }
            if (manifest->readings[i].type != tlv_to_manifest_type(tlv->type)) {
                continue;
            }
        } else {
            continue;
        }
        host->readings[i].value = tlv_to_double(tlv);
        host->readings[i].valid = tlv->valid;
        host->readings[i].ts_ms = ts_ms;
        if (host->readings[i].tlv_id == 0) {
            host->readings[i].tlv_id = tlv->tlv_id;
        }
        return;
    }
}

int host_registry_ingest_frame(const uint8_t *frame, size_t frame_len, uint64_t now_ms,
                               uint16_t short_addr)
{
    return host_registry_ingest_frame_ex(frame, frame_len, now_ms, short_addr, true);
}

int host_registry_ingest_frame_ex(const uint8_t *frame, size_t frame_len, uint64_t now_ms,
                                  uint16_t short_addr, bool publish_to_bus)
{
    fleet_decoded_frame_t decoded;
    if (fleet_tlv_decode(frame, frame_len, &decoded) != 0) {
        return -1;
    }
    if (!fleet_tlv_header_valid(&decoded.header)) {
        return -2;
    }

    const bool has_envelope = fleet_tlv_header_has_envelope(&decoded.header);
    const fleet_manifest_entry_t *manifest =
        fleet_manifest_find(decoded.header.host_type_id);
    if (!manifest && !has_envelope) {
        return -3;
    }

    if (decoded.msg_type != FLEET_MSG_REPORT && decoded.msg_type != FLEET_MSG_HELLO) {
        return -4;
    }

    fleet_registry_host_t *host =
        find_or_alloc_host(decoded.header.device_id, decoded.header.host_type_id, manifest);
    if (!host) {
        return -5;
    }

    if (decoded.header.host_type_id != 0) {
        host->host_type_id = decoded.header.host_type_id;
    }
    if (decoded.header.host_type[0]) {
        strncpy(host->host_type, decoded.header.host_type, sizeof(host->host_type) - 1);
    } else if (manifest && !host->host_type[0]) {
        strncpy(host->host_type, manifest->host_type, sizeof(host->host_type) - 1);
    }
    if (decoded.header.node_id[0]) {
        strncpy(host->node_id, decoded.header.node_id, sizeof(host->node_id) - 1);
    }
    if (decoded.header.schema_id[0]) {
        strncpy(host->schema_id, decoded.header.schema_id, sizeof(host->schema_id) - 1);
    }

    host->short_addr = short_addr;
    host->link_ok = true;
    host->last_seen_ms = now_ms ? now_ms : decoded.header.ts_ms;

    /* Metric maps first so REPORT values in the same frame can bind. */
    for (uint8_t i = 0; i < decoded.reading_count; i++) {
        if (decoded.readings[i].tlv_id == FLEET_TLV_METRIC_MAP) {
            apply_reading(host, manifest, &decoded.readings[i], decoded.header.ts_ms);
        }
    }
    for (uint8_t i = 0; i < decoded.reading_count; i++) {
        if (decoded.readings[i].tlv_id != FLEET_TLV_METRIC_MAP) {
            apply_reading(host, manifest, &decoded.readings[i], decoded.header.ts_ms);
        }
    }

    if (manifest) {
        for (uint8_t i = 0; i < manifest->reading_count; i++) {
            if (!manifest->readings[i].required) {
                continue;
            }
            if (decoded.msg_type == FLEET_MSG_REPORT && !host->readings[i].valid) {
                return -6;
            }
        }
    }

    if (publish_to_bus) {
        telemetry_host_report_t rep;
        if (host_registry_to_telemetry(host, &rep) == 0) {
            telemetry_msg_t msg = {.type = TELEMETRY_HOST_REPORT};
            msg.host_report = rep;
            telemetry_publish(&msg);
        }
    }
    return 0;
}

static void refresh_link_state(uint64_t now_ms)
{
    uint32_t online = 0;
    for (uint8_t i = 0; i < s_registry.host_count; i++) {
        fleet_registry_host_t *h = &s_registry.hosts[i];
        if (h->last_seen_ms > 0 && now_ms > h->last_seen_ms &&
            (now_ms - h->last_seen_ms) > FLEET_HOST_STALE_MS) {
            h->link_ok = false;
        }
        if (h->link_ok) {
            online++;
        }
    }
    s_registry.joined_count = online;
}

void host_registry_refresh_links(uint64_t now_ms)
{
    refresh_link_state(now_ms);
}

bool host_registry_snapshot(fleet_registry_snapshot_t *out)
{
    if (!out) {
        return false;
    }
#ifndef HOST_REGISTRY_HOST_TEST
    refresh_link_state((uint64_t)(esp_timer_get_time() / 1000ULL));
#endif
    *out = s_registry;
    return true;
}

int host_registry_to_telemetry(const fleet_registry_host_t *host, void *telemetry_out)
{
    if (!host || !telemetry_out) {
        return -1;
    }
    telemetry_host_report_t *rep = (telemetry_host_report_t *)telemetry_out;
    memset(rep, 0, sizeof(*rep));
    strncpy(rep->device_id, host->device_id, sizeof(rep->device_id) - 1);
    strncpy(rep->node_id, host->node_id, sizeof(rep->node_id) - 1);
    strncpy(rep->schema_id, host->schema_id, sizeof(rep->schema_id) - 1);
    strncpy(rep->host_type, host->host_type, sizeof(rep->host_type) - 1);
    rep->host_type_id = host->host_type_id;
    rep->ts_ms = host->last_seen_ms;
    rep->reading_count = host->reading_count;
    if (rep->reading_count > FLEET_MAX_READINGS_PER_REPORT) {
        rep->reading_count = FLEET_MAX_READINGS_PER_REPORT;
    }
    for (uint8_t i = 0; i < rep->reading_count; i++) {
        strncpy(rep->readings[i].key, host->readings[i].key, sizeof(rep->readings[i].key) - 1);
        strncpy(rep->readings[i].unit, host->readings[i].unit, sizeof(rep->readings[i].unit) - 1);
        rep->readings[i].value = host->readings[i].value;
        rep->readings[i].valid = host->readings[i].valid;
    }
    return 0;
}
