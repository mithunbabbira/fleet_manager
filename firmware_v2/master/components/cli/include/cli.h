#pragma once

#include "esp_err.h"

/** Start USB console task (stdin/stdout over USB Serial/JTAG). */
esp_err_t cli_start(void);
