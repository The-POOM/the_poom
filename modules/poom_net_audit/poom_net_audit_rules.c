// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Exposure rules (Phase D): observation-only checks over what a scan already
// saw (ports, services, products, versions, UPnP). Each rule is a small text
// condition evaluated by the engine below - a rule can only TEST observed data,
// it can never make the device probe or log into anything.
//
// To add or change a rule, edit k_builtin[] (and, for a new rule, add an id to
// poom_net_audit_exp_t) and rebuild. Condition syntax:
//   atoms : port:N[,N..] | service:NAME[,NAME..]
//         | product:NAME[(<|<=|>|>=|=)VERSION] | upnp
//   joined with & (and), | (or), ! (not) and ( ); each atom is true if ANY
//   observed service on the host satisfies it.
// Fields must fit their arrays INCLUDING the NUL (title < 24, detail < 64);
// test/test_rules.c checks this. Host-tested in test/test_rules.c.

#include "poom_net_audit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- built-in rules (const -> flash, no RAM) ------------------------------ */

static const poom_net_audit_rule_t k_builtin[POOM_NET_AUDIT_EXP_COUNT] = {
    [POOM_NET_AUDIT_EXP_TELNET] = {"telnet", POOM_NET_AUDIT_SEV_DETECTED,
        "port:23 | service:telnet", "Telnet enabled",
        "Logins and commands cross the network in cleartext."},
    [POOM_NET_AUDIT_EXP_FTP] = {"ftp", POOM_NET_AUDIT_SEV_DETECTED,
        "port:21 | service:ftp", "FTP enabled",
        "FTP sends passwords and files in cleartext."},
    [POOM_NET_AUDIT_EXP_DB] = {"database", POOM_NET_AUDIT_SEV_DETECTED,
        "port:3306,5432,6379,27017,9200 | service:mysql", "Database port reachable",
        "A database answers on the LAN; it should not face clients."},
    [POOM_NET_AUDIT_EXP_OLD_SSH] = {"oldssh", POOM_NET_AUDIT_SEV_DETECTED,
        "product:OpenSSH<7.4", "Outdated OpenSSH",
        "OpenSSH older than 7.4 has known security fixes missing."},
    [POOM_NET_AUDIT_EXP_REMOTE_DESKTOP] = {"remote", POOM_NET_AUDIT_SEV_INFO,
        "port:3389,5900 | service:vnc", "Remote desktop open",
        "RDP or VNC is open; make sure it is meant to be."},
    [POOM_NET_AUDIT_EXP_SMB] = {"smb", POOM_NET_AUDIT_SEV_INFO,
        "port:139,445", "File sharing (SMB)",
        "Windows/Samba file sharing is reachable."},
    [POOM_NET_AUDIT_EXP_MQTT] = {"mqtt", POOM_NET_AUDIT_SEV_INFO,
        "port:1883", "MQTT without TLS",
        "IoT messages on port 1883 are not encrypted."},
    [POOM_NET_AUDIT_EXP_HTTP_NO_TLS] = {"webnotls", POOM_NET_AUDIT_SEV_INFO,
        "(port:80,8080,8000 | service:http) & !(port:443,8443 | service:https)",
        "Web UI without HTTPS", "A web interface is served only over plain HTTP."},
    [POOM_NET_AUDIT_EXP_UPNP] = {"upnp", POOM_NET_AUDIT_SEV_INFO,
        "upnp", "UPnP enabled",
        "Device advertises UPnP; disable if not needed."},
    [POOM_NET_AUDIT_EXP_RAW_PRINT] = {"rawprint", POOM_NET_AUDIT_SEV_INFO,
        "port:9100", "Raw print port open",
        "Port 9100 accepts print jobs without authentication."},
};

size_t poom_net_audit_builtin_count(void) { return POOM_NET_AUDIT_EXP_COUNT; }

const poom_net_audit_rule_t *poom_net_audit_builtin(size_t i)
{
    return (i < POOM_NET_AUDIT_EXP_COUNT) ? &k_builtin[i] : NULL;
}

