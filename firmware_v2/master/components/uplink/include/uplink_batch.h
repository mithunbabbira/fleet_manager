#pragma once
/*
 * Host-testable multi-envelope batch for one telemetry tick.
 * Producers (OBD / GPS / Zigbee hosts) add bare envelopes; core builds one JSON array.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UPLINK_BATCH_MAX_EVENTS
#define UPLINK_BATCH_MAX_EVENTS 12
#endif

#ifndef UPLINK_BATCH_ENV_MAX
#define UPLINK_BATCH_ENV_MAX 768
#endif

typedef struct {
    char events[UPLINK_BATCH_MAX_EVENTS][UPLINK_BATCH_ENV_MAX];
    size_t count;
} uplink_batch_t;

void uplink_batch_clear(uplink_batch_t *batch);

/** Copy one bare envelope object into the batch. Returns 0 or -1. */
int uplink_batch_add(uplink_batch_t *batch, const char *envelope_json);

/**
 * Build POST body: JSON array of bare envelopes (even count==1 → "[…]").
 * Returns bytes written or -1.
 */
int uplink_batch_build_array(const uplink_batch_t *batch, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
