// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Pure, hardware-independent logic for the security audit: subnet math,
// port-set construction, "ports" string parsing, and the service-name table.
// Deliberately free of lwIP / esp_netif / FreeRTOS so it host-compiles and is
// unit-tested in test/test_logic.c. The networked parts live in
// poom_security_audit.c and call into these.

#include "poom_security_audit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- subnet math -------------------------------------------------------- */

/* A valid IPv4 mask is a run of 1s followed by a run of 0s (host byte order).
 * Reject anything non-contiguous (e.g. 255.0.255.0). */
static bool security_audit_mask_is_contiguous_(uint32_t mask)
{
    /* ~mask + 1 must be a power of two (or mask == 0xFFFFFFFF). */
    uint32_t inv = ~mask;
    return (inv & (inv + 1U)) == 0U;
}

static uint8_t security_audit_prefix_from_mask_(uint32_t mask)
{
    uint8_t n = 0;
    for (int i = 31; i >= 0; --i)
    {
        if (mask & (1U << (unsigned)i))
        {
            n++;
        }
        else
        {
            break;
        }
    }
    return n;
}

esp_err_t poom_security_audit_subnet_compute(uint32_t ip_host, uint32_t netmask_host,
                                          uint32_t gw_host,
                                          poom_security_audit_subnet_t *out)
{
    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (netmask_host != 0U && !security_audit_mask_is_contiguous_(netmask_host))
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->ip         = ip_host;
    out->netmask    = netmask_host;
    out->gateway    = gw_host;
    out->prefix_len = security_audit_prefix_from_mask_(netmask_host);
    out->network    = ip_host & netmask_host;
    out->broadcast  = out->network | (~netmask_host);

    /* Usable host range. For /31 and /32 there is no network/broadcast pair to
     * exclude, so the single address(es) are treated as the range. */
    if (out->prefix_len >= 31U)
    {
        out->first_host = out->network;
        out->last_host  = out->broadcast; /* /32 => == network */
    }
    else
    {
        out->first_host = out->network + 1U;
        out->last_host  = out->broadcast - 1U;
    }

    uint32_t count = (out->last_host >= out->first_host)
                         ? (out->last_host - out->first_host + 1U)
                         : 0U;

    if (count > POOM_SECURITY_AUDIT_MAX_SWEEP_HOSTS)
    {
        count = POOM_SECURITY_AUDIT_MAX_SWEEP_HOSTS;
        out->last_host = out->first_host + count - 1U;
        out->clamped = true;
    }
    out->host_count = count;
    return ESP_OK;
}

void poom_security_audit_ip_to_str(uint32_t ip_host, char *out, size_t out_cap)
{
    if (out == NULL || out_cap == 0U)
    {
        return;
    }
    (void)snprintf(out, out_cap, "%u.%u.%u.%u",
                   (unsigned)((ip_host >> 24) & 0xFFU),
                   (unsigned)((ip_host >> 16) & 0xFFU),
                   (unsigned)((ip_host >> 8) & 0xFFU),
                   (unsigned)(ip_host & 0xFFU));
}

/* ---- port sets ---------------------------------------------------------- */

/* Quick: the ports worth checking first on a typical LAN host. */
static const uint16_t k_quick_ports[] = {
    21, 22, 23, 25, 53, 80, 110, 111, 135, 139, 143, 443, 445, 993, 995,
    1723, 3306, 3389, 5900, 8000, 8080, 8443, 139, 515, 631, 1883, 5000, 5060,
    5353, 8008, 8888, 9100, 32400,
};

/* Common registered/dynamic high ports to append to the well-known range so
 * the "Extended" set reaches ~top-1000 coverage of services that live above
 * 1024. De-duplicated against the 1..1023 range at build time. */
static const uint16_t k_common_high_ports[] = {
    1025, 1080, 1194, 1433, 1521, 1723, 1883, 1900, 2000, 2049, 2082, 2083,
    2086, 2087, 2095, 2096, 2181, 2222, 2375, 2376, 2483, 2484, 3000, 3128,
    3268, 3269, 3306, 3389, 3690, 4000, 4040, 4444, 4500, 4567, 4789, 5000,
    5001, 5060, 5061, 5222, 5269, 5353, 5432, 5555, 5601, 5672, 5900, 5901,
    5984, 6000, 6001, 6379, 6443, 6666, 6667, 7000, 7001, 7070, 7443, 7474,
    7547, 7777, 8000, 8008, 8009, 8080, 8081, 8086, 8088, 8089, 8123, 8181,
    8200, 8291, 8333, 8443, 8500, 8686, 8765, 8883, 8888, 9000, 9001, 9042,
    9090, 9091, 9100, 9200, 9300, 9418, 9443, 9999, 10000, 11211, 15672,
    16992, 16993, 17500, 18080, 20000, 25565, 27015, 27017, 28017, 32400,
    49152, 49153, 49154, 50000, 51820, 54321, 55443, 62078,
};

