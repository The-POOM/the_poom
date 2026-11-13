// SPDX-License-Identifier: MIT
// Host tests for poom_net_audit_logic.c:
//   cc -std=c11 -Wall -Wextra -Iinclude -Itest test/test_logic.c poom_net_audit_logic.c -o t && ./t
#include "poom_net_audit.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ printf("FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); g_fail++; } \
                         else printf("ok:   %s\n", m); } while(0)

/* A realistic files_exploits.csv row. Title is quoted and contains a comma;
 * codes holds a CVE. Columns: id,file,description,date,author,type,platform,
 * port,date_added,date_updated,verified,codes,... */
static const char *k_row =
    "52345,\"exploits/php/webapps/52345.txt\","
    "\"WordPress Plugin Foo 1.2.3 - SQL Injection, Auth Bypass\","
    "\"2024-01-01\",\"jdoe\",\"webapps\",\"php\",\"\",\"2024-01-01\",\"\",\"\","
    "\"CVE-2024-1234;OSVDB-99999\",\"\",\"\",\"\",\"\"\n";

/* A row whose title contains an escaped double-quote. */
static const char *k_row_q =
    "100,\"a/b.txt\",\"Acme \"\"Pro\"\" Server 2.0 - RCE\",\"d\",\"a\","
    "\"remote\",\"windows\",\"\",\"\",\"\",\"\",\"\",\"\"\n";

static void test_fields(void)
{
    char f[128];
    poom_net_audit_csv_field(k_row, 0, f, sizeof(f));
    CHECK(strcmp(f, "52345") == 0, "field 0 id");
    poom_net_audit_csv_field(k_row, 2, f, sizeof(f));
    CHECK(strcmp(f, "WordPress Plugin Foo 1.2.3 - SQL Injection, Auth Bypass") == 0,
          "field 2 title (embedded comma)");
    poom_net_audit_csv_field(k_row, 5, f, sizeof(f));
    CHECK(strcmp(f, "webapps") == 0, "field 5 type");
    poom_net_audit_csv_field(k_row, 6, f, sizeof(f));
    CHECK(strcmp(f, "php") == 0, "field 6 platform");
    poom_net_audit_csv_field(k_row, 11, f, sizeof(f));
    CHECK(strcmp(f, "CVE-2024-1234;OSVDB-99999") == 0, "field 11 codes");
    /* out-of-range field -> empty */
    CHECK(poom_net_audit_csv_field(k_row, 99, f, sizeof(f)) == 0 && f[0] == '\0', "field oob empty");

    poom_net_audit_csv_field(k_row_q, 2, f, sizeof(f));
    CHECK(strcmp(f, "Acme \"Pro\" Server 2.0 - RCE") == 0, "escaped-quote title");
}

static void test_match(void)
{
    poom_net_audit_match_t m;

    CHECK(poom_net_audit_match_line(k_row, "wordpress", &m), "match wordpress");
    CHECK(strcmp(m.edb_id, "52345") == 0, "match id");
    CHECK(strstr(m.title, "WordPress Plugin Foo") != NULL, "match title");
    CHECK(strcmp(m.type, "webapps") == 0, "match type");
    CHECK(strcmp(m.platform, "php") == 0, "match platform");
    CHECK(strcmp(m.cve, "CVE-2024-1234") == 0, "match first cve");

    CHECK(poom_net_audit_match_line(k_row, "FOO", &m), "match case-insensitive");
    CHECK(!poom_net_audit_match_line(k_row, "nginx", &m), "no match nginx");
    CHECK(!poom_net_audit_match_line(k_row, "", &m), "empty term no match");

    /* a row with no CVE leaves cve empty */
    CHECK(poom_net_audit_match_line(k_row_q, "acme", &m), "match acme");
    CHECK(m.cve[0] == '\0', "no cve -> empty");
}

static void test_wholeword(void)
{
    CHECK(poom_net_audit_title_has_word("Apache HTTP Server 2.4", "apache"), "word apache");
    CHECK(!poom_net_audit_title_has_word("ApacheBench tool", "apache"), "no partial apachebench");
    CHECK(poom_net_audit_title_has_word("nginx 1.24.0 - X", "NGINX"), "word case-insensitive");
    CHECK(!poom_net_audit_title_has_word("mynginxproxy", "nginx"), "no substring mynginxproxy");
}

