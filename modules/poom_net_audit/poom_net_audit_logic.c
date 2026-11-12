// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Pure, hardware-independent logic for the SearchSploit lookup: quote-aware CSV
// field extraction and title matching. Host-unit-tested in test/test_logic.c.
// The SD/file streaming lives in poom_net_audit.c.

#include "poom_net_audit.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Advance past one CSV field starting at *pp; returns a pointer to the comma
 * that ends it (or the terminator/newline). Handles double-quoted fields with
 * embedded commas and "" escapes. */
static const char *net_audit_skip_field_(const char *p)
{
    if (*p == '"')
    {
        p++; /* opening quote */
        while (*p != '\0')
        {
            if (*p == '"')
            {
                if (*(p + 1) == '"') { p += 2; continue; } /* escaped quote */
                p++; /* closing quote */
                break;
            }
            p++;
        }
        /* fall through to find the delimiter after the closing quote */
    }
    while (*p != '\0' && *p != ',' && *p != '\r' && *p != '\n')
    {
        p++;
    }
    return p;
}

size_t poom_net_audit_csv_field(const char *line, int index, char *out, size_t cap)
{
    if (line == NULL || out == NULL || cap == 0U)
    {
        return 0U;
    }
    out[0] = '\0';
    if (index < 0)
    {
        return 0U;
    }

    const char *p = line;
    int field = 0;
    while (field < index && *p != '\0' && *p != '\r' && *p != '\n')
    {
        p = net_audit_skip_field_(p);
        if (*p == ',') { p++; field++; }
        else { return 0U; } /* fewer fields than index */
    }
    if (field != index)
    {
        return 0U;
    }

    /* Copy field `index`, unquoting as needed. */
    size_t n = 0;
    if (*p == '"')
    {
        p++;
        while (*p != '\0')
        {
            if (*p == '"')
            {
                if (*(p + 1) == '"') { if (n + 1 < cap) { out[n++] = '"'; } p += 2; continue; }
                break; /* closing quote */
            }
            if (n + 1 < cap) { out[n++] = *p; }
            p++;
        }
    }
    else
    {
        while (*p != '\0' && *p != ',' && *p != '\r' && *p != '\n')
        {
            if (n + 1 < cap) { out[n++] = *p; }
            p++;
        }
    }
    out[n] = '\0';
    return n;
}

/* Case-insensitive substring search. */
static const char *net_audit_stristr_(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) { return NULL; }
    for (const char *h = hay; *h != '\0'; ++h)
    {
        size_t j = 0;
        while (j < nlen && h[j] != '\0' &&
               tolower((unsigned char)h[j]) == tolower((unsigned char)needle[j]))
        {
            j++;
        }
        if (j == nlen) { return h; }
    }
    return NULL;
}

/* Extract the first CVE token (CVE-####-####) from a "codes" field, which is a
 * ';'-separated list like "CVE-2017-1234;OSVDB-98765". */
static void net_audit_first_cve_(const char *codes, char *out, size_t cap)
{
    out[0] = '\0';
    const char *c = net_audit_stristr_(codes, "CVE-");
    if (c == NULL) { return; }
    size_t n = 0;
    while (c[n] != '\0' && c[n] != ';' && c[n] != ' ' && n + 1 < cap)
    {
        out[n] = c[n];
        n++;
    }
    out[n] = '\0';
}

/* ---- whole-word match + version tiering (pure) -------------------------- */

static bool net_audit_is_alnum_(char c) { return isalnum((unsigned char)c) != 0; }

bool poom_net_audit_title_has_word(const char *title, const char *term)
{
    if (title == NULL || term == NULL || term[0] == '\0')
    {
        return false;
    }
    size_t tl = strlen(term);
    for (const char *h = title; *h != '\0'; ++h)
    {
        size_t j = 0;
        while (j < tl && h[j] != '\0' &&
               tolower((unsigned char)h[j]) == tolower((unsigned char)term[j]))
        {
            j++;
        }
        if (j == tl)
        {
            bool lb = (h == title) || !net_audit_is_alnum_(h[-1]);
            bool rb = (h[tl] == '\0') || !net_audit_is_alnum_(h[tl]);
            if (lb && rb) { return true; }
        }
    }
    return false;
}

