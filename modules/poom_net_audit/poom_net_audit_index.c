// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// Exploit-DB CSV search with an automatic block index.
//
// Index = for every block of NA_IDX_BLOCK_ROWS CSV lines: the block's byte
// offset + a Bloom bit-set (NA_IDX_BITS bits, 2 hashes) of every word in its
// titles. A search reads the small index sequentially and only reads the CSV
// blocks that may contain all words of some term; those rows go through the
// SAME whole-word matcher as a full scan, so results are identical (a Bloom
// filter has false positives, never false negatives).
//
// The index is never built by hand: a search that finds no index, or one
// stamped with a different CSV size/mtime, does the full pass and writes the
// index alongside (temp file, renamed when complete, deleted on cancel).
//
// Plain C stdio only, so it is host-tested end-to-end in test/test_index.c.

#include "poom_net_audit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define NA_LINE_BUF        (1536)
#define NA_IDX_MAGIC       (0x58444550UL) /* "PEDX" */
#define NA_IDX_VERSION     (1UL)

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t csv_size;
    uint32_t csv_mtime;
    uint32_t block_rows;
    uint32_t n_blocks;
    uint32_t bits;
    uint32_t complete;
} na_idx_hdr_t;

/* ---- hashing / Bloom (pure) ------------------------------------------------ */

uint32_t poom_net_audit_word_hash(const char *w, size_t n)
{
    uint32_t h = 2166136261UL;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= (uint32_t)(unsigned char)tolower((unsigned char)w[i]);
        h *= 16777619UL;
    }
    return h;
}

static void na_bloom_pos_(uint32_t h, uint32_t *a, uint32_t *b)
{
    *a = h % POOM_NET_AUDIT_IDX_BITS;
    *b = ((h >> 16) ^ (h * 0x9E3779B1UL)) % POOM_NET_AUDIT_IDX_BITS;
}

/* Iterate the alphanumeric words of @p text (same tokenisation everywhere). */
static const char *na_next_word_(const char *p, size_t *n)
{
    while (*p != '\0' && !isalnum((unsigned char)*p)) { p++; }
    size_t k = 0;
    while (isalnum((unsigned char)p[k])) { k++; }
    *n = k;
    return (k > 0U) ? p : NULL;
}

void poom_net_audit_bloom_add_text(uint8_t *bits, const char *text)
{
    if (bits == NULL || text == NULL) { return; }
    const char *p = text;
    size_t n = 0;
    while ((p = na_next_word_(p, &n)) != NULL)
    {
        uint32_t a, b;
        na_bloom_pos_(poom_net_audit_word_hash(p, n), &a, &b);
        bits[a >> 3] |= (uint8_t)(1U << (a & 7U));
        bits[b >> 3] |= (uint8_t)(1U << (b & 7U));
        p += n;
    }
}

bool poom_net_audit_bloom_has_term(const uint8_t *bits, const char *term)
{
    if (bits == NULL || term == NULL) { return true; }
    const char *p = term;
    size_t n = 0;
    while ((p = na_next_word_(p, &n)) != NULL)
    {
        uint32_t a, b;
        na_bloom_pos_(poom_net_audit_word_hash(p, n), &a, &b);
        if ((bits[a >> 3] & (1U << (a & 7U))) == 0U) { return false; }
        if ((bits[b >> 3] & (1U << (b & 7U))) == 0U) { return false; }
        p += n;
    }
    return true; /* all words possibly present (or no words at all) */
}

/* ---- file helpers ---------------------------------------------------------- */

/* Read one physical line into buf (draining an over-long remainder).
 * @return bytes consumed from the file (0 at EOF). */
static size_t na_read_line_(FILE *f, char *buf, size_t cap)
{
    if (fgets(buf, (int)cap, f) == NULL) { return 0U; }
    size_t len = strlen(buf);
    size_t consumed = len;
    if (len == cap - 1U && buf[len - 1U] != '\n')
    {
        int ch;
        while ((ch = fgetc(f)) != EOF)
        {
            consumed++;
            if (ch == '\n') { break; }
        }
    }
    return consumed;
}

static bool na_csv_stamp_(const char *csv, uint32_t *size, uint32_t *mtime)
{
    struct stat st;
    if (stat(csv, &st) != 0) { return false; }
    *size = (uint32_t)st.st_size;
    *mtime = (uint32_t)st.st_mtime;
    return true;
}

static bool na_idx_valid_(FILE *ix, uint32_t size, uint32_t mtime, na_idx_hdr_t *h)
{
    if (fread(h, sizeof(*h), 1, ix) != 1U) { return false; }
    return h->magic == NA_IDX_MAGIC && h->version == NA_IDX_VERSION &&
           h->bits == POOM_NET_AUDIT_IDX_BITS && h->block_rows == POOM_NET_AUDIT_IDX_ROWS &&
           h->csv_size == size && h->csv_mtime == mtime && h->complete == 1U;
}

