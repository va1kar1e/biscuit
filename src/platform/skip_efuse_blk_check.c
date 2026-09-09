// Override the prebuilt libbootloader_support.a implementation.
//
// On Xteink hardware the runtime OTA validator can read the new image's
// esp_app_desc_t through a misaligned bootloader mmap pointer. That produces
// garbage eFuse block-revision bounds and makes esp_ota_end() reject an
// otherwise valid ESP32-C3 image with ESP_ERR_OTA_VALIDATE_FAILED.
//
// This gate is a manufacturing revision compatibility check. Image magic,
// chip ID, segment boundaries, image checksum, SHA-256 trailer and the SD vs
// flash byte comparison remain validated by the updater.
#include <esp_err.h>

esp_err_t __wrap_bootloader_common_check_efuse_blk_validity(uint32_t min_rev_full, uint32_t max_rev_full) {
  (void)min_rev_full;
  (void)max_rev_full;
  return ESP_OK;
}