size_t poom_security_audit_build_ports(poom_security_audit_profile_t profile,
                                    uint16_t *out, size_t cap)
{
    if (out == NULL || cap == 0U)
    {
        return 0U;
    }

    switch (profile)
    {
    case POOM_SECURITY_AUDIT_PROFILE_QUICK:
    {
        size_t n = 0;
        for (size_t i = 0; i < (sizeof(k_quick_ports) / sizeof(k_quick_ports[0])) && n < cap; ++i)
        {
            /* skip duplicates already emitted (k_quick_ports has a couple) */
            bool dup = false;
            for (size_t j = 0; j < n; ++j)
            {
                if (out[j] == k_quick_ports[i]) { dup = true; break; }
            }
            if (!dup)
            {
                out[n++] = k_quick_ports[i];
            }
        }
        return n;
    }

    case POOM_SECURITY_AUDIT_PROFILE_EXTENDED:
    {
        size_t n = 0;
        /* Well-known range 1..1023. */
        for (uint32_t p = 1; p <= 1023U && n < cap; ++p)
        {
            out[n++] = (uint16_t)p;
        }
        /* Append common high ports not already in range. */
        for (size_t i = 0; i < (sizeof(k_common_high_ports) / sizeof(k_common_high_ports[0])) && n < cap; ++i)
        {
            uint16_t p = k_common_high_ports[i];
            if (p < 1024U)
            {
                continue; /* already covered by the range */
            }
            bool dup = false;
            for (size_t j = 1023; j < n; ++j) /* only the appended tail can dup */
            {
                if (out[j] == p) { dup = true; break; }
            }
            if (!dup)
            {
                out[n++] = p;
            }
        }
        return n;
    }

    case POOM_SECURITY_AUDIT_PROFILE_FULL:
    {
        size_t n = 0;
        for (uint32_t p = 1; p <= 65535U && n < cap; ++p)
        {
            out[n++] = (uint16_t)p;
        }
        return n;
    }

    case POOM_SECURITY_AUDIT_PROFILE_CUSTOM:
    default:
        return 0U; /* caller supplies ports directly */
    }
}