/* Derive the "major.minor" prefix of a version ("9.6p1"->"9.6","1.24.0"->"1.24";
 * a bare "2" yields ""). */
static void net_audit_major_minor_(const char *ver, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    int dots = 0;
    while (ver[i] != '\0' && o + 1 < cap)
    {
        char c = ver[i];
        if (isdigit((unsigned char)c)) { out[o++] = c; i++; }
        else if (c == '.' && dots == 0) { out[o++] = '.'; dots = 1; i++; }
        else { break; }
    }
    out[o] = '\0';
    if (dots == 0) { out[0] = '\0'; } /* need at least d.d to be useful */
}

/* Is there a version-like "<digit>.<digit>" token anywhere in the title? */
static bool net_audit_title_has_version_(const char *title)
{
    for (const char *p = title; p[0] != '\0' && p[1] != '\0' && p[2] != '\0'; ++p)
    {
        if (isdigit((unsigned char)p[0]) && p[1] == '.' && isdigit((unsigned char)p[2]))
        {
            return true;
        }
    }
    return false;
}

poom_net_audit_tier_t poom_net_audit_classify(const char *title, const char *version)
{
    if (title == NULL) { return POOM_NET_AUDIT_TIER_WEAK; }
    if (version != NULL && version[0] != '\0')
    {
        if (net_audit_stristr_(title, version) != NULL)
        {
            return POOM_NET_AUDIT_TIER_STRONG;
        }
        char mm[16];
        net_audit_major_minor_(version, mm, sizeof(mm));
        if (mm[0] != '\0' && net_audit_stristr_(title, mm) != NULL)
        {
            return POOM_NET_AUDIT_TIER_STRONG;
        }
    }
    return net_audit_title_has_version_(title) ? POOM_NET_AUDIT_TIER_MEDIUM
                                               : POOM_NET_AUDIT_TIER_WEAK;
}

/* Fill id/type/platform/first-CVE (+ title) from a line into a match. */
static void net_audit_fill_match_(const char *line, const char *title,
                                  poom_net_audit_match_t *out)
{
    (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_ID, out->edb_id, sizeof(out->edb_id));
    (void)snprintf(out->title, sizeof(out->title), "%.*s",
                   (int)(sizeof(out->title) - 1), title);
    (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_TYPE, out->type, sizeof(out->type));
    (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_PLATFORM, out->platform, sizeof(out->platform));
    char codes[96];
    (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_CODES, codes, sizeof(codes));
    net_audit_first_cve_(codes, out->cve, sizeof(out->cve));
}

bool poom_net_audit_match_line(const char *line, const char *term,
                               poom_net_audit_match_t *out)
{
    if (line == NULL || term == NULL || out == NULL || term[0] == '\0')
    {
        return false;
    }
    char title[160];
    (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_TITLE, title, sizeof(title));
    if (title[0] == '\0' || !poom_net_audit_title_has_word(title, term))
    {
        return false;
    }
    memset(out, 0, sizeof(*out));
    net_audit_fill_match_(line, title, out);
    out->tier = (uint8_t)poom_net_audit_classify(title, NULL);
    out->term_id = 0;
    return true;
}

/* ---- bounded multi-term collector (pure) -------------------------------- */

void poom_net_audit_collector_init(poom_net_audit_collector_t *c,
                                   poom_net_audit_match_t *matches,
                                   uint16_t cap, uint8_t per_term_cap)
{
    if (c == NULL) { return; }
    c->matches = matches;
    c->cap = cap;
    c->count = 0;
    c->per_term_cap = (per_term_cap == 0U) ? 1U : per_term_cap;
    memset(c->term_count, 0, sizeof(c->term_count));
}

static int net_audit_weakest_for_term_(const poom_net_audit_collector_t *c, uint8_t term_id)
{
    int idx = -1, best = 99;
    for (uint16_t i = 0; i < c->count; ++i)
    {
        if (c->matches[i].term_id == term_id && (int)c->matches[i].tier < best)
        {
            best = c->matches[i].tier;
            idx = (int)i;
        }
    }
    return idx;
}

