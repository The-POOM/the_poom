// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Host-side unit tests for poom_net_scanner_logic.c. No ESP32 required:
//   cc -I../include -I. test_logic.c ../poom_net_scanner_logic.c esp_err_shim.c -o t && ./t
// Returns non-zero on first failure.

#include "poom_net_scanner.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) { printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); g_fail++; } \
        else         { printf("ok:   %s\n", msg); }                          \
    } while (0)

/* 1.2.3.4 -> 0x01020304 */
static uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

static void test_subnet(void)
{
    poom_net_scanner_subnet_t s;

    /* /24 */
    CHECK(poom_net_scanner_subnet_compute(ipv4(192,168,1,50), ipv4(255,255,255,0), ipv4(192,168,1,1), &s) == ESP_OK, "/24 computes");
    CHECK(s.prefix_len == 24, "/24 prefix");
    CHECK(s.network == ipv4(192,168,1,0), "/24 network");
    CHECK(s.broadcast == ipv4(192,168,1,255), "/24 broadcast");
    CHECK(s.first_host == ipv4(192,168,1,1), "/24 first host");
    CHECK(s.last_host == ipv4(192,168,1,254), "/24 last host");
    CHECK(s.host_count == 254, "/24 host count");
    CHECK(!s.clamped, "/24 not clamped");

    /* /30 -> 2 usable */
    CHECK(poom_net_scanner_subnet_compute(ipv4(10,0,0,1), ipv4(255,255,255,252), 0, &s) == ESP_OK, "/30 computes");
    CHECK(s.prefix_len == 30, "/30 prefix");
    CHECK(s.host_count == 2, "/30 host count");

    /* /32 -> single address */
    CHECK(poom_net_scanner_subnet_compute(ipv4(10,0,0,7), ipv4(255,255,255,255), 0, &s) == ESP_OK, "/32 computes");
    CHECK(s.prefix_len == 32, "/32 prefix");
    CHECK(s.first_host == s.last_host && s.first_host == ipv4(10,0,0,7), "/32 single host");
    CHECK(s.host_count == 1, "/32 host count");

    /* /16 clamped to MAX_SWEEP_HOSTS */
    CHECK(poom_net_scanner_subnet_compute(ipv4(172,16,5,9), ipv4(255,255,0,0), 0, &s) == ESP_OK, "/16 computes");
    CHECK(s.clamped, "/16 clamped");
    CHECK(s.host_count == POOM_NET_SCANNER_MAX_SWEEP_HOSTS, "/16 clamp count");
    CHECK(s.first_host == ipv4(172,16,0,1), "/16 first host");
    CHECK(s.last_host == s.first_host + POOM_NET_SCANNER_MAX_SWEEP_HOSTS - 1, "/16 clamped last host");

    /* non-contiguous mask rejected */
    CHECK(poom_net_scanner_subnet_compute(ipv4(1,2,3,4), ipv4(255,0,255,0), 0, &s) == ESP_ERR_INVALID_ARG, "non-contiguous mask rejected");

    /* ip_to_str */
    char buf[POOM_NET_SCANNER_IP_STR_CAP];
    poom_net_scanner_ip_to_str(ipv4(192,168,1,254), buf, sizeof(buf));
    CHECK(strcmp(buf, "192.168.1.254") == 0, "ip_to_str");
}

static int contains(const uint16_t *a, size_t n, uint16_t v)
{
    for (size_t i = 0; i < n; ++i) if (a[i] == v) return 1;
    return 0;
}
static int has_dupes(const uint16_t *a, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (a[i] == a[j]) return 1;
    return 0;
}

static void test_ports(void)
{
    static uint16_t buf[POOM_NET_SCANNER_MAX_PORTS];

    size_t nq = poom_net_scanner_build_ports(POOM_NET_SCANNER_PROFILE_QUICK, buf, POOM_NET_SCANNER_MAX_PORTS);
    CHECK(nq > 10 && nq < 64, "quick has a sane count");
    CHECK(contains(buf, nq, 22) && contains(buf, nq, 80) && contains(buf, nq, 443), "quick has 22/80/443");
    CHECK(!has_dupes(buf, nq), "quick de-duped");

    size_t ne = poom_net_scanner_build_ports(POOM_NET_SCANNER_PROFILE_EXTENDED, buf, POOM_NET_SCANNER_MAX_PORTS);
    CHECK(ne >= 1000, "extended >= 1000 ports");
    CHECK(contains(buf, ne, 1) && contains(buf, ne, 1023), "extended covers well-known range");
    CHECK(contains(buf, ne, 3306) && contains(buf, ne, 8080) && contains(buf, ne, 32400), "extended has common high ports");
    CHECK(!has_dupes(buf, ne), "extended de-duped");

    size_t nf = poom_net_scanner_build_ports(POOM_NET_SCANNER_PROFILE_FULL, buf, POOM_NET_SCANNER_MAX_PORTS);
    CHECK(nf == POOM_NET_SCANNER_MAX_PORTS, "full fills to cap");

    /* custom returns 0 from builder */
    CHECK(poom_net_scanner_build_ports(POOM_NET_SCANNER_PROFILE_CUSTOM, buf, 16) == 0, "custom builder returns 0");
}

