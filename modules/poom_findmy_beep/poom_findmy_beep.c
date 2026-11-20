// SPDX-License-Identifier: MIT
// Native POOM transport; protocol provenance and limitations are in README.md.
#include "poom_findmy_beep.h"
#include <string.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CONNECT_MS 7000U
#define OPERATION_MS 5000U
#define RESPONSE_MS 3000U
#define APP_ID 0x50U

typedef struct {
    bool gap;
    int event;
    int status;
    esp_gatt_if_t interface;
    esp_ble_gattc_cb_param_t param;
    uint8_t value[32];
} radio_event_t;
typedef struct {
    esp_gatt_if_t interface;
    uint16_t connection;
    bool connected;
    bool disconnected;
    bool services_ready;
    esp_gatt_status_t services_status;
    uint16_t service_start[POOM_FINDMY_PROTOCOL_COUNT];
    uint16_t service_end[POOM_FINDMY_PROTOCOL_COUNT];
    uint16_t characteristic;
    poom_findmy_key_t target;
    poom_findmy_result_t *result;
} connection_t;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t events;
static bool stopping;
static bool overflow;
static bool accepting_reports;
static bool controller_owned;
static bool controller_enabled;
static bool host_owned;
static bool host_enabled;
static poom_findmy_status_t state;
static poom_findmy_device_t devices[POOM_FINDMY_MAX_DEVICES];
static size_t device_count;
static poom_findmy_key_t batch[POOM_FINDMY_MAX_DEVICES];
/* A service/name may appear only in the scan response. Retain recent ADV event
 * metadata even for advertisers not yet classified as a tracker candidate. */
static struct { poom_findmy_key_t key; uint32_t seen; bool valid; bool connectable; } reports[64];
static size_t report_next;