static int net_audit_weakest_global_(const poom_net_audit_collector_t *c)
{
    int idx = -1, best = 99;
    for (uint16_t i = 0; i < c->count; ++i)
    {
        if ((int)c->matches[i].tier < best)
        {
            best = c->matches[i].tier;
            idx = (int)i;
        }
    }
    return idx;
}

void poom_net_audit_collect_line(poom_net_audit_collector_t *c, const char *line,
                                 const poom_net_audit_term_t *terms, uint8_t n_terms)
{
    if (c == NULL || line == NULL || terms == NULL || n_terms == 0U) { return; }
    if (n_terms > POOM_NET_AUDIT_MAX_TERMS) { n_terms = POOM_NET_AUDIT_MAX_TERMS; }

    char title[160];
    (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_TITLE, title, sizeof(title));
    if (title[0] == '\0') { return; }

    bool filled = false;
    poom_net_audit_match_t m;

    for (uint8_t t = 0; t < n_terms; ++t)
    {
        if (terms[t].term == NULL || terms[t].term[0] == '\0') { continue; }
        if (!poom_net_audit_title_has_word(title, terms[t].term)) { continue; }

        uint8_t tier = (uint8_t)poom_net_audit_classify(title, terms[t].version);

        int target;
        if (c->count < c->cap && c->term_count[t] < c->per_term_cap)
        {
            target = (int)c->count;
            c->count++;
            c->term_count[t]++;
        }
        else
        {
            int victim = (c->term_count[t] >= c->per_term_cap)
                             ? net_audit_weakest_for_term_(c, t)
                             : net_audit_weakest_global_(c);
            if (victim < 0 || (int)c->matches[victim].tier >= (int)tier) { continue; }
            c->term_count[c->matches[victim].term_id]--;
            c->term_count[t]++;
            target = victim;
        }

        if (!filled)
        {
            memset(&m, 0, sizeof(m));
            net_audit_fill_match_(line, title, &m);
            filled = true;
        }
        m.term_id = t;
        m.tier = tier;
        c->matches[target] = m;
    }
}

/* ---- network-wide Assess helpers (pure) --------------------------------- */

static bool net_audit_ieq_(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) { return false; }
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

uint8_t poom_net_audit_intern(poom_net_audit_prodver_t *tab, uint8_t *n, uint8_t cap,
                              const char *product, const char *version)
{
    if (tab == NULL || n == NULL || product == NULL || product[0] == '\0')
    {
        return 0U;
    }
    const char *ver = (version != NULL) ? version : "";
    if (cap >= POOM_NET_AUDIT_INTERN_FULL) { cap = POOM_NET_AUDIT_INTERN_FULL - 1U; }

    for (uint8_t i = 0; i < *n; ++i)
    {
        if (net_audit_ieq_(tab[i].product, product) && strcmp(tab[i].version, ver) == 0)
        {
            return (uint8_t)(i + 1U);
        }
    }
    if (*n >= cap)
    {
        return POOM_NET_AUDIT_INTERN_FULL;
    }
    (void)snprintf(tab[*n].product, sizeof(tab[*n].product), "%.*s",
                   (int)(sizeof(tab[*n].product) - 1U), product);
    (void)snprintf(tab[*n].version, sizeof(tab[*n].version), "%.*s",
                   (int)(sizeof(tab[*n].version) - 1U), ver);
    (*n)++;
    return *n;
}

void poom_net_audit_count_terms(const poom_net_audit_match_t *m, size_t n,
                                uint8_t *counts, uint8_t n_terms)
{
    if (counts == NULL) { return; }
    memset(counts, 0, n_terms);
    if (m == NULL) { return; }
    for (size_t i = 0; i < n; ++i)
    {
        uint8_t t = m[i].term_id;
        if (t < n_terms && counts[t] < 255U) { counts[t]++; }
    }
}

