// SPDX-License-Identifier: MIT
#include "poom_ble_payload.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do {assert(x);++checks;} while(0)
static void valid_(const uint8_t *data,size_t length)
{
    size_t offset=0;
    while(offset<length) {
        CHECK(data[offset]!=0 && (size_t)data[offset]+1<=length-offset);
        offset+=(size_t)data[offset]+1;
    }
    CHECK(offset==length);
}
int main(void)
{
    uint8_t data[33];char label[24];
    for(unsigned mode=0;mode<POOM_BLE_SPAM_MODE_COUNT;++mode)for(unsigned sequence=0;sequence<1000;++sequence) {
        memset(data,0xcc,sizeof(data));memset(label,0xcc,sizeof(label));
        const size_t length=poom_ble_payload_build((poom_ble_spam_mode_t)mode,sequence,sequence*997,data+1,label);
        CHECK(length>0 && length<=31 && data[0]==0xcc && data[32]==0xcc);
        CHECK(label[0] && label[23]==0);
        valid_(data+1,length);
    }
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_MODE_COUNT,0,0,data,label)==0);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_ALL,0,0,NULL,label)==0);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_APPLE_DEVICES,0,0,data,label)==31 && data[7]==2);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_APPLE_DEVICES,16,0,data,label)==31 && data[7]==22);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_APPLE_JUICE,0,0,data,label)==21 && data[4]==7);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_APPLE_JUICE,1,0,data,label)==11 && data[4]==0x0f);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_SOUR_APPLE,1,0,data,label)==11 && data[4]==0x0f);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_SWIFT_PAIR,0,0,data,label)==12 && data[2]==6 && data[6]==0x80);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_SAMSUNG,16,0,data,label)==15 && data[14]==0x20);
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_FAST_PAIR,0,0,data,label)==14 && !memcmp(data+8,(uint8_t[]){0,0xb7,0x27},3));
    CHECK(poom_ble_payload_build(POOM_BLE_SPAM_FLIPPER,2,0,data,label)==27 && data[12]==0x83 && data[17]==9);
    printf("BLE advertising payloads: %u checks passed\n",checks);
    return 0;
}
