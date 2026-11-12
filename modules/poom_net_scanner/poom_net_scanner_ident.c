// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Pure device / OS identification logic for the Network Scanner: folding OS
// clues (banners, SSDP, mDNS model, ping TTL, open-port signature, MAC vendor)
// into one compact identity, plus builders/parsers for the read-only discovery
// packets (ICMP echo, SSDP, NetBIOS node status, mDNS legacy unicast).
// No ESP-IDF dependencies; host-unit-tested in test/test_ident.c.

#include "poom_net_scanner.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- OS table ------------------------------------------------------------ */

typedef struct
{
    const char *label;
    uint8_t     parent; /* poom_ns_os_t ancestor (UNKNOWN = root) */
    uint8_t     rank;   /* specificity: 0 generic .. 3 device type */
    bool        device; /* device-type identity (orthogonal to the OS) */
} ns_os_info_t;

static const ns_os_info_t k_os[POOM_NS_OS_COUNT] = {
    [POOM_NS_OS_UNKNOWN]  = {"?",            POOM_NS_OS_UNKNOWN, 0, false},
    [POOM_NS_OS_POSIX]    = {"Unix-like",    POOM_NS_OS_UNKNOWN, 0, false},
    [POOM_NS_OS_EMBEDDED] = {"Embedded",     POOM_NS_OS_UNKNOWN, 0, false},
    [POOM_NS_OS_WINDOWS]  = {"Windows",      POOM_NS_OS_UNKNOWN, 1, false},
    [POOM_NS_OS_LINUX]    = {"Linux",        POOM_NS_OS_POSIX,   1, false},
    [POOM_NS_OS_UBUNTU]   = {"Ubuntu",       POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_DEBIAN]   = {"Debian",       POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_RHEL]     = {"RHEL/Fedora",  POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_RPI]      = {"Raspberry Pi", POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_OPENWRT]  = {"OpenWrt",      POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_ROUTEROS] = {"RouterOS",     POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_ANDROID]  = {"Android",      POOM_NS_OS_LINUX,   2, false},
    [POOM_NS_OS_BSD]      = {"BSD",          POOM_NS_OS_POSIX,   1, false},
    [POOM_NS_OS_APPLE]    = {"Apple",        POOM_NS_OS_POSIX,   1, false},
    [POOM_NS_OS_MACOS]    = {"macOS",        POOM_NS_OS_APPLE,   2, false},
    [POOM_NS_OS_IOS]      = {"iOS/iPadOS",   POOM_NS_OS_APPLE,   2, false},
    [POOM_NS_OS_NETDEV]   = {"Router/AP",    POOM_NS_OS_EMBEDDED, 3, true},
    [POOM_NS_OS_PRINTER]  = {"Printer",      POOM_NS_OS_EMBEDDED, 3, true},
    [POOM_NS_OS_CAMERA]   = {"IP camera",    POOM_NS_OS_EMBEDDED, 3, true},
    [POOM_NS_OS_MEDIA]    = {"TV/Media",     POOM_NS_OS_EMBEDDED, 3, true},
    [POOM_NS_OS_IOT]      = {"IoT device",   POOM_NS_OS_EMBEDDED, 3, true},
    [POOM_NS_OS_NAS]      = {"NAS",          POOM_NS_OS_EMBEDDED, 3, true},
    [POOM_NS_OS_CONSOLE]  = {"Game console", POOM_NS_OS_EMBEDDED, 3, true},
};

const char *poom_ns_os_label(uint8_t os)
{
    return (os < POOM_NS_OS_COUNT) ? k_os[os].label : "?";
}

const char *poom_ns_osev_label(uint8_t ev)
{
    switch (ev)
    {
    case POOM_NS_OSEV_CONFIRMED: return "confirmed";
    case POOM_NS_OSEV_INFERRED:  return "inferred";
    default:                     return "guess";
    }
}

static bool ns_is_ancestor_(uint8_t anc, uint8_t os)
{
    for (int hops = 0; hops < 6 && os != POOM_NS_OS_UNKNOWN; ++hops)
    {
        if (os == anc) { return true; }
        os = k_os[os].parent;
    }
    return false;
}

