// SPDX-License-Identifier: MIT
// Host tests for poom_net_scanner_ident.c:
//   cc -std=c11 -Wall -Wextra -Iinclude -Itest test/test_ident.c poom_net_scanner_ident.c poom_net_scanner_logic.c -o t && ./t
#include "poom_net_scanner.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); g_fail++; } \
                         else { printf("ok:   %s\n", m); } } while (0)

static poom_net_scanner_ident_t fresh(void)
{
    poom_net_scanner_ident_t id;
    memset(&id, 0, sizeof(id));
    return id;
}

static void test_fold(void)
{
    poom_net_scanner_ident_t id = fresh();
    poom_ns_os_from_ttl(&id, 64);
    CHECK(id.os == POOM_NS_OS_POSIX && id.os_ev == POOM_NS_OSEV_INFERRED, "ttl 64 -> Unix-like inferred");
    poom_ns_os_from_text(&id, "SSH-2.0-OpenSSH_8.9p1 Ubuntu-3ubuntu0.6", POOM_NS_OSEV_CONFIRMED);
    CHECK(id.os == POOM_NS_OS_UBUNTU && id.os_ev == POOM_NS_OSEV_CONFIRMED, "banner refines to Ubuntu, confirmed");
    poom_ns_os_from_ports(&id, (const uint16_t[]){22, 3389}, 2);
    CHECK(id.os == POOM_NS_OS_UBUNTU, "conflicting port guess does not override confirmed");

    id = fresh();
    poom_ns_os_from_ttl(&id, 128);
    CHECK(id.os == POOM_NS_OS_WINDOWS && id.os_ev == POOM_NS_OSEV_INFERRED, "ttl 128 -> Windows inferred");
    poom_ns_os_from_vendor(&id, "Apple, Inc.");
    CHECK(id.os == POOM_NS_OS_WINDOWS, "weaker conflicting vendor ignored");

    id = fresh();
    poom_ns_os_from_text(&id, "Linux/4.9 UPnP/1.0 Sonos/63.2", POOM_NS_OSEV_CONFIRMED);
    CHECK(id.os == POOM_NS_OS_IOT, "Linux + Sonos -> IoT device (device type wins)");

    id = fresh();
    poom_ns_os_from_vendor(&id, "Apple, Inc.");
    poom_ns_os_from_ports(&id, (const uint16_t[]){62078}, 1);
    CHECK(id.os == POOM_NS_OS_IOS && id.os_ev == POOM_NS_OSEV_GUESS, "Apple vendor + 62078 -> iOS guess");
    poom_ns_os_from_ttl(&id, 64);
    CHECK(id.os == POOM_NS_OS_IOS && id.os_ev == POOM_NS_OSEV_INFERRED, "ttl 64 keeps iOS, raises to inferred");

    id = fresh();
    poom_ns_os_from_ports(&id, (const uint16_t[]){9100, 631}, 2);
    poom_ns_os_from_ttl(&id, 255);
    CHECK(id.os == POOM_NS_OS_PRINTER && id.os_ev == POOM_NS_OSEV_INFERRED, "printer ports + ttl 255");

    id = fresh();
    poom_ns_os_from_ports(&id, (const uint16_t[]){9100}, 1);
    poom_ns_os_from_ports(&id, (const uint16_t[]){554}, 1);
    CHECK(id.os == POOM_NS_OS_PRINTER, "two device types at same evidence: first kept");

    id = fresh();
    poom_ns_os_from_text(&id, "Microsoft-IIS/10.0", POOM_NS_OSEV_CONFIRMED);
    CHECK(id.os == POOM_NS_OS_WINDOWS, "IIS -> Windows");
    poom_ns_os_from_ttl(&id, 64);
    CHECK(id.os == POOM_NS_OS_WINDOWS, "weaker TTL conflict ignored");

    id = fresh();
    poom_ns_os_from_text(&id, "MacBookPro18,3", POOM_NS_OSEV_CONFIRMED);
    CHECK(id.os == POOM_NS_OS_MACOS, "mDNS model MacBookPro -> macOS");
    poom_ns_os_add(&id, POOM_NS_OS_UNKNOWN, POOM_NS_OSEV_CONFIRMED);
    poom_ns_os_add(&id, 200, POOM_NS_OSEV_CONFIRMED);
    CHECK(id.os == POOM_NS_OS_MACOS, "unknown / out-of-range ids ignored");

    CHECK(strcmp(poom_ns_os_label(POOM_NS_OS_UBUNTU), "Ubuntu") == 0, "label");
    CHECK(strcmp(poom_ns_os_label(250), "?") == 0, "label out of range");
    CHECK(strcmp(poom_ns_osev_label(POOM_NS_OSEV_INFERRED), "inferred") == 0, "ev label");
}

