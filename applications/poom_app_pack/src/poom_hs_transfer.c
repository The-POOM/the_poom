// SPDX-License-Identifier: MIT
/*
 * "Export Handshake over Wi-Fi" HTTP server.
 *
 * Trimmed copy of the proven wardrive transfer server
 * (modules/poom_wardrive/wardrive_transfer.c). The auth model (10-digit login
 * code, throttled login, separate 128-bit cookie, 10-minute expiry) and the
 * chunked file streaming are kept as-is; the content model is replaced: instead
 * of wardrive sessions it lists the .22000 / .pcap files under
 * /sdcard/pcaps/handshakes and serves them for download.
 */
#include "poom_hs_transfer.h"

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "sd_card.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define HS_TRANSFER_CODE_DIGITS 10
#define HS_DIR                  SD_CARD_PATH "/pcaps/handshakes"

static httpd_handle_t server;
static atomic_bool stopping;
static char secret[33];
static char cookie_secret[33];
static uint64_t expires;
static unsigned failures;
/* established: a client has authenticated, so the initial-connection timeout no
 * longer applies and the server stays up until it is stopped on the POOM.
 * locked: too many wrong codes were entered; stays locked until the server is
 * restarted (there is no timed auto-unlock). */
static bool established;
static bool locked;

static uint64_t now(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

/* The 10-minute window guards only the INITIAL connection: once a client has
 * authenticated it never expires on its own (esp_timer is monotonic uptime, and
 * the server is RAM-only so a reboot ends it outright regardless). */
static bool access_expired(void)
{
    return !established && now() >= expires;
}

bool poom_hs_transfer_expired(void)
{
    return server && access_expired();
}

bool poom_hs_transfer_locked(void)
{
    return server && locked;
}

/* Constant-time compare so a wrong code leaks no timing information. */
static bool equal(const char *a, const char *b, size_t n)
{
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++)
    {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    }
    return !diff;
}

/* Accept only a safe basename ending in .22000 or .pcap: no path separators,
 * no "..", restricted charset. Prevents directory traversal on /download. */
static bool safe_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 64)
    {
        return false;
    }
    bool ext = (n > 6 && !strcmp(s + n - 6, ".22000")) ||
               (n > 5 && !strcmp(s + n - 5, ".pcap"));
    if (!ext)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
        {
            return false;
        }
        if (c == '.' && i + 1 < n && s[i + 1] == '.')
        {
            return false;
        }
    }
    return true;
}

static bool authed(httpd_req_t *r)
{
    if (access_expired())
    {
        return false;
    }
    char cookie[192] = {0};
    if (httpd_req_get_hdr_value_str(r, "Cookie", cookie, sizeof(cookie)) != ESP_OK)
    {
        return false;
    }
    char *p = cookie;
    while (*p)
    {
        while (*p == ' ' || *p == ';')
        {
            p++;
        }
        char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n == 35 && !memcmp(p, "hs=", 3) && equal(p + 3, cookie_secret, 32))
        {
            return true;
        }
        if (!end)
        {
            break;
        }
        p = end + 1;
    }
    return false;
}

static esp_err_t deny(httpd_req_t *r)
{
    return httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED,
                               "Open / and enter the current 10-digit POOM code. "
                               "After too many wrong codes the export locks; "
                               "restart it on the POOM to try again.");
}

