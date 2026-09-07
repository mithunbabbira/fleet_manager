#include "uplink_batch.h"

#include <stdio.h>
#include <string.h>

void uplink_batch_clear(uplink_batch_t *batch)
{
    if (batch == NULL) {
        return;
    }
    batch->count = 0;
}

int uplink_batch_add(uplink_batch_t *batch, const char *envelope_json)
{
    if (batch == NULL || envelope_json == NULL || envelope_json[0] == '\0') {
        return -1;
    }
    if (batch->count >= UPLINK_BATCH_MAX_EVENTS) {
        return -1;
    }
    size_t n = strlen(envelope_json);
    if (n == 0 || n >= UPLINK_BATCH_ENV_MAX) {
        return -1;
    }
    memcpy(batch->events[batch->count], envelope_json, n + 1);
    batch->count++;
    return 0;
}

int uplink_batch_build_array(const uplink_batch_t *batch, char *out, size_t out_len)
{
    if (batch == NULL || out == NULL || out_len < 3) {
        return -1;
    }
    if (batch->count == 0) {
        return -1;
    }

    size_t used = 0;
    out[used++] = '[';
    for (size_t i = 0; i < batch->count; i++) {
        if (i > 0) {
            if (used + 1 >= out_len) {
                return -1;
            }
            out[used++] = ',';
        }
        size_t elen = strlen(batch->events[i]);
        if (used + elen >= out_len) {
            return -1;
        }
        memcpy(out + used, batch->events[i], elen);
        used += elen;
    }
    if (used + 2 > out_len) {
        return -1;
    }
    out[used++] = ']';
    out[used] = '\0';
    return (int)used;
}