static void test_icmp(void)
{
    uint8_t b[64];
    size_t n = poom_ns_icmp_echo_build(b, sizeof(b), 0x1234, 7, 16);
    CHECK(n == 24 && b[0] == 8, "echo build length/type");
    /* checksum over the whole message must verify to 0 */
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 2) { sum += (uint32_t)((b[i] << 8) | b[i + 1]); }
    while (sum >> 16) { sum = (sum & 0xFFFF) + (sum >> 16); }
    CHECK((uint16_t)~sum == 0, "echo checksum valid");
    CHECK(poom_ns_icmp_echo_build(b, 10, 1, 1, 16) == 0, "echo build cap check");

    uint8_t pkt[40] = {0};
    pkt[0] = 0x45; pkt[8] = 63; pkt[9] = 1;
    pkt[12] = 10; pkt[13] = 0; pkt[14] = 0; pkt[15] = 96;
    pkt[20] = 0; pkt[24] = 0x12; pkt[25] = 0x34;
    uint32_t src = 0; uint8_t ttl = 0;
    CHECK(poom_ns_icmp_reply_parse(pkt, 28, 0x1234, &src, &ttl) && src == 0x0A000060 && ttl == 63,
          "reply parse src+ttl");
    CHECK(!poom_ns_icmp_reply_parse(pkt, 28, 0x9999, &src, &ttl), "reply wrong ident");
    pkt[20] = 8;
    CHECK(!poom_ns_icmp_reply_parse(pkt, 28, 0x1234, &src, &ttl), "not an echo reply");
    pkt[20] = 0;
    CHECK(!poom_ns_icmp_reply_parse(pkt, 20, 0x1234, &src, &ttl), "truncated");
    pkt[0] = 0x4F; /* ihl 60 > len */
    CHECK(!poom_ns_icmp_reply_parse(pkt, 28, 0x1234, &src, &ttl), "bad ihl");
}

static void test_ssdp(void)
{
    const char *r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\n"
                    "Server: Linux/3.14 UPnP/1.0 Roku/9.4\r\nST: upnp:rootdevice\r\n\r\n";
    char o[48];
    CHECK(poom_ns_ssdp_server(r, strlen(r), o, sizeof(o)) &&
          strcmp(o, "Linux/3.14 UPnP/1.0 Roku/9.4") == 0, "ssdp SERVER value");
    char s[8];
    CHECK(poom_ns_ssdp_server(r, strlen(r), s, sizeof(s)) && strlen(s) == 7, "ssdp truncated to cap");
    const char *nos = "HTTP/1.1 200 OK\r\nST: x\r\n\r\n";
    CHECK(!poom_ns_ssdp_server(nos, strlen(nos), o, sizeof(o)) && o[0] == '\0', "ssdp no SERVER");
}

static void test_nbns(void)
{
    uint8_t q[64];
    size_t n = poom_ns_nbns_query_build(q, sizeof(q), 0xBEEF);
    CHECK(n == 50 && q[12] == 32 && q[13] == 'C' && q[14] == 'K' && q[15] == 'A' &&
          q[47] == 0x21 && q[49] == 0x01, "nbns query layout");

    uint8_t r[128] = {0};
    r[2] = 0x84; /* response */
    size_t p = 12;
    r[p++] = 0xC0; r[p++] = 0x0C;          /* name pointer */
    r[p++] = 0x00; r[p++] = 0x21;          /* NBSTAT */
    r[p++] = 0x00; r[p++] = 0x01;          /* IN */
    p += 4;                                /* ttl */
    r[p++] = 0x00; r[p++] = 1 + 2 * 18;    /* rdlength */
    r[p++] = 2;                            /* names */
    memcpy(r + p, "WORKGROUP      ", 15); r[p + 15] = 0x00; r[p + 16] = 0x84; p += 18; /* group */
    memcpy(r + p, "DESKTOP-AB12   ", 15); r[p + 15] = 0x00; r[p + 16] = 0x04; p += 18;
    char name[16];
    CHECK(poom_ns_nbns_reply_name(r, p, name, sizeof(name)) && strcmp(name, "DESKTOP-AB12") == 0,
          "nbns unique name, group skipped");
    CHECK(!poom_ns_nbns_reply_name(r, 30, name, sizeof(name)), "nbns truncated");
    r[2] = 0x00;
    CHECK(!poom_ns_nbns_reply_name(r, p, name, sizeof(name)), "nbns not a response");
}