/* Can both clues describe the same host? */
static bool ns_os_consistent_(uint8_t a, uint8_t b)
{
    if (a == b) { return true; }
    bool da = k_os[a].device, db = k_os[b].device;
    if (da && db) { return false; }               /* printer vs camera */
    if (da || db)
    {
        /* A device type runs some embedded/Unix-like OS - never Windows. */
        uint8_t other = da ? b : a;
        return other != POOM_NS_OS_WINDOWS;
    }
    return ns_is_ancestor_(a, b) || ns_is_ancestor_(b, a);
}

void poom_ns_os_add(poom_net_scanner_ident_t *id, uint8_t os, uint8_t ev)
{
    if (id == NULL || os == POOM_NS_OS_UNKNOWN || os >= POOM_NS_OS_COUNT) { return; }
    if (ev > POOM_NS_OSEV_CONFIRMED) { ev = POOM_NS_OSEV_CONFIRMED; }

    if (id->os == POOM_NS_OS_UNKNOWN || id->os >= POOM_NS_OS_COUNT)
    {
        id->os = os;
        id->os_ev = ev;
        return;
    }
    if (ns_os_consistent_(id->os, os))
    {
        if (k_os[os].rank > k_os[id->os].rank) { id->os = os; }
        if (ev > id->os_ev) { id->os_ev = ev; }
        return;
    }
    if (ev > id->os_ev) /* conflict: strictly stronger evidence wins */
    {
        id->os = os;
        id->os_ev = ev;
    }
}

/* ---- text clues ----------------------------------------------------------- */

typedef struct { const char *token; uint8_t os; } ns_token_t;

/* Case-insensitive substring tokens. All matches are folded, so "Linux/4.9
 * UPnP/1.0 Sonos" yields Linux + IoT -> "IoT device". Keep tokens >= 4 chars
 * where possible to avoid accidental hits. */
static const ns_token_t k_text_tokens[] = {
    {"windows", POOM_NS_OS_WINDOWS}, {"win32", POOM_NS_OS_WINDOWS},
    {"win64", POOM_NS_OS_WINDOWS},   {"microsoft-iis", POOM_NS_OS_WINDOWS},
    {"microsoft-httpapi", POOM_NS_OS_WINDOWS},
    {"ubuntu", POOM_NS_OS_UBUNTU},   {"debian", POOM_NS_OS_DEBIAN},
    {"raspbian", POOM_NS_OS_RPI},    {"raspberry", POOM_NS_OS_RPI},
    {"centos", POOM_NS_OS_RHEL},     {"red hat", POOM_NS_OS_RHEL},
    {"fedora", POOM_NS_OS_RHEL},     {"openwrt", POOM_NS_OS_OPENWRT},
    {"routeros", POOM_NS_OS_ROUTEROS}, {"mikrotik", POOM_NS_OS_ROUTEROS},
    {"android", POOM_NS_OS_ANDROID}, {"linux", POOM_NS_OS_LINUX},
    {"freebsd", POOM_NS_OS_BSD},     {"openbsd", POOM_NS_OS_BSD},
    {"netbsd", POOM_NS_OS_BSD},      {"unix", POOM_NS_OS_POSIX},
    {"darwin", POOM_NS_OS_APPLE},    {"mac os", POOM_NS_OS_MACOS},
    {"macos", POOM_NS_OS_MACOS},     {"macbook", POOM_NS_OS_MACOS},
    {"imac", POOM_NS_OS_MACOS},      {"macmini", POOM_NS_OS_MACOS},
    {"macpro", POOM_NS_OS_MACOS},    {"iphone", POOM_NS_OS_IOS},
    {"ipad", POOM_NS_OS_IOS},        {"appletv", POOM_NS_OS_MEDIA},
    {"airport", POOM_NS_OS_NETDEV},  {"synology", POOM_NS_OS_NAS},
    {"diskstation", POOM_NS_OS_NAS}, {"qnap", POOM_NS_OS_NAS},
    {"printer", POOM_NS_OS_PRINTER}, {"laserjet", POOM_NS_OS_PRINTER},
    {"officejet", POOM_NS_OS_PRINTER}, {"deskjet", POOM_NS_OS_PRINTER},
    {"hikvision", POOM_NS_OS_CAMERA}, {"dahua", POOM_NS_OS_CAMERA},
    {"ipcam", POOM_NS_OS_CAMERA},    {"roku", POOM_NS_OS_MEDIA},
    {"chromecast", POOM_NS_OS_MEDIA}, {"bravia", POOM_NS_OS_MEDIA},
    {"webos", POOM_NS_OS_MEDIA},     {"tizen", POOM_NS_OS_MEDIA},
    {"sonos", POOM_NS_OS_IOT},       {"playstation", POOM_NS_OS_CONSOLE},
    {"xbox", POOM_NS_OS_CONSOLE},    {"nintendo", POOM_NS_OS_CONSOLE},
};

