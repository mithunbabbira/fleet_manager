/*
 * Zigbee end device on the UL212 host board.
 *
 * Flow:
 *   1. Join the coordinator's open network (channel FLEET_ZB_CHANNEL, EPAN FLEET_ZB_EPAN_ID).
 *   2. Send HELLO once connected (registers device_id with coordinator).
 *   3. Send REPORT every second with BLE sensor readings (TLV encoded).
 *   4. On disconnect (carrier reboot / RF loss), actively restart steering.
 *
 * Build with FLEET_ZIGBEE_ED_RADIO=1 and board_build.zigbee_mode=ed.
 */

#include "fleet_zigbee_ed.h"

#include <Arduino.h>
#include <string.h>

static FleetZigbeeConfig s_cfg;
static bool s_joined;
static bool s_hello_sent;
static uint8_t s_seq;
static uint32_t s_rejoin_next_ms;
static uint32_t s_rejoin_backoff_ms = 2000;

static uint64_t nowMs()
{
    return (uint64_t)millis();
}

#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO

#ifndef ZIGBEE_MODE_ED
#error "Set board_build.zigbee_mode = ed in platformio.ini"
#endif

#include "Zigbee.h"
#include "bdb/esp_zigbee_bdb_commissioning.h"
#include "esp_zigbee_core.h"
#include "fleet_zb_epan.h"
#include "fleet_zigbee_cluster.h"
#include "zcl/esp_zigbee_zcl_command.h"

/** Minimal endpoint: basic cluster + fleet TLV cluster (client). */
class FleetHostEp : public ZigbeeEP {
public:
    explicit FleetHostEp(uint8_t ep) : ZigbeeEP(ep)
    {
        _device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID;
        _cluster_list = esp_zb_zcl_cluster_list_create();

        esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(nullptr);
        esp_zb_cluster_list_add_basic_cluster(_cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        esp_zb_attribute_list_t *fleet = esp_zb_zcl_attr_list_create(FLEET_ZB_CLUSTER_ID);
        esp_zb_cluster_list_add_custom_cluster(_cluster_list, fleet, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

        _ep_config = {
            .endpoint = ep,
            .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
            .app_device_version = 0,
        };
    }
};

static FleetHostEp *s_ep;

static bool radioSendTlv(const uint8_t *frame, size_t len)
{
    if (!frame || len == 0 || len > 240 || !Zigbee.connected()) {
        return false;
    }

    static uint8_t buf[241];
    buf[0] = (uint8_t)len;
    memcpy(&buf[1], frame, len);

    esp_zb_zcl_custom_cluster_cmd_req_t cmd = {};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000; /* coordinator */
    cmd.zcl_basic_cmd.dst_endpoint = FLEET_ZB_COORD_ENDPOINT;
    cmd.zcl_basic_cmd.src_endpoint = FLEET_ZB_HOST_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    cmd.cluster_id = FLEET_ZB_CLUSTER_ID;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    cmd.dis_default_resp = 1;
    cmd.custom_cmd_id = FLEET_ZB_CMD_TLV;
    cmd.data.type = ESP_ZB_ZCL_ATTR_TYPE_OCTET_STRING;
    cmd.data.size = (uint16_t)(len + 1);
    cmd.data.value = buf;

    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        return false;
    }
    const bool ok = esp_zb_zcl_custom_cluster_cmd_req(&cmd) != 0;
    esp_zb_lock_release();
    return ok;
}

static void requestNetworkSteering(void)
{
    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        return;
    }
    esp_zb_ieee_addr_t epan = {};
    if (!fleet_zb_epan_parse(FLEET_ZB_EPAN_ID, epan)) {
        esp_zb_lock_release();
        Serial.printf("[zb] bad FLEET_ZB_EPAN_ID=%s\n", FLEET_ZB_EPAN_ID);
        delay(1000);
        ESP.restart();
    }
    esp_zb_set_extended_pan_id(epan);
    const esp_err_t err =
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    esp_zb_lock_release();
    if (err != ESP_OK) {
        Serial.printf("[zb] steering request failed (%d)\n", (int)err);
    }
}

static void radioStart()
{
    if (s_ep) {
        return;
    }
    s_ep = new FleetHostEp(FLEET_ZB_HOST_ENDPOINT);
    s_ep->setManufacturerAndModel("Fleet", "UL212Host");
    Zigbee.addEndpoint(s_ep);
    Zigbee.setPrimaryChannelMask(1UL << FLEET_ZB_CHANNEL);
    esp_zb_ieee_addr_t epan = {};
    if (!fleet_zb_epan_parse(FLEET_ZB_EPAN_ID, epan)) {
        Serial.printf("[zb] bad FLEET_ZB_EPAN_ID=%s\n", FLEET_ZB_EPAN_ID);
        delay(1000);
        ESP.restart();
    }
    esp_zb_set_extended_pan_id(epan);
    char epan_str[17];
    fleet_zb_epan_format(epan, epan_str);
    if (!Zigbee.begin()) {
        Serial.println("[zb] begin failed, rebooting");
        delay(1000);
        ESP.restart();
    }
    Serial.printf("[zb] joining coordinator (ch %d EPAN=%s)…\n", FLEET_ZB_CHANNEL, epan_str);
}

#endif /* FLEET_ZIGBEE_ED_RADIO */

static void clearJoinState(void)
{
    s_joined = false;
    s_hello_sent = false;
}

