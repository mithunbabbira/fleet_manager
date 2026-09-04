#pragma once
/* Seeed XIAO ESP32-C6 — UART1 to MAX232 TTL side.
 * Official UART: D6 = GPIO16 (TX), D7 = GPIO17 (RX).
 * Override via -DUL212_UART_RX_PIN=… / -DUL212_UART_TX_PIN=… if needed.
 *
 *   D6 (GPIO16) TX → MAX232 T1IN
 *   D7 (GPIO17) RX ← MAX232 R1OUT
 */
#ifndef UL212_UART_RX_PIN
#define UL212_UART_RX_PIN 17
#endif
#ifndef UL212_UART_TX_PIN
#define UL212_UART_TX_PIN 16
#endif