static bool ns_has_ci_(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p != '\0'; ++p)
    {
        size_t i = 0;
        while (i < nl && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
        {
            i++;
        }
        if (i == nl) { return true; }
    }
    return false;
}

void poom_ns_os_from_text(poom_net_scanner_ident_t *id, const char *text, uint8_t ev)
{
    if (id == NULL || text == NULL || text[0] == '\0') { return; }
    for (size_t i = 0; i < sizeof(k_text_tokens) / sizeof(k_text_tokens[0]); ++i)
    {
        if (ns_has_ci_(text, k_text_tokens[i].token))
        {
            poom_ns_os_add(id, k_text_tokens[i].os, ev);
        }
    }
}

static bool ns_has_port_(const uint16_t *ports, size_t n, uint16_t p)
{
    for (size_t i = 0; i < n; ++i) { if (ports[i] == p) { return true; } }
    return false;
}

void poom_ns_os_from_ports(poom_net_scanner_ident_t *id, const uint16_t *ports, size_t n)
{
    if (id == NULL || ports == NULL || n == 0U) { return; }
    const uint8_t g = POOM_NS_OSEV_GUESS;

    if (ns_has_port_(ports, n, 3389) || ns_has_port_(ports, n, 135))
    {
        poom_ns_os_add(id, POOM_NS_OS_WINDOWS, g);
    }
    if (ns_has_port_(ports, n, 62078)) { poom_ns_os_add(id, POOM_NS_OS_IOS, g); }
    if (ns_has_port_(ports, n, 548))   { poom_ns_os_add(id, POOM_NS_OS_MACOS, g); }
    if (ns_has_port_(ports, n, 5555))  { poom_ns_os_add(id, POOM_NS_OS_ANDROID, g); }
    if (ns_has_port_(ports, n, 9100) || ns_has_port_(ports, n, 515))
    {
        poom_ns_os_add(id, POOM_NS_OS_PRINTER, g);
    }
    if (ns_has_port_(ports, n, 554))   { poom_ns_os_add(id, POOM_NS_OS_CAMERA, g); }
    if (ns_has_port_(ports, n, 8008) || ns_has_port_(ports, n, 8009))
    {
        poom_ns_os_add(id, POOM_NS_OS_MEDIA, g);
    }
    if (ns_has_port_(ports, n, 5000) && ns_has_port_(ports, n, 5001))
    {
        poom_ns_os_add(id, POOM_NS_OS_NAS, g);
    }
    if (ns_has_port_(ports, n, 1883) || ns_has_port_(ports, n, 6668))
    {
        poom_ns_os_add(id, POOM_NS_OS_IOT, g);
    }
    if (ns_has_port_(ports, n, 53))    { poom_ns_os_add(id, POOM_NS_OS_NETDEV, g); }
    if (ns_has_port_(ports, n, 22) &&
        (ns_has_port_(ports, n, 111) || ns_has_port_(ports, n, 2049)))
    {
        poom_ns_os_add(id, POOM_NS_OS_POSIX, g);
    }
}

void poom_ns_os_from_ttl(poom_net_scanner_ident_t *id, uint8_t ttl)
{
    if (id == NULL || ttl == 0U) { return; }
    /* LAN hosts are 0-1 hops away, so the reply TTL sits at/just under the
     * sender's initial value: 64 Unix-like, 128 Windows, 255 embedded stacks. */
    if (ttl <= 64U)       { poom_ns_os_add(id, POOM_NS_OS_POSIX, POOM_NS_OSEV_INFERRED); }
    else if (ttl <= 128U) { poom_ns_os_add(id, POOM_NS_OS_WINDOWS, POOM_NS_OSEV_INFERRED); }
    else                  { poom_ns_os_add(id, POOM_NS_OS_EMBEDDED, POOM_NS_OSEV_INFERRED); }
}