static void test_classify(void)
{
    CHECK(poom_net_audit_classify("nginx 1.24.0 - RCE", "1.24.0") == POOM_NET_AUDIT_TIER_STRONG, "strong exact ver");
    CHECK(poom_net_audit_classify("nginx 1.24.7 - RCE", "1.24.0") == POOM_NET_AUDIT_TIER_STRONG, "strong major.minor");
    CHECK(poom_net_audit_classify("nginx 1.18 - XSS", "9.9.9") == POOM_NET_AUDIT_TIER_MEDIUM, "medium some version");
    CHECK(poom_net_audit_classify("nginx - generic issue", "1.24.0") == POOM_NET_AUDIT_TIER_WEAK, "weak no version");
    CHECK(poom_net_audit_classify("Foo 2.0 bar", NULL) == POOM_NET_AUDIT_TIER_MEDIUM, "medium null ver w/ token");
    CHECK(poom_net_audit_classify("Foo bar", "") == POOM_NET_AUDIT_TIER_WEAK, "weak empty ver");
}

/* Build a minimal CSV line with a given title (other fields empty). */
static void mkline(char *buf, size_t cap, const char *id, const char *title)
{
    snprintf(buf, cap, "%s,\"f\",\"%s\",\"d\",\"a\",\"webapps\",\"php\",\"\",\"\",\"\",\"\",\"\"\n",
             id, title);
}

static void test_collector(void)
{
    poom_net_audit_match_t m[4];
    poom_net_audit_collector_t c;
    poom_net_audit_term_t terms[2] = {
        {.term = "nginx", .version = "1.24.0"},
        {.term = "apache", .version = NULL},
    };
    char line[256];

    /* cap 4, per-term cap 2 */
    poom_net_audit_collector_init(&c, m, 4, 2);
    mkline(line, sizeof(line), "1", "nginx - weak"); poom_net_audit_collect_line(&c, line, terms, 2);
    mkline(line, sizeof(line), "2", "nginx 1.5 - medium"); poom_net_audit_collect_line(&c, line, terms, 2);
    /* third nginx exceeds per-term cap(2); it's STRONG so must evict the WEAK one */
    mkline(line, sizeof(line), "3", "nginx 1.24.0 - strong"); poom_net_audit_collect_line(&c, line, terms, 2);
    /* count stays 2 for nginx */
    int nginx = 0, strong = 0, weak = 0;
    for (uint16_t i = 0; i < c.count; ++i)
    {
        if (c.matches[i].term_id == 0) { nginx++; }
        if (c.matches[i].tier == POOM_NET_AUDIT_TIER_STRONG) { strong++; }
        if (c.matches[i].tier == POOM_NET_AUDIT_TIER_WEAK) { weak++; }
    }
    CHECK(c.count == 2, "collector count 2 after per-term cap");
    CHECK(nginx == 2, "nginx capped at per-term 2");
    CHECK(strong == 1, "strong retained");
    CHECK(weak == 0, "weak evicted by strong");

    /* apache (term 1) matches add independently up to per-term cap */
    mkline(line, sizeof(line), "4", "apache 2.0 - a"); poom_net_audit_collect_line(&c, line, terms, 2);
    mkline(line, sizeof(line), "5", "apache 2.2 - b"); poom_net_audit_collect_line(&c, line, terms, 2);
    CHECK(c.count == 4, "apache fills to global cap 4");
    mkline(line, sizeof(line), "6", "nothing here"); poom_net_audit_collect_line(&c, line, terms, 2);
    CHECK(c.count == 4, "no match line ignored");
}

