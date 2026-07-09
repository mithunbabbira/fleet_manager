#pragma once
#include "cmd_policy.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char cmd[16];
    uint32_t interval_ms;
    char decode[24];
} profile_item_t;

typedef struct {
    char name[32];
    char init_at[8][16];
    int init_at_count;
    profile_item_t items[32];
    int item_count;
} obd_profile_t;

typedef struct {
    uint8_t addr[6];
    bool addr_set;
    char name[32];
    char service_uuid[40];
    char rx_uuid[40];
    char tx_uuid[40];
} ble_bond_t;

/**
 * Open the "elm" NVS namespace and make sure the built-in profiles
 * (fleet_basic, diagnostics) exist and an active profile is selected.
 * Must be called once, after nvs_flash_init().
 */
esp_err_t profile_store_init(void);

/** Fetch the currently active profile. */
esp_err_t profile_store_get_active(obd_profile_t *out);

/** Select an existing profile (by name) as active. */
esp_err_t profile_store_set_active(const char *name);

/** List the names of all stored profiles (up to `max`); `*count` is the total returned. */
esp_err_t profile_store_list(char names[][32], int max, int *count);

/** Create or update a profile. Rejects any init_at/cmd that fails cmd_policy. */
esp_err_t profile_store_upsert(const obd_profile_t *p);

/** Fetch the saved BLE bond (addr_set is false if nothing is bonded yet). */
esp_err_t profile_store_get_bond(ble_bond_t *out);

/** Persist the BLE bond. */
esp_err_t profile_store_set_bond(const ble_bond_t *b);

/** Fetch the current safety configuration (allow_unsafe flag). */
esp_err_t profile_store_get_safety(cmd_policy_config_t *out);

/** Persist the allow_unsafe flag. */
esp_err_t profile_store_set_allow_unsafe(bool allow);

#ifdef __cplusplus
}
#endif