void poom_ns_os_from_vendor(poom_net_scanner_ident_t *id, const char *vendor)
{
    static const ns_token_t k_vendor[] = {
        {"apple", POOM_NS_OS_APPLE},       {"raspberry", POOM_NS_OS_RPI},
        {"espressif", POOM_NS_OS_IOT},     {"tuya", POOM_NS_OS_IOT},
        {"sonos", POOM_NS_OS_IOT},         {"philips", POOM_NS_OS_IOT},
        {"amazon", POOM_NS_OS_IOT},        {"roku", POOM_NS_OS_MEDIA},
        {"sony", POOM_NS_OS_MEDIA},        {"nintendo", POOM_NS_OS_CONSOLE},
        {"hikvision", POOM_NS_OS_CAMERA},  {"dahua", POOM_NS_OS_CAMERA},
        {"synology", POOM_NS_OS_NAS},      {"qnap", POOM_NS_OS_NAS},
        {"tp-link", POOM_NS_OS_NETDEV},    {"netgear", POOM_NS_OS_NETDEV},
        {"ubiquiti", POOM_NS_OS_NETDEV},   {"mikrotik", POOM_NS_OS_ROUTEROS},
        {"cisco", POOM_NS_OS_NETDEV},      {"d-link", POOM_NS_OS_NETDEV},
        {"zyxel", POOM_NS_OS_NETDEV},      {"aruba", POOM_NS_OS_NETDEV},
        {"huawei", POOM_NS_OS_ANDROID},    {"oneplus", POOM_NS_OS_ANDROID},
        {"oppo", POOM_NS_OS_ANDROID},      {"vivo", POOM_NS_OS_ANDROID},
    };
    if (id == NULL || vendor == NULL || vendor[0] == '\0') { return; }
    for (size_t i = 0; i < sizeof(k_vendor) / sizeof(k_vendor[0]); ++i)
    {
        if (ns_has_ci_(vendor, k_vendor[i].token))
        {
            poom_ns_os_add(id, k_vendor[i].os, POOM_NS_OSEV_GUESS);
            return;
        }
    }
}

/* ---- ICMP ----------------------------------------------------------------- */

static uint16_t ns_csum_(const uint8_t *d, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1U < len; i += 2U) { sum += (uint32_t)((d[i] << 8) | d[i + 1U]); }
    if (len & 1U) { sum += (uint32_t)(d[len - 1U] << 8); }
    while (sum >> 16) { sum = (sum & 0xFFFFU) + (sum >> 16); }
    return (uint16_t)~sum;
}

size_t poom_ns_icmp_echo_build(uint8_t *buf, size_t cap, uint16_t ident, uint16_t seq,
                               size_t payload_len)
{
    size_t len = 8U + payload_len;
    if (buf == NULL || cap < len) { return 0U; }
    memset(buf, 0, len);
    buf[0] = 8U; /* echo request */
    buf[4] = (uint8_t)(ident >> 8); buf[5] = (uint8_t)ident;
    buf[6] = (uint8_t)(seq >> 8);   buf[7] = (uint8_t)seq;
    for (size_t i = 0; i < payload_len; ++i) { buf[8U + i] = (uint8_t)('a' + (i % 26U)); }
    uint16_t c = ns_csum_(buf, len);
    buf[2] = (uint8_t)(c >> 8); buf[3] = (uint8_t)c;
    return len;
}

bool poom_ns_icmp_reply_parse(const uint8_t *pkt, size_t len, uint16_t ident,
                              uint32_t *src_host, uint8_t *ttl)
{
    if (pkt == NULL || len < 20U || (pkt[0] >> 4) != 4U) { return false; }
    size_t ihl = (size_t)(pkt[0] & 0x0FU) * 4U;
    if (ihl < 20U || len < ihl + 8U || pkt[9] != 1U /* ICMP */) { return false; }
    const uint8_t *icmp = pkt + ihl;
    if (icmp[0] != 0U /* echo reply */) { return false; }
    if ((uint16_t)((icmp[4] << 8) | icmp[5]) != ident) { return false; }
    if (src_host != NULL)
    {
        *src_host = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
                    ((uint32_t)pkt[14] << 8) | (uint32_t)pkt[15];
    }
    if (ttl != NULL) { *ttl = pkt[8]; }
    return true;
}

/* ---- SSDP ----------------------------------------------------------------- */

