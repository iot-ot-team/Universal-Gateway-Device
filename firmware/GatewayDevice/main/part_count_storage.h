
#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t part_count_storage_init(void);
esp_err_t part_count_storage_write(uint32_t count);
esp_err_t part_count_storage_get_last(uint32_t *out_count);
esp_err_t part_count_storage_clear(void);
