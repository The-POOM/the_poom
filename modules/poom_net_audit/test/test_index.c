// SPDX-License-Identifier: MIT
// End-to-end host test of the automatic Exploit-DB block index:
//   cc -std=gnu11 -Wall -Wextra -Iinclude -Itest test/test_index.c poom_net_audit_index.c poom_net_audit_logic.c -o t && ./t
// Generates a synthetic files_exploits.csv, then checks that an indexed search
// returns exactly what a full scan returns, that the index rebuilds when the
// CSV changes, and that a cancelled build leaves no index behind.
#include "poom_net_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); g_fail++; } \
                         else { printf("ok:   %s\n", m); } } while (0)

static char g_csv[256], g_idx[256], g_tmp[256];

static const char *k_vocab[] = {
    "WordPress", "Plugin", "Joomla", "Drupal", "Remote", "Code", "Execution", "SQL",
    "Injection", "Cross-Site", "Scripting", "Buffer", "Overflow", "Denial", "Service",
    "Apache", "Tomcat", "Microsoft", "Windows", "Linux", "Kernel", "Privilege",
    "Escalation", "Router", "Camera", "Authentication", "Bypass", "Directory", "Traversal",
    "Upload", "Arbitrary", "File", "Disclosure", "PHP", "CMS", "Manager", "Server",
};
static const char *k_products[] = {"nginx", "OpenSSH", "vsftpd", "R7000", "lighttpd", "Exim"};

static unsigned g_seed = 12345U;
static unsigned rnd(void) { g_seed = g_seed * 1103515245U + 12345U; return (g_seed >> 16) & 0x7FFFU; }

static void write_csv(int rows, int extra)
{
    FILE *f = fopen(g_csv, "wb");
    fprintf(f, "id,file,description,date_published,author,type,platform,port,date_added,"
               "date_updated,verified,codes,tags,aliases,screenshot_url,application_url\n");
    for (int i = 0; i < rows + extra; ++i)
    {
        char title[160] = {0};
        int words = 3 + (int)(rnd() % 6U);
        for (int w = 0; w < words; ++w)
        {
            const char *v = k_vocab[rnd() % (sizeof(k_vocab) / sizeof(k_vocab[0]))];
            strcat(title, w ? " " : "");
            strcat(title, v);
        }
        if (rnd() % 40U == 0U) /* sprinkle products, some with versions */
        {
            char p[48];
            snprintf(p, sizeof(p), " %s %u.%u", k_products[rnd() % 6U], rnd() % 9U, rnd() % 20U);
            strcat(title, p);
        }
        fprintf(f, "%d,\"exploits/x/%d.txt\",\"%s - Exploit\",2020-01-01,\"a\",remote,%s,,"
                   "2020-01-01,,1,\"CVE-2020-%04d\",,,,\n",
                i + 1, i + 1, title, (i % 3) ? "linux" : "windows", i % 10000);
    }
    fclose(f);
}

static size_t search(const char *idx, const char *tmp, poom_net_audit_match_t *m, size_t cap,
                     poom_net_audit_index_use_t *how, const volatile bool *cancel)
{
    static const poom_net_audit_term_t terms[] = {
        {"nginx", "1.5"}, {"OpenSSH", NULL}, {"vsftpd", NULL}, {"R7000", NULL},
        {"lighttpd", NULL}, {"Exim", "4.2"}, {"WordPress Plugin", NULL},
    };
    size_t n = 0;
    esp_err_t e = poom_net_audit_search_file(g_csv, idx, tmp, terms, 7, m, cap, 8, &n, how,
                                             cancel, NULL, NULL);
    if (e != ESP_OK) { printf("search error %d\n", e); }
    return n;
}

static int cmp_match(const void *a, const void *b)
{
    const poom_net_audit_match_t *x = a, *y = b;
    int c = strcmp(x->edb_id, y->edb_id);
    return c ? c : (int)x->term_id - (int)y->term_id;
}

static bool same_results(poom_net_audit_match_t *a, size_t na, poom_net_audit_match_t *b, size_t nb)
{
    if (na != nb) { return false; }
    qsort(a, na, sizeof(*a), cmp_match);
    qsort(b, nb, sizeof(*b), cmp_match);
    for (size_t i = 0; i < na; ++i)
    {
        if (cmp_match(&a[i], &b[i]) != 0 || a[i].tier != b[i].tier) { return false; }
    }
    return true;
}

