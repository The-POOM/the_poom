// SPDX-License-Identifier: GPL-3.0-or-later
/* Executes the production state machine with a deterministic task/queue model.
 * It validates navigation and ownership, not FreeRTOS scheduling or radio IO. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../applications/poom_app_pack/src/menu_wifi_action.c"

struct test_queue { unsigned size, head, count; action_event_t items[32]; };
static TaskFunction_t pending_ui, pending_scan;
static void *pending_ui_arg, *pending_scan_arg;
static poom_wifi_scanner_ap_records_t scan_fixture;
static poom_wifi_detector_record_t client_fixture[ACTION_CLIENTS];
static size_t fixture_clients;
static poom_marauder_status_t backend_status;
static poom_marauder_config_t last_started;
static esp_err_t scan_error, start_error, monitor_error, snapshot_error, channel_error;
static unsigned scan_calls, scan_stop_calls, start_calls, monitor_starts, monitor_stops;
static unsigned resumes, receives, phase, test_case, fail_task;
static uint64_t clock_ms;
static bool script_before_scan, request_stop_in_scan;
static test_sbus_callback_t input_callback;

static int scripted_input(action_event_t *event);

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "TEST_ERROR"; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }
void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void)caps; return calloc(n, size); }
void poom_arduboy_clear(void) {}
void poom_arduboy_set_text_size(unsigned n) { (void)n; }
void poom_arduboy_set_cursor(int x, int y) { (void)x; (void)y; }
void poom_arduboy_print(const char *s) { assert(strlen(s) <= 21U); }
void poom_arduboy_fill_rect(int x, int y, int w, int h, int c)
{ (void)x; (void)y; (void)w; (void)h; (void)c; }
void poom_arduboy_display(void) {}

QueueHandle_t xQueueCreate(unsigned n, unsigned size)
{
    assert(n <= 32U && size == sizeof(action_event_t));
    QueueHandle_t q = calloc(1, sizeof(*q)); assert(q); q->size = n; return q;
}
int xQueueSend(QueueHandle_t q, const void *event, unsigned wait)
{
    (void)wait; assert(q && q->count < q->size);
    q->items[(q->head + q->count++) % q->size] = *(const action_event_t *)event;
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *event, unsigned wait)
{
    (void)wait; assert(++receives < 100U); clock_ms += 100U;
    if(!q->count && pending_scan && !script_before_scan) {
        TaskFunction_t task = pending_scan; void *arg = pending_scan_arg;
        pending_scan = NULL; task(arg);
    }
    if(q->count) { *(action_event_t *)event = q->items[q->head++ % q->size]; --q->count; return pdTRUE; }
    return scripted_input(event);
}
void vQueueDelete(QueueHandle_t q) { free(q); }
int xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle)
{
    (void)stack; (void)priority; (void)handle;
    if(strcmp(name, "wifi_action_ui") == 0) {
        if(fail_task == 1U) return pdFALSE;
        assert(!pending_ui); pending_ui = task; pending_ui_arg = arg;
    } else {
        assert(strcmp(name, "action_scan") == 0);
        if(fail_task == 2U) return pdFALSE;
        assert(!pending_scan); pending_scan = task; pending_scan_arg = arg;
    }
    return pdPASS;
}
void vTaskDelete(TaskHandle_t t) { (void)t; }
static void run_ui(void)
{
    assert(pending_ui); TaskFunction_t task = pending_ui; void *arg = pending_ui_arg;
    pending_ui = NULL; task(arg);
    assert(!s_action && !s_input_queue && !pending_scan && resumes == 1U);
}
void vTaskDelay(unsigned ticks)
{
    clock_ms += ticks;
    if(pending_ui) run_ui();
}
bool poom_sbus_subscribe_cb(const char *topic, test_sbus_callback_t cb, void *user)
{ (void)user; assert(strcmp(topic, "input/button") == 0); input_callback = cb; return true; }
bool poom_sbus_unsubscribe_cb(const char *topic, test_sbus_callback_t cb, void *user)
{ (void)topic; (void)user; assert(input_callback == cb); input_callback = NULL; return true; }
bool poom_sbus_publish(const char *topic, const void *data, size_t len, uint32_t wait)
{ (void)wait; assert(strcmp(topic, "poom/menu/resume") == 0 && len == 1U && *(const uint8_t *)data == 1U); ++resumes; return true; }
void poom_wifi_attacks_stop_all(void) {}
esp_err_t poom_wifi_ctrl_deinit(void) { return ESP_OK; }
esp_err_t poom_wifi_ctrl_set_channel(uint8_t channel)
{ assert(channel == s_action->config.ap.primary); return channel_error; }
bool poom_wifi_ctrl_channel_is_valid(uint8_t ch) { return ch >= 1U && ch <= 165U; }
esp_err_t esp_wifi_scan_stop(void) { ++scan_stop_calls; return ESP_OK; }
esp_err_t poom_wifi_scanner_scan_configured(const wifi_scan_config_t *cfg)
{
    assert(cfg && cfg->show_hidden && cfg->scan_time.active.min <= cfg->scan_time.active.max);
    ++scan_calls;
    if(request_stop_in_scan) s_stop_action = true;
    return scan_error;
}
poom_wifi_scanner_ap_records_t *poom_wifi_scanner_get_ap_records(void) { return &scan_fixture; }
esp_err_t poom_wifi_detector_monitor_start(poom_wifi_detector_filter_t filter,
    poom_wifi_detector_record_t *const *seed, size_t count, size_t capacity)
{
    assert(filter == POOM_WIFI_DETECTOR_FILTER_AP_CLIENTS && !seed && count == 0 && capacity == ACTION_CLIENTS);
    ++monitor_starts; return monitor_error;
}
esp_err_t poom_wifi_detector_monitor_select_ap(uint8_t channel, const uint8_t bssid[6])
{ assert(channel == s_action->config.ap.primary && memcmp(bssid, s_action->config.ap.bssid, 6) == 0); return ESP_OK; }
esp_err_t poom_wifi_detector_monitor_stop(void) { ++monitor_stops; return ESP_OK; }
esp_err_t poom_wifi_detector_monitor_snapshot(poom_wifi_detector_record_t *const *records,
    size_t capacity, size_t *count)
{
    if(snapshot_error) return snapshot_error;
    assert(capacity >= fixture_clients);
    for(size_t i = 0; i < fixture_clients; ++i) { assert(records[i]); *records[i] = client_fixture[i]; }
    *count = fixture_clients; return ESP_OK;
}
void poom_wifi_marauder_stop(void) { backend_status.running = false; }
void poom_wifi_marauder_get_status(poom_marauder_status_t *status) { *status = backend_status; }
esp_err_t poom_wifi_marauder_start(const poom_marauder_config_t *config)
{
    ++start_calls; last_started = *config;
    backend_status.running = start_error == ESP_OK; backend_status.last_error = start_error;
    return start_error;
}

static void reset(unsigned which)
{
    assert(!s_action && !pending_ui && !pending_scan);
    test_case = which; phase = receives = 0; clock_ms = 1000;
    scan_calls = scan_stop_calls = start_calls = monitor_starts = monitor_stops = resumes = 0;
    fail_task = 0; scan_error = start_error = monitor_error = snapshot_error = channel_error = ESP_OK;
    script_before_scan = request_stop_in_scan = false;
    memset(&backend_status, 0, sizeof(backend_status));
    memset(&last_started, 0, sizeof(last_started));
    memset(&scan_fixture, 0, sizeof(scan_fixture));
    scan_fixture.count = 2;
    for(unsigned i = 0; i < 2; ++i) {
        memcpy(scan_fixture.records[i].ssid, "same-name", 10);
        scan_fixture.records[i].bssid[0] = 2;
        scan_fixture.records[i].bssid[5] = (uint8_t)(10 + i);
        scan_fixture.records[i].primary = i ? 36 : 6;
        scan_fixture.records[i].rssi = -40;
        scan_fixture.records[i].authmode = WIFI_AUTH_WPA2_PSK;
    }
    fixture_clients = 2;
    memset(client_fixture, 0, sizeof(client_fixture));
    for(unsigned i = 0; i < fixture_clients; ++i) {
        client_fixture[i].bssid[0] = 2; client_fixture[i].bssid[5] = (uint8_t)(30 + i);
    }
}

static int scripted_input(action_event_t *event)
{
    action_session_t *s = s_action;
    assert(s); memset(event, 0, sizeof(*event));
    unsigned step = phase++;
    if(test_case == 1U) { /* AP copies, same-SSID choices, start/stop/repeat/back. */
        switch(step) {
        case 0: assert(s->view == VIEW_APS && s->ap_count == 2); event->button = ACTION_DOWN; break;
        case 1: assert(s->selected_ap == 1); event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_READY && s->config.ap.bssid[5] == 11); scan_fixture.records[1].bssid[5] = 99; event->button = ACTION_DOWN; break;
        case 3: assert(s->config.duration_ms == 60000 && s->config.ap.bssid[5] == 11); event->button = ACTION_A; break;
        case 4: assert(s->view == VIEW_RUNNING && last_started.ap.bssid[5] == 11); event->button = ACTION_B; break;
        case 5: assert(s->view == VIEW_RESULT && !backend_status.running); event->button = ACTION_A; break;
        case 6: assert(s->view == VIEW_READY); event->button = ACTION_B; break;
        case 7: assert(s->view == VIEW_APS); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 2U) { /* SAE auth filter, driver error, empty scan/retry. */
        switch(step) {
        case 0: assert(s->view == VIEW_APS && s->ap_count == 2 && s->aps[0].authmode == WIFI_AUTH_WPA3_PSK && s->aps[1].authmode == WIFI_AUTH_WPA2_WPA3_PSK); event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_READY); start_error = ESP_ERR_NOT_SUPPORTED; event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_ERROR && s->error == ESP_ERR_NOT_SUPPORTED); scan_fixture.count = 0; event->button = ACTION_A; break;
        case 3: assert(s->view == VIEW_APS && !s->ap_count); scan_fixture.count = 1; scan_fixture.records[0].authmode = WIFI_AUTH_WPA3_PSK; event->button = ACTION_A; break;
        case 4: assert(s->view == VIEW_APS && s->ap_count == 1); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 3U) { /* Multi-selection is a MAC set, not snapshot indexes. */
        if(step <= 12U) assert(!start_calls);
        switch(step) {
        case 0: assert(s->view == VIEW_APS); event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_SCOPE && s->client_scope == CLIENT_SCOPE_ALL && !monitor_starts); event->button = ACTION_DOWN; break;
        case 2: assert(s->view == VIEW_SCOPE && s->client_scope == CLIENT_SCOPE_CHOOSE); event->button = ACTION_A; break;
        case 3: assert(s->view == VIEW_CLIENTS && s->client_count == 2 && !s->config.client_count); event->button = ACTION_RIGHT; break;
        case 4: assert(s->view == VIEW_CLIENTS && !s->config.client_count); event->button = ACTION_A; break;
        case 5: assert(s->config.client_count == 1 && s->config.clients[0][5] == 30); event->button = ACTION_DOWN; break;
        case 6: assert(s->selected_client == 1); event->button = ACTION_A; break;
        case 7: {
            assert(s->view == VIEW_CLIENTS && s->config.client_count == 2);
            poom_wifi_detector_record_t temp = client_fixture[0];
            client_fixture[0] = client_fixture[1]; client_fixture[1] = temp;
            clock_ms += 4000; return pdFALSE;
        }
        case 8: assert(s->view == VIEW_CLIENTS && s->selected_client == 0 && s->clients[0].bssid[5] == 31 && s->config.client_count == 2); event->button = ACTION_A; break;
        case 9: assert(s->config.client_count == 1 && s->config.clients[0][5] == 30); event->button = ACTION_A; break;
        case 10:
            assert(s->config.client_count == 2 && s->config.clients[1][5] == 31);
            client_fixture[1].bssid[5] = 32; clock_ms += 600; return pdFALSE;
        case 11:
            assert(s->view == VIEW_CLIENTS && s->client_count == 3 && s->selected_client == 0);
            assert(s->clients[2].bssid[5] == 30 && !s->client_observed[2]);
            assert(s->config.client_count == 2 && s->config.clients[0][5] == 30 && s->config.clients[1][5] == 31);
            event->button = ACTION_RIGHT; break;
        case 12: assert(s->view == VIEW_READY && s->config.client_count == 2 && !s->monitoring); event->button = ACTION_A; break;
        case 13: assert(s->view == VIEW_RUNNING && last_started.client_count == 2 && last_started.clients[0][5] == 30 && last_started.clients[1][5] == 31); event->button = ACTION_B; break;
        case 14: assert(s->view == VIEW_RESULT); event->button = ACTION_B; break;
        case 15: assert(s->view == VIEW_APS); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 4U) { /* Scope/back flow and fresh-discovery selection reset. */
        assert(!start_calls);
        switch(step) {
        case 0: event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_SCOPE && !s->monitoring); event->button = ACTION_B; break;
        case 2: assert(s->view == VIEW_APS); event->button = ACTION_A; break;
        case 3: assert(s->view == VIEW_SCOPE); event->button = ACTION_DOWN; break;
        case 4: assert(s->client_scope == CLIENT_SCOPE_CHOOSE); event->button = ACTION_A; break;
        case 5: assert(s->view == VIEW_CLIENTS && !s->config.client_count); event->button = ACTION_A; break;
        case 6: assert(s->config.client_count == 1); event->button = ACTION_B; break;
        case 7: assert(s->view == VIEW_SCOPE && !s->monitoring); event->button = ACTION_A; break;
        case 8: assert(s->view == VIEW_CLIENTS && !s->config.client_count); event->button = ACTION_A; break;
        case 9: assert(s->config.client_count == 1); event->button = ACTION_RIGHT; break;
        case 10: assert(s->view == VIEW_READY && !s->monitoring); event->button = ACTION_B; break;
        case 11: assert(s->view == VIEW_SCOPE && s->client_scope == CLIENT_SCOPE_CHOOSE); event->button = ACTION_UP; break;
        case 12: assert(s->client_scope == CLIENT_SCOPE_ALL); event->button = ACTION_A; break;
        case 13: assert(s->view == VIEW_CLIENTS && !s->config.client_count); event->button = ACTION_A; break;
        case 14: assert(s->view == VIEW_READY && s->config.client_count == 2); event->button = ACTION_B; break;
        case 15: assert(s->view == VIEW_SCOPE); event->button = ACTION_B; break;
        case 16: assert(s->view == VIEW_APS); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 5U) { /* B before scan worker starts. */
        assert(step == 0 && s->view == VIEW_SCAN); script_before_scan = false; event->button = ACTION_B;
    } else if(test_case == 6U) { /* Scan failure permits retry and exit. */
        if(step == 0) { assert(s->view == VIEW_ERROR && s->error == ESP_FAIL); scan_error = ESP_OK; event->button = ACTION_A; }
        else { assert(step == 1 && s->view == VIEW_APS); event->button = ACTION_B; }
    } else if(test_case == 7U) { /* Scan task allocation failure. */
        assert(step == 0 && s->view == VIEW_ERROR && s->error == ESP_ERR_NO_MEM); event->button = ACTION_B;
    } else if(test_case == 8U) { /* Client snapshot failure stops monitoring. */
        if(step <= 1) { event->button = ACTION_A; }
        else { assert(step == 2 && s->view == VIEW_ERROR && !s->monitoring); event->button = ACTION_B; }
    } else if(test_case == 12U) { /* Automatic all-client selection after 3s. */
        switch(step) {
        case 0: event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_SCOPE); event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_CLIENTS && !s->client_count); clock_ms += 3500; return pdFALSE;
        case 3: assert(s->view == VIEW_CLIENTS && !s->config.client_count); fixture_clients = 2; clock_ms += 600; return pdFALSE;
        case 4: assert(s->view == VIEW_READY && s->config.client_count == 2 && !s->monitoring && !start_calls); event->button = ACTION_B; break;
        case 5: assert(s->view == VIEW_SCOPE); event->button = ACTION_B; break;
        case 6: assert(s->view == VIEW_APS); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 13U) { /* CSA remains in the selected AP's band. */
        switch(step) {
        case 0: event->button = ACTION_DOWN; break;
        case 1: event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_READY && s->config.ap.primary == 36 && s->config.csa_channel != 36 && s->config.csa_channel > 14); event->button = ACTION_LEFT; break;
        case 3: assert(s->config.csa_channel != 36 && s->config.csa_channel > 14); event->button = ACTION_A; break;
        case 4: assert(s->view == VIEW_RUNNING); backend_status.running = false; backend_status.elapsed_ms = last_started.duration_ms; return pdFALSE;
        case 5: assert(s->view == VIEW_RESULT && s->status.elapsed_ms == last_started.duration_ms); event->button = ACTION_B; break;
        case 6: event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 14U) {
        switch(step) {
        case 0: event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_SCOPE); event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_ERROR && s->error == ESP_FAIL && !s->monitoring && !start_calls); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else if(test_case == 15U) { /* CLI cancellation also releases discovery. */
        switch(step) {
        case 0: event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_SCOPE); event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_CLIENTS && s->monitoring && !start_calls); s_stop_action = true; return pdFALSE;
        default: assert(false);
        }
    } else if(test_case == 17U) { /* Named-SSID actions skip hidden APs and need no client discovery. */
        switch(step) {
        case 0: assert(s->view == VIEW_APS && s->ap_count == 1 && s->aps[0].bssid[5] == 11 && !start_calls); event->button = ACTION_A; break;
        case 1: assert(s->view == VIEW_READY && !monitor_starts && !start_calls); event->button = ACTION_A; break;
        case 2: assert(s->view == VIEW_RUNNING && last_started.ap.bssid[5] == 11 && !last_started.client_count); event->button = ACTION_B; break;
        case 3: assert(s->view == VIEW_RESULT); event->button = ACTION_B; break;
        case 4: assert(s->view == VIEW_APS); event->button = ACTION_B; break;
        default: assert(false);
        }
    } else assert(false);
    return pdTRUE;
}