size_t poom_security_audit_parse_ports(const char *spec, uint16_t *out, size_t cap)
{
    if (spec == NULL || out == NULL || cap == 0U)
    {
        return 0U;
    }

    size_t n = 0;
    const char *p = spec;

    while (*p != '\0')
    {
        /* skip separators / whitespace */
        while (*p == ',' || isspace((unsigned char)*p))
        {
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        if (!isdigit((unsigned char)*p))
        {
            return 0U; /* malformed */
        }

        long a = 0;
        while (isdigit((unsigned char)*p))
        {
            a = a * 10 + (*p - '0');
            if (a > 65535L)
            {
                return 0U;
            }
            p++;
        }

        /* Look ahead past optional whitespace for a range dash ("8000 - 8003"),
         * but do NOT consume that whitespace unless a dash follows - otherwise
         * a space is a plain separator ("22 80 443"), which matters because the
         * on-screen keyboard has no comma key. */
        const char *q = p;
        while (isspace((unsigned char)*q))
        {
            q++;
        }

        long b = a;
        if (*q == '-')
        {
            p = q + 1;
            while (isspace((unsigned char)*p))
            {
                p++;
            }
            if (!isdigit((unsigned char)*p))
            {
                return 0U;
            }
            b = 0;
            while (isdigit((unsigned char)*p))
            {
                b = b * 10 + (*p - '0');
                if (b > 65535L)
                {
                    return 0U;
                }
                p++;
            }
        }

        if (b < a)
        {
            long t = a; a = b; b = t;
        }
        for (long v = a; v <= b && n < cap; ++v)
        {
            if (v == 0)
            {
                continue; /* port 0 is not scannable */
            }
            out[n++] = (uint16_t)v;
        }

        /* require a separator or end after a token/range */
        if (*p != '\0' && *p != ',' && !isspace((unsigned char)*p))
        {
            return 0U; /* junk like "80x" */
        }
    }
    return n;
}

/* ---- service table ------------------------------------------------------ */

typedef struct
{
    uint16_t    port;
    const char *name;
} security_audit_service_t;

/* Guess-from-port-number table. Not a verified banner. */
static const security_audit_service_t k_services[] = {
    {20, "ftp-data"}, {21, "ftp"}, {22, "ssh"}, {23, "telnet"}, {25, "smtp"},
    {53, "dns"}, {67, "dhcp"}, {68, "dhcp"}, {69, "tftp"}, {80, "http"},
    {110, "pop3"}, {111, "rpcbind"}, {123, "ntp"}, {135, "msrpc"},
    {137, "netbios"}, {138, "netbios"}, {139, "netbios-ssn"}, {143, "imap"},
    {161, "snmp"}, {162, "snmp"}, {179, "bgp"}, {389, "ldap"}, {427, "slp"},
    {443, "https"}, {445, "smb"}, {465, "smtps"}, {500, "isakmp"},
    {514, "syslog"}, {515, "printer"}, {520, "rip"}, {548, "afp"},
    {554, "rtsp"}, {587, "submission"}, {631, "ipp"}, {636, "ldaps"},
    {873, "rsync"}, {902, "vmware"}, {989, "ftps"}, {990, "ftps"},
    {993, "imaps"}, {995, "pop3s"}, {1080, "socks"}, {1194, "openvpn"},
    {1433, "mssql"}, {1521, "oracle"}, {1723, "pptp"}, {1883, "mqtt"},
    {1900, "ssdp"}, {2049, "nfs"}, {2082, "cpanel"}, {2083, "cpanel"},
    {2181, "zookeeper"}, {2222, "ssh-alt"}, {2375, "docker"}, {2376, "docker-tls"},
    {3000, "dev-http"}, {3128, "squid"}, {3268, "ldap-gc"}, {3306, "mysql"},
    {3389, "rdp"}, {3690, "svn"}, {4444, "metasploit"}, {4500, "ipsec-nat"},
    {5000, "upnp"}, {5060, "sip"}, {5061, "sips"}, {5222, "xmpp"},
    {5353, "mdns"}, {5432, "postgresql"}, {5555, "adb"}, {5601, "kibana"},
    {5672, "amqp"}, {5900, "vnc"}, {5984, "couchdb"}, {6379, "redis"},
    {6443, "kube-api"}, {6667, "irc"}, {7070, "rtsp-alt"}, {7474, "neo4j"},
    {7547, "tr069"}, {8000, "http-alt"}, {8008, "http-alt"}, {8009, "ajp"},
    {8080, "http-proxy"}, {8081, "http-alt"}, {8086, "influxdb"},
    {8088, "http-alt"}, {8123, "home-assistant"}, {8200, "gotify"},
    {8291, "mikrotik"}, {8443, "https-alt"}, {8500, "consul"}, {8883, "mqtt-tls"},
    {8888, "http-alt"}, {9000, "http-alt"}, {9042, "cassandra"},
    {9090, "prometheus"}, {9100, "jetdirect"}, {9200, "elasticsearch"},
    {9418, "git"}, {9443, "https-alt"}, {10000, "webmin"}, {11211, "memcached"},
    {15672, "rabbitmq-mgmt"}, {25565, "minecraft"}, {27017, "mongodb"},
    {32400, "plex"}, {49152, "upnp"}, {51820, "wireguard"}, {62078, "iphone-sync"},
};

const char *poom_security_audit_service_name(uint16_t port)
{
    for (size_t i = 0; i < (sizeof(k_services) / sizeof(k_services[0])); ++i)
    {
        if (k_services[i].port == port)
        {
            return k_services[i].name;
        }
    }
    return "unknown";
}

const char *poom_security_audit_state_label(poom_security_audit_port_state_t state)
{
    switch (state)
    {
    case POOM_SECURITY_AUDIT_PORT_OPEN:     return "open";
    case POOM_SECURITY_AUDIT_PORT_CLOSED:   return "closed";
    case POOM_SECURITY_AUDIT_PORT_FILTERED: return "filtered";
    case POOM_SECURITY_AUDIT_PORT_UNKNOWN:
    default:                             return "-";
    }
}

const char *poom_security_audit_method_label(poom_security_audit_method_t method)
{
    switch (method)
    {
    case POOM_SECURITY_AUDIT_METHOD_TCP_SYN:     return "TCP SYN";
    case POOM_SECURITY_AUDIT_METHOD_TCP_CONNECT:
    default:                                  return "TCP Connect";
    }
}

/* SYN availability is a compile-time feasibility gate. It stays 0 until a
 * raw-socket SYN scan is demonstrated to coexist reliably with the lwIP TCP
 * stack on this target (see Stage 6 / the handover doc). Defined here (pure)
 * so the UI and host tests can query it without the networked backend. */
#ifndef POOM_SECURITY_AUDIT_SYN_IMPL
#define POOM_SECURITY_AUDIT_SYN_IMPL (0)
#endif

bool poom_security_audit_syn_available(void)
{
    return (POOM_SECURITY_AUDIT_SYN_IMPL != 0);
}

/* ---- service / version parsing (pure, host-tested) ---------------------- */

const char *poom_security_audit_evidence_label(poom_security_audit_evidence_t ev)
{
    switch (ev)
    {
    case POOM_SECURITY_AUDIT_EV_VERSION_REPORTED: return "version";
    case POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE:   return "confirmed";
    case POOM_SECURITY_AUDIT_EV_PORT_GUESS:
    default:                                   return "guessed";
    }
}

/* Case-insensitive substring search within a length-bounded buffer. */
static const char *security_audit_memfind_ci_(const char *hay, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) { return NULL; }
    for (size_t i = 0; i + nlen <= hlen; ++i)
    {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j]))
        {
            j++;
        }
        if (j == nlen) { return hay + i; }
    }
    return NULL;
}

/* Copy the first line of src (printable ASCII only), trimmed, into dst. */
static void security_audit_snippet_(char *dst, size_t cap, const char *src, size_t len)
{
    size_t n = 0, i = 0;
    while (i < len && (src[i] == ' ' || src[i] == '\t')) { i++; }
    for (; i < len && n + 1 < cap; ++i)
    {
        char c = src[i];
        if (c == '\r' || c == '\n') { break; }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) { continue; }
        dst[n++] = c;
    }
    dst[n] = '\0';
    while (n > 0 && dst[n - 1] == ' ') { dst[--n] = '\0'; }
}

