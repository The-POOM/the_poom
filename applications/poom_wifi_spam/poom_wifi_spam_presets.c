// SPDX-License-Identifier: MIT
// Native preset data; the Rick Roll identities contain no song lyrics.
#include "poom_wifi_spam.h"
#include <stdio.h>
#include <string.h>

const char *poom_wifi_spam_mode_label(poom_wifi_spam_mode_t mode)
{
    static const char *const names[] = {"Saved SSIDs", "Random SSIDs", "Funny SSIDs", "Rick Roll"};
    return mode >= POOM_WIFI_SPAM_SAVED && mode < POOM_WIFI_SPAM_MODE_COUNT ? names[mode] : "Unknown";
}

bool poom_wifi_spam_preset(poom_wifi_spam_mode_t mode, poom_wifi_spam_ssid_list_t *list)
{
    static const char *const funny[] = {
        "Packet Picnic", "SSID Still Loading", "Tiny Radio Big Plans", "404 Network Found",
        "WiFi With Extra Bytes", "Signal Snack Bar", "The Buffer Is Full", "Unscheduled Packet Break",
    };
    static const char *const rick[] = {"Rick Roll", "youtu.be/dQw4w9WgXcQ", "Video Surprise - 1987"};
    if(!list || mode <= POOM_WIFI_SPAM_SAVED || mode >= POOM_WIFI_SPAM_MODE_COUNT) return false;
    memset(list, 0, sizeof(*list));
    if(mode == POOM_WIFI_SPAM_RANDOM) {
        list->count = 30U;
        for(unsigned i = 0U; i < list->count; ++i) {
            (void)snprintf(list->ssids[i], sizeof(list->ssids[i]), "Random_%02u", i);
        }
        return true; /* Runtime generates fresh random identities before TX. */
    }
    const char *const *preset = mode == POOM_WIFI_SPAM_FUNNY ? funny : rick;
    list->count = mode == POOM_WIFI_SPAM_FUNNY ? sizeof(funny) / sizeof(funny[0]) : sizeof(rick) / sizeof(rick[0]);
    for(unsigned i = 0U; i < list->count; ++i) {
        (void)snprintf(list->ssids[i], sizeof(list->ssids[i]), "%s", preset[i]);
    }
    return true;
}
