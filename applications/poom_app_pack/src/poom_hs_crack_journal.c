// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Persistent crack journal. See poom_hs_crack_journal.h.

#include "poom_hs_crack_journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "sd_card.h"

static const char *TAG = "hs_journal";

#define HS_JOURNAL_CSV   SD_CARD_PATH "/pcaps/handshakes/crack_journal.csv"
#define HS_JOURNAL_NS    "hs_crack"           /* NVS namespace                 */
#define HS_JOURNAL_LINE  (320U)               /* CSV read buffer               */
#define HS_JOURNAL_ESSID_MAX (64U)

/* ---- helpers ------------------------------------------------------------ */

static void hs_bssid_hex_(const uint8_t b[6], char out[13])
{
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 6; i++)
    {
        out[i * 2]     = h[(b[i] >> 4) & 0x0F];
        out[i * 2 + 1] = h[b[i] & 0x0F];
    }
    out[12] = '\0';
}

/* Write one RFC4180-style quoted CSV field (CR/LF folded to space, embedded
 * quotes doubled). Separator/newline are written by the caller. */
static void hs_csv_field_(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; (s != NULL) && (*s != '\0'); s++)
    {
        char c = *s;
        if ((c == '\n') || (c == '\r'))
        {
            c = ' ';
        }
        if (c == '"')
        {
            fputc('"', fp);
        }
        fputc(c, fp);
    }
    fputc('"', fp);
}

/* Split one CSV record (modified in place) into up to max fields, honoring
 * "quoted" fields with "" escapes. Returns the field count. */
static int hs_csv_parse_(char *line, char *fields[], int max)
{
    int n = 0;
    char *p = line;

    while (n < max)
    {
        if (*p == '"')
        {
            p++;
            char *dst = p;
            char *w = p;
            while (*p != '\0')
            {
                if (*p == '"')
                {
                    if (p[1] == '"') { *w++ = '"'; p += 2; continue; }
                    break; /* closing quote */
                }
                *w++ = *p++;
            }
            if (*p == '"') { p++; }
            *w = '\0';
            fields[n++] = dst;
        }
        else
        {
            char *dst = p;
            while ((*p != '\0') && (*p != ',')) { p++; }
            if (*p == ',') { *p = '\0'; p++; fields[n++] = dst; continue; }
            fields[n++] = dst;
            break;
        }
        if (*p == ',') { p++; continue; }
        break;
    }
    return n;
}

/* ---- NVS mirror (cracked results only) ---------------------------------- */

