#pragma once

#include "lte.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int cli_format_time_line(const lte_time_t *t, char *out, size_t out_len);
int cli_format_gps_line(const lte_gps_t *g, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
