#pragma once
#include "ul212_rs232_parse.h"
#include <stdint.h>

void ul212Rs232Begin(int rx_pin, int tx_pin, uint32_t baud = 9600);
Ul212Reading ul212Rs232Poll(uint32_t timeout_ms = 800);
