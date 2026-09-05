#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPLINK_SCHEMA_GPS "1089"
#define UPLINK_SCHEMA_OBD "1087"
#define UPLINK_URL_DEFAULT "https://api.trafyn.info/nc-events-api/v2/messages"
#define UPLINK_GPS_ONLY_MIN_MOVE_M 50.0
#define UPLINK_GPS_ONLY_HEARTBEAT_MS 300000ULL

/** Master device_id → {master}_GPS / node-{master}_GPS (derived; not OTA-writable). */
void uplink_virtual_gps_ids(const char *carrier_device_id, char *gps_device_id, size_t gps_dev_len,
                            char *gps_node_id, size_t gps_node_len);

double uplink_gps_distance_m(double lat1, double lng1, double lat2, double lng2);

bool uplink_gps_worth_sending(bool have_last, double last_lat, double last_lng, uint64_t last_ms,
                              double lat, double lng, uint64_t now_ms);

/** Build GPS payload object into out. Returns bytes written or -1. */
int uplink_build_gps_payload(bool gps_ok, double lat, double lng, char *out, size_t out_len);

/**
 * Build one envelope JSON object.
 * payload_json must be a JSON object (no surrounding array).
 * Returns bytes written or -1.
 */
int uplink_build_envelope(const char *device_id, const char *node_id, const char *schema_id,
                          uint64_t ts_ms, const char *payload_json, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