static void test_net_helpers(void)
{
    /* intern: de-dup case-insensitive product, exact version */
    poom_net_audit_prodver_t tab[3];
    uint8_t n = 0;
    CHECK(poom_net_audit_intern(tab, &n, 3, "", "1") == 0, "intern empty product -> 0");
    CHECK(poom_net_audit_intern(tab, &n, 3, NULL, "1") == 0, "intern NULL product -> 0");
    CHECK(poom_net_audit_intern(tab, &n, 3, "nginx", "1.24.0") == 1, "intern nginx -> 1");
    CHECK(poom_net_audit_intern(tab, &n, 3, "NGINX", "1.24.0") == 1, "intern NGINX same id");
    CHECK(poom_net_audit_intern(tab, &n, 3, "nginx", "1.18.0") == 2, "intern other version -> 2");
    CHECK(poom_net_audit_intern(tab, &n, 3, "OpenSSH", NULL) == 3, "intern NULL version -> 3");
    CHECK(strcmp(tab[2].version, "") == 0, "NULL version stored empty");
    CHECK(poom_net_audit_intern(tab, &n, 3, "OpenSSH", "") == 3, "empty == NULL version");
    CHECK(poom_net_audit_intern(tab, &n, 3, "lighttpd", "1.4") == POOM_NET_AUDIT_INTERN_FULL,
          "intern full -> FULL");
    CHECK(n == 3, "intern count 3");

    /* long strings are truncated safely */
    poom_net_audit_prodver_t t2[1];
    uint8_t n2 = 0;
    CHECK(poom_net_audit_intern(t2, &n2, 1, "AVeryLongProductNameThatOverflowsTheCap", "1.2.3.4.5.6.7.8.9.10.11.12") == 1,
          "intern long -> 1");
    CHECK(strlen(t2[0].product) == POOM_NET_AUDIT_PROD_CAP - 1, "product truncated");
    CHECK(strlen(t2[0].version) == POOM_NET_AUDIT_VER_CAP - 1, "version truncated");

    /* count_terms */
    poom_net_audit_match_t m[5];
    memset(m, 0, sizeof(m));
    for (int i = 0; i < 5; ++i) { snprintf(m[i].edb_id, sizeof(m[i].edb_id), "%d", 100 + i); }
    m[0].term_id = 0; m[0].tier = POOM_NET_AUDIT_TIER_WEAK;
    m[1].term_id = 2; m[1].tier = POOM_NET_AUDIT_TIER_STRONG;
    m[2].term_id = 0; m[2].tier = POOM_NET_AUDIT_TIER_STRONG;
    m[3].term_id = 0; m[3].tier = POOM_NET_AUDIT_TIER_MEDIUM;
    m[4].term_id = 9; m[4].tier = POOM_NET_AUDIT_TIER_STRONG; /* out of range */
    uint8_t counts[3];
    poom_net_audit_count_terms(m, 5, counts, 3);
    CHECK(counts[0] == 3 && counts[1] == 0 && counts[2] == 1, "count_terms per term");

    /* host_class */
    uint8_t h_pot[3]   = {0, 3, 2};   /* term 2 (id 3) has hits */
    uint8_t h_nom[2]   = {2, 0};      /* term 1 (id 2) no hits */
    uint8_t h_una[2]   = {0, POOM_NET_AUDIT_INTERN_FULL};
    CHECK(poom_net_audit_host_class(h_pot, 3, counts, 3) == POOM_NET_AUDIT_HOST_POTENTIAL, "class potential");
    CHECK(poom_net_audit_host_class(h_nom, 2, counts, 3) == POOM_NET_AUDIT_HOST_NOMATCH, "class no match");
    CHECK(poom_net_audit_host_class(h_una, 2, counts, 3) == POOM_NET_AUDIT_HOST_UNASSESSED, "class unassessed");
    CHECK(poom_net_audit_host_class(h_una, 0, counts, 3) == POOM_NET_AUDIT_HOST_NONE, "class none (0 ports)");
    CHECK(poom_net_audit_host_class(h_pot, 3, NULL, 0) == POOM_NET_AUDIT_HOST_NOMATCH, "class no counts -> nomatch");

    /* project: local term 0 -> net 0, local 1 -> net 2; strongest first */
    uint8_t map[2] = {0, 2};
    poom_net_audit_match_t out[8];
    size_t k = poom_net_audit_project(m, 5, map, 2, POOM_NET_AUDIT_OS_UNKNOWN, out, 8);
    CHECK(k == 4, "project copies 4 (skips unmapped term 9)");
    CHECK(out[0].tier == POOM_NET_AUDIT_TIER_STRONG && out[1].tier == POOM_NET_AUDIT_TIER_STRONG,
          "project best-per-term first");
    CHECK(out[2].tier == POOM_NET_AUDIT_TIER_MEDIUM && out[3].tier == POOM_NET_AUDIT_TIER_WEAK,
          "project then strongest-first");
    int l0 = 0, l1 = 0;
    for (size_t i = 0; i < k; ++i) { if (out[i].term_id == 0) l0++; if (out[i].term_id == 1) l1++; }
    CHECK(l0 == 3 && l1 == 1, "project remaps term ids");
    k = poom_net_audit_project(m, 5, map, 2, POOM_NET_AUDIT_OS_UNKNOWN, out, 2);
    CHECK(k == 2 && out[0].tier == POOM_NET_AUDIT_TIER_STRONG && out[1].tier == POOM_NET_AUDIT_TIER_STRONG,
          "project small cap keeps strongest");
    /* a weak-only term is not crowded out by a strong-rich one */
    poom_net_audit_match_t w[4];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < 4; ++i) { snprintf(w[i].edb_id, sizeof(w[i].edb_id), "%d", 200 + i); w[i].term_id = 0; w[i].tier = POOM_NET_AUDIT_TIER_STRONG; }
    w[3].term_id = 1; w[3].tier = POOM_NET_AUDIT_TIER_WEAK;
    uint8_t map01[2] = {0, 1};
    k = poom_net_audit_project(w, 4, map01, 2, POOM_NET_AUDIT_OS_UNKNOWN, out, 2);
    CHECK(k == 2 && out[1].term_id == 1 && out[1].tier == POOM_NET_AUDIT_TIER_WEAK,
          "project keeps >=1 per matched term");
    k = poom_net_audit_project(w, 4, map01, 2, POOM_NET_AUDIT_OS_UNKNOWN, out, 8);
    CHECK(k == 4, "project no duplicates across rounds");
    /* two local terms mapping to the same net term duplicate (per-port view) */
    uint8_t dup[2] = {2, 2};
    k = poom_net_audit_project(m, 5, dup, 2, POOM_NET_AUDIT_OS_UNKNOWN, out, 8);
    CHECK(k == 2 && out[0].term_id == 0 && out[1].term_id == 1, "project duplicate mapping");
}

