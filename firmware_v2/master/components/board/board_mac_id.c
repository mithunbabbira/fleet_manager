#include "board_mac_id.h"

#include "esp_mac.h"

#include <stdio.h>

esp_err_t board_get_mac_id(char *out, size_t out_len)
{
    if (out == NULL || out_len < 13) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (err != ESP_OK) {
        return err;
    }

    snprintf(out, out_len, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return ESP_OK;
}