static esp_err_t login(httpd_req_t *r)
{
    const size_t length = 6 + HS_TRANSFER_CODE_DIGITS; /* "token=" + digits */
    if (locked || access_expired() || r->content_len != (int)length)
    {
        return deny(r);
    }
    char data[39] = {0};
    size_t got = 0;
    while (got < length)
    {
        int n = httpd_req_recv(r, data + got, length - got);
        if (n <= 0)
        {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    if (memcmp(data, "token=", 6) || !equal(data + 6, secret, HS_TRANSFER_CODE_DIGITS))
    {
        if (++failures >= 5)
        {
            locked = true; /* permanent until the export is restarted on the POOM */
        }
        return deny(r);
    }
    /* First successful login establishes the session: the initial-connection
     * timeout no longer applies. Max-Age is long so the cookie does not lapse
     * mid-transfer; the server, not the cookie, bounds validity (it dies on
     * stop/reboot and re-mints a new secret each start). */
    established = true;
    char cookie[96];
    snprintf(cookie, sizeof(cookie),
             "hs=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=86400", cookie_secret);
    httpd_resp_set_hdr(r, "Set-Cookie", cookie);
    httpd_resp_set_status(r, "303 See Other");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, "", 0);
}

/* Emit a download link for each dir entry with the given extension. */
static esp_err_t list_by_ext(httpd_req_t *r, const char *ext, unsigned *shown)
{
    DIR *d = opendir(HS_DIR);
    if (!d)
    {
        return ESP_OK;
    }
    struct dirent *e;
    esp_err_t result = ESP_OK;
    size_t el = strlen(ext);
    while ((e = readdir(d)))
    {
        size_t n = strlen(e->d_name);
        if (n <= el || strcmp(e->d_name + n - el, ext) || !safe_name(e->d_name))
        {
            continue;
        }
        /* No stat() per file: with 100+ captures (x2 for .pcap) a stat() each
         * made the page feel hung. The extension + safe_name filter is enough to
         * build a valid, traversal-safe download link; the %.64s precision keeps
         * -Wformat-truncation satisfied (safe_name bounds the name to <=64). */
        char chunk[192];
        snprintf(chunk, sizeof(chunk),
                 "<p><a href=\"/download?file=%.64s\">%.64s</a></p>",
                 e->d_name, e->d_name);
        if ((result = httpd_resp_sendstr_chunk(r, chunk)) != ESP_OK)
        {
            break;
        }
        (*shown)++;
    }
    closedir(d);
    return result;
}

static esp_err_t index_page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'none'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    if (!authed(r))
    {
        return httpd_resp_send(
            r,
            "<!doctype html><title>POOM Handshakes</title>"
            "<h1>POOM Handshake Export</h1>"
            "<p>HTTP does not encrypt the access code or the capture files. "
            "Use a trusted private network.</p>"
            "<form method=post action=/login><label>10-digit code on POOM "
            "<input name=token inputmode=numeric pattern=\"[0-9]{10}\" minlength=10 "
            "maxlength=10 required autocomplete=off></label>"
            "<button>Open handshakes</button></form>",
            HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t result = httpd_resp_sendstr_chunk(
        r,
        "<!doctype html><title>POOM Handshakes</title><h1>Captured handshakes</h1>"
        "<p>Hashcat <code>.22000</code> files first, then the raw <code>.pcap</code> "
        "captures. HTTP is unencrypted. The export stays open until it is stopped "
        "on the POOM.</p>"
        "<h2>Hashcat (.22000)</h2>");
    if (result != ESP_OK)
    {
        return result;
    }
    unsigned hc = 0, raw = 0;
    if ((result = list_by_ext(r, ".22000", &hc)) != ESP_OK)
    {
        return result;
    }
    if (hc == 0 &&
        (result = httpd_resp_sendstr_chunk(r, "<p>No .22000 files yet.</p>")) != ESP_OK)
    {
        return result;
    }
    if ((result = httpd_resp_sendstr_chunk(r, "<h2>Raw captures (.pcap)</h2>")) != ESP_OK)
    {
        return result;
    }
    if ((result = list_by_ext(r, ".pcap", &raw)) != ESP_OK)
    {
        return result;
    }
    if (raw == 0 &&
        (result = httpd_resp_sendstr_chunk(r, "<p>No .pcap files.</p>")) != ESP_OK)
    {
        return result;
    }
    return httpd_resp_send_chunk(r, NULL, 0);
}

static esp_err_t stream_file(httpd_req_t *r, const char *path, const char *filename)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "File unavailable");
    }
    char disposition[128];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(r, "Content-Disposition", disposition);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_type(r, "application/octet-stream");
    char b[512];
    size_t n;
    esp_err_t result = ESP_OK;
    while ((n = fread(b, 1, sizeof(b), f)))
    {
        if (access_expired() || (result = httpd_resp_send_chunk(r, b, n)) != ESP_OK)
        {
            result = ESP_FAIL;
            break;
        }
    }
    if (ferror(f))
    {
        result = ESP_FAIL;
    }
    fclose(f);
    if (result == ESP_OK)
    {
        result = httpd_resp_send_chunk(r, NULL, 0);
    }
    return result;
}

static esp_err_t download(httpd_req_t *r)
{
    if (!authed(r))
    {
        return deny(r);
    }
    char query[160], file[96];
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "file", file, sizeof(file)) != ESP_OK ||
        !safe_name(file))
    {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid file selection");
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", HS_DIR, file);
    return stream_file(r, path, file);
}

bool poom_hs_transfer_start(char token[33])
{
    if (server)
    {
        return false;
    }
    uint64_t number = 0;
    const uint64_t range = 10000000000ULL;
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % 10000000000ULL);
    bool generated = false;
    for (unsigned attempt = 0; attempt < 8; attempt++)
    {
        esp_fill_random(&number, sizeof(number));
        if (number < limit)
        {
            generated = true;
            break;
        }
    }
    if (!generated)
    {
        return false;
    }
    memset(secret, 0, sizeof(secret));
    snprintf(secret, sizeof(secret), "%010llu", (unsigned long long)(number % range));
    /* The short code is accepted only by the throttled login. Protected routes
     * require a separate 128-bit cookie, never an unthrottled numeric guess. */
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    for (unsigned i = 0; i < 16; i++)
    {
        snprintf(cookie_secret + i * 2, 3, "%02x", random[i]);
    }
    expires = now() + 600000; /* initial-connection window only */
    failures = 0;
    established = false;
    locked = false;
    atomic_store(&stopping, false);

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 3;
    c.max_open_sockets = 2;
    c.stack_size = 6144;
    c.lru_purge_enable = true;
    c.recv_wait_timeout = 3;
    c.send_wait_timeout = 3;
    if (httpd_start(&server, &c) != ESP_OK)
    {
        memset(secret, 0, sizeof(secret));
        memset(cookie_secret, 0, sizeof(cookie_secret));
        server = NULL;
        return false;
    }
    httpd_uri_t u = {.uri = "/", .method = HTTP_GET, .handler = index_page};
    bool ok = httpd_register_uri_handler(server, &u) == ESP_OK;
    u.uri = "/login";
    u.method = HTTP_POST;
    u.handler = login;
    ok = ok && httpd_register_uri_handler(server, &u) == ESP_OK;
    u.uri = "/download";
    u.method = HTTP_GET;
    u.handler = download;
    ok = ok && httpd_register_uri_handler(server, &u) == ESP_OK;
    if (!ok)
    {
        poom_hs_transfer_stop();
        return false;
    }
    memcpy(token, secret, 33);
    return true;
}

void poom_hs_transfer_stop(void)
{
    atomic_store(&stopping, true);
    if (server)
    {
        httpd_stop(server);
        server = NULL;
    }
    memset(secret, 0, sizeof(secret));
    memset(cookie_secret, 0, sizeof(cookie_secret));
}
