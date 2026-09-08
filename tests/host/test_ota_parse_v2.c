/* Version-matching logic for the Trafyn OTA check (ota_parse_check_json).
 *
 * This is a regression suite for a real bug: ota_cloud.c used to compare a
 * *stripped* running version (e.g. "1.0.33-dev.20" -> "1.0.33") against
 * Trafyn's *raw* latestVersion ("1.0.33-dev.20"), so a device already
 * running the exact published build still saw itself as "out of date" on
 * every check that reached this function. ota_strip_version() has been
 * removed; both sides must now be compared as full, unmodified strings.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ota_parse.h"

static ota_check_result_t check(const char *json, const char *current_version)
{
    ota_check_result_t out;
    memset(&out, 0, sizeof(out));
    assert(ota_parse_check_json(json, current_version, &out) == 0);
    return out;
}

int main(void)
{
    /* --- Same build, different build-number suffix: must be an update --- */
    {
        const char *json =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.33-dev.20\","
            "\"presignedUrl\":\"https://s3.example/fw.bin\",\"updateAvailable\":true,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":870960}}";
        ota_check_result_t r = check(json, "1.0.33-dev.15");
        assert(r.kind == OTA_CHECK_UPDATE);
        assert(strcmp(r.latest_version, "1.0.33-dev.20") == 0);
        assert(r.size == 870960);
        assert(r.update_available);
    }

    /* --- Device flashed with a plain, unsuffixed version: still an update
     * against a CI-suffixed latest (this is the live "1.0.33" device case) --- */
    {
        const char *json =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.33-dev.20\","
            "\"presignedUrl\":\"https://s3.example/fw.bin\",\"updateAvailable\":true,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":870960}}";
        ota_check_result_t r = check(json, "1.0.33");
        assert(r.kind == OTA_CHECK_UPDATE);
        assert(strcmp(r.latest_version, "1.0.33-dev.20") == 0);
    }

    /* --- THE REGRESSION: device already running the exact published build.
     * Both sides are now full strings, so this must be recognized as a
     * match and NOT re-flash the identical binary. --- */
    {
        const char *json =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.33-dev.20\","
            "\"presignedUrl\":\"https://s3.example/fw.bin\",\"updateAvailable\":true,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":870960}}";
        ota_check_result_t r = check(json, "1.0.33-dev.20");
        assert(r.kind == OTA_CHECK_NO_UPDATE);
        assert(strcmp(r.latest_version, "1.0.33-dev.20") == 0);
    }

    /* --- A prod build's full label must round-trip the same way --- */
    {
        const char *json =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.33-rel.42\","
            "\"presignedUrl\":\"https://s3.example/fw.bin\",\"updateAvailable\":true,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":1}}";
        assert(check(json, "1.0.33-rel.42").kind == OTA_CHECK_NO_UPDATE);
        assert(check(json, "1.0.33-rel.41").kind == OTA_CHECK_UPDATE);
    }

    /* --- updateAvailable:false must be NO_UPDATE even if the version
     * strings differ (Trafyn's flag is authoritative here) --- */
    {
        const char *json =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.34-dev.5\","
            "\"presignedUrl\":\"https://x\",\"updateAvailable\":false,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":1}}";
        ota_check_result_t r = check(json, "1.0.33-dev.20");
        assert(r.kind == OTA_CHECK_NO_UPDATE);
        assert(!r.update_available);
    }

    /* --- Missing presignedUrl: NO_UPDATE, not a crash or false UPDATE --- */
    {
        const char *json =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.34-dev.5\","
            "\"presignedUrl\":\"\",\"updateAvailable\":true,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":1}}";
        assert(check(json, "1.0.33-dev.20").kind == OTA_CHECK_NO_UPDATE);
    }

    /* --- A real mismatch but missing/invalid sha256 or size must FAIL
     * closed, never silently proceed to flash --- */
    {
        const char *nosha =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.34-dev.5\","
            "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,\"size\":1}}";
        ota_check_result_t r = check(nosha, "1.0.33-dev.20");
        assert(r.kind == OTA_CHECK_FAIL);
        assert(strcmp(r.error, "missing_sha_size") == 0);

        const char *badsize =
            "{\"success\":true,\"data\":{\"latestVersion\":\"1.0.34-dev.5\","
            "\"presignedUrl\":\"https://x\",\"updateAvailable\":true,"
            "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
            "\"size\":-1}}";
        assert(check(badsize, "1.0.33-dev.20").kind == OTA_CHECK_FAIL);
    }

    /* --- Transport/protocol failures fail closed too --- */
    {
        assert(check("{\"success\":false,\"errorMessage\":\"nope\"}", "1.0.33-dev.20").kind ==
               OTA_CHECK_FAIL);
        assert(check("not json", "1.0.33-dev.20").kind == OTA_CHECK_FAIL);
        assert(check("{\"success\":true}", "1.0.33-dev.20").kind == OTA_CHECK_FAIL);
    }

    printf("test_ota_parse_v2: ok\n");
    return 0;
}
