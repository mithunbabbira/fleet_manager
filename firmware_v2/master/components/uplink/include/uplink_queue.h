#pragma once
/* Pure helpers for SD NDJSON queue / batch POST (host-testable). */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Append ,"queued_at_ms":N before the closing brace of an envelope JSON object.
 * out must be large enough; returns bytes written or -1.
 */
int uplink_queue_line_from_envelope(const char *envelope_json, uint64_t queued_at_ms, char *out,
                                    size_t out_len);

/**
 * Build a JSON array body from NDJSON peek buffer (strips queued_at_ms).
 * Returns bytes written or -1.
 */
int uplink_queue_build_batch(const char *events_blob, size_t n_events, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
