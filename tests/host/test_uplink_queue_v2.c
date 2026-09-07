#include <stdio.h>
#include <string.h>

#include "uplink_queue.h"

static int fails;

static void expect_true(int cond, const char *msg)
{
    if (!cond) {
        printf("FAIL: %s\n", msg);
        fails++;
    }
}

int main(void)
{
    char line[256];
    const char *env =
        "{\"device_id\":\"d\",\"node_id\":\"n\",\"schemaId\":\"1089\",\"ts_ms\":1,\"payload\":{}}";
    int n = uplink_queue_line_from_envelope(env, 99, line, sizeof(line));
    expect_true(n > 0, "queue line len");
    expect_true(strstr(line, "\"queued_at_ms\":99") != NULL, "queued_at_ms present");
    expect_true(strstr(line, "schemaId") != NULL, "schema kept");

    char batch[512];
    char blob[512];
    snprintf(blob, sizeof(blob), "%s\n", line);
    int bn = uplink_queue_build_batch(blob, 1, batch, sizeof(batch));
    expect_true(bn > 0, "batch len");
    expect_true(batch[0] == '[', "batch array");
    expect_true(strstr(batch, "queued_at_ms") == NULL, "queued_at stripped");
    expect_true(strstr(batch, "\"schemaId\":\"1089\"") != NULL, "schema in batch");

    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
