// part_count_storage.c
#include "part_count_storage.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "GATEWAY DEVICE";
static const esp_partition_t *s_part = NULL;

#define ENTRY_SIZE      sizeof(uint32_t)
#define EMPTY_VALUE     0xFFFFFFFF   // erased flash reads as all 1s -- this marks "unwritten"

// tracks next free write offset within the partition, valid only for this power cycle
static size_t s_write_offset = 0;

esp_err_t part_count_storage_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "pcount");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "pcount partition not found -- check partitions.csv");
        return ESP_ERR_NOT_FOUND;
    }

    // find first empty slot by scanning from the start
    uint32_t val;
    size_t offset = 0;
    while (offset < s_part->size) {
        esp_err_t err = esp_partition_read(s_part, offset, &val, ENTRY_SIZE);
        if (err != ESP_OK) return err;

        if (val == EMPTY_VALUE) break;   // found first unwritten slot
        offset += ENTRY_SIZE;
    }

    s_write_offset = offset;
    ESP_LOGI(TAG, "Init OK, next write offset = %u / %u", (unsigned)s_write_offset, (unsigned)s_part->size);
    return ESP_OK;
}

esp_err_t part_count_storage_write(uint32_t count)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;

    if (s_write_offset >= s_part->size) {
        ESP_LOGW(TAG, "Buffer full -- overwriting from start");
        s_write_offset = 0;
    }

    esp_err_t err = esp_partition_write(s_part, s_write_offset, &count, ENTRY_SIZE);
    if (err == ESP_OK) {
        s_write_offset += ENTRY_SIZE;
    }
    return err;
}

// Finds the last valid (non-erased) entry and returns it via out_count.
esp_err_t part_count_storage_get_last(uint32_t *out_count)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (out_count == NULL) return ESP_ERR_INVALID_ARG;

    uint32_t val;
    uint32_t last_valid = 0;
    bool found = false;
    size_t offset = 0;

    while (offset < s_part->size) {
        esp_err_t err = esp_partition_read(s_part, offset, &val, ENTRY_SIZE);
        if (err != ESP_OK) return err;

        if (val == EMPTY_VALUE) break;   // reached unwritten region -- stop here

        last_valid = val;
        found = true;
        offset += ENTRY_SIZE;
    }

    if (!found) {
        ESP_LOGW(TAG, "No buffered entries found to recover");
        return ESP_ERR_NOT_FOUND;
    }

    *out_count = last_valid;
    ESP_LOGI(TAG, "Recovered last count: %u", (unsigned)last_valid);
    return ESP_OK;
}

// Erases the whole region and resets the write pointer. Call this ONLY
// after get_last() has been used and the caller has applied the recovery --
// this is the "wipe for next outage" step, separate from reading.
esp_err_t part_count_storage_clear(void)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;

    esp_err_t err = esp_partition_erase_range(s_part, 0, s_part->size);
    if (err == ESP_OK) {
        s_write_offset = 0;
        ESP_LOGI(TAG, "Partition cleared");
    }
    return err;
}
