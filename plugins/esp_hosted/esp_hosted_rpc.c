/* Wire-compatible subset of ESP-Hosted RPC for a bare-metal RP2350 host. */
#include "driver.h"

#if ESP_HOSTED_ENABLE

#include <string.h>
#include "esp_hosted_rpc.h"
#include "esp_hosted_transport.h"

#define RPC_REQ_GET_MAC       257u
#define RPC_REQ_SET_MODE      260u
#define RPC_REQ_WIFI_INIT     278u
#define RPC_REQ_WIFI_START    280u
#define RPC_REQ_WIFI_STOP     281u
#define RPC_REQ_WIFI_CONNECT  282u
#define RPC_REQ_WIFI_DISCONN  283u
#define RPC_REQ_WIFI_CONFIG   284u
#define RPC_REQ_FEATURE       387u
#define RPC_RESP_GET_MAC      513u
#define RPC_RESP_SET_MODE     516u
#define RPC_RESP_WIFI_INIT    534u
#define RPC_RESP_WIFI_START   536u
#define RPC_RESP_WIFI_STOP    537u
#define RPC_RESP_WIFI_CONNECT 538u
#define RPC_RESP_WIFI_DISCONN 539u
#define RPC_RESP_WIFI_CONFIG  540u
#define RPC_RESP_FEATURE      643u
#define RPC_TIMEOUT_MS        3000u
#define RPC_PROTO_MAX         640u

typedef struct {
    uint8_t *data;
    uint16_t length;
    uint16_t capacity;
} pb_writer_t;

typedef struct {
    const uint8_t *data;
    uint16_t length;
    uint16_t offset;
} pb_reader_t;

typedef struct {
    uint32_t field;
    uint8_t wire;
    uint64_t value;
    const uint8_t *bytes;
    uint16_t length;
} pb_field_t;

static struct {
    uint32_t uid, expected_id, deadline;
    esp_hosted_rpc_done_ptr done;
    void *done_context;
    esp_hosted_rpc_event_ptr event_handler;
    void *event_context;
} rpc;

static bool pb_raw(pb_writer_t *w, const void *data, uint16_t length)
{
    if((uint32_t)w->length + length > w->capacity)
        return false;
    if(length) {
        if(!data)
            return false;
        memcpy(w->data + w->length, data, length);
        w->length += length;
    }
    return true;
}

static bool pb_varint(pb_writer_t *w, uint64_t value)
{
    uint8_t encoded[10], length = 0;
    do {
        encoded[length] = (uint8_t)(value & 0x7Fu);
        value >>= 7;
        if(value)
            encoded[length] |= 0x80u;
        length++;
    } while(value && length < sizeof(encoded));
    return pb_raw(w, encoded, length);
}

static bool pb_key(pb_writer_t *w, uint32_t field, uint8_t wire)
{
    return pb_varint(w, ((uint64_t)field << 3) | wire);
}

static bool pb_uint(pb_writer_t *w, uint32_t field, uint64_t value)
{
    return pb_key(w, field, 0) && pb_varint(w, value);
}

static bool pb_bytes(pb_writer_t *w, uint32_t field, const void *data, uint16_t length)
{
    return pb_key(w, field, 2) && pb_varint(w, length) && pb_raw(w, data, length);
}

static bool pb_message(pb_writer_t *w, uint32_t field, const pb_writer_t *message)
{
    return pb_bytes(w, field, message->data, message->length);
}

static bool read_varint(pb_reader_t *r, uint64_t *value)
{
    uint64_t result = 0;
    uint8_t shift = 0;

    while(r->offset < r->length && shift < 64u) {
        uint8_t byte = r->data[r->offset++];
        result |= (uint64_t)(byte & 0x7Fu) << shift;
        if(!(byte & 0x80u)) {
            *value = result;
            return true;
        }
        shift += 7u;
    }
    return false;
}

