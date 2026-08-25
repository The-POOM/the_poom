#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the application
esp_err_t poom_i2c_as5600_start(void);

#ifdef __cplusplus
}
#endif