bool poom_ns_ssdp_server(const char *resp, size_t len, char *out, size_t cap)
{
    if (resp == NULL || out == NULL || cap == 0U) { return false; }
    out[0] = '\0';
    size_t i = 0;
    while (i < len)
    {
        /* line [i, e) */
        size_t e = i;
        while (e < len && resp[e] != '\n' && resp[e] != '\0') { e++; }
        if (e - i >= 7U)
        {
            static const char k[] = "server:";
            size_t k_i = 0;
            while (k_i < 7U && tolower((unsigned char)resp[i + k_i]) == k[k_i]) { k_i++; }
            if (k_i == 7U)
            {
                size_t v = i + 7U;
                while (v < e && (resp[v] == ' ' || resp[v] == '\t')) { v++; }
                size_t ve = e;
                while (ve > v && (resp[ve - 1U] == '\r' || resp[ve - 1U] == ' ')) { ve--; }
                size_t n = ve - v;
                if (n > cap - 1U) { n = cap - 1U; }
                memcpy(out, resp + v, n);
                out[n] = '\0';
                return n > 0U;
            }
        }
        if (e < len && resp[e] == '\0') { break; }
        i = e + 1U;
    }
    return false;
}

/* ---- NetBIOS node status ---------------------------------------------------- */

size_t poom_ns_nbns_query_build(uint8_t *buf, size_t cap, uint16_t txid)
{
    const size_t len = 12U + 34U + 4U;
    if (buf == NULL || cap < len) { return 0U; }
    memset(buf, 0, len);
    buf[0] = (uint8_t)(txid >> 8); buf[1] = (uint8_t)txid;
    buf[5] = 1U; /* QDCOUNT */
    buf[12] = 32U; /* encoded "*" + 15 NULs (first-level encoding) */
    for (int i = 0; i < 16; ++i)
    {
        uint8_t c = (i == 0) ? (uint8_t)'*' : 0U;
        buf[13 + 2 * i] = (uint8_t)('A' + (c >> 4));
        buf[14 + 2 * i] = (uint8_t)('A' + (c & 0x0FU));
    }
    buf[45] = 0U;            /* name terminator */
    buf[46] = 0x00; buf[47] = 0x21; /* NBSTAT */
    buf[48] = 0x00; buf[49] = 0x01; /* IN     */
    return len;
}

bool poom_ns_nbns_reply_name(const uint8_t *pkt, size_t len, char *out, size_t cap)
{
    if (pkt == NULL || out == NULL || cap == 0U) { return false; }
    out[0] = '\0';
    if (len < 12U || (pkt[2] & 0x80U) == 0U) { return false; } /* must be a response */
    size_t p = 12U;
    /* answer name: either a pointer (2 bytes) or labels */
    if (p < len && (pkt[p] & 0xC0U) == 0xC0U) { p += 2U; }
    else
    {
        while (p < len && pkt[p] != 0U) { p += (size_t)pkt[p] + 1U; }
        p += 1U;
    }
    if (p + 10U >= len) { return false; }
    if (pkt[p] != 0x00 || pkt[p + 1U] != 0x21) { return false; } /* NBSTAT */
    p += 10U; /* type, class, ttl, rdlength */
    uint8_t count = pkt[p++];
    for (uint8_t k = 0; k < count; ++k)
    {
        if (p + 18U > len) { break; }
        const uint8_t *e = pkt + p;
        uint8_t suffix = e[15];
        bool group = (e[16] & 0x80U) != 0U;
        if (suffix == 0x00U && !group)
        {
            size_t n = 15U;
            while (n > 0U && (e[n - 1U] == ' ' || e[n - 1U] == 0U)) { n--; }
            if (n > cap - 1U) { n = cap - 1U; }
            for (size_t i = 0; i < n; ++i)
            {
                out[i] = isprint(e[i]) ? (char)e[i] : '?';
            }
            out[n] = '\0';
            return n > 0U;
        }
        p += 18U;
    }
    return false;
}

/* ---- mDNS (legacy unicast) ------------------------------------------------- */

static size_t ns_put_label_(uint8_t *b, size_t p, size_t cap, const char *s, size_t n)
{
    if (n == 0U || n > 63U || p + 1U + n > cap) { return 0U; }
    b[p] = (uint8_t)n;
    memcpy(b + p + 1U, s, n);
    return p + 1U + n;
}