const char *poom_net_audit_exp_title(uint8_t id)
{
    return (id < POOM_NET_AUDIT_EXP_COUNT) ? k_builtin[id].title : "?";
}

const char *poom_net_audit_exp_detail(uint8_t id)
{
    return (id < POOM_NET_AUDIT_EXP_COUNT) ? k_builtin[id].detail : "";
}

poom_net_audit_sev_t poom_net_audit_exp_severity(uint8_t id)
{
    return (id < POOM_NET_AUDIT_EXP_COUNT) ? (poom_net_audit_sev_t)k_builtin[id].sev
                                           : POOM_NET_AUDIT_SEV_INFO;
}

int poom_net_audit_version_cmp(const char *a, const char *b)
{
    if (a == NULL) { a = ""; }
    if (b == NULL) { b = ""; }
    for (int part = 0; part < 6; ++part)
    {
        while (*a != '\0' && !isdigit((unsigned char)*a)) { a++; }
        while (*b != '\0' && !isdigit((unsigned char)*b)) { b++; }
        if (*a == '\0' && *b == '\0') { return 0; }
        unsigned long va = 0, vb = 0;
        while (isdigit((unsigned char)*a)) { va = va * 10U + (unsigned long)(*a - '0'); a++; }
        while (isdigit((unsigned char)*b)) { vb = vb * 10U + (unsigned long)(*b - '0'); b++; }
        if (va != vb) { return (va < vb) ? -1 : 1; }
        /* stop at the first non '.' separator ("7.2p2" compares as 7.2) */
        if (*a != '.' || *b != '.') { return 0; }
    }
    return 0;
}

/* ---- condition evaluator (recursive descent, no AST, depth-limited) ------- */

typedef struct
{
    const char *p;
    const poom_net_audit_svc_t *svcs;
    size_t n;
    bool upnp;
    bool err;
    int depth;
} na_ctx_t;

static void na_ws_(na_ctx_t *c) { while (*c->p == ' ' || *c->p == '\t') { c->p++; } }

static bool na_name_ch_(char ch)
{
    return isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_' || ch == '+';
}

static bool na_ieq_n_(const char *a, size_t an, const char *b)
{
    if (b == NULL) { return false; }
    size_t i = 0;
    for (; i < an; ++i)
    {
        if (b[i] == '\0' || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) { return false; }
    }
    return b[i] == '\0';
}

/* Read a name token; returns its length (0 = none). */
static size_t na_name_(na_ctx_t *c, const char **start)
{
    na_ws_(c);
    *start = c->p;
    size_t k = 0;
    while (na_name_ch_(c->p[k])) { k++; }
    c->p += k;
    return k;
}

static bool na_kw_(na_ctx_t *c, const char *kw)
{
    size_t l = strlen(kw);
    for (size_t i = 0; i < l; ++i)
    {
        if (tolower((unsigned char)c->p[i]) != kw[i]) { return false; }
    }
    c->p += l;
    return true;
}

static bool na_expr_(na_ctx_t *c);

