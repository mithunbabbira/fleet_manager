#include "cmd_policy.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *k_at_allow[] = {
    "ATZ", "ATD", "ATWS", "ATE0", "ATE1", "ATL0", "ATL1",
    "ATS0", "ATS1", "ATH0", "ATH1",
    "ATSP0", "ATSP1", "ATSP2", "ATSP3", "ATSP4",
    "ATSP5", "ATSP6", "ATSP7", "ATSP8", "ATSP9",
    "ATDP", "ATDPN", "ATRV", "ATI", "AT@1",
};

void cmd_policy_normalize(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    if (!in || !out || out_len == 0) {
        if (out && out_len) {
            out[0] = '\0';
        }
        return;
    }
    for (size_t i = 0; in[i] && j + 1 < out_len; ++i) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            continue;
        }
        out[j++] = (char)toupper(c);
    }
    out[j] = '\0';
}

static bool is_hex_str(const char *s)
{
    if (!s || !*s) {
        return false;
    }
    for (; *s; ++s) {
        if (!isxdigit((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static int parse_mode(const char *norm)
{
    size_t len = strlen(norm);
    if (len < 2) {
        return -1;
    }
    if (len >= 2 && norm[0] == 'A' && norm[1] == 'T') {
        return -1;
    }
    if (!is_hex_str(norm)) {
        return -1;
    }
    unsigned mode = 0;
    if (sscanf(norm, "%2x", &mode) != 1) {
        return -1;
    }
    return (int)mode;
}

static bool at_allowed(const char *norm)
{
    for (size_t i = 0; i < sizeof(k_at_allow) / sizeof(k_at_allow[0]); ++i) {
        if (strcmp(norm, k_at_allow[i]) == 0) {
            return true;
        }
    }
    return false;
}

cmd_policy_result_t cmd_policy_check(const char *cmd, const cmd_policy_config_t *cfg)
{
    char norm[64];
    cmd_policy_normalize(cmd, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return CMD_POLICY_DENY_UNKNOWN;
    }

    if (strlen(norm) >= 2 && norm[0] == 'A' && norm[1] == 'T') {
        return at_allowed(norm) ? CMD_POLICY_ALLOW : CMD_POLICY_DENY_UNKNOWN;
    }

    int mode = parse_mode(norm);
    if (mode < 0) {
        return CMD_POLICY_DENY_UNKNOWN;
    }

    if (mode == 0x04) {
        if (cfg && cfg->allow_unsafe) {
            return CMD_POLICY_ALLOW;
        }
        return CMD_POLICY_DENY_MODE04;
    }
    if (mode == 0x08) {
        return CMD_POLICY_DENY_MODE08;
    }

    switch (mode) {
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x07:
    case 0x0A:
    case 0x09:
        return CMD_POLICY_ALLOW;
    default:
        return CMD_POLICY_DENY_UNKNOWN;
    }
}

bool cmd_policy_is_allowed(const char *cmd, const cmd_policy_config_t *cfg)
{
    return cmd_policy_check(cmd, cfg) == CMD_POLICY_ALLOW;
}

const char *cmd_policy_result_str(cmd_policy_result_t r)
{
    switch (r) {
    case CMD_POLICY_ALLOW:
        return "allow";
    case CMD_POLICY_DENY_MODE04:
        return "deny_mode04";
    case CMD_POLICY_DENY_MODE08:
        return "deny_mode08";
    case CMD_POLICY_DENY_UNKNOWN:
        return "deny_unknown";
    case CMD_POLICY_DENY_UNSAFE_LOCKED:
        return "deny_unsafe_locked";
    default:
        return "deny";
    }
}
