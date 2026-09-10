#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_parse.h"

int main(void)
{
    char v[32];
    ota_strip_version("1.0.34-dev.23", v, sizeof(v));
    assert(strcmp(v, "1.0.34") == 0);

    ota_check_result_t r;

    const char *from_older =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.34-dev.23\","
        "\"presignedUrl\":\"https://s3.example/fw.bin\",\"updateAvailable\":true,"
        "\"sha256\":\"0036d69f1fb6060c9a495454c05f3485ee7047dedbd145521d296e20e0456a3a\","
        "\"size\":870960}}";
    assert(ota_parse_check_json(from_older, "1.0.33", &r) == 0);
    assert(r.kind == OTA_CHECK_UPDATE);
    assert(strcmp(r.latest_version, "1.0.34-dev.23") == 0);
    assert(r.size == 870960);

    const char *same_base =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.34-dev.23\","
        "\"presignedUrl\":\"https://s3.example/fw.bin\",\"updateAvailable\":true,"
        "\"sha256\":\"0036d69f1fb6060c9a495454c05f3485ee7047dedbd145521d296e20e0456a3a\","
        "\"size\":870960}}";
    assert(ota_parse_check_json(same_base, "1.0.34", &r) == 0);
    assert(r.kind == OTA_CHECK_NO_UPDATE);
    assert(strcmp(r.latest_version, "1.0.34-dev.23") == 0);

    printf("test_ota_parse_v2: ok\n");
    return 0;
}