static bool next_field(pb_reader_t *r, pb_field_t *field)
{
    uint64_t key, value;
    if(!read_varint(r, &key))
        return false;

    memset(field, 0, sizeof(*field));
    field->field = (uint32_t)(key >> 3);
    field->wire = (uint8_t)(key & 7u);

    if(field->wire == 0u) {
        if(!read_varint(r, &value))
            return false;
        field->value = value;
        return true;
    }
    if(field->wire == 2u) {
        if(!read_varint(r, &value) || value > UINT16_MAX ||
           (uint32_t)r->offset + value > r->length)
            return false;
        field->bytes = r->data + r->offset;
        field->length = (uint16_t)value;
        r->offset += field->length;
        return true;
    }
    if(field->wire == 1u && r->offset + 8u <= r->length) {
        r->offset += 8u;
        return true;
    }
    if(field->wire == 5u && r->offset + 4u <= r->length) {
        r->offset += 4u;
        return true;
    }
    return false;
}

static bool send_request(uint32_t request_id, uint32_t response_id,
                         const uint8_t *payload, uint16_t payload_length,
                         esp_hosted_rpc_done_ptr done, void *context)
{
    if(rpc.done)
        return false;

    uint8_t proto[RPC_PROTO_MAX], framed[ESP_HOSTED_RPC_MAX];
    pb_writer_t envelope = { proto, 0, sizeof(proto) };
    uint32_t uid = ++rpc.uid;
    if(uid == 0u)
        uid = ++rpc.uid;

    if(!pb_uint(&envelope, 1, 1) || !pb_uint(&envelope, 2, request_id) ||
       !pb_uint(&envelope, 3, uid) ||
       !pb_bytes(&envelope, request_id, payload, payload_length))
        return false;

    static const char endpoint[] = "RPCRsp";
    uint16_t pos = 0;
    framed[pos++] = 0x01;
    framed[pos++] = sizeof(endpoint) - 1u;
    framed[pos++] = 0;
    memcpy(framed + pos, endpoint, sizeof(endpoint) - 1u);
    pos += sizeof(endpoint) - 1u;
    framed[pos++] = 0x02;
    framed[pos++] = (uint8_t)envelope.length;
    framed[pos++] = (uint8_t)(envelope.length >> 8);
    memcpy(framed + pos, proto, envelope.length);
    pos += envelope.length;

    if(!esp_hosted_transport_rpc_tx(framed, pos))
        return false;

    rpc.expected_id = response_id;
    rpc.deadline = hal.get_elapsed_ticks() + RPC_TIMEOUT_MS;
    rpc.done = done;
    rpc.done_context = context;
    return true;
}

static bool empty_request(uint32_t request_id, uint32_t response_id,
                          esp_hosted_rpc_done_ptr done, void *context)
{
    return send_request(request_id, response_id, NULL, 0, done, context);
}

static bool parse_pserial(const uint8_t *data, uint16_t length,
                          const uint8_t **protobuf, uint16_t *protobuf_length)
{
    if(length < 12u || data[0] != 0x01u)
        return false;
    uint16_t endpoint_length = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    if(endpoint_length != 6u || (uint32_t)3u + endpoint_length + 3u > length)
        return false;
    if(memcmp(data + 3, "RPCRsp", 6) && memcmp(data + 3, "RPCEvt", 6))
        return false;

    uint16_t pos = (uint16_t)(3u + endpoint_length);
    if(data[pos++] != 0x02u)
        return false;
    uint16_t data_length = (uint16_t)data[pos] | ((uint16_t)data[pos + 1u] << 8);
    pos += 2u;
    if((uint32_t)pos + data_length > length)
        return false;

    *protobuf = data + pos;
    *protobuf_length = data_length;
    return true;
}

static int32_t response_status(uint32_t id, const uint8_t *data, uint16_t length,
                               const uint8_t **result, uint16_t *result_length)
{
    pb_reader_t reader = { data, length, 0 };
    pb_field_t field;
    int32_t status = -1;
    *result = NULL;
    *result_length = 0;

    while(reader.offset < reader.length && next_field(&reader, &field)) {
        if(id == RPC_RESP_GET_MAC && field.field == 1u && field.wire == 2u) {
            *result = field.bytes;
            *result_length = field.length;
        } else if(field.wire == 0u && field.field == (id == RPC_RESP_GET_MAC ? 2u : 1u))
            status = (int32_t)field.value;
    }
    return status;
}

