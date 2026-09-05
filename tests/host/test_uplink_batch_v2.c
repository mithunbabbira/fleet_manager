#include <stdio.h>
#include <string.h>

#include "uplink_batch.h"

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
    uplink_batch_t b;
    uplink_batch_clear(&b);
    expect_true(b.count == 0, "clear");

    const char *e1 =
        "{\"device_id\":\"c\",\"node_id\":\"n\",\"schemaId\":\"1087\",\"ts_ms\":1,\"payload\":{}}";
    const char *e2 =
        "{\"device_id\":\"c_GPS\",\"node_id\":\"node-c_GPS\",\"schemaId\":\"1089\",\"ts_ms\":2,"
        "\"payload\":{\"gps_ok\":true,\"lat\":1.0,\"lng\":2.0}}";

    expect_true(uplink_batch_add(&b, e1) == 0, "add e1");
    expect_true(uplink_batch_add(&b, e2) == 0, "add e2");
    expect_true(b.count == 2, "count 2");

    char out[2048];
    int n = uplink_batch_build_array(&b, out, sizeof(out));
    expect_true(n > 0, "build len");
    expect_true(out[0] == '[', "starts array");
    expect_true(out[n - 1] == ']', "ends array");
    expect_true(strstr(out, "1087") != NULL, "has 1087");
    expect_true(strstr(out, "1089") != NULL, "has 1089");
    expect_true(strstr(out, "},{") != NULL, "comma between objects");

    uplink_batch_clear(&b);
    expect_true(uplink_batch_build_array(&b, out, sizeof(out)) < 0, "empty fails");

    if (fails) {
        printf("%d failures\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
