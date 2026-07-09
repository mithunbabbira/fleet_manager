#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cmd_policy.h"

static void expect_allow(const char *cmd)
{
    cmd_policy_config_t cfg = { .allow_unsafe = false };
    assert(cmd_policy_is_allowed(cmd, &cfg));
}

static void expect_deny(const char *cmd, cmd_policy_result_t why)
{
    cmd_policy_config_t cfg = { .allow_unsafe = false };
    assert(cmd_policy_check(cmd, &cfg) == why);
}

int main(void)
{
    char norm[32];
    cmd_policy_normalize(" 01 0c\r", norm, sizeof(norm));
    assert(strcmp(norm, "010C") == 0);

    expect_allow("ATZ");
    expect_allow("ATE0");
    expect_allow("010C");
    expect_allow("03");
    expect_allow("0902");
    expect_allow("ATRV");

    expect_deny("04", CMD_POLICY_DENY_MODE04);
    expect_deny("08", CMD_POLICY_DENY_MODE08);
    expect_deny("081f", CMD_POLICY_DENY_MODE08);
    expect_deny("ZZZZ", CMD_POLICY_DENY_UNKNOWN);
    expect_deny("22F190", CMD_POLICY_DENY_UNKNOWN);

    /* unsafe flag still blocks mode 08 */
    cmd_policy_config_t unsafe = { .allow_unsafe = true };
    assert(cmd_policy_check("04", &unsafe) == CMD_POLICY_ALLOW);
    assert(cmd_policy_check("08", &unsafe) == CMD_POLICY_DENY_MODE08);

    printf("test_cmd_policy: PASS\n");
    return 0;
}
