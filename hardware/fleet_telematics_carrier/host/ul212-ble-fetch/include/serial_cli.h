#pragma once

/* USB serial provisioning — no Wi-Fi SoftAP or on-device web UI. */

void serialCliBegin();
void serialCliTask(void *param);
