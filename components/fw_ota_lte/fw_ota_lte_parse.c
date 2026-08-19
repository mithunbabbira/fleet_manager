#include "fw_ota_lte_parse.h"

#include "cJSON.h"

#include <ctype.h>
#include <string.h>

void fw_ota_strip_version(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) {
        return;
    }

    const char *dash = strchr(in, '-');
    if (dash && dash > in) {
        size_t prefix_len = (size_t)(dash - in);
        if (prefix_len >= out_len) {
            prefix_len = out_len - 1;
        }
        memcpy(out, in, prefix_len);
        out[prefix_len] = '\0';
    } else {
        strncpy(out, in, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static bool sha256_valid(const char *s)
{
    if (!s) {
        return false;
    }
    if (strlen(s) != 64) {
        return false;
    }
    for (int i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

static void set_fail(fw_ota_check_result_t *out, const char *msg)
{
    out->kind = FW_OTA_CHECK_FAIL;
    strncpy(out->error, msg, sizeof(out->error) - 1);
    out->error[sizeof(out->error) - 1] = '\0';
}

int fw_ota_parse_check_json(const char *json, const char *stripped_current,
                            fw_ota_check_result_t *out)
{
    if (!json || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        const char *start = strchr(json, '{');
        if (start) {
            root = cJSON_Parse(start);
        }
    }
    if (!root) {
        set_fail(out, "parse_error");
        return 0;
    }

    cJSON *success = cJSON_GetObjectItem(root, "success");
    if (!cJSON_IsTrue(success)) {
        cJSON *err = cJSON_GetObjectItem(root, "errorMessage");
        if (cJSON_IsString(err) && err->valuestring) {
            set_fail(out, err->valuestring);
        } else {
            set_fail(out, "success_false");
        }
        cJSON_Delete(root);
        return 0;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        set_fail(out, "missing_data");
        cJSON_Delete(root);
        return 0;
    }

    cJSON *update_avail = cJSON_GetObjectItem(data, "updateAvailable");
    cJSON *latest = cJSON_GetObjectItem(data, "latestVersion");
    cJSON *url = cJSON_GetObjectItem(data, "presignedUrl");
    cJSON *sha = cJSON_GetObjectItem(data, "sha256");
    cJSON *size_item = cJSON_GetObjectItem(data, "size");

    bool update_available = cJSON_IsTrue(update_avail);
    const char *latest_version = cJSON_IsString(latest) ? latest->valuestring : "";
    const char *presigned_url = cJSON_IsString(url) ? url->valuestring : "";
    const char *sha256 = cJSON_IsString(sha) ? sha->valuestring : NULL;
    size_t size = 0;
    if (cJSON_IsNumber(size_item) && size_item->valuedouble > 0) {
        size = (size_t)size_item->valuedouble;
    }

    out->update_available = update_available;

    if (!update_available || strcmp(latest_version, stripped_current) == 0 ||
        presigned_url[0] == '\0') {
        out->kind = FW_OTA_CHECK_NO_UPDATE;
        strncpy(out->latest_version, latest_version, sizeof(out->latest_version) - 1);
        out->latest_version[sizeof(out->latest_version) - 1] = '\0';
        cJSON_Delete(root);
        return 0;
    }

    if (!sha256_valid(sha256) || size <= 0) {
        set_fail(out, "missing_sha_size");
        cJSON_Delete(root);
        return 0;
    }

    out->kind = FW_OTA_CHECK_UPDATE;
    strncpy(out->latest_version, latest_version, sizeof(out->latest_version) - 1);
    out->latest_version[sizeof(out->latest_version) - 1] = '\0';
    strncpy(out->presigned_url, presigned_url, sizeof(out->presigned_url) - 1);
    out->presigned_url[sizeof(out->presigned_url) - 1] = '\0';
    strncpy(out->sha256, sha256, sizeof(out->sha256) - 1);
    out->sha256[sizeof(out->sha256) - 1] = '\0';
    out->size = size;

    cJSON_Delete(root);
    return 0;
}