static void test_platform_fit(void)
{
    CHECK(poom_net_audit_platform_fit("windows", POOM_NET_AUDIT_OS_WINDOWS) == POOM_NET_AUDIT_FIT_YES, "win on win");
    CHECK(poom_net_audit_platform_fit("windows_x86-64", POOM_NET_AUDIT_OS_LINUX) == POOM_NET_AUDIT_FIT_NO, "win on linux = no");
    CHECK(poom_net_audit_platform_fit("linux_mips", POOM_NET_AUDIT_OS_HARDWARE) == POOM_NET_AUDIT_FIT_NEUTRAL, "linux on hw = neutral");
    CHECK(poom_net_audit_platform_fit("hardware", POOM_NET_AUDIT_OS_HARDWARE) == POOM_NET_AUDIT_FIT_YES, "hw on hw");
    CHECK(poom_net_audit_platform_fit("hardware", POOM_NET_AUDIT_OS_WINDOWS) == POOM_NET_AUDIT_FIT_NO, "hw on win = no");
    CHECK(poom_net_audit_platform_fit("php", POOM_NET_AUDIT_OS_WINDOWS) == POOM_NET_AUDIT_FIT_NEUTRAL, "php neutral");
    CHECK(poom_net_audit_platform_fit("multiple", POOM_NET_AUDIT_OS_IOS) == POOM_NET_AUDIT_FIT_NEUTRAL, "multiple neutral");
    CHECK(poom_net_audit_platform_fit("", POOM_NET_AUDIT_OS_LINUX) == POOM_NET_AUDIT_FIT_NEUTRAL, "empty neutral");
    CHECK(poom_net_audit_platform_fit("Linux", POOM_NET_AUDIT_OS_LINUX) == POOM_NET_AUDIT_FIT_YES, "case-insensitive");
    CHECK(poom_net_audit_platform_fit("windows", POOM_NET_AUDIT_OS_UNKNOWN) == POOM_NET_AUDIT_FIT_NEUTRAL, "unknown os neutral");
    CHECK(poom_net_audit_platform_fit("windows", POOM_NET_AUDIT_OS_POSIX) == POOM_NET_AUDIT_FIT_NO, "win on posix = no");
    CHECK(poom_net_audit_platform_fit("linux", POOM_NET_AUDIT_OS_POSIX) == POOM_NET_AUDIT_FIT_NEUTRAL, "linux on posix neutral");
    CHECK(poom_net_audit_platform_fit("osx", POOM_NET_AUDIT_OS_MACOS) == POOM_NET_AUDIT_FIT_YES, "osx on mac");
    CHECK(poom_net_audit_platform_fit("ios", POOM_NET_AUDIT_OS_MACOS) == POOM_NET_AUDIT_FIT_NO, "ios on mac = no");
    CHECK(poom_net_audit_platform_fit("android", POOM_NET_AUDIT_OS_LINUX) == POOM_NET_AUDIT_FIT_NEUTRAL, "android on linux neutral");
    CHECK(poom_net_audit_platform_fit("solaris", POOM_NET_AUDIT_OS_BSD) == POOM_NET_AUDIT_FIT_NEUTRAL, "unix on bsd neutral");

    /* projection drops misfits for a known OS, keeps them when unknown */
    poom_net_audit_match_t m[3];
    memset(m, 0, sizeof(m));
    snprintf(m[0].edb_id, sizeof(m[0].edb_id), "1"); snprintf(m[0].platform, sizeof(m[0].platform), "windows");
    snprintf(m[1].edb_id, sizeof(m[1].edb_id), "2"); snprintf(m[1].platform, sizeof(m[1].platform), "linux");
    snprintf(m[2].edb_id, sizeof(m[2].edb_id), "3"); snprintf(m[2].platform, sizeof(m[2].platform), "php");
    uint8_t map[1] = {0};
    poom_net_audit_match_t out[4];
    CHECK(poom_net_audit_project(m, 3, map, 1, POOM_NET_AUDIT_OS_LINUX, out, 4) == 2, "project drops windows on linux");
    CHECK(poom_net_audit_project(m, 3, map, 1, POOM_NET_AUDIT_OS_UNKNOWN, out, 4) == 3, "project keeps all when unknown");
}

int main(void)
{
    test_fields();
    test_match();
    test_wholeword();
    test_classify();
    test_collector();
    test_net_helpers();
    test_platform_fit();
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