static bool sendTlv(fleet_msg_type_t type, const FleetZigbeeReading *readings, size_t count,
                    uint8_t status, uint8_t seq, bool include_metric_map)
{
    fleet_encode_input_t in{};
    in.msg_type = type;
    strncpy(in.header.device_id, s_cfg.deviceId, sizeof(in.header.device_id) - 1);
    strncpy(in.header.node_id, s_cfg.nodeId, sizeof(in.header.node_id) - 1);
    strncpy(in.header.schema_id, s_cfg.schemaId, sizeof(in.header.schema_id) - 1);
    strncpy(in.header.host_type, s_cfg.hostType, sizeof(in.header.host_type) - 1);
    in.header.host_type_id = s_cfg.hostTypeId;
    in.header.seq = seq;
    in.header.ts_ms = nowMs();
    in.header.status = status;
    in.header.manifest_version = s_cfg.manifestVersion;

    if (include_metric_map && s_cfg.metricMap[0] &&
        in.reading_count < FLEET_TLV_MAX_READINGS) {
        fleet_tlv_value_t *map = &in.readings[in.reading_count++];
        map->tlv_id = FLEET_TLV_METRIC_MAP;
        map->type = FLEET_VAL_STRING;
        map->valid = true;
        strncpy(map->value.str, s_cfg.metricMap, sizeof(map->value.str) - 1);
    }

    for (size_t i = 0; i < count && in.reading_count < FLEET_TLV_MAX_READINGS; i++) {
        fleet_tlv_value_t *out = &in.readings[in.reading_count++];
        out->tlv_id = readings[i].tlvId;
        out->type = readings[i].type;
        out->valid = true;
        switch (readings[i].type) {
        case FLEET_VAL_FLOAT: out->value.f32 = readings[i].value.f32; break;
        case FLEET_VAL_UINT8: out->value.u8 = readings[i].value.u8; break;
        case FLEET_VAL_UINT16: out->value.u16 = readings[i].value.u16; break;
        case FLEET_VAL_INT32: out->value.i32 = readings[i].value.i32; break;
        default: in.reading_count--; break;
        }
    }

    uint8_t frame[FLEET_TLV_MAX_FRAME];
    const int n = fleet_tlv_encode(&in, frame, sizeof(frame));
    if (n <= 0) {
        return false;
    }

#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
    return radioSendTlv(frame, (size_t)n);
#else
    Serial.printf("[zb] %s seq=%u (%d B)\n", type == FLEET_MSG_HELLO ? "HELLO" : "REPORT",
                  (unsigned)seq, n);
    return true;
#endif
}

extern "C" {

void fleetZigbeeEdBegin(const FleetZigbeeConfig *cfg)
{
    if (!cfg) {
        return;
    }
    s_cfg = *cfg;
    clearJoinState();
    s_seq = 0;
    s_rejoin_next_ms = 0;
    s_rejoin_backoff_ms = 2000;
    Serial.printf("[zb] device=%s node=%s schema=%s type=%u\n", s_cfg.deviceId, s_cfg.nodeId,
                  s_cfg.schemaId, (unsigned)s_cfg.hostTypeId);
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
    radioStart();
#endif
}

bool fleetZigbeeEdSendHello(void)
{
    if (!s_cfg.deviceId[0]) {
        return false;
    }
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
    if (!Zigbee.connected()) {
        return false;
    }
#endif
    const bool ok = sendTlv(FLEET_MSG_HELLO, nullptr, 0, FLEET_STATUS_SENSOR_CONNECTED, ++s_seq,
                            true);
    if (ok) {
        s_joined = true;
    } else {
        clearJoinState();
    }
    return ok;
}

bool fleetZigbeeEdSendReport(const FleetZigbeeReading *readings, size_t count, uint8_t status,
                             uint8_t seq)
{
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
    if (!Zigbee.connected() || !s_joined || !s_hello_sent) {
        return false;
    }
#else
    if (!s_joined) {
        return false;
    }
#endif
    const bool ok = sendTlv(FLEET_MSG_REPORT, readings, count, status, seq, false);
    /* Do not clearJoinState on a single TX fail — BLE coexistence often
     * drops one frame; tearing join caused report bursts then long gaps. */
    return ok;
}

bool fleetZigbeeEdIsJoined(void)
{
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
    return Zigbee.connected() && s_joined && s_hello_sent;
#else
    return s_joined && s_hello_sent;
#endif
}

void fleetZigbeeEdLoop(void)
{
#if defined(FLEET_ZIGBEE_ED_RADIO) && FLEET_ZIGBEE_ED_RADIO
    if (!Zigbee.connected()) {
        clearJoinState();
        const uint32_t now = millis();
        if (now >= s_rejoin_next_ms) {
            Serial.println("[zb] disconnected — rejoining…");
            requestNetworkSteering();
            s_rejoin_next_ms = now + s_rejoin_backoff_ms;
            if (s_rejoin_backoff_ms < 30000U) {
                s_rejoin_backoff_ms *= 2U;
            }
        }
        return;
    }

    s_rejoin_backoff_ms = 2000;
    s_rejoin_next_ms = 0;

    if (!s_hello_sent && fleetZigbeeEdSendHello()) {
        s_hello_sent = true;
        Serial.println("[zb] joined, HELLO sent");
    }
#endif
}

} /* extern "C" */