static void security_audit_svc_clear_(poom_security_audit_service_t *o, uint16_t port)
{
    memset(o, 0, sizeof(*o));
    o->port = port;
    o->evidence = POOM_SECURITY_AUDIT_EV_PORT_GUESS;
    (void)snprintf(o->service, sizeof(o->service), "%s", poom_security_audit_service_name(port));
}

/* Split a "product<sep>version" token (sep = '_' '/' or the first digit) into
 * out->product / out->version. */
static void security_audit_split_pv_(const char *tok, poom_security_audit_service_t *o)
{
    int sep = -1;
    size_t seplen = 0;
    for (size_t i = 0; tok[i] != '\0'; ++i)
    {
        if (tok[i] == '_' || tok[i] == '/') { sep = (int)i; seplen = 1; break; }
        if (isdigit((unsigned char)tok[i]))  { sep = (int)i; seplen = 0; break; }
    }
    if (sep < 0)
    {
        (void)snprintf(o->product, sizeof(o->product), "%s", tok);
        o->version[0] = '\0';
        return;
    }
    int plen = sep;
    while (plen > 0 && tok[plen - 1] == ' ') { plen--; }
    (void)snprintf(o->product, sizeof(o->product), "%.*s", plen, tok);
    const char *v = tok + sep + seplen;
    size_t n = 0;
    while (v[n] != '\0' && v[n] != ' ' && n + 1 < sizeof(o->version)) { n++; }
    (void)snprintf(o->version, sizeof(o->version), "%.*s", (int)n, v);
}


/* ---- product extraction helpers (pure) ------------------------------------ */

/* Known server software named in FTP/SMTP/POP3/IMAP greetings, mapped to the
 * spelling Exploit-DB titles use. Longest/most specific first. */
static const struct { const char *tok; const char *product; } k_banner_products[] = {
    {"vsftpd", "vsftpd"},          {"proftpd", "ProFTPD"},     {"pure-ftpd", "Pure-FTPd"},
    {"filezilla", "FileZilla"},    {"microsoft ftp", "Microsoft FTP"},
    {"postfix", "Postfix"},        {"exim", "Exim"},           {"sendmail", "Sendmail"},
    {"opensmtpd", "OpenSMTPD"},    {"microsoft esmtp", "Microsoft Exchange"},
    {"dovecot", "Dovecot"},        {"courier", "Courier"},     {"cyrus", "Cyrus"},
    {"mikrotik", "MikroTik"},
};

/* Copy a version-like token ([0-9][0-9A-Za-z.-]*) found within a few chars
 * after @p p into @p out. */
static void security_audit_version_after_(const char *p, char *out, size_t cap)
{
    out[0] = '\0';
    for (int skip = 0; skip < 4 && *p != '\0' && !isdigit((unsigned char)*p); ++skip) { p++; }
    if (!isdigit((unsigned char)*p)) { return; }
    size_t n = 0;
    while (p[n] != '\0' && (isalnum((unsigned char)p[n]) || p[n] == '.' || p[n] == '-') &&
           n + 1 < cap)
    {
        n++;
    }
    while (n > 0 && (p[n - 1] == '.' || p[n - 1] == '-')) { n--; }
    memcpy(out, p, n);
    out[n] = '\0';
}

static void security_audit_banner_product_(const char *s, poom_security_audit_service_t *out)
{
    size_t sl = strlen(s);
    for (size_t i = 0; i < sizeof(k_banner_products) / sizeof(k_banner_products[0]); ++i)
    {
        const char *hit = security_audit_memfind_ci_(s, sl, k_banner_products[i].tok);
        if (hit == NULL) { continue; }
        (void)snprintf(out->product, sizeof(out->product), "%s", k_banner_products[i].product);
        security_audit_version_after_(hit + strlen(k_banner_products[i].tok), out->version,
                                   sizeof(out->version));
        if (out->version[0] != '\0') { out->evidence = POOM_SECURITY_AUDIT_EV_VERSION_REPORTED; }
        return;
    }
}

size_t poom_security_audit_model_token(const char *text, char *out, size_t cap)
{
    if (out == NULL || cap == 0U) { return 0U; }
    out[0] = '\0';
    if (text == NULL) { return 0U; }
    const char *p = text;
    while (*p != '\0')
    {
        while (*p != '\0' && !(isalnum((unsigned char)*p))) { p++; }
        const char *b = p;
        size_t n = 0;
        bool alpha = false, digit = false, dot = false;
        while (p[n] != '\0' && (isalnum((unsigned char)p[n]) || p[n] == '-' || p[n] == '+' ||
                                 p[n] == '.' || p[n] == '_'))
        {
            if (isalpha((unsigned char)p[n])) { alpha = true; }
            if (isdigit((unsigned char)p[n])) { digit = true; }
            if (p[n] == '.') { dot = true; }
            n++;
        }
        p += n;
        while (n > 0 && (b[n - 1] == '.' || b[n - 1] == '-' || b[n - 1] == '_')) { n--; }
        /* a model looks like R7000, RT-AC68U, DS918+, WRT54GL - not "1.0",
         * "v2.1.3" or "HTTP" */
        bool version_like = isdigit((unsigned char)b[0]) || ((b[0] == 'v' || b[0] == 'V') && dot);
        if (alpha && digit && !version_like && n >= 3U && n <= 20U)
        {
            if (n > cap - 1U) { n = cap - 1U; }
            memcpy(out, b, n);
            out[n] = '\0';
            return n;
        }
    }
    return 0U;
}

