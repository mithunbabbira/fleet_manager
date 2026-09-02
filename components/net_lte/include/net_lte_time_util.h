#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** India IST = UTC+5:30 → +22 quarter-hours (Quectel +CCLK convention). */
#define NET_LTE_TZ_QUARTERS_IST 22

/**
 * @brief Gregorian UTC datetime → Unix epoch ms (no libc TZ).
 * @return epoch ms, or -1 if out of range.
 */
int64_t net_lte_utc_datetime_to_epoch_ms(int year, int month, int day, int hour, int minute,
                                        int second);

/**
 * @brief Parse modem `AT+CCLK?` response into UTC epoch ms.
 *
 * India networks report local IST with timezone `+22`. If the operator omits
 * the timezone field, IST is assumed (this product ships in India only).
 *
 * @param resp Full AT response containing `+CCLK: "yy/MM/dd,hh:mm:ss±zz"`
 * @param epoch_ms_utc_out Output UTC epoch milliseconds
 * @return true on successful parse
 */
bool net_lte_time_parse_cclk(const char *resp, int64_t *epoch_ms_utc_out);

/**
 * @brief Format UTC epoch ms as IST wall clock `YYYY-MM-DD HH:MM:SS` (no TZ suffix).
 * @return bytes written (excluding NUL), or -1
 */
int net_lte_format_ist(uint64_t epoch_ms_utc, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