bool poom_net_audit_index_current(const char *csv_path, const char *idx_path)
{
    uint32_t size, mtime;
    if (csv_path == NULL || idx_path == NULL || !na_csv_stamp_(csv_path, &size, &mtime)) { return false; }
    FILE *ix = fopen(idx_path, "rb");
    if (ix == NULL) { return false; }
    na_idx_hdr_t h;
    bool ok = na_idx_valid_(ix, size, mtime, &h);
    (void)fclose(ix);
    return ok;
}

static bool na_cancelled_(const volatile bool *c) { return c != NULL && *c; }

/* ---- full pass (+ build) ---------------------------------------------------- */

static esp_err_t na_full_pass_(FILE *f, uint32_t total, FILE *tmp, poom_net_audit_collector_t *col,
                               const poom_net_audit_term_t *terms, uint8_t n_terms,
                               char *line, uint8_t *bits, uint32_t *n_blocks,
                               const volatile bool *cancel,
                               poom_net_audit_progress_cb_t cb, void *user)
{
    uint32_t scanned = 0;
    uint32_t rows_in_block = 0;
    uint32_t block_off = 0;
    bool header = true;
    unsigned lines = 0;
    bool write_ok = (tmp != NULL);
    *n_blocks = 0;
    memset(bits, 0, POOM_NET_AUDIT_IDX_BITS / 8U);

    for (;;)
    {
        uint32_t line_off = scanned;
        size_t used = na_read_line_(f, line, NA_LINE_BUF);
        if (used == 0U) { break; }
        scanned += (uint32_t)used;
        if (na_cancelled_(cancel)) { return ESP_ERR_INVALID_STATE; }

        if (header) { header = false; block_off = scanned; continue; }

        if (rows_in_block == 0U) { block_off = line_off; }
        poom_net_audit_collect_line(col, line, terms, n_terms);
        if (write_ok)
        {
            char title[160];
            (void)poom_net_audit_csv_field(line, POOM_NET_AUDIT_COL_TITLE, title, sizeof(title));
            poom_net_audit_bloom_add_text(bits, title);
        }
        if (++rows_in_block == POOM_NET_AUDIT_IDX_ROWS)
        {
            if (write_ok &&
                (fwrite(&block_off, sizeof(block_off), 1, tmp) != 1U ||
                 fwrite(bits, POOM_NET_AUDIT_IDX_BITS / 8U, 1, tmp) != 1U))
            {
                write_ok = false;
            }
            (*n_blocks)++;
            rows_in_block = 0;
            memset(bits, 0, POOM_NET_AUDIT_IDX_BITS / 8U);
        }
        if (cb != NULL && (++lines % 256U) == 0U) { cb(scanned, total ? total : 1U, user); }
    }
    if (rows_in_block > 0U)
    {
        if (write_ok &&
            (fwrite(&block_off, sizeof(block_off), 1, tmp) != 1U ||
             fwrite(bits, POOM_NET_AUDIT_IDX_BITS / 8U, 1, tmp) != 1U))
        {
            write_ok = false;
        }
        (*n_blocks)++;
    }
    return write_ok ? ESP_OK : ESP_FAIL; /* ESP_FAIL = searched fine, index not written */
}

/* ---- indexed pass ----------------------------------------------------------- */

static esp_err_t na_indexed_pass_(FILE *f, FILE *ix, const na_idx_hdr_t *h,
                                  poom_net_audit_collector_t *col,
                                  const poom_net_audit_term_t *terms, uint8_t n_terms,
                                  char *line, uint8_t *bits, const volatile bool *cancel,
                                  poom_net_audit_progress_cb_t cb, void *user)
{
    for (uint32_t blk = 0; blk < h->n_blocks; ++blk)
    {
        if (na_cancelled_(cancel)) { return ESP_ERR_INVALID_STATE; }
        uint32_t off = 0;
        if (fread(&off, sizeof(off), 1, ix) != 1U ||
            fread(bits, POOM_NET_AUDIT_IDX_BITS / 8U, 1, ix) != 1U)
        {
            return ESP_FAIL; /* truncated index */
        }
        bool candidate = false;
        for (uint8_t t = 0; t < n_terms && !candidate; ++t)
        {
            if (terms[t].term != NULL && terms[t].term[0] != '\0' &&
                poom_net_audit_bloom_has_term(bits, terms[t].term))
            {
                candidate = true;
            }
        }
        if (candidate)
        {
            if (fseek(f, (long)off, SEEK_SET) != 0) { return ESP_FAIL; }
            for (uint32_t r = 0; r < h->block_rows; ++r)
            {
                if (na_read_line_(f, line, NA_LINE_BUF) == 0U) { break; }
                poom_net_audit_collect_line(col, line, terms, n_terms);
            }
        }
        if (cb != NULL && (blk % 16U) == 0U) { cb(blk, h->n_blocks ? h->n_blocks : 1U, user); }
    }
    if (cb != NULL) { cb(1U, 1U, user); }
    return ESP_OK;
}