static void test_selection_capacity(void)
{
    /* Detector churn at its capacity must preserve every chosen MAC. A
     * selected station absent from the new snapshot remains deselectable. */
    action_session_t s = {0};
    s.client_scope = CLIENT_SCOPE_CHOOSE;
    s.config.ap.primary = 6;
    s.config.client_count = ACTION_CLIENTS;
    fixture_clients = ACTION_CLIENTS;
    for(size_t i = 0U; i < ACTION_CLIENTS; ++i) {
        s.client_ptrs[i] = &s.clients[i];
        s.config.clients[i][0] = client_fixture[i].bssid[0] = 2;
        s.config.clients[i][5] = (uint8_t)(40U + i);
        client_fixture[i].bssid[5] = (uint8_t)(80U + i);
    }
    clock_ms += 600;
    refresh_clients_(&s);
    assert(s.client_count == ACTION_CLIENTS && s.config.client_count == ACTION_CLIENTS);
    for(size_t i = 0U; i < ACTION_CLIENTS; ++i) {
        assert(s.clients[i].bssid[5] == 40U + i && !s.client_observed[i]);
    }
    s.selected_client = ACTION_CLIENTS - 1U;
    toggle_client_(&s);
    assert(s.config.client_count == ACTION_CLIENTS - 1U);
    assert(selected_client_index_(&s, s.clients[s.selected_client].bssid) == -1);
    toggle_client_(&s);
    assert(s.config.client_count == ACTION_CLIENTS);
    s.selected_client = 3U;
    toggle_client_(&s);
    assert(s.config.client_count == ACTION_CLIENTS - 1U && s.config.clients[3][5] == 44U);
    clock_ms += 600;
    refresh_clients_(&s);
    assert(s.client_count == ACTION_CLIENTS && s.config.client_count == ACTION_CLIENTS - 1U);
    for(size_t i = 0U; i < s.config.client_count; ++i) {
        bool found = false;
        for(size_t j = 0U; j < s.client_count; ++j) {
            if(memcmp(s.config.clients[i], s.clients[j].bssid, 6U) == 0) found = true;
        }
        assert(found);
    }
    /* Add the one newly observed, unselected station without exceeding 16. */
    for(size_t i = 0U; i < s.client_count; ++i) {
        if(selected_client_index_(&s, s.clients[i].bssid) < 0) { s.selected_client = i; break; }
    }
    toggle_client_(&s);
    assert(s.config.client_count == ACTION_CLIENTS);
    s.clients[s.selected_client].bssid[5] = 120U;
    toggle_client_(&s);
    assert(s.config.client_count == ACTION_CLIENTS && selected_client_index_(&s, s.clients[s.selected_client].bssid) == -1);
}