poom_net_audit_host_class_t poom_net_audit_host_class(const uint8_t *prod_ids, uint8_t n_ports,
                                                      const uint8_t *term_counts, uint8_t n_terms)
{
    if (prod_ids == NULL || n_ports == 0U)
    {
        return POOM_NET_AUDIT_HOST_NONE;
    }
    bool any_product = false;
    for (uint8_t i = 0; i < n_ports; ++i)
    {
        uint8_t id = prod_ids[i];
        if (id == 0U || id == POOM_NET_AUDIT_INTERN_FULL) { continue; }
        any_product = true;
        uint8_t t = (uint8_t)(id - 1U);
        if (term_counts != NULL && t < n_terms && term_counts[t] > 0U)
        {
            return POOM_NET_AUDIT_HOST_POTENTIAL;
        }
    }
    return any_product ? POOM_NET_AUDIT_HOST_NOMATCH : POOM_NET_AUDIT_HOST_UNASSESSED;
}

/* ---- platform fit (pure) ------------------------------------------------- */

typedef enum { NA_PLAT_NEUTRAL, NA_PLAT_WIN, NA_PLAT_LINUX, NA_PLAT_ANDROID, NA_PLAT_BSD,
               NA_PLAT_MAC, NA_PLAT_IOS, NA_PLAT_UNIX, NA_PLAT_HW } na_plat_t;

static bool na_prefix_(const char *s, const char *pre)
{
    while (*pre != '\0')
    {
        if (tolower((unsigned char)*s) != *pre) { return false; }
        s++;
        pre++;
    }
    return true;
}

static na_plat_t na_plat_class_(const char *p)
{
    if (p == NULL || p[0] == '\0') { return NA_PLAT_NEUTRAL; }
    if (na_prefix_(p, "win"))      { return NA_PLAT_WIN; }      /* windows, win_x86-64 */
    if (na_prefix_(p, "linux"))    { return NA_PLAT_LINUX; }    /* linux, linux_mips   */
    if (na_prefix_(p, "android"))  { return NA_PLAT_ANDROID; }
    if (na_prefix_(p, "freebsd") || na_prefix_(p, "openbsd") || na_prefix_(p, "netbsd") ||
        na_prefix_(p, "bsd"))      { return NA_PLAT_BSD; }
    if (na_prefix_(p, "osx") || na_prefix_(p, "macos")) { return NA_PLAT_MAC; }
    if (na_prefix_(p, "ios"))      { return NA_PLAT_IOS; }
    if (na_prefix_(p, "unix") || na_prefix_(p, "solaris") || na_prefix_(p, "aix") ||
        na_prefix_(p, "hp-ux") || na_prefix_(p, "irix")) { return NA_PLAT_UNIX; }
    if (na_prefix_(p, "hardware")) { return NA_PLAT_HW; }
    return NA_PLAT_NEUTRAL; /* php, asp, cgi, java, multiple, python, ... */
}

