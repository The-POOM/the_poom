// SPDX-License-Identifier: MIT
// Copyright (c) 2026 THE POOM
//
// SD-backed streaming search over the Exploit-DB metadata CSV. The quote-aware
// field parsing and title matching live in poom_net_audit_logic.c (host-tested);
// this file mounts the card, streams the file line by line, and collects
// matches. Phase C increment 1: a direct scan (no index yet).

#include "poom_net_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"

/* Max CSV row we buffer. Rows longer than this are matched on their leading
 * fields and the physical remainder is drained (so a long row is never split
 * into a bogus second "row"). The id/title/type/platform fields are early, so
 * matching stays correct; only a very late "codes" CVE might be missed. */
#define NET_AUDIT_LINE_BUF (1536)

/* true when THIS module mounted the card (so release() only undoes our own
 * mount and never pulls the card from under another app). */
static bool s_mounted_by_audit = false;

static esp_err_t s_last_mount_err = ESP_OK;
static poom_net_audit_index_use_t s_last_index_use = POOM_NET_AUDIT_IDX_NONE;

static bool net_audit_ensure_sd_(void)
{
    if (sd_card_is_mounted())
    {
        return true;
    }
    /* Lean mount (2 open files, CSV + index): the default 4-slot mount needs a ~21 KB
     * contiguous block, which a Wi-Fi-connected, post-scan heap often lacks.
     * One retry for transient failures (the driver frees the bus on error). */
    s_last_mount_err = sd_card_mount_lean();
    if (s_last_mount_err != ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_last_mount_err = sd_card_mount_lean();
        if (s_last_mount_err != ESP_OK)
        {
            return false;
        }
    }
    s_mounted_by_audit = true;
    return true;
}

esp_err_t poom_net_audit_last_mount_err(void)
{
    return s_last_mount_err;
}

void poom_net_audit_release(void)
{
    if (s_mounted_by_audit && sd_card_is_mounted())
    {
        (void)sd_card_unmount(); /* frees FATFS + SDSPI and the SPI bus */
    }
    s_mounted_by_audit = false;
}

esp_err_t poom_net_audit_db_status(void)
{
    if (!net_audit_ensure_sd_())
    {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(POOM_NET_AUDIT_CSV_PATH, "r");
    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    (void)fclose(f);
    return ESP_OK;
}

bool poom_net_audit_db_present(void)
{
    return poom_net_audit_db_status() == ESP_OK;
}

esp_err_t poom_net_audit_search(const char *term, const char *version,
                                poom_net_audit_match_t *out, size_t cap,
                                size_t *out_count, const volatile bool *cancel,
                                poom_net_audit_progress_cb_t cb, void *user)
{
    (void)version; /* not a hard filter in increment 1 */
    if (term == NULL || out == NULL || out_count == NULL || cap == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (term[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!net_audit_ensure_sd_())
    {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(POOM_NET_AUDIT_CSV_PATH, "r");
    if (f == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    long total = 0;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        total = ftell(f);
        (void)fseek(f, 0, SEEK_SET);
    }

    char *line = (char *)malloc(NET_AUDIT_LINE_BUF);
    if (line == NULL)
    {
        (void)fclose(f);
        return ESP_ERR_NO_MEM;
    }

    long scanned = 0;
    bool header = true;
    unsigned lines = 0;

    while (fgets(line, NET_AUDIT_LINE_BUF, f) != NULL)
    {
        size_t len = strlen(line);
        scanned += (long)len;

        /* If this row was longer than the buffer, drain the rest of the
         * physical line so the continuation isn't parsed as a new row. */
        bool truncated = (len == (size_t)(NET_AUDIT_LINE_BUF - 1)) &&
                         (line[len - 1] != '\n');
        if (truncated)
        {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n')
            {
                scanned++;
            }
        }

        if (cancel != NULL && *cancel)
        {
            break;
        }

        if (header) /* skip the CSV header row */
        {
            header = false;
        }
        else if (poom_net_audit_match_line(line, term, &out[*out_count]))
        {
            (*out_count)++;
            if (*out_count >= cap)
            {
                break;
            }
        }

        if (cb != NULL && (++lines % 256U) == 0U)
        {
            cb((uint32_t)scanned, (uint32_t)((total > 0) ? total : 1), user);
        }
    }

    if (cb != NULL)
    {
        cb((uint32_t)((total > 0) ? total : 1), (uint32_t)((total > 0) ? total : 1), user);
    }

    free(line);
    (void)fclose(f);
    return ESP_OK;
}

esp_err_t poom_net_audit_search_multi(const poom_net_audit_term_t *terms,
                                      uint8_t n_terms,
                                      poom_net_audit_match_t *out, size_t cap,
                                      uint8_t per_term_cap, size_t *out_count,
                                      const volatile bool *cancel,
                                      poom_net_audit_progress_cb_t cb, void *user)
{
    if (terms == NULL || out == NULL || out_count == NULL || cap == 0U || n_terms == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!net_audit_ensure_sd_())
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* Indexed when the index is current; otherwise a full pass that also
     * (re)builds the index for next time. */
    poom_net_audit_index_use_t how = POOM_NET_AUDIT_IDX_NONE;
    esp_err_t err = poom_net_audit_search_file(POOM_NET_AUDIT_CSV_PATH, POOM_NET_AUDIT_IDX_PATH,
                                               POOM_NET_AUDIT_IDX_TMP, terms, n_terms, out, cap,
                                               per_term_cap, out_count, &how, cancel, cb, user);
    s_last_index_use = how;
    return err;
}

poom_net_audit_index_use_t poom_net_audit_last_index_use(void)
{
    return s_last_index_use;
}

esp_err_t poom_net_audit_sd_open(void)
{
    return net_audit_ensure_sd_() ? ESP_OK : ESP_ERR_INVALID_STATE;
}