size_t poom_ns_mdns_query_build(uint8_t *buf, size_t cap, uint16_t txid, uint32_t ip_host)
{
    if (buf == NULL || cap < 12U) { return 0U; }
    memset(buf, 0, 12U);
    buf[0] = (uint8_t)(txid >> 8); buf[1] = (uint8_t)txid;
    buf[5] = 2U; /* QDCOUNT = 2 */
    size_t p = 12U;

    /* Q1: d.c.b.a.in-addr.arpa PTR */
    char oct[4];
    for (int i = 0; i < 4; ++i)
    {
        unsigned v = (unsigned)((ip_host >> (8 * i)) & 0xFFU);
        int n = snprintf(oct, sizeof(oct), "%u", v);
        p = ns_put_label_(buf, p, cap, oct, (size_t)n);
        if (p == 0U) { return 0U; }
    }
    p = ns_put_label_(buf, p, cap, "in-addr", 7U); if (p == 0U) { return 0U; }
    p = ns_put_label_(buf, p, cap, "arpa", 4U);    if (p == 0U) { return 0U; }
    if (p + 5U > cap) { return 0U; }
    buf[p++] = 0U;
    buf[p++] = 0x00; buf[p++] = 0x0C; /* PTR */
    buf[p++] = 0x00; buf[p++] = 0x01; /* IN  */

    /* Q2: _device-info._tcp.local PTR */
    p = ns_put_label_(buf, p, cap, "_device-info", 12U); if (p == 0U) { return 0U; }
    p = ns_put_label_(buf, p, cap, "_tcp", 4U);          if (p == 0U) { return 0U; }
    p = ns_put_label_(buf, p, cap, "local", 5U);         if (p == 0U) { return 0U; }
    if (p + 5U > cap) { return 0U; }
    buf[p++] = 0U;
    buf[p++] = 0x00; buf[p++] = 0x0C;
    buf[p++] = 0x00; buf[p++] = 0x01;
    return p;
}

/* Decode a (possibly compressed) DNS name at *pp into "a.b.c" (bounded).
 * Advances *pp past the name in the record. Returns false on malformed. */
static bool ns_dns_name_(const uint8_t *pkt, size_t len, size_t *pp, char *out, size_t cap)
{
    size_t p = *pp;
    size_t o = 0;
    bool jumped = false;
    int hops = 0;
    if (cap > 0U) { out[0] = '\0'; }

    while (p < len)
    {
        uint8_t l = pkt[p];
        if (l == 0U)
        {
            if (!jumped) { *pp = p + 1U; }
            if (cap > 0U) { out[(o < cap) ? o : cap - 1U] = '\0'; }
            return true;
        }
        if ((l & 0xC0U) == 0xC0U)
        {
            if (p + 1U >= len || ++hops > 8) { return false; }
            size_t target = ((size_t)(l & 0x3FU) << 8) | pkt[p + 1U];
            if (!jumped) { *pp = p + 2U; }
            jumped = true;
            if (target >= len) { return false; }
            p = target;
            continue;
        }
        if ((l & 0xC0U) != 0U || p + 1U + l > len) { return false; }
        if (o > 0U && o + 1U < cap) { out[o++] = '.'; }
        for (size_t i = 0; i < l && o + 1U < cap; ++i)
        {
            char c = (char)pkt[p + 1U + i];
            out[o++] = isprint((unsigned char)c) ? c : '?';
        }
        p += 1U + l;
    }
    return false;
}