static void hs_nvs_put_cracked_(const uint8_t bssid[6], const char *essid,
                                const char *password)
{
    nvs_handle_t h;
    if (nvs_open(HS_JOURNAL_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        return; /* NVS not available -- CSV still holds the record */
    }

    char key[13];
    hs_bssid_hex_(bssid, key);

    size_t el = strlen(essid);
    if (el > 255U) { el = 255U; }
    const size_t pl = strlen(password);
    const size_t blen = 1U + el + pl + 1U;

    uint8_t *blob = (uint8_t *)malloc(blen);
    if (blob != NULL)
    {
        blob[0] = (uint8_t)el;
        memcpy(blob + 1, essid, el);
        memcpy(blob + 1U + el, password, pl + 1U); /* include NUL */
        if (nvs_set_blob(h, key, blob, blen) == ESP_OK)
        {
            (void)nvs_commit(h);
        }
        free(blob);
    }
    nvs_close(h);
}

static bool hs_nvs_get_cracked_(const uint8_t bssid[6], char *pw, size_t pwsz)
{
    nvs_handle_t h;
    if (nvs_open(HS_JOURNAL_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return false;
    }

    char key[13];
    hs_bssid_hex_(bssid, key);

    bool ok = false;
    size_t blen = 0U;
    if ((nvs_get_blob(h, key, NULL, &blen) == ESP_OK) && (blen >= 2U))
    {
        uint8_t *blob = (uint8_t *)malloc(blen);
        if ((blob != NULL) && (nvs_get_blob(h, key, blob, &blen) == ESP_OK))
        {
            const uint8_t el = blob[0];
            if ((size_t)(1U + el) < blen)
            {
                (void)snprintf(pw, pwsz, "%s", (const char *)(blob + 1U + el));
                ok = true;
            }
        }
        free(blob);
    }
    nvs_close(h);
    return ok;
}

/* ---- public API --------------------------------------------------------- */

void poom_hs_journal_record(const uint8_t bssid[6], const char *essid,
                            const char *list_id, hs_journal_outcome_t outcome,
                            const char *password)
{
    if ((bssid == NULL) || (outcome == HS_JOURNAL_NONE))
    {
        return;
    }
    if (essid == NULL)    { essid = ""; }
    if (list_id == NULL)  { list_id = "?"; }
    const bool cracked = (outcome == HS_JOURNAL_CRACK);
    const char *pw = (cracked && (password != NULL)) ? password : "";

    struct stat st;
    const bool need_header = (stat(HS_JOURNAL_CSV, &st) != 0) || (st.st_size == 0);

    FILE *fp = fopen(HS_JOURNAL_CSV, "a");
    if (fp != NULL)
    {
        if (need_header)
        {
            fputs("bssid,essid,list,outcome,password,epoch\n", fp);
        }
        char hex[13];
        hs_bssid_hex_(bssid, hex);
        fputs(hex, fp);                 fputc(',', fp);
        hs_csv_field_(fp, essid);       fputc(',', fp);
        hs_csv_field_(fp, list_id);     fputc(',', fp);
        fputs(cracked ? "crack" : "miss", fp); fputc(',', fp);
        hs_csv_field_(fp, pw);          fputc(',', fp);
        fprintf(fp, "%lld\n", (long long)time(NULL));
        (void)fclose(fp);
    }
    else
    {
        ESP_LOGW(TAG, "cannot append %s", HS_JOURNAL_CSV);
    }

    if (cracked)
    {
        hs_nvs_put_cracked_(bssid, essid, pw);
    }
}

hs_journal_outcome_t poom_hs_journal_lookup(const uint8_t bssid[6],
                                            const char *list_id)
{
    if (bssid == NULL)
    {
        return HS_JOURNAL_NONE;
    }
    FILE *fp = fopen(HS_JOURNAL_CSV, "r");
    if (fp == NULL)
    {
        return HS_JOURNAL_NONE;
    }

    char hex[13];
    hs_bssid_hex_(bssid, hex);

    char line[HS_JOURNAL_LINE];
    hs_journal_outcome_t res = HS_JOURNAL_NONE; /* last match wins */
    while (fgets(line, (int)sizeof(line), fp) != NULL)
    {
        size_t l = strlen(line);
        while ((l > 0U) && ((line[l - 1U] == '\n') || (line[l - 1U] == '\r')))
        {
            line[--l] = '\0';
        }
        if (l == 0U) { continue; }

        char *f[6];
        const int nf = hs_csv_parse_(line, f, 6);
        if (nf < 4) { continue; }
        if (strcasecmp(f[0], hex) != 0) { continue; }
        if ((list_id != NULL) && (strcmp(f[2], list_id) != 0)) { continue; }
        res = (strcmp(f[3], "crack") == 0) ? HS_JOURNAL_CRACK : HS_JOURNAL_MISS;
    }
    (void)fclose(fp);
    return res;
}

bool poom_hs_journal_get_cracked(const uint8_t bssid[6], char *pw, size_t pwsz)
{
    if ((bssid == NULL) || (pw == NULL) || (pwsz == 0U))
    {
        return false;
    }
    if (hs_nvs_get_cracked_(bssid, pw, pwsz))
    {
        return true;
    }

    /* Fallback: scan the CSV for the latest crack row for this BSSID. */
    FILE *fp = fopen(HS_JOURNAL_CSV, "r");
    if (fp == NULL)
    {
        return false;
    }
    char hex[13];
    hs_bssid_hex_(bssid, hex);

    char line[HS_JOURNAL_LINE];
    bool ok = false;
    while (fgets(line, (int)sizeof(line), fp) != NULL)
    {
        size_t l = strlen(line);
        while ((l > 0U) && ((line[l - 1U] == '\n') || (line[l - 1U] == '\r')))
        {
            line[--l] = '\0';
        }
        if (l == 0U) { continue; }

        char *f[6];
        const int nf = hs_csv_parse_(line, f, 6);
        if (nf < 5) { continue; }
        if (strcasecmp(f[0], hex) != 0) { continue; }
        if (strcmp(f[3], "crack") != 0) { continue; }
        (void)snprintf(pw, pwsz, "%s", f[4]);
        ok = true;
    }
    (void)fclose(fp);
    return ok;
}

void poom_hs_journal_iterate_cracked(poom_hs_journal_iter_fn cb, void *user)
{
    if (cb == NULL)
    {
        return;
    }
    FILE *fp = fopen(HS_JOURNAL_CSV, "r");
    if (fp == NULL)
    {
        return;
    }
    char line[HS_JOURNAL_LINE];
    while (fgets(line, (int)sizeof(line), fp) != NULL)
    {
        size_t l = strlen(line);
        while ((l > 0U) && ((line[l - 1U] == '\n') || (line[l - 1U] == '\r')))
        {
            line[--l] = '\0';
        }
        if (l == 0U) { continue; }

        char *f[6];
        const int nf = hs_csv_parse_(line, f, 6);
        if (nf < 5) { continue; }
        if (strcmp(f[3], "crack") != 0) { continue; }
        cb(f[1], f[0], f[4], user);   /* essid, bssid_hex, password */
    }
    (void)fclose(fp);
}