void poom_security_audit_parse_banner(const char *banner, size_t len, uint16_t port,
                                   poom_security_audit_service_t *out)
{
    if (out == NULL) { return; }
    security_audit_svc_clear_(out, port);
    if (banner == NULL || len == 0) { return; }

    /* Binary greetings first (the printable snippet would lose them). */
    const unsigned char *ub = (const unsigned char *)banner;
    if (ub[0] == 0xFFU) /* Telnet option negotiation (IAC) */
    {
        (void)snprintf(out->service, sizeof(out->service), "telnet");
        out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
        security_audit_snippet_(out->banner, sizeof(out->banner), banner, len);
        return;
    }
    if (len > 6U && ub[3] == 0x00U && ub[4] == 0x0AU) /* MySQL / MariaDB handshake v10 */
    {
        char ver[48];
        size_t n = 0;
        for (size_t i = 5; i < len && ub[i] != 0U && n + 1 < sizeof(ver); ++i)
        {
            if (ub[i] >= 0x20U && ub[i] <= 0x7EU) { ver[n++] = (char)ub[i]; }
        }
        ver[n] = '\0';
        if (n > 0U && isdigit((unsigned char)ver[0]))
        {
            bool maria = security_audit_memfind_ci_(ver, n, "mariadb") != NULL;
            const char *v = ver;
            if (maria && strncmp(v, "5.5.5-", 6) == 0) { v += 6; } /* compat prefix */
            (void)snprintf(out->service, sizeof(out->service), "mysql");
            (void)snprintf(out->product, sizeof(out->product), "%s", maria ? "MariaDB" : "MySQL");
            size_t vn = 0;
            while (v[vn] != '\0' && v[vn] != '-' && vn + 1 < sizeof(out->version)) { vn++; }
            (void)snprintf(out->version, sizeof(out->version), "%.*s", (int)vn, v);
            (void)snprintf(out->banner, sizeof(out->banner), "%s", ver); /* keeps "ubuntu" etc. */
            out->evidence = (out->version[0] != '\0') ? POOM_SECURITY_AUDIT_EV_VERSION_REPORTED
                                                       : POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
            return;
        }
    }

    security_audit_snippet_(out->banner, sizeof(out->banner), banner, len);
    const char *s = out->banner;
    size_t sl = strlen(s);
    if (sl == 0) { return; } /* no printable greeting -> PORT_GUESS */

    if (sl >= 4 && strncmp(s, "RFB ", 4) == 0)
    {
        (void)snprintf(out->service, sizeof(out->service), "vnc");
        out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
        return;
    }

    if (sl >= 4 && strncmp(s, "SSH-", 4) == 0)
    {
        (void)snprintf(out->service, sizeof(out->service), "ssh");
        out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
        const char *d1 = strchr(s + 4, '-'); /* SSH-<proto>-<software> */
        if (d1 != NULL)
        {
            security_audit_split_pv_(d1 + 1, out);
            if (out->version[0] != '\0') { out->evidence = POOM_SECURITY_AUDIT_EV_VERSION_REPORTED; }
        }
        return;
    }
    if (strncmp(s, "+OK", 3) == 0)
    {
        (void)snprintf(out->service, sizeof(out->service), "pop3");
        out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
        security_audit_banner_product_(s, out);
        return;
    }
    if (strncmp(s, "* OK", 4) == 0 || strncmp(s, "* PREAUTH", 9) == 0)
    {
        (void)snprintf(out->service, sizeof(out->service), "imap");
        out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
        security_audit_banner_product_(s, out);
        return;
    }
    if (strncmp(s, "220", 3) == 0) /* SMTP and FTP both greet with 220 */
    {
        bool is_smtp = security_audit_memfind_ci_(s, sl, "smtp") != NULL;
        bool is_ftp  = security_audit_memfind_ci_(s, sl, "ftp") != NULL;
        if (is_smtp)      { (void)snprintf(out->service, sizeof(out->service), "smtp"); out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE; }
        else if (is_ftp)  { (void)snprintf(out->service, sizeof(out->service), "ftp");  out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE; }
        else if (port == 25 || port == 587 || port == 465) { (void)snprintf(out->service, sizeof(out->service), "smtp"); }
        else if (port == 21) { (void)snprintf(out->service, sizeof(out->service), "ftp"); }
        /* else: ambiguous -> keep port-guess service + the banner snippet */
        security_audit_banner_product_(s, out);
        if (out->product[0] != '\0' && out->evidence < POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE)
        {
            out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE; /* named software answered */
        }
        return;
    }
    /* Unrecognised greeting: keep the port-guess service but retain the snippet
     * as raw evidence; do NOT claim PROTO_RESPONSE for an unnamed protocol. */
}