int main(void)
{
    reset(1); menu_wifi_action_show(POOM_WIFI_ACTION_QUIET);
    assert(menu_wifi_action_is_active() && pending_ui && !scan_calls); run_ui();
    assert(scan_calls == 1 && start_calls == 1 && phase == 8);

    reset(2); scan_fixture.count = 4;
    scan_fixture.records[0].authmode = WIFI_AUTH_WPA2_PSK;
    scan_fixture.records[1].authmode = WIFI_AUTH_WPA3_PSK;
    scan_fixture.records[2] = scan_fixture.records[0]; scan_fixture.records[2].authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    scan_fixture.records[3] = scan_fixture.records[0]; scan_fixture.records[3].authmode = WIFI_AUTH_WPA3_ENTERPRISE;
    menu_wifi_action_show(POOM_WIFI_ACTION_SAE_COMMIT); run_ui(); assert(scan_calls == 3 && start_calls == 1);

    reset(3); menu_wifi_action_show(POOM_WIFI_ACTION_BAD_MSG); run_ui(); assert(monitor_starts == 1 && monitor_stops == 1 && start_calls == 1);
    reset(4); menu_wifi_action_show(POOM_WIFI_ACTION_ASSOC_SLEEP); run_ui(); assert(monitor_starts == 3 && monitor_stops == 3);
    reset(5); script_before_scan = true; menu_wifi_action_show(POOM_WIFI_ACTION_QUIET); run_ui(); assert(!scan_calls && scan_stop_calls);
    reset(6); scan_error = ESP_FAIL; menu_wifi_action_show(POOM_WIFI_ACTION_QUIET); run_ui(); assert(scan_calls == 2);
    reset(7); fail_task = 2; menu_wifi_action_show(POOM_WIFI_ACTION_QUIET); run_ui(); assert(!scan_calls);
    reset(8); snapshot_error = ESP_FAIL; menu_wifi_action_show(POOM_WIFI_ACTION_BAD_MSG); run_ui(); assert(monitor_stops == 1);

    reset(9); menu_wifi_action_show(POOM_WIFI_ACTION_QUIET); menu_wifi_action_stop();
    assert(!menu_wifi_action_is_active() && !scan_calls && !start_calls && resumes == 1);
    reset(10); request_stop_in_scan = true; menu_wifi_action_show(POOM_WIFI_ACTION_QUIET); run_ui(); assert(scan_calls == 1 && !start_calls);
    reset(11); fail_task = 1; menu_wifi_action_show(POOM_WIFI_ACTION_QUIET); assert(!s_action && resumes == 1 && !pending_ui);
    reset(12); fixture_clients = 0; menu_wifi_action_show(POOM_WIFI_ACTION_ASSOC_SLEEP); run_ui(); assert(!start_calls && monitor_starts == 1 && monitor_stops == 1);
    reset(13); menu_wifi_action_show(POOM_WIFI_ACTION_CHANNEL_SWITCH); run_ui(); assert(start_calls == 1);
    reset(14); channel_error = ESP_FAIL; menu_wifi_action_show(POOM_WIFI_ACTION_BAD_MSG); run_ui(); assert(monitor_starts == 1 && monitor_stops == 1);
    reset(15); menu_wifi_action_show(POOM_WIFI_ACTION_BAD_MSG); run_ui(); assert(!start_calls && monitor_starts == 1 && monitor_stops == 1);
    reset(16); test_selection_capacity();
    reset(17); scan_fixture.records[0].ssid[0] = 0;
    menu_wifi_action_show(POOM_WIFI_ACTION_PROBE_REQUEST); run_ui();
    assert(start_calls == 1 && !monitor_starts && last_started.mode == POOM_MARAUDER_PROBE_REQUEST);
    reset(17); scan_fixture.records[0].ssid[0] = 0;
    menu_wifi_action_show(POOM_WIFI_ACTION_AP_CLONE); run_ui();
    assert(start_calls == 1 && !monitor_starts && last_started.mode == POOM_MARAUDER_AP_CLONE);

    puts("Wi-Fi action UI: discovery, scope, MAC multiselection/reordering, navigation, errors, cancellation and CLI stop passed");
    return 0;
}
