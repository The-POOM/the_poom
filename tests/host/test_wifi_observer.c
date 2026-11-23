// SPDX-License-Identifier: MIT
#include "poom_wifi_observer_model.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while(0)
static const uint8_t ap[6] = {0x02, 0x25, 0x46, 0x11, 0x22, 0x33};
static const uint8_t sta[6] = {0x04, 0x88, 0x99, 0x12, 0x23, 0x34};
static uint8_t frame[1800];
static poom_observer_model_t model;
static size_t ie(size_t offset, uint8_t id, const void *bytes, size_t length) {
    CHECK(length <= 255U); frame[offset++] = id; frame[offset++] = (uint8_t)length;
    if(length) memcpy(frame + offset, bytes, length); return offset + length;
}
static size_t beacon(const void *ssid, size_t size) {
    memset(frame, 0, sizeof(frame)); frame[0] = 0x80; memset(frame + 4, 0xff, 6);
    memcpy(frame + 10, ap, 6); memcpy(frame + 16, ap, 6);
    frame[34] = 1; size_t length = ie(36U, 0U, ssid, size); const uint8_t ch = 6;
    return ie(length, 3U, &ch, 1U);
}
static size_t probe(const void *ssid, size_t size) {
    memset(frame, 0, sizeof(frame)); frame[0] = 0x40; memcpy(frame + 10, sta, 6);
    return ie(24U, 0U, ssid, size);
}
static bool consume(size_t length, uint32_t now) { return poom_observer_model_frame(&model, frame, length, 6U, -51, now); }
static void reset(void) { poom_observer_model_init(&model, 0U); }
static void test_bounds(void) {
    reset(); size_t length = beacon("lab", 3U);
    for(size_t i = 0; i < 36U; ++i) CHECK(!consume(i, 1U));
    CHECK(model.ap_count == 0U);
    for(size_t i = 36U; i < length; ++i) {
        reset(); const bool boundary = i == 41U; CHECK(consume(i, 2U) == boundary);
    }
    reset(); CHECK(consume(length, 3U)); CHECK(model.ap_count == 1U);
    CHECK(model.aps[0].channel == 6U); CHECK(model.aps[0].rssi == -51);
    CHECK(!consume(ie(length, 0U, "dup", 3U), 4U));
    CHECK(model.aps[0].observations == 1U);
    length = beacon("", 0U); CHECK(consume(length, 5U)); CHECK(model.aps[0].name_count == 1U);
    frame[length++] = 42U; CHECK(!consume(length, 6U));
    reset(); memset(frame, 0, sizeof(frame)); frame[0] = 0x88; frame[1] = 1;
    memcpy(frame + 4, ap, 6); memcpy(frame + 10, sta, 6);
    CHECK(!consume(24U, 7U)); CHECK(model.station_count == 0U);
    CHECK(!consume(25U, 7U)); CHECK(consume(26U, 7U)); CHECK(model.station_count == 1U);
    frame[1] = 0x83; CHECK(!consume(35U, 8U)); CHECK(consume(36U, 8U));
}
static void test_classifiers(void) {
    reset(); size_t length = beacon("one", 3U);
    CHECK(consume(length, 100U)); CHECK(!(model.aps[0].flags & POOM_OBSERVER_PINE_CONFIRMED));
    CHECK(consume(length, 200U)); CHECK(consume(length, 300U));
    CHECK(model.aps[0].flags & POOM_OBSERVER_PINE_PATTERN);
    CHECK(model.aps[0].flags & POOM_OBSERVER_PINE_CONFIRMED);
    length = beacon("two", 3U); CHECK(consume(length, 400U));
    length = beacon("three", 5U); CHECK(consume(length, 500U));
    CHECK(model.aps[0].flags & POOM_OBSERVER_MULTI_SSID);
    CHECK(model.aps[0].name_count == 3U);
    reset(); const uint8_t name1[] = {'a', 0}, name2[] = {'a', 1}, name3[] = {'a', 2};
    CHECK(consume(beacon(name1, 2), 1)); CHECK(consume(beacon(name2, 2), 2)); CHECK(consume(beacon(name3, 2), 3));
    CHECK(model.aps[0].name_count == 3U); /* Display replacement must not merge raw SSIDs. */
    reset(); length = beacon("same", 4); CHECK(consume(length, 1));
    frame[21]++; frame[34] = 0x11; CHECK(consume(length, 2));
    CHECK(model.aps[0].flags & POOM_OBSERVER_SECURITY_CONFLICT);
    CHECK(model.aps[1].flags & POOM_OBSERVER_SECURITY_CONFLICT);
    reset(); length = beacon("lab", 3); frame[34] = 0x11;
    const uint8_t rsn[] = {1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8};
    length = ie(length, 48, rsn, sizeof(rsn)); const uint8_t wpa[] = {0,80,242,1};
    length = ie(length, 221, wpa, sizeof(wpa)); CHECK(consume(length, 2)); CHECK(model.aps[0].security == POOM_OBSERVER_WPA3);
    const uint8_t csa[] = {1,11,1}; CHECK(consume(ie(length,37,csa,3),3)); CHECK(model.activity[POOM_OBSERVER_CSA].count == 1);
    reset(); length = beacon("pine",4); frame[10] = 0; frame[11] = 0x13; frame[12] = 0x37;
    CHECK(consume(length,1)); CHECK(model.aps[0].flags & POOM_OBSERVER_PINE_OUI);
    CHECK(consume(length,10002)); CHECK(model.aps[0].pine_hits == 1);
    reset();CHECK(consume(beacon("Lab-FlOcK-TEST",14),1));
    CHECK(model.aps[0].flags&POOM_OBSERVER_FLOCK_NAME);CHECK(!(model.aps[0].flags&POOM_OBSERVER_FLOCK_OUI));
    length=probe("unrelated",9);frame[10]=0x58;frame[11]=0x8e;frame[12]=0x81;CHECK(consume(length,2));
    CHECK(model.probes[0].flags==POOM_OBSERVER_FLOCK_OUI);
    length=probe("PENGUIN-LAB",11);CHECK(consume(length,3));CHECK(model.probes[1].flags==POOM_OBSERVER_FLOCK_NAME);
    CHECK(consume(beacon("ordinary lab",12),4));CHECK(model.aps[0].observations==2);
    reset();CHECK(consume(beacon("ordinary lab",12),1));CHECK(!(model.aps[0].flags&(POOM_OBSERVER_FLOCK_OUI|POOM_OBSERVER_FLOCK_NAME)));
}
static void test_json(void) {
    static const char json[] = "{\"name\":\"Lab \\\"Pwn\\\"\",\"pwnd_tot\":42,\"version\":\"1.5.6\",\"uptime\":99,\"policy\":{\"deauth\":true}}";
    for(size_t cut = 1; cut < sizeof(json) - 1U; ++cut) {
        reset(); size_t length = beacon("pwn", 3);
        length = ie(length,222,json,cut); length = ie(length,222,json+cut,sizeof(json)-1-cut);
        CHECK(consume(length,1)); CHECK(model.aps[0].flags & POOM_OBSERVER_PWNAGOTCHI);
        CHECK(model.aps[0].pwn_total == 42); CHECK(model.aps[0].pwn_uptime == 99);
        CHECK(model.aps[0].pwn_deauth); CHECK(strcmp(model.aps[0].pwn_name,"Lab \"Pwn\"") == 0);
    }
    const char *invalid[] = {"{", "{\"name\":\"x\",\"pwnd_tot\":-1}", "{\"name\":\"x\",\"pwnd_tot\":1.5}",
        "{\"name\":\"x\",\"pwnd_tot\":4294967296}", "{\"name\":\"x\",\"pwnd_tot\":\"2\"}",
        "{\"name\":\"x\",\"pwnd_tot\":1}junk", "{\"name\":\"x\",\"pwnd_tot\":1,\"nested\":[[[[[[[[[]]]]]]]]]}"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        reset(); size_t length = beacon("pwn",3); length=ie(length,222,invalid[i],strlen(invalid[i]));
        CHECK(consume(length,1)); CHECK(!(model.aps[0].flags & POOM_OBSERVER_PWNAGOTCHI));
    }
    reset(); size_t length = beacon("pwn",3); char big[255]; memset(big,' ',sizeof(big));
    for(unsigned i=0;i<5;++i) length=ie(length,222,big,sizeof(big));
    CHECK(consume(length,1)); CHECK(!(model.aps[0].flags & POOM_OBSERVER_PWNAGOTCHI));
}
static void test_relations(void) {
    reset(); size_t length=probe("known",5); CHECK(consume(length,1)); CHECK(consume(length,2));
    CHECK(model.probe_repeats==1); CHECK(model.probes[0].count==2); CHECK(!model.stations[0].has_ap);
    memset(frame,0,sizeof(frame)); frame[0]=8; frame[1]=1; memcpy(frame+4,ap,6); memcpy(frame+10,sta,6);
    CHECK(consume(24,3)); CHECK(model.stations[0].has_ap); CHECK(model.relationship_changes==0);
    frame[9]++; CHECK(consume(24,4)); CHECK(model.relationship_changes==1);
    frame[1]=9; CHECK(consume(24,5)); CHECK(model.retries==1); CHECK(model.relationship_changes==1);
    poom_observer_model_tick(&model,30005); CHECK(model.active_stations==0); CHECK(model.station_departures==1);
    CHECK(consume(24,30006)); CHECK(model.station_joins==2);
    reset(); length=beacon("wrap",4); CHECK(consume(length,UINT32_MAX-100));
    poom_observer_model_tick(&model,29900); CHECK(model.active_aps==0); CHECK(model.ap_departures==1);
    reset(); length=beacon("cap",3);
    for(unsigned i=0;i<POOM_OBSERVER_APS+5;++i) {frame[21]=(uint8_t)i; CHECK(consume(length,i));}
    CHECK(model.ap_count==POOM_OBSERVER_APS); CHECK(model.ap_overflow==5);
    length=probe("cap",3);
    for(unsigned i=0;i<POOM_OBSERVER_STATIONS+5;++i) {frame[15]=(uint8_t)i; CHECK(consume(length,i));}
    CHECK(model.station_count==POOM_OBSERVER_STATIONS); CHECK(model.station_overflow==5);
    CHECK(model.probe_entries==POOM_OBSERVER_PROBES); CHECK(model.probe_overflow==POOM_OBSERVER_STATIONS+5-POOM_OBSERVER_PROBES);
}
static void test_report(void) {
    reset(); CHECK(consume(beacon("=\"a\\b",5),1)); CHECK(consume(probe("probe",5),2));
    FILE *file=tmpfile(); CHECK(file); CHECK(poom_observer_model_write_report(&model,file,5,2,3)); rewind(file);
    char line[4096]; unsigned rows=0;
    while(fgets(line,sizeof(line),file)) { cJSON *row=cJSON_Parse(line); CHECK(cJSON_IsObject(row)); cJSON_Delete(row); ++rows; }
    CHECK(rows==5); CHECK(fclose(file)==0);
}
static void test_fuzz(void) {
    uint32_t rng=0xdeadbeefU;
    for(unsigned round=0;round<30000;++round) {
        if(round%100==0) reset();
        rng=rng*1664525U+1013904223U; size_t length=(rng>>16)%sizeof(frame);
        for(size_t i=0;i<length;++i) {rng=rng*1664525U+1013904223U; frame[i]=(uint8_t)(rng>>24);}
        (void)consume(length,round); poom_observer_model_tick(&model,round);
        CHECK(model.ap_count<=POOM_OBSERVER_APS); CHECK(model.station_count<=POOM_OBSERVER_STATIONS);
        CHECK(model.active_aps<=model.ap_count); CHECK(model.active_stations<=model.station_count);
    }
}
int main(void) {
    test_bounds();test_classifiers();test_json();test_relations();test_report();test_fuzz();
    printf("WiFi observer: %u checks passed\n",checks);return 0;
}