static uint32_t now_(void) { return (uint32_t)(esp_timer_get_time() / 1000LL); }
static bool cancelled_(void)
{
    portENTER_CRITICAL(&lock);
    const bool value = stopping;
    portEXIT_CRITICAL(&lock);
    return value;
}
static void phase_(poom_findmy_phase_t phase)
{
    portENTER_CRITICAL(&lock);
    state.phase = phase;
    portEXIT_CRITICAL(&lock);
}
static void enqueue_(const radio_event_t *event)
{
    portENTER_CRITICAL(&lock);
    if(events && xQueueSend(events, event, 0) != pdTRUE) overflow = true;
    portEXIT_CRITICAL(&lock);
}
static esp_bt_uuid_t uuid_(const uint8_t value[16])
{
    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_128};
    memcpy(uuid.uuid.uuid128, value, 16);
    return uuid;
}
static bool service_matches_(const esp_bt_uuid_t *uuid, unsigned index)
{
    const poom_findmy_profile_t *p = poom_findmy_profile((poom_findmy_protocol_t)index);
    return p->service16 ? uuid->len == ESP_UUID_LEN_16 && uuid->uuid.uuid16 == p->service16 :
                         uuid->len == ESP_UUID_LEN_128 && !memcmp(uuid->uuid.uuid128, p->service128, 16);
}
static void advertisement_(const esp_ble_gap_cb_param_t *p)
{
    if(p->scan_rst.adv_data_len > 31 || p->scan_rst.scan_rsp_len > 31) return;
    poom_findmy_device_t found = {0};
    if(!poom_findmy_parse_ad(p->scan_rst.ble_adv, p->scan_rst.adv_data_len, &found.candidates, found.name) ||
       !poom_findmy_parse_ad(p->scan_rst.ble_adv + p->scan_rst.adv_data_len, p->scan_rst.scan_rsp_len,
                            &found.candidates, found.name)) return;
    memcpy(found.key.mac, p->scan_rst.bda, 6);
    found.key.address_type = (uint8_t)p->scan_rst.ble_addr_type;
    found.last_seen_ms = now_();
    found.rssi = (int8_t)p->scan_rst.rssi;
    found.connectable = p->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_ADV ||
                        p->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_DIR_ADV;
    portENTER_CRITICAL(&lock);
    if(!accepting_reports) { portEXIT_CRITICAL(&lock); return; }
    size_t report = 0;
    while(report < 64 && (!reports[report].valid || !poom_findmy_key_equal(&reports[report].key, &found.key))) ++report;
    if(p->scan_rst.ble_evt_type != ESP_BLE_EVT_SCAN_RSP) {
        if(report == 64) { report = report_next; report_next = (report_next + 1) % 64; }
        reports[report].key = found.key;
        reports[report].seen = found.last_seen_ms;
        reports[report].valid = true;
        reports[report].connectable = found.connectable;
    } else if(report < 64 && (uint32_t)(found.last_seen_ms - reports[report].seen) < 5000U) {
        found.connectable = reports[report].connectable;
    }
    size_t i = 0;
    while(i < device_count && !poom_findmy_key_equal(&devices[i].key, &found.key)) ++i;
    if(i < device_count) {
        found.candidates |= devices[i].candidates;
        if(!found.name[0]) memcpy(found.name, devices[i].name, sizeof(found.name));
    } else if(!found.candidates || device_count == POOM_FINDMY_MAX_DEVICES) {
        portEXIT_CRITICAL(&lock);
        return;
    } else ++device_count;
    devices[i] = found;
    portEXIT_CRITICAL(&lock);
}
static void gap_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    if(event == ESP_GAP_BLE_SCAN_RESULT_EVT && p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
        advertisement_(p);
        return;
    }
    if(event == ESP_GAP_BLE_SEC_REQ_EVT) {
        /* No implicit pairing, stored keys, or automatic security escalation. */
        (void)esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, false);
        return;
    }
    if(event == ESP_GAP_BLE_NC_REQ_EVT) {
        (void)esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, false);
        return;
    }
    radio_event_t e = {.gap = true, .event = event};
    switch(event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: e.status = p->scan_param_cmpl.status; break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT: e.status = p->scan_start_cmpl.status; break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: e.status = p->scan_stop_cmpl.status; break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if(p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_CMPL_EVT) return;
        break;
    default: return;
    }
    enqueue_(&e);
}
static void gatt_(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t *p)
{
    radio_event_t e = {.event = event, .interface = interface};
    switch(event) {
    case ESP_GATTC_REG_EVT: case ESP_GATTC_OPEN_EVT: case ESP_GATTC_CONNECT_EVT:
    case ESP_GATTC_DISCONNECT_EVT: case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        /* Unrelated services cannot saturate the worker's bounded event queue. */
        if(!service_matches_(&p->search_res.srvc_id.uuid, POOM_FINDMY_AIRTAG) &&
           !service_matches_(&p->search_res.srvc_id.uuid, POOM_FINDMY_ACCESSORY) &&
           !service_matches_(&p->search_res.srvc_id.uuid, POOM_FINDMY_DULT_SOUND)) return;
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: case ESP_GATTC_WRITE_DESCR_EVT:
    case ESP_GATTC_WRITE_CHAR_EVT: case ESP_GATTC_NOTIFY_EVT: break;
    default: return;
    }
    e.param = *p;
    if(event == ESP_GATTC_NOTIFY_EVT) {
        /* Preserve exact length: an oversized response is not a valid short DULT reply. */
        const size_t n = p->notify.value_len < sizeof(e.value) ? p->notify.value_len : sizeof(e.value);
        if(n && p->notify.value) memcpy(e.value, p->notify.value, n);
    }
    enqueue_(&e);
}

