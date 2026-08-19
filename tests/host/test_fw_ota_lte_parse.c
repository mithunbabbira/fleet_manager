#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fw_ota_lte_parse.h"

int main(void)
{
    char v[32];
    fw_ota_strip_version("1.0.4-lab", v, sizeof(v));
    assert(strcmp(v, "1.0.4") == 0);
    fw_ota_strip_version("1.0.4", v, sizeof(v));
    assert(strcmp(v, "1.0.4") == 0);
    fw_ota_strip_version("-lab", v, sizeof(v));
    assert(strcmp(v, "-lab") == 0); /* empty prefix → full string */

    const char *ok =
        "{\"success\":true,\"status\":200,\"errorMessage\":null,"
        "\"data\":{\"latestVersion\":\"1.0.7\","
        "\"presignedUrl\":\"https://s3.example/fw.bin\","
        "\"updateAvailable\":true,\"deviceId\":\"fleet-demo-001\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1109248}}";
    fw_ota_check_result_t r;
    assert(fw_ota_parse_check_json(ok, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_UPDATE);
    assert(strcmp(r.latest_version, "1.0.7") == 0);
    assert(r.size == 1109248);
    assert(r.update_available);

    const char *none =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.4\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":false,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1}}";
    assert(fw_ota_parse_check_json(none, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_NO_UPDATE);
    assert(strcmp(r.latest_version, "1.0.4") == 0);

    const char *same =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.4\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":1}}";
    assert(fw_ota_parse_check_json(same, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_NO_UPDATE);
    assert(strcmp(r.latest_version, "1.0.4") == 0);

    const char *fail = "{\"success\":false,\"errorMessage\":\"nope\"}";
    assert(fw_ota_parse_check_json(fail, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_FAIL);

    const char *nosha =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.9\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,\"size\":1}}";
    assert(fw_ota_parse_check_json(nosha, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_FAIL);

    const char *neg_size =
        "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.9\","
        "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,"
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
        "\"size\":-1}}";
    assert(fw_ota_parse_check_json(neg_size, "1.0.4", &r) == 0);
    assert(r.kind == FW_OTA_CHECK_FAIL);
    assert(strcmp(r.error, "missing_sha_size") == 0);

    printf("test_fw_ota_lte_parse: ok\n");
    return 0;
}