poom_net_audit_fit_t poom_net_audit_platform_fit(const char *platform, uint8_t os)
{
    na_plat_t pc = na_plat_class_(platform);
    if (os == POOM_NET_AUDIT_OS_UNKNOWN || pc == NA_PLAT_NEUTRAL) { return POOM_NET_AUDIT_FIT_NEUTRAL; }

    switch (os)
    {
    case POOM_NET_AUDIT_OS_WINDOWS:
        return (pc == NA_PLAT_WIN) ? POOM_NET_AUDIT_FIT_YES : POOM_NET_AUDIT_FIT_NO;
    case POOM_NET_AUDIT_OS_LINUX:
        if (pc == NA_PLAT_LINUX) { return POOM_NET_AUDIT_FIT_YES; }
        return (pc == NA_PLAT_ANDROID || pc == NA_PLAT_UNIX || pc == NA_PLAT_HW)
                   ? POOM_NET_AUDIT_FIT_NEUTRAL : POOM_NET_AUDIT_FIT_NO;
    case POOM_NET_AUDIT_OS_ANDROID:
        if (pc == NA_PLAT_ANDROID) { return POOM_NET_AUDIT_FIT_YES; }
        return (pc == NA_PLAT_LINUX || pc == NA_PLAT_HW) ? POOM_NET_AUDIT_FIT_NEUTRAL
                                                         : POOM_NET_AUDIT_FIT_NO;
    case POOM_NET_AUDIT_OS_BSD:
        if (pc == NA_PLAT_BSD) { return POOM_NET_AUDIT_FIT_YES; }
        return (pc == NA_PLAT_UNIX || pc == NA_PLAT_HW) ? POOM_NET_AUDIT_FIT_NEUTRAL
                                                        : POOM_NET_AUDIT_FIT_NO;
    case POOM_NET_AUDIT_OS_MACOS:
        if (pc == NA_PLAT_MAC) { return POOM_NET_AUDIT_FIT_YES; }
        return (pc == NA_PLAT_UNIX || pc == NA_PLAT_BSD) ? POOM_NET_AUDIT_FIT_NEUTRAL
                                                         : POOM_NET_AUDIT_FIT_NO;
    case POOM_NET_AUDIT_OS_IOS:
        if (pc == NA_PLAT_IOS) { return POOM_NET_AUDIT_FIT_YES; }
        return (pc == NA_PLAT_MAC) ? POOM_NET_AUDIT_FIT_NEUTRAL : POOM_NET_AUDIT_FIT_NO;
    case POOM_NET_AUDIT_OS_HARDWARE:
        if (pc == NA_PLAT_HW) { return POOM_NET_AUDIT_FIT_YES; }
        return (pc == NA_PLAT_WIN || pc == NA_PLAT_MAC || pc == NA_PLAT_IOS)
                   ? POOM_NET_AUDIT_FIT_NO : POOM_NET_AUDIT_FIT_NEUTRAL;
    case POOM_NET_AUDIT_OS_POSIX:
    default:
        return (pc == NA_PLAT_WIN) ? POOM_NET_AUDIT_FIT_NO : POOM_NET_AUDIT_FIT_NEUTRAL;
    }
}

/* Is net[i] (as local term t) already in out[0..k)? */
static bool net_audit_projected_(const poom_net_audit_match_t *out, size_t k,
                                 const poom_net_audit_match_t *m, uint8_t t)
{
    for (size_t j = 0; j < k; ++j)
    {
        if (out[j].term_id == t && strcmp(out[j].edb_id, m->edb_id) == 0) { return true; }
    }
    return false;
}

size_t poom_net_audit_project(const poom_net_audit_match_t *net, size_t net_n,
                              const uint8_t *local_to_net, uint8_t n_local,
                              uint8_t os, poom_net_audit_match_t *out, size_t cap)
{
    if (net == NULL || local_to_net == NULL || out == NULL || cap == 0U)
    {
        return 0U;
    }
    size_t k = 0;

    /* Round 1: every local term with any match keeps its single best lead, so
     * a small cap can never make a matched service look like "no match". */
    for (uint8_t t = 0; t < n_local && k < cap; ++t)
    {
        int best = -1;
        for (size_t i = 0; i < net_n; ++i)
        {
            if (net[i].term_id != local_to_net[t]) { continue; }
            if (poom_net_audit_platform_fit(net[i].platform, os) == POOM_NET_AUDIT_FIT_NO) { continue; }
            if (best < 0 || net[i].tier > net[best].tier) { best = (int)i; }
        }
        if (best >= 0)
        {
            out[k] = net[best];
            out[k].term_id = t;
            k++;
        }
    }

    /* Round 2: fill the rest strongest tier first. */
    for (int tier = (int)POOM_NET_AUDIT_TIER_STRONG; tier >= (int)POOM_NET_AUDIT_TIER_WEAK; --tier)
    {
        for (uint8_t t = 0; t < n_local; ++t)
        {
            for (size_t i = 0; i < net_n; ++i)
            {
                if (net[i].term_id != local_to_net[t] || (int)net[i].tier != tier) { continue; }
                if (poom_net_audit_platform_fit(net[i].platform, os) == POOM_NET_AUDIT_FIT_NO) { continue; }
                if (net_audit_projected_(out, k, &net[i], t)) { continue; }
                if (k >= cap) { return k; }
                out[k] = net[i];
                out[k].term_id = t;
                k++;
            }
        }
    }
    return k;
}