static size_t put_name(uint8_t *b, size_t p, const char *dotted)
{
    const char *s = dotted;
    while (*s)
    {
        const char *e = strchr(s, '.');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        b[p++] = (uint8_t)l;
        memcpy(b + p, s, l);
        p += l;
        s += l;
        if (*s == '.') { s++; }
    }
    b[p++] = 0;
    return p;
}

static void test_mdns(void)
{
    uint8_t q[128];
    size_t n = poom_ns_mdns_query_build(q, sizeof(q), 0x0102, 0x0A000060); /* 10.0.0.96 */
    CHECK(n > 12 && q[5] == 2, "mdns query has 2 questions");
    CHECK(q[12] == 2 && memcmp(q + 13, "96", 2) == 0, "reverse name starts with last octet");
    CHECK(poom_ns_mdns_query_build(q, 20, 1, 1) == 0, "mdns build cap check");

    /* Response: 1 question echoed, answers: reverse PTR + devinfo PTR (compressed)
     * + TXT model=. */
    uint8_t r[256] = {0};
    r[2] = 0x84; r[5] = 1; r[7] = 2; r[11] = 1; /* qd=1, an=2, ar=1 */
    size_t p = 12;
    size_t qname = p;
    p = put_name(r, p, "96.0.0.10.in-addr.arpa");
    r[p++] = 0; r[p++] = 12; r[p++] = 0; r[p++] = 1;
    /* answer 1: pointer to qname, PTR -> "office-mac.local" */
    r[p++] = (uint8_t)(0xC0 | (qname >> 8)); r[p++] = (uint8_t)qname;
    r[p++] = 0; r[p++] = 12; r[p++] = 0x80; r[p++] = 1; p += 4;
    size_t rdl_at = p; p += 2;
    size_t rd = p;
    p = put_name(r, p, "office-mac.local");
    r[rdl_at] = 0; r[rdl_at + 1] = (uint8_t)(p - rd);
    /* answer 2: _device-info._tcp.local PTR -> "Office Mac._device-info._tcp.local" */
    size_t di = p;
    p = put_name(r, p, "_device-info._tcp.local");
    r[p++] = 0; r[p++] = 12; r[p++] = 0; r[p++] = 1; p += 4;
    rdl_at = p; p += 2; rd = p;
    r[p++] = 10; memcpy(r + p, "Office Mac", 10); p += 10;
    r[p++] = (uint8_t)(0xC0 | (di >> 8)); r[p++] = (uint8_t)di; /* compressed tail */
    r[rdl_at] = 0; r[rdl_at + 1] = (uint8_t)(p - rd);
    /* additional: TXT model= */
    r[p++] = 0xC0; r[p++] = (uint8_t)di;
    r[p++] = 0; r[p++] = 16; r[p++] = 0; r[p++] = 1; p += 4;
    const char *txt = "model=MacBookPro18,3";
    r[p++] = 0; r[p++] = (uint8_t)(strlen(txt) + 1);
    r[p++] = (uint8_t)strlen(txt); memcpy(r + p, txt, strlen(txt)); p += strlen(txt);

    char name[16], model[24];
    CHECK(poom_ns_mdns_reply_parse(r, p, name, sizeof(name), model, sizeof(model)), "mdns parse ok");
    CHECK(strcmp(name, "office-mac") == 0, "mdns hostname from reverse PTR, .local stripped");
    CHECK(strcmp(model, "MacBookPro18,3") == 0, "mdns TXT model");

    /* Without the reverse PTR the device-info instance name is used. */
    uint8_t r2[256];
    memcpy(r2, r, sizeof(r2));
    r2[qname + 1] = 'X'; /* break nothing structurally; reverse name now "9X..." */
    size_t ans1_type = qname + (size_t)(strlen("96.0.0.10.in-addr.arpa") + 2) + 4 + 2;
    r2[ans1_type + 1] = 5; /* answer 1 becomes CNAME (type 5), ignored */
    CHECK(poom_ns_mdns_reply_parse(r2, p, name, sizeof(name), model, sizeof(model)) &&
          strcmp(name, "Office Mac") == 0, "mdns falls back to device-info instance");

    /* Malformed: pointer loop must terminate, not hang or overrun. */
    uint8_t bad[32] = {0};
    bad[2] = 0x84; bad[5] = 1;
    bad[12] = 0xC0; bad[13] = 12; /* points at itself */
    CHECK(!poom_ns_mdns_reply_parse(bad, 16, name, sizeof(name), model, sizeof(model)),
          "mdns pointer loop rejected");
    bad[2] = 0x00;
    CHECK(!poom_ns_mdns_reply_parse(bad, 16, name, sizeof(name), model, sizeof(model)),
          "mdns query (not response) rejected");
}

