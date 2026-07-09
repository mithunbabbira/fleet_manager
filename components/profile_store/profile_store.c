#include "profile_store.h"

#include "builtin_profiles.h"
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "profile_store";

#define PS_NAMESPACE "elm"
#define PS_KEY_NAMES "names"
#define PS_KEY_ACTIVE "active"
#define PS_KEY_BOND "bond"
#define PS_KEY_UNSAFE "unsafe"
#define PS_MAX_PROFILES 16

static nvs_handle_t s_handle;
static bool s_ready = false;

/*
 * NVS key names are capped at 15 characters (NVS_KEY_NAME_MAX_SIZE - 1),
 * which is too short for "prof_<name>" once name approaches its 32-byte
 * budget (e.g. "prof_fleet_basic" is already 16 chars). Instead we keep a
 * small JSON registry of profile names under "names" and store each
 * profile's JSON blob under a short positional key ("p0", "p1", ...). The
 * `active`, `bond`, and `unsafe` keys match the design doc directly since
 * they are short enough to fit as-is.
 */
typedef struct {
    char name[32];
} ps_registry_entry_t;

static esp_err_t read_string_alloc(const char *key, char **out_str)
{
    *out_str = NULL;
    size_t len = 0;
    esp_err_t err = nvs_get_str(s_handle, key, NULL, &len);
    if (err != ESP_OK) {
        return err;
    }
    char *buf = malloc(len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(s_handle, key, buf, &len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *out_str = buf;
    return ESP_OK;
}

static esp_err_t registry_load(ps_registry_entry_t entries[PS_MAX_PROFILES], int *count)
{
    *count = 0;
    char *json_str = NULL;
    esp_err_t err = read_string_alloc(PS_KEY_NAMES, &json_str);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    if (!root || !cJSON_IsArray(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return ESP_OK;
    }

    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n && *count < PS_MAX_PROFILES; ++i) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (cJSON_IsString(item) && item->valuestring) {
            strncpy(entries[*count].name, item->valuestring, sizeof(entries[*count].name) - 1);
            entries[*count].name[sizeof(entries[*count].name) - 1] = '\0';
            (*count)++;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t registry_save(const ps_registry_entry_t entries[], int count)
{
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < count; ++i) {
        cJSON_AddItemToArray(root, cJSON_CreateString(entries[i].name));
    }
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_set_str(s_handle, PS_KEY_NAMES, json_str);
    cJSON_free(json_str);
    return err;
}

static int registry_find(const ps_registry_entry_t entries[], int count, const char *name)
{
    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void profile_key(int slot, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "p%d", slot);
}

/* ---- JSON <-> obd_profile_t --------------------------------------------- */

static char *profile_to_json(const obd_profile_t *p)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "name", p->name);

    cJSON *init_at = cJSON_CreateArray();
    for (int i = 0; i < p->init_at_count && i < 8; ++i) {
        cJSON_AddItemToArray(init_at, cJSON_CreateString(p->init_at[i]));
    }
    cJSON_AddItemToObject(root, "init_at", init_at);

    cJSON *items = cJSON_CreateArray();
    for (int i = 0; i < p->item_count && i < 32; ++i) {
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "cmd", p->items[i].cmd);
        cJSON_AddNumberToObject(it, "interval_ms", p->items[i].interval_ms);
        cJSON_AddStringToObject(it, "decode", p->items[i].decode);
        cJSON_AddItemToArray(items, it);
    }
    cJSON_AddItemToObject(root, "items", items);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static esp_err_t json_to_profile(const char *json_str, obd_profile_t *out)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name) && name->valuestring) {
        strncpy(out->name, name->valuestring, sizeof(out->name) - 1);
    }

    cJSON *init_at = cJSON_GetObjectItem(root, "init_at");
    if (cJSON_IsArray(init_at)) {
        int n = cJSON_GetArraySize(init_at);
        for (int i = 0; i < n && out->init_at_count < 8; ++i) {
            cJSON *item = cJSON_GetArrayItem(init_at, i);
            if (cJSON_IsString(item) && item->valuestring) {
                strncpy(out->init_at[out->init_at_count], item->valuestring,
                        sizeof(out->init_at[out->init_at_count]) - 1);
                out->init_at_count++;
            }
        }
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (cJSON_IsArray(items)) {
        int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n && out->item_count < 32; ++i) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(item)) {
                continue;
            }
            profile_item_t *dst = &out->items[out->item_count];
            cJSON *cmd = cJSON_GetObjectItem(item, "cmd");
            cJSON *interval = cJSON_GetObjectItem(item, "interval_ms");
            cJSON *decode = cJSON_GetObjectItem(item, "decode");
            if (cJSON_IsString(cmd) && cmd->valuestring) {
                strncpy(dst->cmd, cmd->valuestring, sizeof(dst->cmd) - 1);
            }
            if (cJSON_IsNumber(interval)) {
                dst->interval_ms = (uint32_t)interval->valuedouble;
            }
            if (cJSON_IsString(decode) && decode->valuestring) {
                strncpy(dst->decode, decode->valuestring, sizeof(dst->decode) - 1);
            }
            out->item_count++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ---- JSON <-> ble_bond_t ------------------------------------------------- */