static void ns_copy_trim_suffix_(char *dst, size_t cap, const char *src, const char *suffix)
{
    size_t n = strlen(src), sl = strlen(suffix);
    if (n >= sl && strcmp(src + n - sl, suffix) == 0) { n -= sl; }
    if (n > cap - 1U) { n = cap - 1U; }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool poom_ns_mdns_reply_parse(const uint8_t *pkt, size_t len,
                              char *name, size_t name_cap, char *model, size_t model_cap)
{
    if (pkt == NULL || name == NULL || model == NULL || name_cap == 0U || model_cap == 0U)
    {
        return false;
    }
    name[0] = '\0';
    model[0] = '\0';
    if (len < 12U || (pkt[2] & 0x80U) == 0U) { return false; } /* response only */

    unsigned qd = ((unsigned)pkt[4] << 8) | pkt[5];
    unsigned rr = (((unsigned)pkt[6] << 8) | pkt[7]) + (((unsigned)pkt[8] << 8) | pkt[9]) +
                  (((unsigned)pkt[10] << 8) | pkt[11]);
    size_t p = 12U;
    char tmp[96];
    char devinfo[POOM_NS_NAME_CAP + 8U] = {0};

    for (unsigned i = 0; i < qd; ++i)
    {
        if (!ns_dns_name_(pkt, len, &p, tmp, sizeof(tmp)) || p + 4U > len) { return false; }
        p += 4U;
    }
    for (unsigned i = 0; i < rr && i < 32U; ++i)
    {
        char rname[96];
        if (!ns_dns_name_(pkt, len, &p, rname, sizeof(rname)) || p + 10U > len) { break; }
        uint16_t type = (uint16_t)((pkt[p] << 8) | pkt[p + 1U]);
        size_t rdlen = ((size_t)pkt[p + 8U] << 8) | pkt[p + 9U];
        size_t rd = p + 10U;
        if (rd + rdlen > len) { break; }

        if (type == 12U) /* PTR */
        {
            size_t q = rd;
            if (ns_dns_name_(pkt, len, &q, tmp, sizeof(tmp)))
            {
                if (ns_has_ci_(rname, "in-addr.arpa") && name[0] == '\0')
                {
                    ns_copy_trim_suffix_(name, name_cap, tmp, ".local");
                }
                else if (ns_has_ci_(rname, "_device-info") && devinfo[0] == '\0')
                {
                    ns_copy_trim_suffix_(devinfo, sizeof(devinfo), tmp,
                                         "._device-info._tcp.local");
                }
            }
        }
        else if (type == 16U && model[0] == '\0') /* TXT: look for model= */
        {
            size_t q = rd;
            while (q < rd + rdlen)
            {
                size_t sl = pkt[q];
                if (q + 1U + sl > rd + rdlen) { break; }
                if (sl > 6U && memcmp(pkt + q + 1U, "model=", 6U) == 0)
                {
                    size_t n = sl - 6U;
                    if (n > model_cap - 1U) { n = model_cap - 1U; }
                    memcpy(model, pkt + q + 7U, n);
                    model[n] = '\0';
                    break;
                }
                q += 1U + sl;
            }
        }
        p = rd + rdlen;
    }

    if (name[0] == '\0' && devinfo[0] != '\0')
    {
        (void)snprintf(name, name_cap, "%.*s", (int)(name_cap - 1U), devinfo);
    }
    return name[0] != '\0' || model[0] != '\0';
}

/* ---- UPnP device description ------------------------------------------------ */

bool poom_ns_ssdp_location(const char *resp, size_t len, uint32_t *ip_host, uint16_t *port,
                           char *path, size_t path_cap)
{
    if (resp == NULL || ip_host == NULL || port == NULL || path == NULL || path_cap < 2U)
    {
        return false;
    }
    /* find "location:" at a line start, case-insensitive */
    size_t i = 0;
    const char *v = NULL;
    while (i < len)
    {
        size_t e = i;
        while (e < len && resp[e] != '\n' && resp[e] != '\0') { e++; }
        static const char k[] = "location:";
        size_t kk = 0;
        while (kk < 9U && i + kk < e && tolower((unsigned char)resp[i + kk]) == k[kk]) { kk++; }
        if (kk == 9U) { v = resp + i + 9; len = e; break; }
        if (e >= len || resp[e] == '\0') { break; }
        i = e + 1U;
    }
    if (v == NULL) { return false; }
    while ((size_t)(v - resp) < len && (*v == ' ' || *v == '\t')) { v++; }
    if ((size_t)(v - resp) + 7U > len || strncmp(v, "http://", 7) != 0) { return false; }
    v += 7;

    unsigned a, b, c, d;
    int consumed = 0;
    if (sscanf(v, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &consumed) != 4 || a > 255U ||
        b > 255U || c > 255U || d > 255U)
    {
        return false;
    }
    v += consumed;
    *ip_host = (a << 24) | (b << 16) | (c << 8) | d;
    *port = 80U;
    if (*v == ':')
    {
        unsigned p = 0;
        v++;
        while (isdigit((unsigned char)*v) && p <= 65535U) { p = p * 10U + (unsigned)(*v - '0'); v++; }
        if (p == 0U || p > 65535U) { return false; }
        *port = (uint16_t)p;
    }
    size_t n = 0;
    if (*v != '/') { path[n++] = '/'; }
    while ((size_t)(v - resp) < len && *v != '\r' && *v != '\n' && *v != ' ' && *v != '\0' &&
           n + 1U < path_cap)
    {
        path[n++] = *v++;
    }
    path[n] = '\0';
    return true;
}

bool poom_ns_xml_tag(const char *xml, size_t len, const char *tag, char *out, size_t cap)
{
    if (xml == NULL || tag == NULL || out == NULL || cap == 0U) { return false; }
    out[0] = '\0';
    char open[40];
    int ol = snprintf(open, sizeof(open), "<%s>", tag);
    if (ol <= 0 || (size_t)ol >= sizeof(open)) { return false; }
    for (size_t i = 0; i + (size_t)ol <= len; ++i)
    {
        if (memcmp(xml + i, open, (size_t)ol) != 0) { continue; }
        size_t p = i + (size_t)ol;
        while (p < len && isspace((unsigned char)xml[p])) { p++; }
        size_t n = 0;
        while (p < len && xml[p] != '<' && n + 1U < cap)
        {
            char ch = xml[p];
            if (ch == '&')
            {
                static const struct { const char *e; char c; } k_ent[] = {
                    {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
                bool done = false;
                for (size_t k = 0; k < sizeof(k_ent) / sizeof(k_ent[0]); ++k)
                {
                    size_t el = strlen(k_ent[k].e);
                    if (p + el <= len && memcmp(xml + p, k_ent[k].e, el) == 0)
                    {
                        out[n++] = k_ent[k].c;
                        p += el;
                        done = true;
                        break;
                    }
                }
                if (done) { continue; }
            }
            out[n++] = isprint((unsigned char)ch) ? ch : ' ';
            p++;
        }
        while (n > 0U && isspace((unsigned char)out[n - 1U])) { n--; }
        out[n] = '\0';
        return n > 0U;
    }
    return false;
}

/* Vendor names too generic to search on their own (would match thousands of
 * unrelated Exploit-DB titles). */
static bool ns_generic_vendor_(const char *m)
{
    static const char *const k[] = {"microsoft", "apple", "google", "intel", "amazon",
                                    "linux", "upnp", "unknown", "generic", "manufacturer"};
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); ++i)
    {
        if (ns_has_ci_(m, k[i])) { return true; }
    }
    return false;
}

bool poom_ns_upnp_from_xml(const char *xml, size_t len, poom_net_scanner_upnp_t *u,
                           char *friendly, size_t friendly_cap)
{
    if (xml == NULL || u == NULL) { return false; }
    char mname[48], mnum[32], fw[24];
    (void)poom_ns_xml_tag(xml, len, "manufacturer", u->manufacturer, sizeof(u->manufacturer));
    (void)poom_ns_xml_tag(xml, len, "modelName", mname, sizeof(mname));
    (void)poom_ns_xml_tag(xml, len, "modelNumber", mnum, sizeof(mnum));
    if (!poom_ns_xml_tag(xml, len, "firmwareVersion", fw, sizeof(fw)))
    {
        (void)poom_ns_xml_tag(xml, len, "softwareVersion", fw, sizeof(fw));
    }
    if (friendly != NULL && friendly_cap > 0U)
    {
        (void)poom_ns_xml_tag(xml, len, "friendlyName", friendly, friendly_cap);
    }

    u->model[0] = '\0';
    if (poom_net_scanner_model_token(mname, u->model, sizeof(u->model)) == 0U &&
        poom_net_scanner_model_token(mnum, u->model, sizeof(u->model)) == 0U)
    {
        /* No model number anywhere: fall back to the manufacturer's first
         * word, unless it is too generic to be a useful search term. */
        if (u->manufacturer[0] != '\0' && !ns_generic_vendor_(u->manufacturer))
        {
            size_t n = 0;
            while (u->manufacturer[n] != '\0' && u->manufacturer[n] != ' ' &&
                   u->manufacturer[n] != ',' && n + 1U < sizeof(u->model))
            {
                u->model[n] = u->manufacturer[n];
                n++;
            }
            u->model[n] = '\0';
            if (n < 3U) { u->model[0] = '\0'; }
        }
    }
    (void)snprintf(u->version, sizeof(u->version), "%.*s", (int)(sizeof(u->version) - 1U), fw);
    return u->model[0] != '\0';
}