static void test_parse(void)
{
    uint16_t buf[64];
    size_t n;

    n = poom_net_scanner_parse_ports("22,80,443", buf, 64);
    CHECK(n == 3 && buf[0] == 22 && buf[1] == 80 && buf[2] == 443, "parse list");

    n = poom_net_scanner_parse_ports(" 8000 - 8003 , 22 ", buf, 64);
    CHECK(n == 5 && buf[0] == 8000 && buf[3] == 8003 && buf[4] == 22, "parse range+ws");

    n = poom_net_scanner_parse_ports("443-440", buf, 64); /* reversed */
    CHECK(n == 4 && buf[0] == 440 && buf[3] == 443, "parse reversed range");

    n = poom_net_scanner_parse_ports("22 80 443", buf, 64); /* keyboard has no comma */
    CHECK(n == 3 && buf[0] == 22 && buf[1] == 80 && buf[2] == 443, "parse space-separated");

    n = poom_net_scanner_parse_ports("22 8000-8002 443", buf, 64); /* mixed */
    CHECK(n == 5 && buf[0] == 22 && buf[1] == 8000 && buf[3] == 8002 && buf[4] == 443, "parse mixed space+range");

    n = poom_net_scanner_parse_ports("80x", buf, 64);
    CHECK(n == 0, "parse junk rejected");

    n = poom_net_scanner_parse_ports("70000", buf, 64);
    CHECK(n == 0, "parse out-of-range rejected");

    n = poom_net_scanner_parse_ports("0,22", buf, 64); /* port 0 skipped */
    CHECK(n == 1 && buf[0] == 22, "parse skips port 0");
}

static void test_services(void)
{
    CHECK(strcmp(poom_net_scanner_service_name(22), "ssh") == 0, "svc 22=ssh");
    CHECK(strcmp(poom_net_scanner_service_name(443), "https") == 0, "svc 443=https");
    CHECK(strcmp(poom_net_scanner_service_name(3306), "mysql") == 0, "svc 3306=mysql");
    CHECK(strcmp(poom_net_scanner_service_name(12345), "unknown") == 0, "svc unknown");
    CHECK(poom_net_scanner_service_name(80)[0] != ' ', "svc name has no leading space");
    CHECK(strcmp(poom_net_scanner_state_label(POOM_NET_SCANNER_PORT_OPEN), "open") == 0, "state open");
    CHECK(strcmp(poom_net_scanner_method_label(POOM_NET_SCANNER_METHOD_TCP_CONNECT), "TCP Connect") == 0, "method connect");
    CHECK(!poom_net_scanner_syn_available(), "syn not available by default");
}

