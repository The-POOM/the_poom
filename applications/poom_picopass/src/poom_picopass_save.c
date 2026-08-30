// SPDX-License-Identifier: GPL-3.0-or-later
// Save a PicoPass dump to the SD card as a Flipper-compatible .picopass file.

#include "poom_picopass.h"
#include "sd_card.h"

#include <stdio.h>

#define POOM_PICOPASS_DIR "/picopass"
// Flipper writes at most this many blocks (0..31) into a .picopass file.
#define POOM_PICOPASS_FILE_MAX_BLOCKS 32

// Data for card block `i`, or NULL when we didn't read it (written as "??").
static const uint8_t* block_for_index(const PoomPicopassDump* d, int i)
{
    switch(i)
    {
        case POOM_PICOPASS_CSN_BLOCK:
            return d->csn;
        case POOM_PICOPASS_CONFIG_BLOCK:
            return d->config;
        case POOM_PICOPASS_EPURSE_BLOCK:
            return d->epurse;
        case POOM_PICOPASS_KD_BLOCK:
            return d->kd_valid ? d->kd : NULL;
        case POOM_PICOPASS_KC_BLOCK:
            return d->kc_valid ? d->kc : NULL;
        case POOM_PICOPASS_AIA_BLOCK:
            return d->aia;
        default:
        {
            int app = i - POOM_PICOPASS_PACS_CFG_BLOCK;
            return (app < d->app_block_count) ? d->blocks[app] : NULL;
        }
    }
}

static void fprint_block(FILE* f, const uint8_t* b)
{
    for(int j = 0; j < POOM_PICOPASS_BLOCK_LEN; j++)
    {
        (void)fprintf(f, "%02X%s", b[j],
                      (j + 1 < POOM_PICOPASS_BLOCK_LEN) ? " " : "\n");
    }
}

bool poom_picopass_sd_ready(void)
{
    if(sd_card_is_not_mounted())
    {
        sd_card_begin();
        return sd_card_mount() == ESP_OK;
    }
    return true;
}

esp_err_t poom_picopass_save(const PoomPicopassDump* dump,
                             char* out_rel_path,
                             size_t out_rel_path_len)
{
    if(dump == NULL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t err;
    if(sd_card_is_not_mounted())
    {
        sd_card_begin();
        err = sd_card_mount();
        if(err != ESP_OK)
            return err;
    }

    // sd_card_create_dir takes a FATFS volume-relative path (no /sdcard),
    // while fopen() below goes through the VFS and needs the /sdcard prefix.
    err = sd_card_create_dir(POOM_PICOPASS_DIR);
    if(err != ESP_OK)
        return err;

    // Name the file after the CSN. Keep the filename component within POOM's
    // FATFS long-name limit (CONFIG_FATFS_MAX_LFN, 31): 16 hex + ".picopass"
    // is 25 chars; a longer prefix would overflow and fail to open.
    char rel[64];
    (void)snprintf(rel, sizeof(rel),
                   "%s/%02X%02X%02X%02X%02X%02X%02X%02X.picopass",
                   POOM_PICOPASS_DIR, dump->csn[0], dump->csn[1], dump->csn[2],
                   dump->csn[3], dump->csn[4], dump->csn[5], dump->csn[6],
                   dump->csn[7]);

    char abs[96];
    (void)snprintf(abs, sizeof(abs), "%s%s", SD_CARD_PATH, rel);

    FILE* f = fopen(abs, "w");
    if(f == NULL)
        return ESP_ERR_FILE_OPEN_FAILED;

    (void)fprintf(f, "Filetype: Flipper Picopass device\n");
    (void)fprintf(f, "Version: 1\n");
    (void)fprintf(f, "Credential: ");
    fprint_block(f, dump->credential);
    (void)fprintf(f, "# Picopass blocks\n");

    int blocks = dump->app_limit;
    if(blocks > POOM_PICOPASS_FILE_MAX_BLOCKS)
        blocks = POOM_PICOPASS_FILE_MAX_BLOCKS;
    for(int i = 0; i < blocks; i++)
    {
        const uint8_t* b = block_for_index(dump, i);
        (void)fprintf(f, "Block %d: ", i);
        if(b == NULL)
            (void)fprintf(f, "?? ?? ?? ?? ?? ?? ?? ??\n");
        else
            fprint_block(f, b);
    }

    (void)fclose(f);

    if((out_rel_path != NULL) && (out_rel_path_len > 0U))
        (void)snprintf(out_rel_path, out_rel_path_len, "%s", rel);
    return ESP_OK;
}
