// SPDX-License-Identifier: MIT
// Host tests for the exposure rule engine (poom_net_audit_rules.c):
//   cc -std=gnu11 -Wall -Wextra -Iinclude -Itest test/test_rules.c poom_net_audit_rules.c -o t && ./t
#include "poom_net_audit.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); g_fail++; } \
                         else { printf("ok:   %s\n", m); } } while (0)

static void test_exposures(void)
{
    CHECK(poom_net_audit_version_cmp("7.2p2", "7.4") < 0, "ver 7.2p2 < 7.4");
    CHECK(poom_net_audit_version_cmp("8.9p1", "7.4") > 0, "ver 8.9p1 > 7.4");
    CHECK(poom_net_audit_version_cmp("7.4", "7.4") == 0, "ver equal");
    CHECK(poom_net_audit_version_cmp("10.0", "9.9") > 0, "ver numeric not lexical");
    CHECK(poom_net_audit_version_cmp("", "7.4") < 0, "empty version is lowest");

    poom_net_audit_svc_t a[] = {
        {23, "telnet", NULL, NULL},
        {22, "ssh", "OpenSSH", "6.6.1p1"},
        {80, "http", "lighttpd", "1.4"},
        {3306, "mysql", "MySQL", "5.7.33"},
    };
    uint32_t m = poom_net_audit_exposures(a, 4, true);
    CHECK(m & (1U << POOM_NET_AUDIT_EXP_TELNET), "telnet finding");
    CHECK(m & (1U << POOM_NET_AUDIT_EXP_OLD_SSH), "old OpenSSH finding");
    CHECK(m & (1U << POOM_NET_AUDIT_EXP_HTTP_NO_TLS), "http without https");
    CHECK(m & (1U << POOM_NET_AUDIT_EXP_DB), "database reachable");
    CHECK(m & (1U << POOM_NET_AUDIT_EXP_UPNP), "upnp");
    CHECK(!(m & (1U << POOM_NET_AUDIT_EXP_FTP)), "no ftp");

    poom_net_audit_svc_t b[] = {
        {22, "ssh", "OpenSSH", "9.6p1"}, {80, "http", NULL, NULL}, {443, "https", NULL, NULL},
        {445, NULL, NULL, NULL}, {9100, NULL, NULL, NULL},
    };
    m = poom_net_audit_exposures(b, 5, false);
    CHECK(!(m & (1U << POOM_NET_AUDIT_EXP_OLD_SSH)), "current OpenSSH not flagged");
    CHECK(!(m & (1U << POOM_NET_AUDIT_EXP_HTTP_NO_TLS)), "http with https not flagged");
    CHECK((m & (1U << POOM_NET_AUDIT_EXP_SMB)) && (m & (1U << POOM_NET_AUDIT_EXP_RAW_PRINT)), "smb + raw print");
    CHECK(poom_net_audit_exposures(NULL, 0, false) == 0, "nothing observed -> no findings");
    poom_net_audit_svc_t c[] = {{22, "ssh", "OpenSSH", ""}};
    CHECK(poom_net_audit_exposures(c, 1, false) == 0, "unknown ssh version not flagged");
    CHECK(strcmp(poom_net_audit_exp_title(POOM_NET_AUDIT_EXP_TELNET), "Telnet enabled") == 0 &&
          poom_net_audit_exp_severity(POOM_NET_AUDIT_EXP_TELNET) == POOM_NET_AUDIT_SEV_DETECTED &&
          poom_net_audit_exp_severity(POOM_NET_AUDIT_EXP_UPNP) == POOM_NET_AUDIT_SEV_INFO, "titles/severity");
}


static void test_grammar(void)
{
    poom_net_audit_svc_t s[] = {
        {22, "ssh", "OpenSSH", "8.9p1"}, {80, "http", "nginx", "1.18.0"}, {3306, "mysql", "MySQL", ""},
    };
    CHECK(poom_net_audit_rule_eval("port:22", s, 3, false) == 1, "port hit");
    CHECK(poom_net_audit_rule_eval("port:23,2323", s, 3, false) == 0, "port list miss");
    CHECK(poom_net_audit_rule_eval("service:HTTP", s, 3, false) == 1, "service case-insensitive");
    CHECK(poom_net_audit_rule_eval("product:nginx<1.20", s, 3, false) == 1, "product version <");
    CHECK(poom_net_audit_rule_eval("product:nginx>=1.20", s, 3, false) == 0, "product version >=");
    CHECK(poom_net_audit_rule_eval("product:nginx=1.18.0", s, 3, false) == 1, "product version =");
    CHECK(poom_net_audit_rule_eval("product:MySQL<9", s, 3, false) == 0, "unknown version never matches");
    CHECK(poom_net_audit_rule_eval("product:MySQL", s, 3, false) == 1, "product without version");
    CHECK(poom_net_audit_rule_eval("port:22 & !port:23", s, 3, false) == 1, "and + not");
    CHECK(poom_net_audit_rule_eval("port:23 | port:80 & port:443", s, 3, false) == 0, "& binds tighter than |");
    CHECK(poom_net_audit_rule_eval("(port:23 | port:80) & !port:443", s, 3, false) == 1, "parentheses");
    CHECK(poom_net_audit_rule_eval("!!port:22", s, 3, false) == 1, "double negation");
    CHECK(poom_net_audit_rule_eval("upnp", s, 3, true) == 1 && poom_net_audit_rule_eval("upnp", s, 3, false) == 0, "upnp");
    CHECK(poom_net_audit_rule_eval("port:22", NULL, 0, false) == 0, "no services");
    /* syntax errors */
    const char *bad[] = {"", "port:", "port:70000", "port:0", "service:", "(port:22", "port:22)",
                         "port:22 &", "| port:22", "banana:1", "upnpx", "product:nginx<", "port:22 port:80",
                         "((((((((((((((((((port:22))))))))))))))))))"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        char m[64];
        snprintf(m, sizeof(m), "syntax error: \"%.30s\"", bad[i]);
        CHECK(poom_net_audit_rule_eval(bad[i], s, 3, false) == -1, m);
    }
}

static void test_builtins(void)
{
    for (size_t i = 0; i < poom_net_audit_builtin_count(); ++i)
    {
        const poom_net_audit_rule_t *b = poom_net_audit_builtin(i);
        /* every field must be NUL-terminated inside its array (a string that
         * exactly fills the array compiles silently on older GCC) */
        bool terminated = b != NULL && memchr(b->id, 0, sizeof(b->id)) && memchr(b->expr, 0, sizeof(b->expr)) &&
                          memchr(b->title, 0, sizeof(b->title)) && memchr(b->detail, 0, sizeof(b->detail));
        CHECK(terminated, "built-in rule fields fit their arrays");
        CHECK(terminated && poom_net_audit_rule_eval(b->expr, NULL, 0, false) >= 0, "built-in rule is valid syntax");
    }
    CHECK(poom_net_audit_builtin_count() <= POOM_NET_AUDIT_RULES_MAX, "built-ins fit the findings mask");
}

int main(void)
{
    test_exposures();
    test_grammar();
    test_builtins();
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