static void test_banner(void)
{
    poom_net_scanner_service_t s;

    const char *ssh = "SSH-2.0-OpenSSH_9.6p1 Ubuntu-3ubuntu13.4\r\n";
    poom_net_scanner_parse_banner(ssh, strlen(ssh), 22, &s);
    CHECK(strcmp(s.service, "ssh") == 0, "ssh service");
    CHECK(strcmp(s.product, "OpenSSH") == 0, "ssh product");
    CHECK(strcmp(s.version, "9.6p1") == 0, "ssh version");
    CHECK(s.evidence == POOM_NET_SCANNER_EV_VERSION_REPORTED, "ssh evidence=version");

    const char *ftp = "220 ProFTPD Server (Debian)\r\n";
    poom_net_scanner_parse_banner(ftp, strlen(ftp), 21, &s);
    CHECK(strcmp(s.service, "ftp") == 0, "ftp service");
    CHECK(s.evidence == POOM_NET_SCANNER_EV_PROTO_RESPONSE, "ftp confirmed");

    const char *smtp = "220 mx.example.com ESMTP Postfix\r\n";
    poom_net_scanner_parse_banner(smtp, strlen(smtp), 25, &s);
    CHECK(strcmp(s.service, "smtp") == 0, "smtp service");

    const char *pop = "+OK Dovecot ready.\r\n";
    poom_net_scanner_parse_banner(pop, strlen(pop), 110, &s);
    CHECK(strcmp(s.service, "pop3") == 0, "pop3 service");

    const char *imap = "* OK [CAPABILITY IMAP4rev1] Dovecot ready\r\n";
    poom_net_scanner_parse_banner(imap, strlen(imap), 143, &s);
    CHECK(strcmp(s.service, "imap") == 0, "imap service");

    /* empty / no greeting -> port guess */
    poom_net_scanner_parse_banner("", 0, 3306, &s);
    CHECK(strcmp(s.service, "mysql") == 0 && s.evidence == POOM_NET_SCANNER_EV_PORT_GUESS, "empty -> port guess");

    /* binary junk -> port guess, no false protocol claim */
    const char junk[] = {0x00, 0x01, 0x02, (char)0xff, 0x03};
    poom_net_scanner_parse_banner(junk, sizeof(junk), 1234, &s);
    CHECK(s.evidence == POOM_NET_SCANNER_EV_PORT_GUESS, "junk -> port guess");
}

static void test_http(void)
{
    poom_net_scanner_service_t s;

    const char *nginx = "HTTP/1.1 200 OK\r\nDate: x\r\nServer: nginx/1.24.0\r\nContent-Type: text/html\r\n\r\n";
    CHECK(poom_net_scanner_parse_http(nginx, strlen(nginx), &s), "http detected");
    CHECK(strcmp(s.service, "http") == 0, "http service");
    CHECK(strcmp(s.product, "nginx") == 0, "http product nginx");
    CHECK(strcmp(s.version, "1.24.0") == 0, "http version");
    CHECK(s.evidence == POOM_NET_SCANNER_EV_VERSION_REPORTED, "http evidence=version");

    const char *apache = "HTTP/1.1 404 Not Found\r\nServer: Apache/2.4.58 (Ubuntu)\r\n\r\n";
    CHECK(poom_net_scanner_parse_http(apache, strlen(apache), &s), "apache detected");
    CHECK(strcmp(s.product, "Apache") == 0 && strcmp(s.version, "2.4.58") == 0, "apache product/version");

    const char *noserver = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
    CHECK(poom_net_scanner_parse_http(noserver, strlen(noserver), &s), "http no-server detected");
    CHECK(strcmp(s.service, "http") == 0 && s.version[0] == '\0', "http no version");
    CHECK(s.evidence == POOM_NET_SCANNER_EV_PROTO_RESPONSE, "http no-server confirmed only");

    const char *nothttp = "220 some ftp\r\n";
    CHECK(!poom_net_scanner_parse_http(nothttp, strlen(nothttp), &s), "non-http rejected");

    CHECK(strcmp(poom_net_scanner_evidence_label(POOM_NET_SCANNER_EV_VERSION_REPORTED), "version") == 0, "evidence label");
}

static void test_svc_id(void)
{
    uint8_t id = poom_net_scanner_service_id("http");
    CHECK(id != 0 && strcmp(poom_net_scanner_service_by_id(id), "http") == 0, "svc id roundtrip http");
    CHECK(poom_net_scanner_service_id("SSH") == poom_net_scanner_service_id("ssh"), "svc id case-insensitive");
    CHECK(poom_net_scanner_service_id("nope") == 0, "svc id unknown=0");
    CHECK(strcmp(poom_net_scanner_service_by_id(0), "") == 0, "svc by id 0 empty");
    CHECK(strcmp(poom_net_scanner_service_by_id(250), "") == 0, "svc by id oob empty");
}

