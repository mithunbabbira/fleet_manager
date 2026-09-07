#pragma once
/* PCB pins for the printed fleet carrier — do not change (soldered). */

#define BOARD_LTE_TX_GPIO 16 /* ESP TX → modem RX */
#define BOARD_LTE_RX_GPIO 17 /* ESP RX ← modem TX */

/* Reserved for later milestones (same copper): */
#define BOARD_MCP_SCK_GPIO  21
#define BOARD_MCP_MOSI_GPIO 22
#define BOARD_MCP_MISO_GPIO 23
#define BOARD_MCP_CS_GPIO   20
#define BOARD_MCP_INT_GPIO  14
#define BOARD_SD_SCK_GPIO   4
#define BOARD_SD_MOSI_GPIO  5
#define BOARD_SD_MISO_GPIO  6
#define BOARD_SD_CS_GPIO    18
