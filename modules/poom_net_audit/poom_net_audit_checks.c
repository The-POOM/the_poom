// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Active read-only web checks (Phase D, "Confirmed" tier). Each check is one
// GET of a fixed path on a device's web port plus a text match on the reply -
// the Nuclei idea reduced to its safest form: GET only, no payloads, no
// logins, nothing stored but the hit itself. A soft-404 guard compares with
// the reply to a random nonsense path so devices that answer every URL with
// the same page cannot produce false hits.
//
// To add a check, append to k_checks[] (max POOM_NET_AUDIT_CHECKS_MAX) and
// rebuild; test/test_checks.c verifies every field fits its array.

#include "poom_net_audit.h"

#include <ctype.h>
#include <string.h>

static const poom_net_audit_check_t k_checks[] = {
    {"dirlist", "/", 0, "Index of /|Directory listing for",
     "Directory listing on", "The web server lists its files to anyone on the LAN."},
    {"git", "/.git/HEAD", 0, "ref: refs/",
     "Git repo exposed", "Source code and its history can be downloaded."},
    {"apachestat", "/server-status", 0, "Apache Server Status",
     "Apache status page open", "Shows live requests and client addresses."},
    {"nginxstat", "/nginx_status", 0, "Active connections:",
     "nginx status page open", "Shows live server connection statistics."},
    {"phpinfo", "/phpinfo.php", 0, "phpinfo()",
     "phpinfo page exposed", "Reveals server configuration, paths and modules."},
    {"dotenv", "/.env", 0, "APP_KEY=|DB_PASSWORD=|SECRET_KEY=",
     "Config file (.env) open", "An application secrets file is readable over HTTP."},
};

size_t poom_net_audit_check_count(void)
{
    return sizeof(k_checks) / sizeof(k_checks[0]);
}

const poom_net_audit_check_t *poom_net_audit_check(size_t i)
{
    return (i < poom_net_audit_check_count()) ? &k_checks[i] : NULL;
}

int poom_net_audit_http_status(const char *resp, size_t len)
{
    if (resp == NULL || len < 12U || strncmp(resp, "HTTP/", 5) != 0) { return -1; }
    size_t i = 5;
    while (i < len && resp[i] != ' ') { i++; }          /* skip "1.1" */
    while (i < len && resp[i] == ' ') { i++; }
    if (i + 3U > len || !isdigit((unsigned char)resp[i]) || !isdigit((unsigned char)resp[i + 1U]) ||
        !isdigit((unsigned char)resp[i + 2U]))
    {
        return -1;
    }
    return (resp[i] - '0') * 100 + (resp[i + 1U] - '0') * 10 + (resp[i + 2U] - '0');
}

static bool na_find_ci_(const char *hay, size_t len, const char *needle, size_t nl)
{
    if (nl == 0U || nl > len) { return false; }
    for (size_t i = 0; i + nl <= len; ++i)
    {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)needle[k])) { k++; }
        if (k == nl) { return true; }
    }
    return false;
}

bool poom_net_audit_text_has_any(const char *hay, size_t len, const char *alts)
{
    if (hay == NULL || alts == NULL) { return false; }
    const char *p = alts;
    while (*p != '\0')
    {
        const char *e = strchr(p, '|');
        size_t n = (e != NULL) ? (size_t)(e - p) : strlen(p);
        if (na_find_ci_(hay, len, p, n)) { return true; }
        if (e == NULL) { break; }
        p = e + 1;
    }
    return false;
}

bool poom_net_audit_check_match(const poom_net_audit_check_t *c, const char *resp, size_t len,
                                const char *baseline, size_t blen)
{
    if (c == NULL || resp == NULL || c->match[0] == '\0') { return false; }
    int st = poom_net_audit_http_status(resp, len);
    if (c->status != 0U) { if (st != (int)c->status) { return false; } }
    else if (st < 200 || st > 299) { return false; }
    if (!poom_net_audit_text_has_any(resp, len, c->match)) { return false; }

    /* soft-404: the same tell-tale text on a nonsense path means the device
     * just serves one page for everything */
    if (baseline != NULL && blen > 0U)
    {
        int bst = poom_net_audit_http_status(baseline, blen);
        if (bst >= 200 && bst <= 299 && poom_net_audit_text_has_any(baseline, blen, c->match))
        {
            return false;
        }
    }
    return true;
}