/* ---- OS / device hint + compact service ids (pure, host-tested) --------- */

/* Compact service-id table: id = index + 1; 0 = unknown. Order is stable
 * (append only) so stored ids remain valid across builds. */
static const char *const k_service_ids[] = {
    "http", "https", "ssh", "ftp", "smtp", "pop3", "imap", "dns", "telnet",
    "rdp", "vnc", "smb", "mysql", "postgresql", "redis", "mongodb", "mqtt",
    "snmp", "ntp", "sip", "rtsp", "upnp", "mdns", "printer",
};

uint8_t poom_security_audit_service_id(const char *name)
{
    if (name == NULL || name[0] == '\0') { return 0; }
    for (size_t i = 0; i < sizeof(k_service_ids) / sizeof(k_service_ids[0]); ++i)
    {
        size_t j = 0;
        while (name[j] != '\0' && k_service_ids[i][j] != '\0' &&
               tolower((unsigned char)name[j]) == (unsigned char)k_service_ids[i][j])
        {
            j++;
        }
        if (name[j] == '\0' && k_service_ids[i][j] == '\0') { return (uint8_t)(i + 1); }
    }
    return 0;
}

const char *poom_security_audit_service_by_id(uint8_t id)
{
    if (id == 0 || id > (sizeof(k_service_ids) / sizeof(k_service_ids[0]))) { return ""; }
    return k_service_ids[id - 1];
}

/* OS tokens we trust from a banner (case-insensitive substring -> family). */
typedef struct { const char *token; const char *family; } security_audit_os_token_t;
static const security_audit_os_token_t k_os_tokens[] = {
    {"ubuntu", "Linux/Ubuntu"}, {"debian", "Linux/Debian"}, {"raspbian", "Linux/RPi"},
    {"centos", "Linux/CentOS"}, {"red hat", "Linux/RHEL"},  {"fedora", "Linux/Fedora"},
    {"alpine", "Linux/Alpine"}, {"openwrt", "Linux/OpenWrt"},{"gentoo", "Linux"},
    {"linux", "Linux"},         {"unix", "Unix"},           {"freebsd", "FreeBSD"},
    {"openbsd", "OpenBSD"},      {"netbsd", "NetBSD"},       {"windows", "Windows"},
    {"win32", "Windows"},        {"win64", "Windows"},       {"microsoft", "Windows"},
    {"darwin", "macOS"},         {"mac os", "macOS"},        {"macos", "macOS"},
    {"mikrotik", "RouterOS"},    {"routeros", "RouterOS"},   {"cisco", "Cisco"},
    {"android", "Android"},
};

void poom_security_audit_os_hint(const poom_security_audit_service_t *svc,
                              uint8_t ttl, bool ttl_known, const char *vendor,
                              char *out, size_t cap, bool *out_strong)
{
    if (out == NULL || cap == 0) { return; }
    out[0] = '\0';
    if (out_strong != NULL) { *out_strong = false; }

    /* 1) Strong: OS token in a service banner / product string. */
    if (svc != NULL)
    {
        char hay[POOM_SECURITY_AUDIT_BANNER_CAP + POOM_SECURITY_AUDIT_PRODUCT_CAP + 2];
        (void)snprintf(hay, sizeof(hay), "%s %s", svc->banner, svc->product);
        size_t hlen = strlen(hay);
        for (size_t i = 0; i < sizeof(k_os_tokens) / sizeof(k_os_tokens[0]); ++i)
        {
            if (security_audit_memfind_ci_(hay, hlen, k_os_tokens[i].token) != NULL)
            {
                (void)snprintf(out, cap, "%s", k_os_tokens[i].family);
                if (out_strong != NULL) { *out_strong = true; }
                return;
            }
        }
    }

    /* 2) Inferred: TTL family (0-1 hops on a LAN keeps TTL near its initial). */
    if (ttl_known && ttl > 0)
    {
        if (ttl <= 64)       { (void)snprintf(out, cap, "Linux/Unix?"); }
        else if (ttl <= 128) { (void)snprintf(out, cap, "Windows?"); }
        else                 { (void)snprintf(out, cap, "Net device?"); }
        return;
    }

    /* 3) Inferred: MAC vendor hints at a device type/OS family. */
    if (vendor != NULL && vendor[0] != '\0')
    {
        static const security_audit_os_token_t k_vendor_hints[] = {
            {"apple", "Apple?"}, {"raspberry", "Raspberry Pi?"},
            {"espressif", "ESP/IoT?"}, {"samsung", "Samsung?"},
            {"google", "Google?"}, {"amazon", "Amazon?"},
            {"intel", "PC?"}, {"dell", "PC?"}, {"hewlett", "PC?"},
            {"microsoft", "MS/Xbox?"}, {"liteon", "PC?"}, {"asustek", "PC?"},
            {"huawei", "Phone/IoT?"}, {"xiaomi", "Phone/IoT?"},
            {"oneplus", "Phone?"}, {"oppo", "Phone?"}, {"vivo", "Phone?"},
            {"tp-link", "Network dev?"}, {"netgear", "Network dev?"},
            {"ubiquiti", "Network dev?"}, {"mikrotik", "Network dev?"},
            {"cisco", "Network dev?"}, {"d-link", "Network dev?"},
            {"zyxel", "Network dev?"}, {"aruba", "Network dev?"},
            {"sony", "Media/TV?"}, {"roku", "Media/TV?"},
            {"nintendo", "Console?"}, {"sonos", "IoT?"}, {"philips", "IoT?"},
            {"tuya", "IoT?"}, {"shenzhen", "IoT?"}, {"hon hai", "Device?"},
            {"murata", "WiFi module?"}, {"azurewave", "WiFi module?"},
            {"realtek", "Device?"}, {"broadcom", "Device?"},
        };
        size_t vl = strlen(vendor);
        for (size_t i = 0; i < sizeof(k_vendor_hints) / sizeof(k_vendor_hints[0]); ++i)
        {
            if (security_audit_memfind_ci_(vendor, vl, k_vendor_hints[i].token) != NULL)
            {
                (void)snprintf(out, cap, "%s", k_vendor_hints[i].family);
                return;
            }
        }
        /* Unmapped vendor: leave empty (the Mfr line already shows the vendor). */
    }
}

