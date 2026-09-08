/* "Memory limits while offline" and "too many queued events" behavior.
 *
 * Two independent bounds exist in the offline path:
 *  - uplink_batch_t (RAM, one telemetry tick): a fixed UPLINK_BATCH_MAX_EVENTS
 *    array — uplink_batch_add() must refuse the 13th event, not overflow or
 *    silently corrupt the first 12.
 *  - uplink_queue_build_batch() (SD-backed NDJSON backlog -> POST body): must
 *    never write past the caller's out_len even with a much larger backlog
 *    than fits, and must also respect an independent n_events cap on how
 *    many lines it will even look at.
 *
 * Neither of these needs ESP-IDF; both are already header-documented as
 * "Pure helpers ... (host-testable)" / "Host-testable ... batch".
 */
#include "uplink_batch.h"
#include "uplink_queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_batch_cap(void)
{
    uplink_batch_t batch;
    uplink_batch_clear(&batch);
    assert(batch.count == 0);

    char env[64];
    for (int i = 0; i < UPLINK_BATCH_MAX_EVENTS; i++) {
        snprintf(env, sizeof(env), "{\"schemaId\":\"1087\",\"idx\":%d}", i);
        assert(uplink_batch_add(&batch, env) == 0);
    }
    assert(batch.count == UPLINK_BATCH_MAX_EVENTS);

    /* One too many: rejected, and the full batch must stay intact
     * (not partially overwritten or corrupted). */
    snprintf(env, sizeof(env), "{\"schemaId\":\"1087\",\"idx\":%d}", UPLINK_BATCH_MAX_EVENTS);
    assert(uplink_batch_add(&batch, env) == -1);
    assert(batch.count == UPLINK_BATCH_MAX_EVENTS);

    char out[4096];
    int n = uplink_batch_build_array(&batch, out, sizeof(out));
    assert(n > 0);
    assert(out[0] == '[' && out[n - 1] == ']');
    assert(strstr(out, "\"idx\":0") != NULL);
    assert(strstr(out, "\"idx\":11") != NULL);
    /* The rejected 13th event must never have made it in. */
    char rejected[32];
    snprintf(rejected, sizeof(rejected), "\"idx\":%d}", UPLINK_BATCH_MAX_EVENTS);
    assert(strstr(out, rejected) == NULL);

    /* An oversized single envelope is rejected outright, same guarantee. */
    static char huge[UPLINK_BATCH_ENV_MAX + 16];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[0] = '{';
    huge[sizeof(huge) - 2] = '}';
    huge[sizeof(huge) - 1] = '\0';
    uplink_batch_clear(&batch);
    assert(uplink_batch_add(&batch, huge) == -1);
    assert(batch.count == 0);

    printf("  batch cap: ok\n");
}

/** Build one valid SD-queued NDJSON line the same way production code does. */
static int make_queued_line(int idx, uint64_t queued_at_ms, char *out, size_t out_len)
{
    char env[64];
    snprintf(env, sizeof(env), "{\"schemaId\":\"1087\",\"idx\":%d}", idx);
    return uplink_queue_line_from_envelope(env, queued_at_ms, out, out_len);
}

static void test_queue_line_bounds(void)
{
    char line[128];
    /* Buffer under the 8-byte floor is rejected outright. */
    assert(uplink_queue_line_from_envelope("{\"a\":1}", 1, line, 4) < 0);
    /* A buffer too small to hold envelope + ,"queued_at_ms":N} is rejected,
     * not silently truncated into invalid JSON. */
    assert(uplink_queue_line_from_envelope("{\"schemaId\":\"1087\"}", 1, line, 20) < 0);
    /* A properly sized buffer succeeds and round-trips. */
    int n = make_queued_line(7, 99, line, sizeof(line));
    assert(n > 0);
    assert(strstr(line, "\"idx\":7") != NULL);
    assert(strstr(line, "\"queued_at_ms\":99}") != NULL);

    printf("  queue line bounds: ok\n");
}

static void test_queue_backlog_memory_limit(void)
{
    /* Simulate a large offline backlog: far more queued lines than a
     * bounded POST-body buffer can hold. */
    enum { BACKLOG = 50 };
    char blob[BACKLOG * 96];
    size_t off = 0;
    for (int i = 0; i < BACKLOG; i++) {
        char line[96];
        int n = make_queued_line(i, 1000 + (uint64_t)i, line, sizeof(line));
        assert(n > 0);
        assert(off + (size_t)n + 1 < sizeof(blob));
        memcpy(blob + off, line, (size_t)n);
        off += (size_t)n;
        blob[off++] = '\n';
    }
    blob[off] = '\0';

    /* Deliberately too small to hold all 50 events. */
    char out[512];
    int n = uplink_queue_build_batch(blob, BACKLOG, out, sizeof(out));
    assert(n > 0);
    assert((size_t)n < sizeof(out)); /* never overflowed the buffer */
    assert(out[0] == '[' && out[n - 1] == ']');
    /* Well-formed: every opened object closes, none left dangling. */
    int opens = 0, closes = 0;
    for (int i = 0; i < n; i++) {
        if (out[i] == '{') opens++;
        if (out[i] == '}') closes++;
    }
    assert(opens == closes && opens > 0);
    /* Events are drained in order, so the earliest ones are the ones kept. */
    assert(strstr(out, "\"idx\":0") != NULL);
    /* Buffer is too small for all 50 — the tail must have been left behind,
     * not truncated mid-object. */
    assert(strstr(out, "\"idx\":49") == NULL);

    printf("  queue backlog memory limit: ok (kept %d/%d bytes worth, backlog=%d)\n", n,
           (int)sizeof(out), BACKLOG);
}

static void test_queue_n_events_cap(void)
{
    /* Independent of buffer size: n_events itself limits how many lines
     * are even considered, even with plenty of room in out_len. */
    enum { AVAILABLE = 10, TAKE = 3 };
    char blob[AVAILABLE * 96];
    size_t off = 0;
    for (int i = 0; i < AVAILABLE; i++) {
        char line[96];
        int n = make_queued_line(i, 2000 + (uint64_t)i, line, sizeof(line));
        assert(n > 0);
        memcpy(blob + off, line, (size_t)n);
        off += (size_t)n;
        blob[off++] = '\n';
    }
    blob[off] = '\0';

    char out[4096]; /* deliberately generous: only n_events should bind here */
    int n = uplink_queue_build_batch(blob, TAKE, out, sizeof(out));
    assert(n > 0);
    assert(strstr(out, "\"idx\":0") != NULL);
    assert(strstr(out, "\"idx\":1") != NULL);
    assert(strstr(out, "\"idx\":2") != NULL);
    assert(strstr(out, "\"idx\":3") == NULL); /* beyond the n_events cap */

    printf("  queue n_events cap: ok\n");
}

int main(void)
{
    test_batch_cap();
    test_queue_line_bounds();
    test_queue_backlog_memory_limit();
    test_queue_n_events_cap();
    printf("test_uplink_offline_limits_v2: OK\n");
    return 0;
}