static char *bond_to_json(const ble_bond_t *b)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *addr = cJSON_CreateArray();
    for (int i = 0; i < 6; ++i) {
        cJSON_AddItemToArray(addr, cJSON_CreateNumber(b->addr[i]));
    }
    cJSON_AddItemToObject(root, "addr", addr);
    cJSON_AddBoolToObject(root, "addr_set", b->addr_set);
    cJSON_AddStringToObject(root, "name", b->name);
    cJSON_AddStringToObject(root, "service_uuid", b->service_uuid);
    cJSON_AddStringToObject(root, "rx_uuid", b->rx_uuid);
    cJSON_AddStringToObject(root, "tx_uuid", b->tx_uuid);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

static esp_err_t json_to_bond(const char *json_str, ble_bond_t *out)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    cJSON *addr = cJSON_GetObjectItem(root, "addr");
    if (cJSON_IsArray(addr)) {
        int n = cJSON_GetArraySize(addr);
        for (int i = 0; i < n && i < 6; ++i) {
            cJSON *v = cJSON_GetArrayItem(addr, i);
            if (cJSON_IsNumber(v)) {
                out->addr[i] = (uint8_t)v->valueint;
            }
        }
    }
    cJSON *addr_set = cJSON_GetObjectItem(root, "addr_set");
    out->addr_set = cJSON_IsTrue(addr_set);

    cJSON *name = cJSON_GetObjectItem(root, "name");
    if (cJSON_IsString(name) && name->valuestring) {
        strncpy(out->name, name->valuestring, sizeof(out->name) - 1);
    }
    cJSON *service_uuid = cJSON_GetObjectItem(root, "service_uuid");
    if (cJSON_IsString(service_uuid) && service_uuid->valuestring) {
        strncpy(out->service_uuid, service_uuid->valuestring, sizeof(out->service_uuid) - 1);
    }
    cJSON *rx_uuid = cJSON_GetObjectItem(root, "rx_uuid");
    if (cJSON_IsString(rx_uuid) && rx_uuid->valuestring) {
        strncpy(out->rx_uuid, rx_uuid->valuestring, sizeof(out->rx_uuid) - 1);
    }
    cJSON *tx_uuid = cJSON_GetObjectItem(root, "tx_uuid");
    if (cJSON_IsString(tx_uuid) && tx_uuid->valuestring) {
        strncpy(out->tx_uuid, tx_uuid->valuestring, sizeof(out->tx_uuid) - 1);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ---- validation ---------------------------------------------------------- */

/* Profiles must stay read-only under the safety policy regardless of the
 * runtime `allow_unsafe` flag, so validation always uses allow_unsafe=false. */
static esp_err_t validate_profile_cmds(const obd_profile_t *p)
{
    cmd_policy_config_t cfg = {.allow_unsafe = false};
    for (int i = 0; i < p->init_at_count && i < 8; ++i) {
        if (!cmd_policy_is_allowed(p->init_at[i], &cfg)) {
            ESP_LOGW(TAG, "profile '%s' rejected: init_at '%s' not allowed", p->name, p->init_at[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (int i = 0; i < p->item_count && i < 32; ++i) {
        if (!cmd_policy_is_allowed(p->items[i].cmd, &cfg)) {
            ESP_LOGW(TAG, "profile '%s' rejected: cmd '%s' not allowed", p->name, p->items[i].cmd);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

/* Stores/updates a profile's registry slot + JSON blob without validation;
 * used by profile_store_upsert (post-validation) and builtin seeding. */
static esp_err_t store_profile_raw(const obd_profile_t *p)
{
    ps_registry_entry_t entries[PS_MAX_PROFILES];
    int count = 0;
    esp_err_t err = registry_load(entries, &count);
    if (err != ESP_OK) {
        return err;
    }

    int slot = registry_find(entries, count, p->name);
    if (slot < 0) {
        if (count >= PS_MAX_PROFILES) {
            return ESP_ERR_NO_MEM;
        }
        slot = count;
        strncpy(entries[slot].name, p->name, sizeof(entries[slot].name) - 1);
        entries[slot].name[sizeof(entries[slot].name) - 1] = '\0';
        count++;
        err = registry_save(entries, count);
        if (err != ESP_OK) {
            return err;
        }
    }

    char *json_str = profile_to_json(p);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    char key[16];
    profile_key(slot, key, sizeof(key));
    err = nvs_set_str(s_handle, key, json_str);
    cJSON_free(json_str);
    if (err != ESP_OK) {
        return err;
    }

    return nvs_commit(s_handle);
}

static esp_err_t seed_builtins(void)
{
    for (size_t i = 0; i < BUILTIN_PROFILE_COUNT; ++i) {
        ps_registry_entry_t entries[PS_MAX_PROFILES];
        int count = 0;
        esp_err_t err = registry_load(entries, &count);
        if (err != ESP_OK) {
            return err;
        }
        if (registry_find(entries, count, k_builtin_profiles[i].name) >= 0) {
            continue; /* already present; don't clobber a user edit */
        }
        err = store_profile_raw(&k_builtin_profiles[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to seed builtin '%s': %s",
                     k_builtin_profiles[i].name, esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "seeded builtin profile '%s'", k_builtin_profiles[i].name);
    }
    return ESP_OK;
}

/* ---- public API ------------------------------------------------------------ */

esp_err_t profile_store_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_open(PS_NAMESPACE, NVS_READWRITE, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;

    err = seed_builtins();
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 0;
    err = nvs_get_str(s_handle, PS_KEY_ACTIVE, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_set_str(s_handle, PS_KEY_ACTIVE, k_builtin_profiles[0].name);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_commit(s_handle);
        if (err != ESP_OK) {
            return err;
        }
    } else if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

esp_err_t profile_store_get_active(obd_profile_t *out)
{
    if (!s_ready || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    char *active_name = NULL;
    esp_err_t err = read_string_alloc(PS_KEY_ACTIVE, &active_name);
    if (err != ESP_OK) {
        return err;
    }

    ps_registry_entry_t entries[PS_MAX_PROFILES];
    int count = 0;
    err = registry_load(entries, &count);
    if (err != ESP_OK) {
        free(active_name);
        return err;
    }

    int slot = registry_find(entries, count, active_name);
    free(active_name);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    char key[16];
    profile_key(slot, key, sizeof(key));
    char *json_str = NULL;
    err = read_string_alloc(key, &json_str);
    if (err != ESP_OK) {
        return err;
    }
    err = json_to_profile(json_str, out);
    free(json_str);
    return err;
}

esp_err_t profile_store_set_active(const char *name)
{
    if (!s_ready || !name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_registry_entry_t entries[PS_MAX_PROFILES];
    int count = 0;
    esp_err_t err = registry_load(entries, &count);
    if (err != ESP_OK) {
        return err;
    }
    if (registry_find(entries, count, name) < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_set_str(s_handle, PS_KEY_ACTIVE, name);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}

esp_err_t profile_store_list(char names[][32], int max, int *count)
{
    if (!s_ready || !names || !count || max <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ps_registry_entry_t entries[PS_MAX_PROFILES];
    int total = 0;
    esp_err_t err = registry_load(entries, &total);
    if (err != ESP_OK) {
        return err;
    }

    int n = total < max ? total : max;
    for (int i = 0; i < n; ++i) {
        strncpy(names[i], entries[i].name, 32);
        names[i][31] = '\0';
    }
    *count = total;
    return ESP_OK;
}

esp_err_t profile_store_upsert(const obd_profile_t *p)
{
    if (!s_ready || !p || !p->name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = validate_profile_cmds(p);
    if (err != ESP_OK) {
        return err;
    }

    return store_profile_raw(p);
}

esp_err_t profile_store_get_bond(ble_bond_t *out)
{
    if (!s_ready || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_str = NULL;
    esp_err_t err = read_string_alloc(PS_KEY_BOND, &json_str);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = json_to_bond(json_str, out);
    free(json_str);
    return err;
}

esp_err_t profile_store_set_bond(const ble_bond_t *b)
{
    if (!s_ready || !b) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json_str = bond_to_json(b);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nvs_set_str(s_handle, PS_KEY_BOND, json_str);
    cJSON_free(json_str);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}

esp_err_t profile_store_get_safety(cmd_policy_config_t *out)
{
    if (!s_ready || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t val = 0;
    esp_err_t err = nvs_get_u8(s_handle, PS_KEY_UNSAFE, &val);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        val = 0;
    } else if (err != ESP_OK) {
        return err;
    }

    out->allow_unsafe = (val != 0);
    return ESP_OK;
}

esp_err_t profile_store_set_allow_unsafe(bool allow)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_u8(s_handle, PS_KEY_UNSAFE, allow ? 1 : 0);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(s_handle);
}
