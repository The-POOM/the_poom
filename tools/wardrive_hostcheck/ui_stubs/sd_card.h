#pragma once
#include <stdbool.h>
#include "esp_err.h"
bool sd_card_is_mounted(void);
esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);