static void consume_(connection_t *c, const radio_event_t *e)
{
    if(!c || e->gap) return;
    const esp_ble_gattc_cb_param_t *p = &e->param;
    switch(e->event) {
    case ESP_GATTC_CONNECT_EVT:
        if(!memcmp(p->connect.remote_bda, c->target.mac, 6) &&
           (uint8_t)p->connect.ble_addr_type == c->target.address_type) {
            c->connection = p->connect.conn_id;
            c->connected = true;
        }
        break;
    case ESP_GATTC_OPEN_EVT:
        if(p->open.status == ESP_GATT_OK && !memcmp(p->open.remote_bda, c->target.mac, 6)) {
            c->connection = p->open.conn_id;
            c->connected = true;
        }
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        if(!memcmp(p->disconnect.remote_bda, c->target.mac, 6)) c->disconnected = true;
        break;
    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if(c->connected && p->dis_srvc_cmpl.conn_id == c->connection) {
            c->services_ready = true;
            c->services_status = p->dis_srvc_cmpl.status;
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if(c->connected && p->search_res.conn_id == c->connection) {
            for(unsigned i = 0; i < POOM_FINDMY_PROTOCOL_COUNT; ++i) {
                if(service_matches_(&p->search_res.srvc_id.uuid, i) && !c->service_start[i]) {
                    c->service_start[i] = p->search_res.start_handle;
                    c->service_end[i] = p->search_res.end_handle;
                }
            }
        }
        break;
    case ESP_GATTC_NOTIFY_EVT:
        if(c->result && c->characteristic && p->notify.conn_id == c->connection &&
           p->notify.handle == c->characteristic && p->notify.value_len <= sizeof(e->value) &&
           !memcmp(p->notify.remote_bda, c->target.mac, 6)) {
            const poom_findmy_reply_t reply = poom_findmy_response(c->result->protocol, e->value,
                    p->notify.value_len, &c->result->response_status);
            /* A rejection must never be replaced by an unrelated later notification. */
            if(reply != POOM_FINDMY_REPLY_NONE && c->result->reply != POOM_FINDMY_REPLY_REJECTED)
                c->result->reply = reply;
        }
        break;
    default: break;
    }
}
static esp_err_t receive_(connection_t *c, radio_event_t *e, uint32_t deadline)
{
    while((int32_t)(deadline - now_()) > 0) {
        if(cancelled_()) return ESP_ERR_INVALID_STATE;
        portENTER_CRITICAL(&lock);
        const bool full = overflow;
        portEXIT_CRITICAL(&lock);
        if(full) return ESP_ERR_NO_MEM;
        if(xQueueReceive(events, e, pdMS_TO_TICKS(25)) == pdTRUE) {
            consume_(c, e);
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}
static esp_err_t wait_(connection_t *c, bool gap, int event, uint16_t handle,
                       uint32_t timeout_ms, radio_event_t *out)
{
    const uint32_t deadline = now_() + timeout_ms;
    radio_event_t e;
    for(;;) {
        const esp_err_t error = receive_(c, &e, deadline);
        if(error != ESP_OK) return error;
        if(c && c->disconnected) return ESP_FAIL;
        if(e.gap != gap || e.event != event) continue;
        if(!gap && c) {
            if(event == ESP_GATTC_OPEN_EVT && memcmp(e.param.open.remote_bda, c->target.mac, 6)) continue;
            if(event == ESP_GATTC_SEARCH_CMPL_EVT && e.param.search_cmpl.conn_id != c->connection) continue;
            if((event == ESP_GATTC_WRITE_CHAR_EVT || event == ESP_GATTC_WRITE_DESCR_EVT) &&
               (e.param.write.conn_id != c->connection || e.param.write.handle != handle)) continue;
            if(event == ESP_GATTC_REG_FOR_NOTIFY_EVT && e.param.reg_for_notify.handle != handle) continue;
        }
        if(out) *out = e;
        return ESP_OK;
    }
}

static esp_err_t cleanup_(void)
{
    esp_err_t first = ESP_OK, error;
    portENTER_CRITICAL(&lock);
    accepting_reports = false;
    portEXIT_CRITICAL(&lock);
    /* Disabling the owned host/controller also cancels pending opens. Do not begin
     * another target until all four synchronous lifecycle operations succeed. */
    if(host_enabled) {
        error = esp_bluedroid_disable();
        if(error == ESP_OK) host_enabled = false; else first = error;
    }
    if(host_owned && !host_enabled) {
        error = esp_bluedroid_deinit();
        if(error == ESP_OK) host_owned = false; else if(first == ESP_OK) first = error;
    }
    if(controller_enabled && !host_enabled) {
        error = esp_bt_controller_disable();
        if(error == ESP_OK) controller_enabled = false; else if(first == ESP_OK) first = error;
    }
    if(controller_owned && !controller_enabled && !host_owned) {
        error = esp_bt_controller_deinit();
        if(error == ESP_OK) controller_owned = false; else if(first == ESP_OK) first = error;
    }
    return first;
}
static esp_err_t initialize_(esp_gatt_if_t *interface)
{
    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE ||
       esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) return ESP_ERR_INVALID_STATE;
    xQueueReset(events);
    portENTER_CRITICAL(&lock);
    overflow = false;
    portEXIT_CRITICAL(&lock);
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t error = esp_bt_controller_init(&config);
    if(error != ESP_OK) return error;
    controller_owned = true;
    error = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(error != ESP_OK) return error;
    controller_enabled = true;
    error = esp_bluedroid_init();
    if(error != ESP_OK) return error;
    host_owned = true;
    error = esp_bluedroid_enable();
    if(error != ESP_OK) return error;
    host_enabled = true;
    if((error = esp_ble_gap_register_callback(gap_)) != ESP_OK ||
       (error = esp_ble_gattc_register_callback(gatt_)) != ESP_OK ||
       (error = esp_ble_gattc_app_register(APP_ID)) != ESP_OK) return error;
    radio_event_t e;
    error = wait_(NULL, false, ESP_GATTC_REG_EVT, 0, OPERATION_MS, &e);
    if(error != ESP_OK) return error;
    if(e.param.reg.status != ESP_GATT_OK) return ESP_FAIL;
    *interface = e.interface;
    return ESP_OK;
}
static esp_err_t scan_(void)
{
    esp_ble_scan_params_t params = {.scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x80, .scan_window = 0x60, .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};
    esp_err_t error = esp_ble_gap_set_scan_params(&params);
    radio_event_t e;
    if(error == ESP_OK) error = wait_(NULL, true, ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, 0, OPERATION_MS, &e);
    if(error != ESP_OK) return error;
    if(e.status != ESP_BT_STATUS_SUCCESS) return ESP_FAIL;
    portENTER_CRITICAL(&lock);
    accepting_reports = true;
    portEXIT_CRITICAL(&lock);
    error = esp_ble_gap_start_scanning(0); /* User-controlled scan; B always cancels. */
    if(error == ESP_OK) error = wait_(NULL, true, ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, 0, OPERATION_MS, &e);
    if(error != ESP_OK) return error;
    return e.status == ESP_BT_STATUS_SUCCESS ? ESP_OK : ESP_FAIL;
}
static esp_err_t att_(connection_t *c, int event, uint16_t handle)
{
    radio_event_t e;
    const esp_err_t error = wait_(c, false, event, handle, OPERATION_MS, &e);
    if(error != ESP_OK) return error;
    c->result->att_status = event == ESP_GATTC_REG_FOR_NOTIFY_EVT ? e.param.reg_for_notify.status : e.param.write.status;
    if(c->result->att_status != ESP_GATT_OK) {
        c->result->code = POOM_FINDMY_REJECTED;
        return ESP_FAIL;
    }
    return ESP_OK;
}
static esp_err_t sound_(esp_gatt_if_t interface, poom_findmy_result_t *result)
{
    connection_t c = {.interface = interface, .target = result->key, .result = result};
    phase_(POOM_FINDMY_CONNECTING);
    esp_err_t error = esp_ble_gattc_open(interface, c.target.mac,
                                        (esp_ble_addr_type_t)c.target.address_type, true);
    radio_event_t e;
    if(error == ESP_OK) error = wait_(&c, false, ESP_GATTC_OPEN_EVT, 0, CONNECT_MS, &e);
    if(error != ESP_OK) return error;
    result->att_status = e.param.open.status;
    if(e.param.open.status != ESP_GATT_OK) return ESP_FAIL;
    phase_(POOM_FINDMY_DISCOVERING);
    const uint32_t discover_deadline = now_() + OPERATION_MS;
    while(!c.services_ready) {
        error = receive_(&c, &e, discover_deadline);
        if(error != ESP_OK) return error;
        if(c.disconnected) return ESP_FAIL;
    }
    if(c.services_status != ESP_GATT_OK) { result->att_status = c.services_status; return ESP_FAIL; }
    error = esp_ble_gattc_search_service(interface, c.connection, NULL);
    if(error == ESP_OK) error = wait_(&c, false, ESP_GATTC_SEARCH_CMPL_EVT, 0, OPERATION_MS, &e);
    if(error != ESP_OK) return error;
    if(e.param.search_cmpl.status != ESP_GATT_OK) { result->att_status = e.param.search_cmpl.status; return ESP_FAIL; }
    const poom_findmy_profile_t *profile = NULL;
    esp_gattc_char_elem_t characteristic = {0};
    for(unsigned i = 0; i < POOM_FINDMY_PROTOCOL_COUNT; ++i) {
        if(!c.service_start[i]) continue;
        const poom_findmy_profile_t *candidate = poom_findmy_profile((poom_findmy_protocol_t)i);
        uint16_t count = 1;
        const esp_gatt_status_t status = esp_ble_gattc_get_char_by_uuid(interface, c.connection,
                c.service_start[i], c.service_end[i], uuid_(candidate->characteristic128), &characteristic, &count);
        if(status == ESP_GATT_NOT_FOUND || (status == ESP_GATT_OK && !count)) continue;
        if(status != ESP_GATT_OK) { result->att_status = status; return ESP_FAIL; }
        if(!(characteristic.properties & ESP_GATT_CHAR_PROP_BIT_WRITE)) continue;
        if(candidate->responses && !(characteristic.properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE))) continue;
        profile = candidate;
        result->protocol = (poom_findmy_protocol_t)i;
        c.characteristic = characteristic.char_handle;
        break;
    }
    if(!profile) { result->code = POOM_FINDMY_UNSUPPORTED; return ESP_ERR_NOT_SUPPORTED; }
    if(profile->responses) {
        phase_(POOM_FINDMY_SUBSCRIBING);
        error = esp_ble_gattc_register_for_notify(interface, c.target.mac, c.characteristic);
        if(error == ESP_OK) error = att_(&c, ESP_GATTC_REG_FOR_NOTIFY_EVT, c.characteristic);
        if(error != ESP_OK) return error;
        esp_gattc_descr_elem_t descriptor;
        uint16_t count = 1;
        const esp_bt_uuid_t cccd = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG};
        const esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(interface, c.connection,
                c.characteristic, cccd, &descriptor, &count);
        if(status != ESP_GATT_OK || !count) {
            result->att_status = status;
            result->code = POOM_FINDMY_UNSUPPORTED;
            return ESP_ERR_NOT_SUPPORTED;
        }
        uint8_t subscription[2] = {(characteristic.properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) ? 2 : 1, 0};
        error = esp_ble_gattc_write_char_descr(interface, c.connection, descriptor.handle, 2,
                    subscription, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        if(error == ESP_OK) error = att_(&c, ESP_GATTC_WRITE_DESCR_EVT, descriptor.handle);
        if(error != ESP_OK) return error;
    }
    if(cancelled_()) return ESP_ERR_INVALID_STATE;
    phase_(POOM_FINDMY_WRITING);
    uint8_t command[3];
    memcpy(command, profile->command, sizeof(command));
    error = esp_ble_gattc_write_char(interface, c.connection, c.characteristic, profile->command_length,
                                  command, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    result->command_submitted = error == ESP_OK;
    if(error == ESP_OK) error = att_(&c, ESP_GATTC_WRITE_CHAR_EVT, c.characteristic);
    if(error != ESP_OK) return error;
    result->write_acknowledged = true;
    result->code = POOM_FINDMY_ACKNOWLEDGED;
    if(profile->responses) {
        phase_(POOM_FINDMY_WAITING);
        const uint32_t deadline = now_() + RESPONSE_MS;
        while(result->reply != POOM_FINDMY_REPLY_REJECTED && result->reply != POOM_FINDMY_REPLY_COMPLETED) {
            error = receive_(&c, &e, deadline);
            if(error == ESP_ERR_TIMEOUT) break; /* ATT acknowledgement is retained, even with no protocol reply. */
            if(error != ESP_OK) return error;
            if(c.disconnected) break;
        }
        if(result->reply == POOM_FINDMY_REPLY_REJECTED) result->code = POOM_FINDMY_REJECTED;
    }
    return ESP_OK;
}

static bool fresh_target_(const poom_findmy_key_t *key)
{
    bool fresh = false;
    const uint32_t now = now_();
    portENTER_CRITICAL(&lock);
    for(size_t i = 0; i < device_count; ++i)
        if(poom_findmy_key_equal(key, &devices[i].key)) fresh = poom_findmy_eligible(&devices[i], now);
    portEXIT_CRITICAL(&lock);
    return fresh;
}
static void worker_(void *arg)
{
    (void)arg;
    esp_gatt_if_t interface = ESP_GATT_IF_NONE;
    esp_err_t error = initialize_(&interface);
    if(error == ESP_OK) error = scan_();
    if(error == ESP_OK) {
        phase_(POOM_FINDMY_SCANNING);
        while(!cancelled_()) {
            portENTER_CRITICAL(&lock);
            const bool begun = state.batch_started;
            portEXIT_CRITICAL(&lock);
            if(begun) break;
            radio_event_t e;
            error = receive_(NULL, &e, now_() + 100);
            if(error == ESP_ERR_TIMEOUT) error = ESP_OK;
            if(error != ESP_OK) break;
        }
    }
    /* Stop discovery and tear it down before any connection. Report failures and
     * never assume that a failed stop operation released the shared controller. */
    const esp_err_t scan_cleanup = cleanup_();
    if(error == ESP_OK && scan_cleanup != ESP_OK) error = scan_cleanup;
    if(error == ESP_OK && !cancelled_()) {
        for(size_t i = 0; i < state.count && !cancelled_(); ++i) {
            poom_findmy_result_t result = {.key = batch[i], .protocol = POOM_FINDMY_PROTOCOL_COUNT};
            portENTER_CRITICAL(&lock);
            state.current = i;
            portEXIT_CRITICAL(&lock);
            if(!fresh_target_(&batch[i])) {
                result.code = POOM_FINDMY_STALE;
            } else {
                error = initialize_(&interface);
                if(error == ESP_OK) {
                    if(fresh_target_(&batch[i])) error = sound_(interface, &result);
                    else result.code = POOM_FINDMY_STALE;
                }
                result.error = error;
                if(cancelled_()) result.code = POOM_FINDMY_CANCELLED;
                else if(result.code == POOM_FINDMY_PENDING) result.code = error == ESP_ERR_TIMEOUT ? POOM_FINDMY_TIMEOUT : POOM_FINDMY_FAILED;
                const esp_err_t release_error = cleanup_();
                if(release_error != ESP_OK) {
                    error = release_error;
                    result.error = release_error;
                    if(!result.write_acknowledged) result.code = POOM_FINDMY_FAILED;
                }
                portENTER_CRITICAL(&lock);
                state.results[i] = result;
                portEXIT_CRITICAL(&lock);
                if(release_error != ESP_OK) break;
                error = ESP_OK; /* One target's timeout/rejection does not retry it or skip the next. */
                continue;
            }
            portENTER_CRITICAL(&lock);
            state.results[i] = result;
            portEXIT_CRITICAL(&lock);
        }
    }
    const esp_err_t final_cleanup = cleanup_();
    if(final_cleanup != ESP_OK) error = final_cleanup;
    portENTER_CRITICAL(&lock);
    for(size_t i = 0; i < state.count; ++i) {
        if(state.results[i].code == POOM_FINDMY_PENDING) {
            state.results[i].code = stopping ? POOM_FINDMY_CANCELLED : POOM_FINDMY_FAILED;
            state.results[i].error = error;
        }
    }
    state.error = stopping && error == ESP_ERR_INVALID_STATE ? ESP_OK : error;
    state.radio_released = !controller_owned && !host_owned;
    state.phase = POOM_FINDMY_FINISHED;
    QueueHandle_t old_events = events;
    events = NULL;
    portEXIT_CRITICAL(&lock);
    vQueueDelete(old_events);
    portENTER_CRITICAL(&lock);
    state.active = false;
    portEXIT_CRITICAL(&lock);
    vTaskDelete(NULL);
}

esp_err_t poom_findmy_beep_scan(void)
{
    portENTER_CRITICAL(&lock);
    if(state.active || controller_owned || host_owned) { portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_STATE; }
    memset(&state, 0, sizeof(state));
    memset(devices, 0, sizeof(devices));
    memset(reports, 0, sizeof(reports));
    report_next = 0;
    device_count = 0;
    stopping = overflow = accepting_reports = false;
    state.active = true;
    state.phase = POOM_FINDMY_STARTING;
    portEXIT_CRITICAL(&lock);
    QueueHandle_t queue = xQueueCreate(24, sizeof(radio_event_t));
    portENTER_CRITICAL(&lock);
    events = queue;
    portEXIT_CRITICAL(&lock);
    if(!queue || xTaskCreate(worker_, "findmy_beep", 5120, NULL, 4, NULL) != pdPASS) {
        if(queue) vQueueDelete(queue);
        portENTER_CRITICAL(&lock);
        events = NULL;
        state.active = false;
        state.radio_released = true;
        state.error = ESP_ERR_NO_MEM;
        state.phase = POOM_FINDMY_FINISHED;
        portEXIT_CRITICAL(&lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
size_t poom_findmy_beep_devices(poom_findmy_device_t *out, size_t capacity)
{
    if(!out) return 0;
    portENTER_CRITICAL(&lock);
    const size_t count = device_count < capacity ? device_count : capacity;
    memcpy(out, devices, count * sizeof(*out));
    portEXIT_CRITICAL(&lock);
    return count;
}
esp_err_t poom_findmy_beep_begin(const poom_findmy_key_t *keys, size_t count)
{
    if(!keys || !count || count > POOM_FINDMY_MAX_DEVICES) return ESP_ERR_INVALID_ARG;
    const uint32_t now = now_();
    portENTER_CRITICAL(&lock);
    if(!state.active || state.phase != POOM_FINDMY_SCANNING || state.batch_started || stopping) {
        portEXIT_CRITICAL(&lock);
        return ESP_ERR_INVALID_STATE;
    }
    for(size_t i = 0; i < count; ++i) {
        bool eligible = false;
        for(size_t j = 0; j < i; ++j) if(poom_findmy_key_equal(&keys[i], &keys[j])) {
            portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_ARG;
        }
        for(size_t j = 0; j < device_count; ++j)
            if(poom_findmy_key_equal(&keys[i], &devices[j].key)) eligible = poom_findmy_eligible(&devices[j], now);
        if(!eligible) { portEXIT_CRITICAL(&lock); return ESP_ERR_NOT_FOUND; }
    }
    memcpy(batch, keys, count * sizeof(*keys));
    state.count = count;
    for(size_t i = 0; i < count; ++i) {
        state.results[i].key = keys[i];
        state.results[i].protocol = POOM_FINDMY_PROTOCOL_COUNT;
    }
    accepting_reports = false;
    state.batch_started = true;
    portEXIT_CRITICAL(&lock);
    return ESP_OK;
}
void poom_findmy_beep_status(poom_findmy_status_t *out)
{
    if(!out) return;
    portENTER_CRITICAL(&lock);
    *out = state;
    portEXIT_CRITICAL(&lock);
}
esp_err_t poom_findmy_beep_stop(void)
{
    portENTER_CRITICAL(&lock);
    stopping = true;
    portEXIT_CRITICAL(&lock);
    for(;;) {
        portENTER_CRITICAL(&lock);
        const bool active = state.active;
        portEXIT_CRITICAL(&lock);
        if(!active) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    portENTER_CRITICAL(&lock);
    const esp_err_t error = controller_owned || host_owned ?
        (state.error != ESP_OK ? state.error : ESP_ERR_INVALID_STATE) : ESP_OK;
    portEXIT_CRITICAL(&lock);
    return error;
}
const char *poom_findmy_result_name(poom_findmy_result_code_t code)
{
    static const char *const names[] = {"Pending", "Write ACK", "Rejected", "Unsupported", "Timeout", "Stale", "Cancelled", "Failed"};
    return (unsigned)code < sizeof(names) / sizeof(names[0]) ? names[code] : "Unknown";
}