static bool file_exists(const char *p) { return access(p, F_OK) == 0; }

int main(void)
{
    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(g_csv, sizeof(g_csv), "%s/poom_idx_test.csv", dir);
    snprintf(g_idx, sizeof(g_idx), "%s/poom_idx_test.idx", dir);
    snprintf(g_tmp, sizeof(g_tmp), "%s/poom_idx_test.idt", dir);
    remove(g_idx); remove(g_tmp);

    /* Bloom basics */
    uint8_t bits[POOM_NET_AUDIT_IDX_BITS / 8U] = {0};
    poom_net_audit_bloom_add_text(bits, "Apache Tomcat 9.0 - Remote Code Execution");
    CHECK(poom_net_audit_bloom_has_term(bits, "tomcat"), "bloom: word present (case-insensitive)");
    CHECK(poom_net_audit_bloom_has_term(bits, "Remote Code"), "bloom: multi-word term present");
    CHECK(!poom_net_audit_bloom_has_term(bits, "vsftpd"), "bloom: absent word rejected");
    CHECK(poom_net_audit_bloom_has_term(bits, "--"), "bloom: wordless term never rejects");

    write_csv(5000, 0);
    static poom_net_audit_match_t full[64], built[64], indexed[64];
    poom_net_audit_index_use_t how;

    size_t nf = search(NULL, NULL, full, 64, &how, NULL);
    CHECK(nf > 0 && how == POOM_NET_AUDIT_IDX_NONE, "reference full scan (no index)");
    CHECK(!poom_net_audit_index_current(g_csv, g_idx), "no index yet");

    size_t nb = search(g_idx, g_tmp, built, 64, &how, NULL);
    CHECK(how == POOM_NET_AUDIT_IDX_BUILT && file_exists(g_idx) && !file_exists(g_tmp),
          "first search builds the index (temp renamed)");
    CHECK(same_results(full, nf, built, nb), "building pass returns the same results");
    CHECK(poom_net_audit_index_current(g_csv, g_idx), "index current after build");

    size_t ni = search(g_idx, g_tmp, indexed, 64, &how, NULL);
    CHECK(how == POOM_NET_AUDIT_IDX_USED, "second search uses the index");
    nf = search(NULL, NULL, full, 64, &how, NULL);
    CHECK(same_results(full, nf, indexed, ni), "indexed results identical to a full scan");

    /* Small caps exercise tier-aware eviction: order must still agree. */
    static poom_net_audit_match_t f8[8], i8[8];
    size_t a = search(NULL, NULL, f8, 8, &how, NULL);
    size_t b = search(g_idx, g_tmp, i8, 8, &how, NULL);
    CHECK(how == POOM_NET_AUDIT_IDX_USED && same_results(f8, a, i8, b), "identical under tight caps");

    /* CSV changes -> stale index -> rebuilt on the next search. */
    sleep(1);
    write_csv(5000, 77);
    CHECK(!poom_net_audit_index_current(g_csv, g_idx), "index stale after CSV change");
    nb = search(g_idx, g_tmp, built, 64, &how, NULL);
    CHECK(how == POOM_NET_AUDIT_IDX_BUILT, "stale index rebuilt");
    nf = search(NULL, NULL, full, 64, &how, NULL);
    ni = search(g_idx, g_tmp, indexed, 64, &how, NULL);
    CHECK(how == POOM_NET_AUDIT_IDX_USED && same_results(full, nf, indexed, ni),
          "rebuilt index matches full scan");

    /* Corrupt (truncated) index -> falls back to a full pass and rebuilds. */
    FILE *ix = fopen(g_idx, "r+b");
    fseek(ix, 0, SEEK_END);
    long sz = ftell(ix);
    fclose(ix);
    if (truncate(g_idx, sz / 2) != 0) { printf("truncate failed\n"); }
    ni = search(g_idx, g_tmp, indexed, 64, &how, NULL);
    CHECK(how == POOM_NET_AUDIT_IDX_BUILT && same_results(full, nf, indexed, ni),
          "truncated index -> full pass + rebuild, same results");

    /* Cancelled build leaves no index and no temp file. */
    remove(g_idx);
    volatile bool cancel = true;
    (void)search(g_idx, g_tmp, built, 64, &how, &cancel);
    CHECK(!file_exists(g_idx) && !file_exists(g_tmp), "cancelled build leaves nothing behind");

    remove(g_csv); remove(g_idx); remove(g_tmp);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
