#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "poom_wifi_spam.h"
int main(void)
{
    poom_wifi_spam_ssid_list_t list;
    assert(!poom_wifi_spam_preset(POOM_WIFI_SPAM_SAVED, &list));
    assert(!poom_wifi_spam_preset(POOM_WIFI_SPAM_MODE_COUNT, &list));
    assert(!poom_wifi_spam_preset(POOM_WIFI_SPAM_FUNNY, NULL));
    for(poom_wifi_spam_mode_t mode=POOM_WIFI_SPAM_RANDOM; mode<POOM_WIFI_SPAM_MODE_COUNT; ++mode) {
        memset(&list, 0xcc, sizeof(list));
        assert(poom_wifi_spam_preset(mode, &list));
        assert(list.count>0 && list.count<=POOM_WIFI_SPAM_SSIDS_MAX);
        for(unsigned i=0; i<list.count; ++i) {
            assert(list.ssids[i][32]==0 && strlen(list.ssids[i])>0);
            for(unsigned j=0; j<i; ++j) assert(strcmp(list.ssids[i],list.ssids[j])!=0);
        }
    }
    puts("Beacon presets: bounded unique SSIDs and invalid modes passed.");
    return 0;
}
