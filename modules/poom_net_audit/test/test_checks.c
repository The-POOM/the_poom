// SPDX-License-Identifier: MIT
// Host tests for the active read-only web checks (poom_net_audit_checks.c):
//   cc -std=gnu11 -Wall -Wextra -Iinclude -Itest test/test_checks.c poom_net_audit_checks.c -o t && ./t
#include "poom_net_audit.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (%s:%d)\n", m, __FILE__, __LINE__); g_fail++; } \
                         else { printf("ok:   %s\n", m); } } while (0)

static const poom_net_audit_check_t *by_id(const char *id)
{
    for (size_t i = 0; i < poom_net_audit_check_count(); ++i)
    {
        if (strcmp(poom_net_audit_check(i)->id, id) == 0) { return poom_net_audit_check(i); }
    }
    return NULL;
}

int main(void)
{
    /* table sanity: NUL-terminated fields, GET paths, fits the mask */
    CHECK(poom_net_audit_check_count() <= POOM_NET_AUDIT_CHECKS_MAX, "checks fit the mask");
    for (size_t i = 0; i < poom_net_audit_check_count(); ++i)
    {
        const poom_net_audit_check_t *c = poom_net_audit_check(i);
        bool ok = memchr(c->id, 0, sizeof(c->id)) && memchr(c->path, 0, sizeof(c->path)) &&
                  memchr(c->match, 0, sizeof(c->match)) && memchr(c->title, 0, sizeof(c->title)) &&
                  memchr(c->detail, 0, sizeof(c->detail));
        CHECK(ok && c->path[0] == '/' && c->match[0] != '\0', "check fields valid");
    }
    CHECK(poom_net_audit_check(99) == NULL, "out of range -> NULL");

    CHECK(poom_net_audit_http_status("HTTP/1.1 200 OK\r\n", 17) == 200, "status 200");
    CHECK(poom_net_audit_http_status("HTTP/1.0 404 Not Found\r\n", 24) == 404, "status 404");
    CHECK(poom_net_audit_http_status("SSH-2.0-OpenSSH\r\n", 17) == -1, "not http");
    CHECK(poom_net_audit_http_status("HTTP/1.1 ", 9) == -1, "truncated");

    CHECK(poom_net_audit_text_has_any("xx DIRECTORY LISTING FOR /", 26, "Index of /|Directory listing for"),
          "alternatives, case-insensitive");
    CHECK(!poom_net_audit_text_has_any("hello", 5, "Index of /|nope"), "no alternative present");
    CHECK(!poom_net_audit_text_has_any("Index of", 8, "Index of /"), "bounded by len");
    const char nonul[5] = {'a','b','c','d','e'};
    CHECK(poom_net_audit_text_has_any(nonul, 5, "cde") && !poom_net_audit_text_has_any(nonul, 4, "cde"),
          "works on non-NUL-terminated buffers");

    const poom_net_audit_check_t *dl = by_id("dirlist");
    const poom_net_audit_check_t *git = by_id("git");
    CHECK(dl != NULL && git != NULL, "built-ins present");

    const char *py = "HTTP/1.0 200 OK\r\nServer: SimpleHTTP/0.6 Python/3.12\r\n\r\n"
                     "<html><head><title>Directory listing for /</title>";
    const char *b404 = "HTTP/1.0 404 File not found\r\n\r\nNope";
    CHECK(poom_net_audit_check_match(dl, py, strlen(py), b404, strlen(b404)), "python http.server listing hit");
    CHECK(poom_net_audit_check_match(dl, py, strlen(py), NULL, 0), "hit without baseline");

    const char *e404 = "HTTP/1.1 404 Not Found\r\n\r\nIndex of /";
    CHECK(!poom_net_audit_check_match(dl, e404, strlen(e404), NULL, 0), "404 never hits");
    const char *head = "HTTP/1.1 200 OK\r\n\r\nref: refs/heads/main\n";
    CHECK(poom_net_audit_check_match(git, head, strlen(head), b404, strlen(b404)), ".git/HEAD hit");
    const char *plain = "HTTP/1.1 200 OK\r\n\r\n<html>Router login</html>";
    CHECK(!poom_net_audit_check_match(git, plain, strlen(plain), b404, strlen(b404)), "no text -> no hit");

    /* soft-404: device serves the same page (that happens to contain the
     * text) for any path -> never a hit */
    const char *same = "HTTP/1.1 200 OK\r\n\r\n<html>Index of / welcome</html>";
    CHECK(!poom_net_audit_check_match(dl, same, strlen(same), same, strlen(same)), "soft-404 guard");
    /* baseline 200 but without the text -> still a real hit */
    CHECK(poom_net_audit_check_match(dl, py, strlen(py), plain, strlen(plain)), "baseline 200 w/o text keeps hit");

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