bool poom_security_audit_parse_http(const char *resp, size_t len,
                                 poom_security_audit_service_t *out)
{
    if (out == NULL || resp == NULL || len < 5) { return false; }
    if (strncmp(resp, "HTTP/", 5) != 0) { return false; }

    (void)snprintf(out->service, sizeof(out->service), "http");
    out->product[0] = '\0';
    out->version[0] = '\0';
    out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
    security_audit_snippet_(out->banner, sizeof(out->banner), resp, len); /* status line */

    /* Find a "Server:" header value (case-insensitive), bounded to the head. */
    const char *sv = security_audit_memfind_ci_(resp, len, "server:");
    if (sv != NULL)
    {
        sv += strlen("server:");
        size_t rem = len - (size_t)(sv - resp);
        char val[POOM_SECURITY_AUDIT_PRODUCT_CAP + POOM_SECURITY_AUDIT_VERSION_CAP];
        security_audit_snippet_(val, sizeof(val), sv, rem); /* value to EOL, trimmed */
        if (val[0] != '\0')
        {
            security_audit_split_pv_(val, out);
            if (out->version[0] != '\0') { out->evidence = POOM_SECURITY_AUDIT_EV_VERSION_REPORTED; }
            /* keep the Server line as the banner evidence */
            (void)snprintf(out->banner, sizeof(out->banner), "%s", val);
        }
    }

    /* Device model from the Basic-auth realm or the page <title> (routers,
     * NAS, cameras put "NETGEAR R7000" / "RT-AC68U" there). Preferred over a
     * version-less Server header, which names only the embedded web server. */
    char hint[64] = {0};
    const char *rl = security_audit_memfind_ci_(resp, len, "realm=\"");
    if (rl != NULL)
    {
        rl += 7;
        size_t n = 0;
        while ((size_t)(rl - resp) + n < len && rl[n] != '"' && rl[n] != '\r' &&
               rl[n] != '\n' && n + 1 < sizeof(hint))
        {
            n++;
        }
        memcpy(hint, rl, n);
        hint[n] = '\0';
    }
    if (hint[0] == '\0')
    {
        const char *t = security_audit_memfind_ci_(resp, len, "<title>");
        if (t != NULL)
        {
            t += 7;
            size_t n = 0;
            while ((size_t)(t - resp) + n < len && t[n] != '<' && n + 1 < sizeof(hint)) { n++; }
            memcpy(hint, t, n);
            hint[n] = '\0';
        }
    }
    char model[POOM_SECURITY_AUDIT_PRODUCT_CAP];
    if (hint[0] != '\0' && out->version[0] == '\0' &&
        poom_security_audit_model_token(hint, model, sizeof(model)) > 0U)
    {
        (void)snprintf(out->product, sizeof(out->product), "%s", model);
        out->evidence = POOM_SECURITY_AUDIT_EV_PROTO_RESPONSE;
    }
    if (hint[0] != '\0')
    {
        /* keep the page/realm text as evidence (feeds the OS / device fold) */
        size_t bl = strlen(out->banner);
        if (bl + 3U < sizeof(out->banner))
        {
            (void)snprintf(out->banner + bl, sizeof(out->banner) - bl, "%s%.*s",
                           bl ? " | " : "", (int)(sizeof(out->banner) - bl - 4U), hint);
        }
    }

    /* Language runtime as a last resort ("X-Powered-By: PHP/5.6.40"). */
    if (out->product[0] == '\0')
    {
        const char *xp = security_audit_memfind_ci_(resp, len, "x-powered-by:");
        if (xp != NULL)
        {
            xp += 13;
            char v2[POOM_SECURITY_AUDIT_PRODUCT_CAP + POOM_SECURITY_AUDIT_VERSION_CAP];
            security_audit_snippet_(v2, sizeof(v2), xp, len - (size_t)(xp - resp));
            if (v2[0] != '\0')
            {
                security_audit_split_pv_(v2, out);
                if (out->version[0] != '\0') { out->evidence = POOM_SECURITY_AUDIT_EV_VERSION_REPORTED; }
            }
        }
    }
    return true;
}