/* ---- public entry ----------------------------------------------------------- */

esp_err_t poom_net_audit_search_file(const char *csv_path, const char *idx_path,
                                     const char *idx_tmp_path,
                                     const poom_net_audit_term_t *terms, uint8_t n_terms,
                                     poom_net_audit_match_t *out, size_t cap,
                                     uint8_t per_term_cap, size_t *out_count,
                                     poom_net_audit_index_use_t *how,
                                     const volatile bool *cancel,
                                     poom_net_audit_progress_cb_t cb, void *user)
{
    if (csv_path == NULL || terms == NULL || out == NULL || out_count == NULL || cap == 0U ||
        n_terms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (how != NULL) { *how = POOM_NET_AUDIT_IDX_NONE; }

    uint32_t size = 0, mtime = 0;
    if (!na_csv_stamp_(csv_path, &size, &mtime)) { return ESP_ERR_NOT_FOUND; }
    FILE *f = fopen(csv_path, "rb");
    if (f == NULL) { return ESP_ERR_NOT_FOUND; }

    char *line = (char *)malloc(NA_LINE_BUF);
    uint8_t *bits = (uint8_t *)malloc(POOM_NET_AUDIT_IDX_BITS / 8U);
    if (line == NULL || bits == NULL)
    {
        free(line);
        free(bits);
        (void)fclose(f);
        return ESP_ERR_NO_MEM;
    }

    poom_net_audit_collector_t col;
    poom_net_audit_collector_init(&col, out, (uint16_t)cap, per_term_cap);
    esp_err_t ret = ESP_OK;

    /* 1) Current index -> indexed pass. */
    FILE *ix = (idx_path != NULL) ? fopen(idx_path, "rb") : NULL;
    na_idx_hdr_t h;
    if (ix != NULL && na_idx_valid_(ix, size, mtime, &h))
    {
        ret = na_indexed_pass_(f, ix, &h, &col, terms, n_terms, line, bits, cancel, cb, user);
        (void)fclose(ix);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE)
        {
            if (how != NULL) { *how = POOM_NET_AUDIT_IDX_USED; }
            goto done; /* cancelled keeps partial matches */
        }
        /* broken index: fall through to a full pass (and rebuild) */
        poom_net_audit_collector_init(&col, out, (uint16_t)cap, per_term_cap);
        if (fseek(f, 0, SEEK_SET) != 0) { ret = ESP_FAIL; goto done; }
    }
    else if (ix != NULL)
    {
        (void)fclose(ix);
    }

    /* 2) Full pass, building a fresh index alongside. */
    {
        FILE *tmp = NULL;
        na_idx_hdr_t nh = {NA_IDX_MAGIC, NA_IDX_VERSION, size, mtime, POOM_NET_AUDIT_IDX_ROWS,
                           0U, POOM_NET_AUDIT_IDX_BITS, 0U};
        if (idx_path != NULL && idx_tmp_path != NULL)
        {
            tmp = fopen(idx_tmp_path, "wb");
            if (tmp != NULL && fwrite(&nh, sizeof(nh), 1, tmp) != 1U)
            {
                (void)fclose(tmp);
                (void)remove(idx_tmp_path);
                tmp = NULL;
            }
        }
        uint32_t n_blocks = 0;
        esp_err_t pr = na_full_pass_(f, size, tmp, &col, terms, n_terms, line, bits, &n_blocks,
                                     cancel, cb, user);
        if (cb != NULL && pr != ESP_ERR_INVALID_STATE) { cb(size ? size : 1U, size ? size : 1U, user); }
        if (tmp != NULL)
        {
            bool keep = (pr == ESP_OK);
            if (keep)
            {
                nh.n_blocks = n_blocks;
                nh.complete = 1U;
                keep = (fseek(tmp, 0, SEEK_SET) == 0 && fwrite(&nh, sizeof(nh), 1, tmp) == 1U);
            }
            keep = (fclose(tmp) == 0) && keep;
            if (keep)
            {
                (void)remove(idx_path); /* FAT rename fails onto an existing file */
                keep = (rename(idx_tmp_path, idx_path) == 0);
            }
            if (!keep) { (void)remove(idx_tmp_path); }
            else if (how != NULL) { *how = POOM_NET_AUDIT_IDX_BUILT; }
        }
        ret = (pr == ESP_ERR_INVALID_STATE || pr == ESP_OK || pr == ESP_FAIL) ? ESP_OK : pr;
    }

done:
    *out_count = col.count;
    free(line);
    free(bits);
    (void)fclose(f);
    return (ret == ESP_ERR_INVALID_STATE) ? ESP_OK : ret; /* cancel = partial, not an error */
}