static uint32_t event_detail(uint32_t id, const uint8_t *data, uint16_t length)
{
    if(id != ESPHostedRpcEvent_StaDisconnected)
        return 0;

    pb_reader_t outer = { data, length, 0 };
    pb_field_t field;
    while(outer.offset < outer.length && next_field(&outer, &field)) {
        if(field.field == 2u && field.wire == 2u) {
            pb_reader_t nested = { field.bytes, field.length, 0 };
            while(nested.offset < nested.length && next_field(&nested, &field)) {
                if(field.field == 4u && field.wire == 0u)
                    return (uint32_t)field.value;
            }
        }
    }
    return 0;
}

static void process_protobuf(const uint8_t *data, uint16_t length)
{
    pb_reader_t reader = { data, length, 0 };
    pb_field_t field;
    uint32_t type = 0, id = 0, uid = 0;
    const uint8_t *payload = NULL;
    uint16_t payload_length = 0;

    while(reader.offset < reader.length && next_field(&reader, &field)) {
        if(field.field == 1u && field.wire == 0u)
            type = (uint32_t)field.value;
        else if(field.field == 2u && field.wire == 0u)
            id = (uint32_t)field.value;
        else if(field.field == 3u && field.wire == 0u)
            uid = (uint32_t)field.value;
        else if(field.wire == 2u && field.field == id) {
            payload = field.bytes;
            payload_length = field.length;
        }
    }

    if(type == 2u && rpc.done && id == rpc.expected_id && uid == rpc.uid) {
        const uint8_t *result;
        uint16_t result_length;
        int32_t status = response_status(id, payload, payload_length, &result, &result_length);
        esp_hosted_rpc_done_ptr done = rpc.done;
        void *context = rpc.done_context;
        rpc.done = NULL;
        rpc.done_context = NULL;
        done(status == 0, status, result, result_length, context);
    } else if(type == 3u && rpc.event_handler &&
              (id == ESPHostedRpcEvent_Init || id == ESPHostedRpcEvent_StaConnected ||
               id == ESPHostedRpcEvent_StaDisconnected))
        rpc.event_handler((esp_hosted_rpc_event_t)id,
                          event_detail(id, payload, payload_length), rpc.event_context);
}

void esp_hosted_rpc_init(esp_hosted_rpc_event_ptr event_handler, void *context)
{
    memset(&rpc, 0, sizeof(rpc));
    rpc.event_handler = event_handler;
    rpc.event_context = context;
}

void esp_hosted_rpc_reset(void)
{
    rpc.done = NULL;
    rpc.done_context = NULL;
    rpc.expected_id = 0;
}

void esp_hosted_rpc_poll(uint32_t now_ms)
{
    esp_hosted_rpc_packet_t packet;
    while(esp_hosted_transport_rpc_rx(&packet)) {
        const uint8_t *protobuf;
        uint16_t length;
        if(parse_pserial(packet.data, packet.length, &protobuf, &length))
            process_protobuf(protobuf, length);
    }

    if(rpc.done && (int32_t)(now_ms - rpc.deadline) >= 0) {
        esp_hosted_rpc_done_ptr done = rpc.done;
        void *context = rpc.done_context;
        rpc.done = NULL;
        rpc.done_context = NULL;
        done(false, -1, NULL, 0, context);
    }
}

bool esp_hosted_rpc_busy(void) { return rpc.done != NULL; }

bool esp_hosted_rpc_get_mac(esp_hosted_rpc_done_ptr done, void *context)
{
    uint8_t payload[4];
    pb_writer_t request = { payload, 0, sizeof(payload) };
    return pb_uint(&request, 1, 0) &&
           send_request(RPC_REQ_GET_MAC, RPC_RESP_GET_MAC,
                        request.data, request.length, done, context);
}