static void test_upnp(void)
{
    const char *r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\n"
                    "Location: http://10.0.0.1:5000/rootDesc.xml\r\nSERVER: x\r\n\r\n";
    uint32_t ip = 0; uint16_t port = 0; char path[48];
    CHECK(poom_ns_ssdp_location(r, strlen(r), &ip, &port, path, sizeof(path)) &&
          ip == 0x0A000001 && port == 5000 && strcmp(path, "/rootDesc.xml") == 0, "ssdp LOCATION parse");
    const char *r2 = "HTTP/1.1 200 OK\r\nLOCATION: http://10.0.0.96\r\n\r\n";
    CHECK(poom_ns_ssdp_location(r2, strlen(r2), &ip, &port, path, sizeof(path)) &&
          port == 80 && strcmp(path, "/") == 0, "ssdp LOCATION default port/path");
    const char *r3 = "HTTP/1.1 200 OK\r\nLOCATION: http://printer.local/desc.xml\r\n\r\n";
    CHECK(!poom_ns_ssdp_location(r3, strlen(r3), &ip, &port, path, sizeof(path)), "hostname LOCATION rejected");
    const char *r4 = "HTTP/1.1 200 OK\r\nLOCATION: http://10.0.0.1:99999/x\r\n\r\n";
    CHECK(!poom_ns_ssdp_location(r4, strlen(r4), &ip, &port, path, sizeof(path)), "bad port rejected");

    const char *x = "<?xml version=\"1.0\"?><root><device>"
                    "<friendlyName>Living Room Router</friendlyName>"
                    "<manufacturer>NETGEAR, Inc.</manufacturer>"
                    "<modelName>NETGEAR Nighthawk R7000</modelName>"
                    "<modelNumber>R7000</modelNumber>"
                    "<firmwareVersion> V1.0.11.128 </firmwareVersion></device></root>";
    poom_net_scanner_upnp_t u;
    memset(&u, 0, sizeof(u));
    char fr[24];
    CHECK(poom_ns_upnp_from_xml(x, strlen(x), &u, fr, sizeof(fr)) && strcmp(u.model, "R7000") == 0 &&
          strcmp(u.version, "V1.0.11.128") == 0 && strcmp(fr, "Living Room Router") == 0 &&
          strcmp(u.manufacturer, "NETGEAR, Inc.") == 0, "upnp router description");

    const char *x2 = "<root><device><manufacturer>Sonos, Inc.</manufacturer>"
                     "<modelName>Sonos One</modelName></device></root>";
    memset(&u, 0, sizeof(u));
    CHECK(poom_ns_upnp_from_xml(x2, strlen(x2), &u, NULL, 0) && strcmp(u.model, "Sonos") == 0,
          "upnp no model number -> manufacturer word");
    const char *x3 = "<root><device><manufacturer>Microsoft Corporation</manufacturer>"
                     "<modelName>Windows Media Player Sharing</modelName></device></root>";
    memset(&u, 0, sizeof(u));
    CHECK(!poom_ns_upnp_from_xml(x3, strlen(x3), &u, NULL, 0) && u.model[0] == '\0',
          "generic vendor not used as a term");

    char t[16];
    CHECK(poom_ns_xml_tag("<a>Tom &amp; Jerry</a>", 22, "a", t, sizeof(t)) && strcmp(t, "Tom & Jerry") == 0,
          "xml entity decode");
    CHECK(!poom_ns_xml_tag("<a>unterminated", 15, "b", t, sizeof(t)), "xml missing tag");
    CHECK(poom_ns_xml_tag("<a>0123456789abcdefghij</a>", 27, "a", t, sizeof(t)) && strlen(t) == 15,
          "xml value bounded");
}

int main(void)
{
    test_upnp();
    test_fold();
    test_icmp();
    test_ssdp();
    test_nbns();
    test_mdns();
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
