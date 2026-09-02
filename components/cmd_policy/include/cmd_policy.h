#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CMD_POLICY_ALLOW = 0,
    CMD_POLICY_DENY_MODE04 = 1,
    CMD_POLICY_DENY_MODE08 = 2,
    CMD_POLICY_DENY_UNKNOWN = 3,
    CMD_POLICY_DENY_UNSAFE_LOCKED = 4,
} cmd_policy_result_t;

typedef struct {
    bool allow_unsafe; /* NVS flag; still never allows mode 08 in v1 */
} cmd_policy_config_t;

/** @brief Uppercase and strip whitespace from an OBD/AT command string. */
void cmd_policy_normalize(const char *in, char *out, size_t out_len);
/** @brief Read-only allowlist check (Mode 08 never; Mode 04 gated by allow_unsafe). */
cmd_policy_result_t cmd_policy_check(const char *cmd, const cmd_policy_config_t *cfg);
/** @brief True if cmd_policy_check returns CMD_POLICY_ALLOW. */
bool cmd_policy_is_allowed(const char *cmd, const cmd_policy_config_t *cfg);
/** @brief Short string for a policy result (telemetry / logs). */
const char *cmd_policy_result_str(cmd_policy_result_t r);

#ifdef __cplusplus
}
#endif