static void test_os_hint(void)
{
    char buf[24];
    bool strong;
    poom_net_scanner_service_t s;

    /* banner OS token -> strong */
    const char *ssh = "SSH-2.0-OpenSSH_9.6p1 Ubuntu-3ubuntu13.4";
    poom_net_scanner_parse_banner(ssh, strlen(ssh), 22, &s);
    poom_net_scanner_os_hint(&s, 0, false, NULL, buf, sizeof(buf), &strong);
    CHECK(strcmp(buf, "Linux/Ubuntu") == 0 && strong, "os from banner ubuntu (strong)");

    /* TTL family (inferred) */
    poom_net_scanner_os_hint(NULL, 64, true, NULL, buf, sizeof(buf), &strong);
    CHECK(strcmp(buf, "Linux/Unix?") == 0 && !strong, "os ttl 64 -> linux");
    poom_net_scanner_os_hint(NULL, 128, true, NULL, buf, sizeof(buf), &strong);
    CHECK(strcmp(buf, "Windows?") == 0, "os ttl 128 -> windows");
    poom_net_scanner_os_hint(NULL, 250, true, NULL, buf, sizeof(buf), &strong);
    CHECK(strcmp(buf, "Net device?") == 0, "os ttl 255 -> net device");

    /* vendor (inferred) */
    poom_net_scanner_os_hint(NULL, 0, false, "Apple, Inc.", buf, sizeof(buf), &strong);
    CHECK(strcmp(buf, "Apple?") == 0 && !strong, "os vendor apple");

    /* nothing */
    poom_net_scanner_os_hint(NULL, 0, false, NULL, buf, sizeof(buf), &strong);
    CHECK(buf[0] == '\0', "os nothing -> empty");

    /* banner beats ttl */
    poom_net_scanner_os_hint(&s, 128, true, NULL, buf, sizeof(buf), &strong);
    CHECK(strcmp(buf, "Linux/Ubuntu") == 0 && strong, "banner overrides ttl");
}

static void test_estimate(void)
{
    char b[12];
    /* 20 ports, fanout 8 -> 3 batches x 600 ms = 1.8 s -> 2 s per host */
    CHECK(poom_net_scanner_estimate_scan_s(1, 20, 8, 600) == 2, "est 1 host quick");
    CHECK(poom_net_scanner_estimate_scan_s(10, 20, 0, 0) == 18, "est defaults (8, 600ms)");
    CHECK(poom_net_scanner_estimate_scan_s(0, 20, 8, 600) == 0, "est 0 hosts");
    CHECK(poom_net_scanner_estimate_scan_s(1, 0, 8, 600) == 0, "est 0 ports");
    CHECK(poom_net_scanner_estimate_scan_s(1, 65535, 8, 600) == 4916, "est full range 1 host");
    CHECK(poom_net_scanner_estimate_scan_s(1, 100, 99, 1000) == 9, "est fanout clamped to max 12");
    CHECK(poom_net_scanner_estimate_scan_s(0xFFFFFFFFu, 65535, 1, 0xFFFFFFFFu) == 0xFFFFFFFFu,
          "est saturates");

    poom_net_scanner_fmt_duration(45, b, sizeof(b));   CHECK(strcmp(b, "45s") == 0, "fmt 45s");
    poom_net_scanner_fmt_duration(60, b, sizeof(b));   CHECK(strcmp(b, "1m") == 0, "fmt 1m");
    poom_net_scanner_fmt_duration(61, b, sizeof(b));   CHECK(strcmp(b, "2m") == 0, "fmt rounds up");
    poom_net_scanner_fmt_duration(3600, b, sizeof(b)); CHECK(strcmp(b, "1h00m") == 0, "fmt 1h00m");
    poom_net_scanner_fmt_duration(4916, b, sizeof(b)); CHECK(strcmp(b, "1h22m") == 0, "fmt 1h22m");
    poom_net_scanner_fmt_duration(400000, b, sizeof(b)); CHECK(strcmp(b, ">99h") == 0, "fmt >99h");
    poom_net_scanner_fmt_duration(45, b, 2);           CHECK(strlen(b) == 1, "fmt truncates safely");
}