bool esp_hosted_rpc_wifi_init(esp_hosted_rpc_done_ptr done, void *context)
{
    uint8_t config_data[96], request_data[112];
    pb_writer_t config = { config_data, 0, sizeof(config_data) };
    pb_writer_t request = { request_data, 0, sizeof(request_data) };

    bool ok = pb_uint(&config, 1, 10) && pb_uint(&config, 2, 32) &&
              pb_uint(&config, 3, 1) && pb_uint(&config, 5, 32) &&
              pb_uint(&config, 8, 1) && pb_uint(&config, 9, 1) &&
              pb_uint(&config, 11, 1) && pb_uint(&config, 13, 6) &&
              pb_uint(&config, 15, 752) && pb_uint(&config, 16, 32) &&
              pb_uint(&config, 19, 7) && pb_uint(&config, 20, 0x1F2F3F4Fu) &&
              pb_uint(&config, 22, 5) && pb_uint(&config, 23, 3) &&
              pb_message(&request, 1, &config);
    return ok && send_request(RPC_REQ_WIFI_INIT, RPC_RESP_WIFI_INIT,
                              request.data, request.length, done, context);
}

bool esp_hosted_rpc_wifi_set_mode(uint8_t mode, esp_hosted_rpc_done_ptr done, void *context)
{
    uint8_t payload[4];
    pb_writer_t request = { payload, 0, sizeof(payload) };
    return pb_uint(&request, 1, mode) &&
           send_request(RPC_REQ_SET_MODE, RPC_RESP_SET_MODE,
                        request.data, request.length, done, context);
}

bool esp_hosted_rpc_wifi_set_config(const char *ssid, const char *password,
                                    esp_hosted_rpc_done_ptr done, void *context)
{
    uint8_t sta_data[112], config_data[128], request_data[144];
    pb_writer_t sta = { sta_data, 0, sizeof(sta_data) };
    pb_writer_t config = { config_data, 0, sizeof(config_data) };
    pb_writer_t request = { request_data, 0, sizeof(request_data) };
    uint16_t ssid_length = (uint16_t)strnlen(ssid, 64);
    uint16_t password_length = (uint16_t)strnlen(password, 64);

    bool ok = pb_bytes(&sta, 1, ssid, ssid_length) &&
              pb_bytes(&sta, 2, password, password_length) &&
              pb_message(&config, 2, &sta) && pb_uint(&request, 1, 0) &&
              pb_message(&request, 2, &config);
    return ok && send_request(RPC_REQ_WIFI_CONFIG, RPC_RESP_WIFI_CONFIG,
                              request.data, request.length, done, context);
}

bool esp_hosted_rpc_wifi_start(esp_hosted_rpc_done_ptr done, void *context)
{
    return empty_request(RPC_REQ_WIFI_START, RPC_RESP_WIFI_START, done, context);
}

bool esp_hosted_rpc_wifi_stop(esp_hosted_rpc_done_ptr done, void *context)
{
    return empty_request(RPC_REQ_WIFI_STOP, RPC_RESP_WIFI_STOP, done, context);
}

bool esp_hosted_rpc_wifi_connect(esp_hosted_rpc_done_ptr done, void *context)
{
    return empty_request(RPC_REQ_WIFI_CONNECT, RPC_RESP_WIFI_CONNECT, done, context);
}

bool esp_hosted_rpc_wifi_disconnect(esp_hosted_rpc_done_ptr done, void *context)
{
    return empty_request(RPC_REQ_WIFI_DISCONN, RPC_RESP_WIFI_DISCONN, done, context);
}

bool esp_hosted_rpc_bt_control(uint8_t command, uint8_t option,
                               esp_hosted_rpc_done_ptr done, void *context)
{
    uint8_t payload[12];
    pb_writer_t request = { payload, 0, sizeof(payload) };
    return pb_uint(&request, 1, 1) && pb_uint(&request, 2, command) &&
           pb_uint(&request, 3, option) &&
           send_request(RPC_REQ_FEATURE, RPC_RESP_FEATURE,
                        request.data, request.length, done, context);
}

#endif /* ESP_HOSTED_ENABLE */
