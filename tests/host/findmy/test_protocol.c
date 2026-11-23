// SPDX-License-Identifier: MIT
#include "poom_findmy_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { assert(x); ++checks; } while(0)
int main(void)
{
    uint8_t flags = 0;
    char name[24] = {0};
    uint8_t apple[31] = {30,0xff,0x4c,0,0x12,0x19};
    CHECK(poom_findmy_parse_ad(apple, sizeof(apple), &flags, name));
    CHECK(flags == POOM_FINDMY_APPLE);
    for(size_t i = 1; i < sizeof(apple); ++i) {
        flags = 0;
        CHECK(!poom_findmy_parse_ad(apple, i, &flags, name));
        CHECK(!flags);
    }
    apple[4] = 7; flags = 0;
    CHECK(poom_findmy_parse_ad(apple, sizeof(apple), &flags, name) && !flags);
    apple[4] = 0x12; apple[5] = 0x18;
    CHECK(poom_findmy_parse_ad(apple, sizeof(apple), &flags, name) && !flags);
    const uint8_t fmna[] = {3,3,0x44,0xfd};
    CHECK(poom_findmy_parse_ad(fmna, sizeof(fmna), &flags, name) && flags == POOM_FINDMY_FMNA);
    flags = 0;
    const uint8_t fast_pair[] = {3,3,0x2c,0xfe};
    CHECK(poom_findmy_parse_ad(fast_pair, sizeof(fast_pair), &flags, name) && !flags);
    const uint8_t newer_dult[] = {3,3,0xb2,0xfc};
    CHECK(poom_findmy_parse_ad(newer_dult, sizeof(newer_dult), &flags, name) && flags == POOM_FINDMY_DULT);
    uint8_t dult[18] = {17,7};
    const poom_findmy_profile_t *p = poom_findmy_profile(POOM_FINDMY_DULT_SOUND);
    memcpy(dult + 2, p->service128, 16);
    flags = 0;
    CHECK(poom_findmy_parse_ad(dult, sizeof(dult), &flags, name) && flags == POOM_FINDMY_DULT);
    dult[17] ^= 1; flags = 0;
    CHECK(poom_findmy_parse_ad(dult, sizeof(dult), &flags, name) && !flags);
    const uint8_t malformed[] = {3,3,0x44};
    CHECK(!poom_findmy_parse_ad(malformed, sizeof(malformed), &flags, name) && !flags);
    const uint8_t sanitize[] = {4,9,'A',0xff,10};
    CHECK(poom_findmy_parse_ad(sanitize, sizeof(sanitize), &flags, name) && !strcmp(name,"A??"));
    CHECK(poom_findmy_parse_ad(NULL,0,&flags,name));
    CHECK(!poom_findmy_parse_ad(NULL,1,&flags,name));
    CHECK(!poom_findmy_profile(POOM_FINDMY_PROTOCOL_COUNT));
    CHECK(poom_findmy_profile(POOM_FINDMY_AIRTAG)->command_length == 1 && poom_findmy_profile(POOM_FINDMY_AIRTAG)->command[0] == 0xaf);
    CHECK(poom_findmy_profile(POOM_FINDMY_ACCESSORY)->service16 == 0xfd44);
    CHECK(!memcmp(poom_findmy_profile(POOM_FINDMY_ACCESSORY)->command, (uint8_t[]){1,0,3},3));
    CHECK(p->command_length == 2 && !memcmp(p->command,(uint8_t[]){0,3},2));
    uint16_t status = 99;
    const uint8_t accepted[] = {2,3,0,3,0,0};
    const uint8_t rejected[] = {2,3,0,3,0x34,0x12};
    CHECK(poom_findmy_response(POOM_FINDMY_DULT_SOUND,accepted,6,&status) == POOM_FINDMY_REPLY_ACCEPTED && status == 0);
    CHECK(poom_findmy_response(POOM_FINDMY_DULT_SOUND,rejected,6,&status) == POOM_FINDMY_REPLY_REJECTED && status == 0x1234);
    CHECK(poom_findmy_response(POOM_FINDMY_DULT_SOUND,(uint8_t[]){3,3},2,NULL) == POOM_FINDMY_REPLY_COMPLETED);
    for(size_t i = 0; i < 6; ++i) CHECK(poom_findmy_response(POOM_FINDMY_DULT_SOUND,accepted,i,NULL) == POOM_FINDMY_REPLY_NONE);
    CHECK(poom_findmy_response(POOM_FINDMY_ACCESSORY,rejected,6,NULL) == POOM_FINDMY_REPLY_RECEIVED);
    poom_findmy_device_t list[4] = {0};
    list[0].key.mac[5] = 7; list[0].last_seen_ms = UINT32_MAX - 100;
    list[0].candidates = POOM_FINDMY_APPLE; list[0].connectable = true;
    CHECK(poom_findmy_eligible(&list[0], 200));
    CHECK(!poom_findmy_eligible(&list[0], list[0].last_seen_ms + POOM_FINDMY_FRESH_MS));
    list[1] = list[0]; list[1].key.address_type = 1;
    CHECK(!poom_findmy_key_equal(&list[0].key,&list[1].key));
    list[2] = list[0]; /* Same identity is deduplicated. */
    list[3] = list[0]; list[3].key.mac[5] = 9; list[3].connectable = false;
    poom_findmy_key_t frozen[4];
    CHECK(poom_findmy_freeze(list,4,200,frozen,4) == 2);
    CHECK(frozen[0].address_type == 0 && frozen[1].address_type == 1);
    list[0].key.mac[5] = 88;
    CHECK(frozen[0].mac[5] == 7);
    CHECK(poom_findmy_freeze(list,4,200,frozen,1) == 1);
    /* Exercise every AD length/type over short buffers, with sentinel outputs. */
    uint8_t fuzz[32];
    for(unsigned n=0;n<256;++n) for(unsigned t=0;t<256;++t) {
        memset(fuzz,0,sizeof(fuzz)); fuzz[0]=(uint8_t)n; fuzz[1]=(uint8_t)t;
        flags=0; name[0]=0;
        const bool valid=poom_findmy_parse_ad(fuzz,sizeof(fuzz),&flags,name);
        CHECK(n<=31 || !valid);
        CHECK(name[23]==0);
    }
    printf("Find My protocol: %u checks passed\n",checks);
    return 0;
}