/* ---- duration estimate (pure) ------------------------------------------- */

#define SECURITY_AUDIT_EST_DEFAULT_TIMEOUT_MS (600U) /* matches the engine default */

uint32_t poom_security_audit_estimate_scan_s(uint32_t hosts, uint32_t ports_per_host,
                                          uint32_t fanout, uint32_t timeout_ms)
{
    if (fanout == 0U) { fanout = POOM_SECURITY_AUDIT_DEFAULT_FANOUT; }
    if (fanout > POOM_SECURITY_AUDIT_MAX_FANOUT) { fanout = POOM_SECURITY_AUDIT_MAX_FANOUT; }
    if (timeout_ms == 0U) { timeout_ms = SECURITY_AUDIT_EST_DEFAULT_TIMEOUT_MS; }

    uint64_t batches = ((uint64_t)ports_per_host + fanout - 1U) / fanout;
    uint64_t ms = (uint64_t)hosts * batches * timeout_ms;
    uint64_t s = (ms + 999U) / 1000U;
    return (s > UINT32_MAX) ? UINT32_MAX : (uint32_t)s;
}

void poom_security_audit_fmt_duration(uint32_t seconds, char *out, size_t cap)
{
    if (out == NULL || cap == 0U) { return; }
    if (seconds < 60U)
    {
        (void)snprintf(out, cap, "%us", (unsigned)seconds);
    }
    else if (seconds < 3600U)
    {
        (void)snprintf(out, cap, "%um", (unsigned)((seconds + 59U) / 60U));
    }
    else
    {
        uint32_t m = (seconds + 59U) / 60U;
        uint32_t h = m / 60U;
        if (h > 99U) { (void)snprintf(out, cap, ">99h"); }
        else { (void)snprintf(out, cap, "%uh%02um", (unsigned)h, (unsigned)(m % 60U)); }
    }
}

/* ---- word wrap (pure) --------------------------------------------------- */

const char *poom_security_audit_wrap(const char *text, size_t width, char *out, size_t cap)
{
    if (out == NULL || cap == 0U) { return NULL; }
    out[0] = '\0';
    if (text == NULL || width == 0U) { return NULL; }
    if (width > cap - 1U) { width = cap - 1U; }

    while (*text == ' ') { text++; }
    size_t len = strlen(text);
    if (len == 0U) { return NULL; }

    size_t take = len;
    size_t next = len;
    if (len > width)
    {
        /* Break at the last space within width+1 (a space right after a full
         * line is a clean break too); else hard-break the long word. */
        size_t brk = 0;
        for (size_t i = width; i > 0U; --i)
        {
            if (text[i] == ' ') { brk = i; break; }
        }
        take = (brk > 0U) ? brk : width;
        next = take;
    }
    while (take > 0U && text[take - 1U] == ' ') { take--; }
    memcpy(out, text, take);
    out[take] = '\0';

    const char *rest = text + next;
    while (*rest == ' ') { rest++; }
    return (*rest != '\0') ? rest : NULL;
}

/* ---- windowed port lists (pure) ------------------------------------------ */

/* Is table[i] a repeat of an earlier table entry (or, when @p skip_low, below
 * 1024 = already in the Extended well-known range)? */
static bool security_audit_tbl_skip_(const uint16_t *tbl, size_t i, bool skip_low)
{
    if (skip_low && tbl[i] < 1024U) { return true; }
    for (size_t j = 0; j < i; ++j)
    {
        if (tbl[j] == tbl[i]) { return true; }
    }
    return false;
}

size_t poom_security_audit_build_ports_window(poom_security_audit_profile_t profile, uint32_t offset,
                                           uint16_t *out, size_t cap)
{
    if (out == NULL || cap == 0U) { return 0U; }
    size_t n = 0;
    uint32_t idx = 0; /* position in the virtual full list */

    switch (profile)
    {
    case POOM_SECURITY_AUDIT_PROFILE_QUICK:
        for (size_t i = 0; i < sizeof(k_quick_ports) / sizeof(k_quick_ports[0]) && n < cap; ++i)
        {
            if (security_audit_tbl_skip_(k_quick_ports, i, false)) { continue; }
            if (idx++ >= offset) { out[n++] = k_quick_ports[i]; }
        }
        return n;

    case POOM_SECURITY_AUDIT_PROFILE_EXTENDED:
        for (uint32_t p = 1; p <= 1023U && n < cap; ++p)
        {
            if (idx++ >= offset) { out[n++] = (uint16_t)p; }
        }
        for (size_t i = 0; i < sizeof(k_common_high_ports) / sizeof(k_common_high_ports[0]) && n < cap; ++i)
        {
            if (security_audit_tbl_skip_(k_common_high_ports, i, true)) { continue; }
            if (idx++ >= offset) { out[n++] = k_common_high_ports[i]; }
        }
        return n;

    case POOM_SECURITY_AUDIT_PROFILE_FULL:
        for (uint32_t p = offset + 1U; p <= 65535U && n < cap; ++p)
        {
            out[n++] = (uint16_t)p;
        }
        return n;

    default:
        return 0U;
    }
}