static bool na_atom_(na_ctx_t *c)
{
    na_ws_(c);
    if (na_kw_(c, "port:"))
    {
        bool hit = false, any = false;
        do
        {
            na_ws_(c);
            if (!isdigit((unsigned char)*c->p)) { c->err = true; return false; }
            unsigned long v = 0;
            while (isdigit((unsigned char)*c->p)) { v = v * 10U + (unsigned long)(*c->p - '0'); c->p++; }
            if (v == 0U || v > 65535U) { c->err = true; return false; }
            any = true;
            for (size_t i = 0; i < c->n; ++i) { if (c->svcs[i].port == v) { hit = true; } }
            na_ws_(c);
        } while (*c->p == ',' && c->p++);
        return any && hit;
    }
    if (na_kw_(c, "service:"))
    {
        bool hit = false;
        do
        {
            const char *s;
            size_t k = na_name_(c, &s);
            if (k == 0U) { c->err = true; return false; }
            for (size_t i = 0; i < c->n; ++i) { if (na_ieq_n_(s, k, c->svcs[i].service)) { hit = true; } }
            na_ws_(c);
        } while (*c->p == ',' && c->p++);
        return hit;
    }
    if (na_kw_(c, "product:"))
    {
        const char *s;
        size_t k = na_name_(c, &s);
        if (k == 0U) { c->err = true; return false; }
        /* optional version comparison: < <= > >= = */
        na_ws_(c);
        int op = 0; /* 0 none, 1 <, 2 <=, 3 >, 4 >=, 5 = */
        if (c->p[0] == '<' && c->p[1] == '=')      { op = 2; c->p += 2; }
        else if (c->p[0] == '>' && c->p[1] == '=') { op = 4; c->p += 2; }
        else if (c->p[0] == '<')                   { op = 1; c->p += 1; }
        else if (c->p[0] == '>')                   { op = 3; c->p += 1; }
        else if (c->p[0] == '=')                   { op = 5; c->p += 1; }
        char ver[24] = {0};
        if (op != 0)
        {
            const char *v;
            size_t vk = na_name_(c, &v);
            if (vk == 0U || vk >= sizeof(ver)) { c->err = true; return false; }
            memcpy(ver, v, vk);
        }
        for (size_t i = 0; i < c->n; ++i)
        {
            if (!na_ieq_n_(s, k, c->svcs[i].product)) { continue; }
            if (op == 0) { return true; }
            const char *sv = c->svcs[i].version;
            if (sv == NULL || sv[0] == '\0') { continue; } /* unknown version never matches */
            int r = poom_net_audit_version_cmp(sv, ver);
            if ((op == 1 && r < 0) || (op == 2 && r <= 0) || (op == 3 && r > 0) ||
                (op == 4 && r >= 0) || (op == 5 && r == 0))
            {
                return true;
            }
        }
        return false;
    }
    if (na_kw_(c, "upnp"))
    {
        if (na_name_ch_(*c->p)) { c->err = true; return false; } /* e.g. "upnpx" */
        return c->upnp;
    }
    c->err = true;
    return false;
}

static bool na_factor_(na_ctx_t *c)
{
    na_ws_(c);
    if (++c->depth > 16) { c->err = true; return false; }
    bool v;
    if (*c->p == '!')
    {
        c->p++;
        v = !na_factor_(c);
    }
    else if (*c->p == '(')
    {
        c->p++;
        v = na_expr_(c);
        na_ws_(c);
        if (*c->p != ')') { c->err = true; }
        else { c->p++; }
    }
    else
    {
        v = na_atom_(c);
    }
    c->depth--;
    return v;
}

static bool na_term_(na_ctx_t *c)
{
    bool v = na_factor_(c);
    na_ws_(c);
    while (!c->err && *c->p == '&')
    {
        c->p++;
        bool r = na_factor_(c); /* always parse both sides (syntax check) */
        v = v && r;
        na_ws_(c);
    }
    return v;
}

static bool na_expr_(na_ctx_t *c)
{
    bool v = na_term_(c);
    na_ws_(c);
    while (!c->err && *c->p == '|')
    {
        c->p++;
        bool r = na_term_(c);
        v = v || r;
        na_ws_(c);
    }
    return v;
}

int poom_net_audit_rule_eval(const char *expr, const poom_net_audit_svc_t *svcs, size_t n,
                             bool has_upnp)
{
    if (expr == NULL) { return -1; }
    na_ctx_t c = {expr, svcs, (svcs != NULL) ? n : 0U, has_upnp, false, 0};
    bool v = na_expr_(&c);
    na_ws_(&c);
    if (c.err || *c.p != '\0' || c.p == expr) { return -1; }
    return v ? 1 : 0;
}

uint32_t poom_net_audit_exposures(const poom_net_audit_svc_t *svcs, size_t n, bool has_upnp)
{
    uint32_t m = 0;
    for (size_t i = 0; i < POOM_NET_AUDIT_EXP_COUNT; ++i)
    {
        if (poom_net_audit_rule_eval(k_builtin[i].expr, svcs, n, has_upnp) == 1) { m |= 1UL << i; }
    }
    return m;
}