static void test_wrap(void)
{
    char o[24];
    const char *r;
    r = poom_net_scanner_wrap("short", 20, o, sizeof(o));
    CHECK(strcmp(o, "short") == 0 && r == NULL, "wrap fits in one line");
    r = poom_net_scanner_wrap("Apache HTTP Server 2.4.49 - Path Traversal", 20, o, sizeof(o));
    CHECK(strcmp(o, "Apache HTTP Server") == 0, "wrap breaks at last space");
    r = poom_net_scanner_wrap(r, 20, o, sizeof(o));
    CHECK(strcmp(o, "2.4.49 - Path") == 0, "wrap line 2");
    r = poom_net_scanner_wrap(r, 20, o, sizeof(o));
    CHECK(strcmp(o, "Traversal") == 0 && r == NULL, "wrap last line");
    r = poom_net_scanner_wrap("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 20, o, sizeof(o));
    CHECK(strcmp(o, "ABCDEFGHIJKLMNOPQRST") == 0 && r != NULL && strcmp(r, "UVWXYZ") == 0,
          "wrap hard-breaks a long word");
    r = poom_net_scanner_wrap("exactly twenty chars one", 20, o, sizeof(o));
    CHECK(strcmp(o, "exactly twenty chars") == 0 && r != NULL && strcmp(r, "one") == 0,
          "wrap: space right after a full line");
    r = poom_net_scanner_wrap("   lead", 20, o, sizeof(o));
    CHECK(strcmp(o, "lead") == 0 && r == NULL, "wrap skips leading spaces");
    r = poom_net_scanner_wrap("", 20, o, sizeof(o));
    CHECK(o[0] == '\0' && r == NULL, "wrap empty");
    r = poom_net_scanner_wrap("hello world", 20, o, 4);
    CHECK(strcmp(o, "hel") == 0 && r != NULL, "wrap bounded by cap");
    r = poom_net_scanner_wrap(NULL, 20, o, sizeof(o));
    CHECK(o[0] == '\0' && r == NULL, "wrap NULL");
}

static void test_port_window(void)
{
    static uint16_t full[2048], win[2048], chunk[100];
    const poom_net_scanner_profile_t profs[2] = {POOM_NET_SCANNER_PROFILE_QUICK,
                                                 POOM_NET_SCANNER_PROFILE_EXTENDED};
    for (int k = 0; k < 2; ++k)
    {
        size_t nf = poom_net_scanner_build_ports(profs[k], full, 2048);
        size_t nw = 0;
        uint32_t off = 0;
        for (;;)
        {
            size_t got = poom_net_scanner_build_ports_window(profs[k], off, chunk, 100);
            if (got == 0) { break; }
            memcpy(win + nw, chunk, got * sizeof(uint16_t));
            nw += got;
            off += (uint32_t)got;
        }
        CHECK(nw == nf && memcmp(full, win, nf * sizeof(uint16_t)) == 0,
              k == 0 ? "window concat == Quick list" : "window concat == Extended list");
    }
    size_t g = poom_net_scanner_build_ports_window(POOM_NET_SCANNER_PROFILE_FULL, 65530, chunk, 100);
    CHECK(g == 5 && chunk[0] == 65531 && chunk[4] == 65535, "Full window tail");
    CHECK(poom_net_scanner_build_ports_window(POOM_NET_SCANNER_PROFILE_FULL, 65535, chunk, 100) == 0,
          "Full window past end");
    CHECK(poom_net_scanner_build_ports_window(POOM_NET_SCANNER_PROFILE_CUSTOM, 0, chunk, 100) == 0,
          "Custom has no window");
}

static void test_products(void)
{
    poom_net_scanner_service_t o;
    const char *ftp = "220 (vsFTPd 3.0.3)\r\n";
    poom_net_scanner_parse_banner(ftp, strlen(ftp), 21, &o);
    CHECK(strcmp(o.service, "ftp") == 0 && strcmp(o.product, "vsftpd") == 0 &&
          strcmp(o.version, "3.0.3") == 0 && o.evidence == POOM_NET_SCANNER_EV_VERSION_REPORTED,
          "vsFTPd product+version");
    const char *smtp = "220 mail.example.com ESMTP Exim 4.92.3 Ubuntu\r\n";
    poom_net_scanner_parse_banner(smtp, strlen(smtp), 25, &o);
    CHECK(strcmp(o.service, "smtp") == 0 && strcmp(o.product, "Exim") == 0 &&
          strcmp(o.version, "4.92.3") == 0, "Exim product+version");
    const char *pf = "220 host ESMTP Postfix (Debian/GNU)\r\n";
    poom_net_scanner_parse_banner(pf, strlen(pf), 25, &o);
    CHECK(strcmp(o.product, "Postfix") == 0 && o.version[0] == '\0', "Postfix without version");
    const char *imap = "* OK [CAPABILITY IMAP4rev1] Dovecot ready.\r\n";
    poom_net_scanner_parse_banner(imap, strlen(imap), 143, &o);
    CHECK(strcmp(o.service, "imap") == 0 && strcmp(o.product, "Dovecot") == 0, "Dovecot imap");
    const char *pro = "220 ProFTPD 1.3.5e Server (Debian) [::ffff:10.0.0.5]\r\n";
    poom_net_scanner_parse_banner(pro, strlen(pro), 2121, &o);
    CHECK(strcmp(o.service, "ftp") == 0 && strcmp(o.product, "ProFTPD") == 0 &&
          strcmp(o.version, "1.3.5e") == 0, "ProFTPD on odd port");

    const unsigned char my[] = {0x4a,0,0,0,0x0a,'5','.','7','.','3','3','-','0','u','b','u','n','t','u',0,1,2};
    poom_net_scanner_parse_banner((const char *)my, sizeof(my), 3306, &o);
    CHECK(strcmp(o.service, "mysql") == 0 && strcmp(o.product, "MySQL") == 0 &&
          strcmp(o.version, "5.7.33") == 0 && strstr(o.banner, "ubuntu") != NULL, "MySQL handshake");
    const char mdb[] = "\x5b\x00\x00\x00\x0a" "5.5.5-10.3.29-MariaDB-0+deb10u1";
    poom_net_scanner_parse_banner(mdb, sizeof(mdb), 3306, &o);
    CHECK(strcmp(o.product, "MariaDB") == 0 && strcmp(o.version, "10.3.29") == 0, "MariaDB handshake");
    const unsigned char tn[] = {0xFF, 0xFD, 0x18, 0xFF, 0xFD, 0x20};
    poom_net_scanner_parse_banner((const char *)tn, sizeof(tn), 23, &o);
    CHECK(strcmp(o.service, "telnet") == 0 && o.evidence == POOM_NET_SCANNER_EV_PROTO_RESPONSE, "telnet IAC");
    const char *vnc = "RFB 003.008\n";
    poom_net_scanner_parse_banner(vnc, strlen(vnc), 5900, &o);
    CHECK(strcmp(o.service, "vnc") == 0, "VNC RFB");

    const char *h1 = "HTTP/1.1 401 Unauthorized\r\nServer: httpd\r\n"
                     "WWW-Authenticate: Basic realm=\"NETGEAR R7000\"\r\n\r\n";
    memset(&o, 0, sizeof(o));
    CHECK(poom_net_scanner_parse_http(h1, strlen(h1), &o) && strcmp(o.product, "R7000") == 0,
          "HTTP realm -> model R7000");
    const char *h2 = "HTTP/1.1 200 OK\r\nServer: lighttpd/1.4.35\r\n\r\n<html><title>RT-AC68U</title>";
    memset(&o, 0, sizeof(o));
    CHECK(poom_net_scanner_parse_http(h2, strlen(h2), &o) && strcmp(o.product, "lighttpd") == 0 &&
          strcmp(o.version, "1.4.35") == 0, "versioned Server header kept over title");
    const char *h3 = "HTTP/1.1 200 OK\r\nServer: lighttpd\r\n\r\n<html><head><title>ASUS RT-AC68U</title>";
    memset(&o, 0, sizeof(o));
    CHECK(poom_net_scanner_parse_http(h3, strlen(h3), &o) && strcmp(o.product, "RT-AC68U") == 0,
          "title model beats version-less Server");
    const char *h4 = "HTTP/1.1 200 OK\r\nX-Powered-By: PHP/5.6.40\r\n\r\n<title>Login</title>";
    memset(&o, 0, sizeof(o));
    CHECK(poom_net_scanner_parse_http(h4, strlen(h4), &o) && strcmp(o.product, "PHP") == 0 &&
          strcmp(o.version, "5.6.40") == 0, "X-Powered-By fallback; generic title ignored");

    char m[24];
    CHECK(poom_net_scanner_model_token("Synology DiskStation DS918+", m, sizeof(m)) && strcmp(m, "DS918+") == 0, "model DS918+");
    CHECK(poom_net_scanner_model_token("Router v1.2.3 build 1.0", m, sizeof(m)) == 0, "versions are not models");
    CHECK(poom_net_scanner_model_token("WRT54GL", m, sizeof(m)) && strcmp(m, "WRT54GL") == 0, "model WRT54GL");
    CHECK(poom_net_scanner_model_token("Login", m, sizeof(m)) == 0 && m[0] == '\0', "no model in plain word");
}

int main(void)
{
    test_products();
    test_port_window();
    test_wrap();
    test_estimate();
    test_subnet();
    test_ports();
    test_parse();
    test_services();
    test_banner();
    test_http();
    test_svc_id();
    test_os_hint();
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
