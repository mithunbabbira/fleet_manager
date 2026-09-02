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
 * @brief Open NVS "elm", seed builtins, ensure an active OBD poll profile.
 * @note Call once after nvs_flash_init(); not re-entrant across tasks without external sync.
 */
esp_err_t profile_store_init(void);

/** @brief Load the currently active OBD poll profile from NVS. */
esp_err_t profile_store_get_active(obd_profile_t *out);

/**
 * @brief Select an existing profile by name as active.
 * @note Commits NVS; fails with ESP_ERR_NOT_FOUND if name is unknown.
 */
esp_err_t profile_store_set_active(const char *name);

/** @brief List stored profile names (up to @p max); @p *count is total in registry. */
esp_err_t profile_store_list(char names[][32], int max, int *count);

/**
 * @brief Create or update a profile; rejects cmds that fail cmd_policy.
 * @note Always validates with allow_unsafe=false; commits NVS on success.
 */
esp_err_t profile_store_upsert(const obd_profile_t *p);

/** @brief Load BLE bond from NVS (addr_set false if none). */
esp_err_t profile_store_get_bond(ble_bond_t *out);

/**
 * @brief Persist BLE bond JSON under NVS key "bond".
 * @note Commits NVS.
 */
esp_err_t profile_store_set_bond(const ble_bond_t *b);

/** @brief Read cmd_policy allow_unsafe from NVS (default false). */
esp_err_t profile_store_get_safety(cmd_policy_config_t *out);

/**
 * @brief Persist allow_unsafe flag to NVS.
 * @note Commits NVS.
 */
esp_err_t profile_store_set_allow_unsafe(bool allow);

#ifdef __cplusplus
}
#endif
