// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM contributors
// Signature concepts adapted from JustCallMeKoko/ESP32Marauder; see PROVENANCE.md.
#include "poom_wifi_observer_model.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static bool unicast(const uint8_t *mac)
{
    static const uint8_t zero[6] = {0};
    return !(mac[0] & 1U) && memcmp(mac, zero, 6U) != 0;
}
static void text_copy(char *out, size_t capacity, const uint8_t *bytes, size_t length)
{
    size_t n = length < capacity - 1U ? length : capacity - 1U;
    for(size_t i = 0; i < n; ++i) out[i] = (bytes[i] >= 32U && bytes[i] < 127U) ? (char)bytes[i] : '.';
    out[n] = '\0';
}
static void increment(uint32_t *value) { if(*value != UINT32_MAX) ++*value; }

void poom_observer_model_init(poom_observer_model_t *m, uint32_t now)
{
    if(m) { memset(m, 0, sizeof(*m)); m->started_ms = now; }
}
const char *poom_observer_security_name(poom_observer_security_t security)
{
    static const char *const names[] = {"OPEN", "WEP", "WPA", "WPA2", "WPA3"};
    return (unsigned)security < 5U ? names[security] : "UNKNOWN";
}
const char *poom_observer_activity_name(poom_observer_activity_kind_t kind)
{
    static const char *const names[] = {"AUTH BURST", "PROBE BURST", "CHANNEL SWITCH",
                                      "EAPOL LOGOFF", "SAE BURST", "DEAUTH BURST"};
    return (unsigned)kind < POOM_OBSERVER_ACTIVITY_COUNT ? names[kind] : "UNKNOWN";
}
static void activity(poom_observer_model_t *m, poom_observer_activity_kind_t kind,
                     const uint8_t *bssid, uint8_t channel, uint32_t now)
{
    poom_observer_activity_t *a = &m->activity[kind];
    if(a->window_count == 0U || now - a->window_ms >= 5000U) { a->window_ms = now; a->window_count = 0U; }
    increment(&a->count); increment(&a->window_count);
    a->last_ms = now; a->channel = channel;
    if(bssid) memcpy(a->bssid, bssid, 6U); else memset(a->bssid, 0, 6U);
}
bool poom_observer_activity_alert(const poom_observer_activity_t *a,
                                 poom_observer_activity_kind_t kind, uint32_t now)
{
    static const uint32_t limits[] = {24U, 40U, 3U, 3U, 12U, 12U};
    return a && (unsigned)kind < POOM_OBSERVER_ACTIVITY_COUNT &&
           now - a->window_ms < 5000U && a->window_count >= limits[kind];
}
static poom_observer_ap_t *ap_entry(poom_observer_model_t *m, const uint8_t *mac, uint32_t now)
{
    if(!unicast(mac)) return NULL;
    for(size_t i = 0; i < POOM_OBSERVER_APS; ++i) {
        poom_observer_ap_t *a = &m->aps[i];
        if(a->used && memcmp(a->bssid, mac, 6U) == 0) {
            if(!a->active) { a->active = true; ++m->active_aps; increment(&m->ap_joins); }
            a->last_ms = now; return a;
        }
    }
    for(size_t i = 0; i < POOM_OBSERVER_APS; ++i) if(!m->aps[i].used) {
        poom_observer_ap_t *a = &m->aps[i];
        a->used = a->active = true; memcpy(a->bssid, mac, 6U);
        a->first_ms = a->last_ms = now; ++m->ap_count; ++m->active_aps; increment(&m->ap_joins); return a;
    }
    increment(&m->ap_overflow); return NULL;
}
static void station(poom_observer_model_t *m, const uint8_t *mac, const uint8_t *bssid,
                    uint8_t channel, int8_t rssi, uint32_t now)
{
    if(!unicast(mac) || (bssid && memcmp(mac, bssid, 6U) == 0)) return;
    poom_observer_station_t *s = NULL, *free_slot = NULL;
    for(size_t i = 0; i < POOM_OBSERVER_STATIONS; ++i) {
        if(!m->stations[i].used) { if(!free_slot) free_slot = &m->stations[i]; }
        else if(memcmp(m->stations[i].mac, mac, 6U) == 0) { s = &m->stations[i]; break; }
    }
    if(!s) {
        if(!free_slot) { increment(&m->station_overflow); return; }
        s = free_slot; s->used = true; memcpy(s->mac, mac, 6U); s->first_ms = now; ++m->station_count;
    }
    if(!s->active) { s->active = true; ++m->active_stations; increment(&m->station_joins); }
    if(bssid && unicast(bssid)) {
        if(s->has_ap && memcmp(s->bssid, bssid, 6U) != 0) {
            increment(&s->relationship_changes); increment(&m->relationship_changes);
        }
        memcpy(s->bssid, bssid, 6U); s->has_ap = true;
    }
    s->channel = channel; s->rssi = rssi; s->last_ms = now; increment(&s->observations);
}
static bool pine_oui(const uint8_t *mac, bool protected_network)
{
    const uint32_t oui = ((uint32_t)mac[0] << 16U) | ((uint32_t)mac[1] << 8U) | mac[2];
    switch(oui) {
        case 0x001337: case 0x000A00: case 0x000C43: case 0x000CE7:
        case 0x0017A5: case 0x9CEFD5: case 0x9CE5D5: case 0xDEADBE: return true;
        case 0x00C0CA: case 0x1CBFCE: case 0x0CEFAF: return !protected_network;
        case 0x02C0CA: case 0x021337: return protected_network;
        default: return false;
    }
}
static uint8_t flock_flags(const uint8_t *mac, const uint8_t *ssid, size_t length)
{
    /* Shared vendor prefixes are only weak candidates, never product identity. */
    static const uint32_t prefixes[] = {
        0x588E81,0xCCCCCC,0xEC1BBD,0x9035EA,0x040D84,0xF082C0,0x1C34F1,
        0x385B44,0x943469,0xB4E3F9,0x70C94E,0x3C9180,0xD8F3BC,0x803049,
        0x145AFC,0x744CA1,0x083A88,0x9C2F9D,0x940853,0xE4AAEA,0xF46ADD,
        0xF8A2D6,0xE00AF6,0x00F48D,0xD03957,0xE8D0FC,0xB41E52,
    };
    static const char *const terms[] = {"flock", "penguin", "pigvision", "fs ext battery"};
    uint8_t flags=0;const uint32_t oui=((uint32_t)mac[0]<<16U)|((uint32_t)mac[1]<<8U)|mac[2];
    for(size_t i=0;i<sizeof(prefixes)/sizeof(prefixes[0]);++i) if(prefixes[i]==oui) flags|=POOM_OBSERVER_FLOCK_OUI;
    for(size_t t=0;t<sizeof(terms)/sizeof(terms[0]);++t) {
        const size_t n=strlen(terms[t]);
        for(size_t i=0;i+n<=length;++i) {
            size_t j=0;
            for(;j<n;++j) {uint8_t c=ssid[i+j];if(c>='A'&&c<='Z')c+='a'-'A';if(c!=(uint8_t)terms[t][j])break;}
            if(j==n){flags|=POOM_OBSERVER_FLOCK_NAME;break;}
        }
    }
    return flags;
}
static bool json_depth_safe(const char *json, size_t length)
{
    unsigned depth = 0; bool quoted = false, escape = false;
    for(size_t i = 0; i < length; ++i) {
        const char c = json[i];
        if(quoted) { if(escape) escape = false; else if(c == '\\') escape = true; else if(c == '"') quoted = false; }
        else if(c == '"') quoted = true;
        else if(c == '{' || c == '[') { if(++depth > 8U) return false; }
        else if(c == '}' || c == ']') { if(depth == 0U) return false; --depth; }
    }
    return depth == 0U && !quoted;
}
static bool json_uint(const cJSON *item, uint32_t *out)
{
    if(!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0.0 ||
       item->valuedouble > (double)UINT32_MAX || floor(item->valuedouble) != item->valuedouble) return false;
    *out = (uint32_t)item->valuedouble; return true;
}
static void pwn_info(poom_observer_ap_t *a, char *json, size_t length)
{
    if(length == 0U || !json_depth_safe(json, length)) return;
    json[length] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1U, NULL, true);
    if(!cJSON_IsObject(root)) { cJSON_Delete(root); return; }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *total = cJSON_GetObjectItemCaseSensitive(root, "pwnd_tot");
    uint32_t count;
    if(cJSON_IsString(name) && name->valuestring[0] && json_uint(total, &count)) {
        a->flags |= POOM_OBSERVER_PWNAGOTCHI;
        text_copy(a->pwn_name, sizeof(a->pwn_name), (const uint8_t *)name->valuestring, strlen(name->valuestring));
        a->pwn_total = count;
        const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
        if(cJSON_IsString(version)) text_copy(a->pwn_version, sizeof(a->pwn_version),
                                              (const uint8_t *)version->valuestring, strlen(version->valuestring));
        (void)json_uint(cJSON_GetObjectItemCaseSensitive(root, "uptime"), &a->pwn_uptime);
        const cJSON *policy = cJSON_GetObjectItemCaseSensitive(root, "policy");
        a->pwn_deauth = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(policy, "deauth"));
    }
    cJSON_Delete(root);
}
static bool beacon(poom_observer_model_t *m, const uint8_t *f, size_t length,
                   uint8_t rx_channel, int8_t rssi, uint32_t now)
{
    if(length < 36U) return false;
    char ssid[33] = {0}, json[POOM_OBSERVER_JSON_MAX + 1U];
    size_t json_len = 0U, tags = 0U;
    uint8_t channel = rx_channel, ssid_length = 0U, raw_ssid[32] = {0};
    bool ssid_seen = false, ds_seen = false, csa = false, json_overflow = false;
    const uint16_t capabilities = (uint16_t)f[34] | ((uint16_t)f[35] << 8U);
    poom_observer_security_t security = (capabilities & 0x10U) ? POOM_OBSERVER_WEP : POOM_OBSERVER_OPEN;
    for(size_t off = 36U; off < length;) {
        if(length - off < 2U || (size_t)f[off + 1U] > length - off - 2U) return false;
        const uint8_t id = f[off], n = f[off + 1U]; const uint8_t *v = f + off + 2U;
        ++tags;
        if(id == 0U) { if(ssid_seen || n > 32U) return false; ssid_seen = true; ssid_length = n; memcpy(raw_ssid, v, n); text_copy(ssid, sizeof(ssid), v, n); }
        else if(id == 3U && n == 1U) { ds_seen = true; if(v[0] >= 1U && v[0] <= 14U) channel = v[0]; }
        else if(id == 37U && n == 3U) csa = true;
        else if(id == 221U && n >= 4U && memcmp(v, "\x00\x50\xf2\x01", 4U) == 0 && security < POOM_OBSERVER_WPA) security = POOM_OBSERVER_WPA;
        else if(id == 48U) {
            if(security < POOM_OBSERVER_WPA2) security = POOM_OBSERVER_WPA2;
            if(n >= 8U) {
                size_t pos = 6U; const size_t pair_count = (size_t)v[pos] | ((size_t)v[pos + 1U] << 8U); pos += 2U;
                if(pair_count <= (n - pos) / 4U) {
                    pos += pair_count * 4U;
                    if(n - pos >= 2U) {
                        const size_t akms = (size_t)v[pos] | ((size_t)v[pos + 1U] << 8U); pos += 2U;
                        if(akms <= (n - pos) / 4U) for(size_t i = 0; i < akms; ++i, pos += 4U)
                            if(memcmp(v + pos, "\x00\x0f\xac", 3U) == 0 && (v[pos + 3U] == 8U || v[pos + 3U] == 9U)) security = POOM_OBSERVER_WPA3;
                    }
                }
            }
        } else if(id == 222U) {
            if(n > POOM_OBSERVER_JSON_MAX - json_len) json_overflow = true;
            else if(!json_overflow) { memcpy(json + json_len, v, n); json_len += n; }
        }
        off += 2U + n;
    }
    if(!ssid_seen) return false;
    poom_observer_ap_t *a = ap_entry(m, f + 16U, now);
    if(!a) return true;
    a->channel = channel; a->rssi = rssi; a->security = security; increment(&a->observations);
    memcpy(a->ssid, ssid, sizeof(ssid));
    a->ssid_length = ssid_length; memcpy(a->raw_ssid, raw_ssid, sizeof(raw_ssid));
    a->flags |= flock_flags(f+10U,raw_ssid,ssid_length);
    if(ssid_length) {
        bool known = false;
        for(uint8_t i = 0; i < a->name_count; ++i) if(a->name_lengths[i] == ssid_length && memcmp(a->raw_names[i], raw_ssid, ssid_length) == 0) known = true;
        if(!known && a->name_count < POOM_OBSERVER_NAMES) {
            const uint8_t index = a->name_count++;
            memcpy(a->names[index], ssid, sizeof(ssid)); memcpy(a->raw_names[index], raw_ssid, sizeof(raw_ssid)); a->name_lengths[index] = ssid_length;
        }
        if(a->name_count >= POOM_OBSERVER_NAMES) a->flags |= POOM_OBSERVER_MULTI_SSID;
        for(size_t i = 0; i < POOM_OBSERVER_APS; ++i) if(m->aps[i].used && &m->aps[i] != a &&
            m->aps[i].ssid_length == ssid_length && memcmp(m->aps[i].raw_ssid, raw_ssid, ssid_length) == 0 && m->aps[i].security != security) {
            a->flags |= POOM_OBSERVER_SECURITY_CONFLICT; m->aps[i].flags |= POOM_OBSERVER_SECURITY_CONFLICT;
        }
    }
    const bool oui = pine_oui(f + 10U, (capabilities & 0x10U) != 0U);
    const bool pattern = (f[0] >> 4U) == 8U && capabilities == 1U && tags == 2U && ds_seen;
    if(oui || pattern) {
        if(a->pine_hits == 0U || now - a->pine_last_ms > 10000U) a->pine_hits = 0U;
        if(a->pine_hits < 3U) ++a->pine_hits;
        a->pine_last_ms = now;
        if(oui) a->flags |= POOM_OBSERVER_PINE_OUI;
        if(pattern) a->flags |= POOM_OBSERVER_PINE_PATTERN;
        if(a->pine_hits >= 3U) a->flags |= POOM_OBSERVER_PINE_CONFIRMED;
    }
    if(!json_overflow) pwn_info(a, json, json_len);
    if(csa) activity(m, POOM_OBSERVER_CSA, a->bssid, channel, now);
    return true;
}
static bool probe(poom_observer_model_t *m, const uint8_t *f, size_t length,
                  uint8_t channel, int8_t rssi, uint32_t now)
{
    char ssid[33] = {0}; uint8_t raw_ssid[32] = {0}, ssid_length = 0U; bool found = false;
    for(size_t off = 24U; off < length;) {
        if(length - off < 2U || (size_t)f[off + 1U] > length - off - 2U) return false;
        if(f[off] == 0U) { if(found || f[off + 1U] > 32U) return false; found = true; ssid_length = f[off + 1U]; memcpy(raw_ssid, f + off + 2U, ssid_length); text_copy(ssid, sizeof(ssid), raw_ssid, ssid_length); }
        off += 2U + f[off + 1U];
    }
    if(!found || !unicast(f + 10U)) return false;
    station(m, f + 10U, NULL, channel, rssi, now); increment(&m->probe_count);
    activity(m, POOM_OBSERVER_PROBE, NULL, channel, now);
    poom_observer_probe_t *p = NULL, *free_slot = NULL;
    for(size_t i = 0; i < POOM_OBSERVER_PROBES; ++i) {
        if(!m->probes[i].used) { if(!free_slot) free_slot = &m->probes[i]; }
        else if(memcmp(m->probes[i].mac, f + 10U, 6U) == 0 && m->probes[i].ssid_length == ssid_length && memcmp(m->probes[i].raw_ssid, raw_ssid, ssid_length) == 0) { p = &m->probes[i]; break; }
    }
    if(!p) {
        if(!free_slot) { increment(&m->probe_overflow); return true; }
        p = free_slot; p->used = true; memcpy(p->mac, f + 10U, 6U); memcpy(p->ssid, ssid, sizeof(ssid)); memcpy(p->raw_ssid, raw_ssid, sizeof(raw_ssid)); p->ssid_length = ssid_length; p->first_ms = now; ++m->probe_entries;
    } else increment(&m->probe_repeats);
    p->flags |= flock_flags(f+10U,raw_ssid,ssid_length);
    increment(&p->count); p->last_ms = now; p->channel = channel; p->rssi = rssi; return true;
}
bool poom_observer_model_frame(poom_observer_model_t *m, const uint8_t *f, size_t length,
                               uint8_t channel, int8_t rssi, uint32_t now)
{
    if(!m || !f) return false;
    increment(&m->frames);
    if(length < 24U || (f[0] & 3U) != 0U) { increment(&m->malformed); return false; }
    const uint8_t type = (f[0] >> 2U) & 3U, subtype = f[0] >> 4U;
    if(f[1] & 8U) increment(&m->retries);
    bool ok = true;
    if(type == 0U) {
        increment(&m->management);
        if(subtype == 8U || subtype == 5U) ok = beacon(m, f, length, channel, rssi, now);
        else if(subtype == 4U) ok = probe(m, f, length, channel, rssi, now);
        else if(subtype == 11U) {
            if(length < 30U) ok = false;
            else activity(m, (f[24] == 3U && f[25] == 0U) ? POOM_OBSERVER_SAE : POOM_OBSERVER_AUTH, f + 16U, channel, now);
        } else if(subtype == 10U || subtype == 12U) {
            if(length < 26U) ok = false; else activity(m, POOM_OBSERVER_DEAUTH, f + 16U, channel, now);
        } else if(subtype == 0U || subtype == 2U) {
            if(length < (subtype == 0U ? 28U : 34U)) ok = false;
            else station(m, f + 10U, f + 16U, channel, rssi, now);
        }
    } else if(type == 2U) {
        increment(&m->data);
        const uint8_t ds = f[1] & 3U;
        const uint8_t *bssid = ds == 1U ? f + 4U : (ds == 2U ? f + 10U : NULL);
        const size_t header = 24U + (ds == 3U ? 6U : 0U) + ((subtype & 8U) ? 2U : 0U) + (((subtype & 8U) && (f[1] & 0x80U)) ? 4U : 0U);
        if(length < header) ok = false;
        else {
            if(ds == 1U) station(m, f + 10U, bssid, channel, rssi, now);
            else if(ds == 2U) station(m, f + 4U, bssid, channel, rssi, now);
            if(!(f[1] & 0x40U) && length - header >= 12U &&
                memcmp(f + header, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8U) == 0 && f[header + 9U] == 2U)
                activity(m, POOM_OBSERVER_LOGOFF, bssid, channel, now);
        }
    }
    if(!ok) increment(&m->malformed);
    return ok;
}
void poom_observer_model_tick(poom_observer_model_t *m, uint32_t now)
{
    if(!m) return;
    for(size_t i = 0; i < POOM_OBSERVER_APS; ++i) if(m->aps[i].active && now - m->aps[i].last_ms >= POOM_OBSERVER_STALE_MS) {
        m->aps[i].active = false; --m->active_aps; increment(&m->ap_departures);
    }
    for(size_t i = 0; i < POOM_OBSERVER_STATIONS; ++i) if(m->stations[i].active && now - m->stations[i].last_ms >= POOM_OBSERVER_STALE_MS) {
        m->stations[i].active = false; --m->active_stations; increment(&m->station_departures);
    }
}
static void json_string(FILE *file, const char *text)
{
    fputc('"', file);
    for(; *text; ++text) { if(*text == '"' || *text == '\\') fputc('\\', file); fputc((unsigned char)*text, file); }
    fputc('"', file);
}
static void mac_string(FILE *file, const uint8_t *mac)
{
    fprintf(file, "\"%02X:%02X:%02X:%02X:%02X:%02X\"", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
bool poom_observer_model_write_report(const poom_observer_model_t *m, FILE *file,
                                      uint32_t now, uint32_t dropped, uint32_t tune_errors)
{
    if(!m || !file) return false;
    fprintf(file, "{\"kind\":\"session\",\"schema\":1,\"elapsed_ms\":%lu,\"frames\":%lu,\"malformed\":%lu,\"rx_dropped\":%lu,\"tune_errors\":%lu,\"ap_joins\":%lu,\"ap_departures\":%lu,\"station_joins\":%lu,\"station_departures\":%lu,\"probe_repeats\":%lu,\"relationship_changes\":%lu,\"ap_overflow\":%lu,\"station_overflow\":%lu,\"probe_overflow\":%lu}\n",
        (unsigned long)(now - m->started_ms), (unsigned long)m->frames, (unsigned long)m->malformed, (unsigned long)dropped, (unsigned long)tune_errors,
        (unsigned long)m->ap_joins, (unsigned long)m->ap_departures, (unsigned long)m->station_joins, (unsigned long)m->station_departures,
        (unsigned long)m->probe_repeats, (unsigned long)m->relationship_changes, (unsigned long)m->ap_overflow, (unsigned long)m->station_overflow, (unsigned long)m->probe_overflow);
    for(size_t i = 0; i < POOM_OBSERVER_APS; ++i) if(m->aps[i].used) {
        const poom_observer_ap_t *a = &m->aps[i];
        fputs("{\"kind\":\"ap\",\"bssid\":", file); mac_string(file, a->bssid);
        fputs(",\"ssid\":", file); json_string(file, a->ssid);
        fprintf(file, ",\"channel\":%u,\"rssi\":%d,\"active\":%s,\"flags\":%u,\"observations\":%lu,\"security\":", a->channel, a->rssi, a->active ? "true" : "false", a->flags, (unsigned long)a->observations);
        json_string(file, poom_observer_security_name(a->security)); fputs(",\"names\":[", file);
        for(uint8_t j = 0; j < a->name_count; ++j) { if(j) fputc(',', file); json_string(file, a->names[j]); }
        fputs("],\"pwn_name\":", file); json_string(file, a->pwn_name); fputs(",\"pwn_version\":", file); json_string(file, a->pwn_version);
        fprintf(file, ",\"pwn_total\":%lu,\"pwn_uptime\":%lu,\"pwn_deauth\":%s}\n", (unsigned long)a->pwn_total, (unsigned long)a->pwn_uptime, a->pwn_deauth ? "true" : "false");
    }
    for(size_t i = 0; i < POOM_OBSERVER_STATIONS; ++i) if(m->stations[i].used) {
        const poom_observer_station_t *s = &m->stations[i];
        fputs("{\"kind\":\"station\",\"mac\":", file); mac_string(file, s->mac); fputs(",\"bssid\":", file);
        if(s->has_ap) mac_string(file, s->bssid); else fputs("null", file);
        fprintf(file, ",\"channel\":%u,\"rssi\":%d,\"active\":%s,\"observations\":%lu,\"relationship_changes\":%lu}\n", s->channel, s->rssi, s->active ? "true" : "false", (unsigned long)s->observations, (unsigned long)s->relationship_changes);
    }
    for(size_t i = 0; i < POOM_OBSERVER_PROBES; ++i) if(m->probes[i].used) {
        const poom_observer_probe_t *p = &m->probes[i];
        fputs("{\"kind\":\"probe\",\"mac\":", file); mac_string(file, p->mac); fputs(",\"ssid\":", file); json_string(file, p->ssid);
        fprintf(file, ",\"count\":%lu,\"channel\":%u,\"rssi\":%d,\"flags\":%u}\n", (unsigned long)p->count, p->channel, p->rssi,p->flags);
    }
    for(unsigned i = 0; i < POOM_OBSERVER_ACTIVITY_COUNT; ++i) if(m->activity[i].count) {
        fputs("{\"kind\":\"activity\",\"name\":", file); json_string(file, poom_observer_activity_name((poom_observer_activity_kind_t)i));
        fprintf(file, ",\"count\":%lu,\"channel\":%u}\n", (unsigned long)m->activity[i].count, m->activity[i].channel);
    }
    return ferror(file) == 0;
}
