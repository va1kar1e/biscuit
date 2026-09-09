#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MIN_FIRMWARE_SIZE = 1024;
constexpr size_t WRITE_BUFFER_SIZE = 16384;

const char* shortErrorName(const esp_err_t error) {
  if (error == ESP_ERR_OTA_VALIDATE_FAILED) return "OTA_VALIDATE_FAILED";
  if (error == ESP_ERR_INVALID_SIZE) return "INVALID_SIZE";
  if (error == ESP_ERR_NO_MEM) return "OUT_OF_MEMORY";
  return esp_err_to_name(error);
}

size_t firstMismatch(const uint8_t* expected, const uint8_t* actual, const size_t length) {
  for (size_t index = 0; index < length; index++) {
    if (expected[index] != actual[index]) return index;
  }
  return length;
}
}  // namespace

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  state = State::VALIDATING;

  // Header validation is quick and does not need its own e-paper refresh. Going
  // straight to the confirmation screen avoids a full flash immediately
  // followed by another refresh for the prompt.
  if (!validateFirmware()) {
    state = State::FAILED;
    requestUpdate();
    return;
  }

  promptConfirmation();
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  FsFile file;
  if (!Storage.openFileForRead("FW", UPDATE_PATH, file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_MISSING);
    return false;
  }

  firmwareSize = file.fileSize();
  const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
  if (!destination || firmwareSize < MIN_FIRMWARE_SIZE || firmwareSize > destination->size) {
    LOG_ERR("FW", "Invalid update size: %u", static_cast<unsigned>(firmwareSize));
    errorMessage = firmwareSize > (destination ? destination->size : 0) ? tr(STR_FIRMWARE_TOO_LARGE)
                                                                        : tr(STR_INVALID_FIRMWARE);
    return false;
  }

  esp_image_header_t header{};
  if (file.read(&header, sizeof(header)) != sizeof(header) || header.magic != ESP_IMAGE_HEADER_MAGIC ||
      header.chip_id != ESP_CHIP_ID_ESP32C3) {
    LOG_ERR("FW", "Invalid ESP32-C3 image header");
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }

  LOG_INF("FW", "Validated %s (%u bytes)", UPDATE_PATH, static_cast<unsigned>(firmwareSize));
  return true;
}

void SdFirmwareUpdateActivity::promptConfirmation() {
  state = State::CONFIRMING;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_FIRMWARE_UPDATE_PROMPT), UPDATE_PATH),
      [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SdFirmwareUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    leaveUpdater();
    return;
  }

  state = State::UPDATING;
  writtenBytes = 0;
  // Leave the already-visible confirmation screen untouched while writing.
  // Rendering an "Updating" page here costs another complete e-paper waveform
  // and was perceived as continuous flashing. Success or failure is the next
  // and only refresh after confirmation.
  performUpdate();
}

void SdFirmwareUpdateActivity::performUpdate() {
  if (!validateFirmware()) {
    state = State::FAILED;
    requestUpdate();
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("FW", UPDATE_PATH, file) || !file) {
    fail(tr(STR_FIRMWARE_FILE_MISSING));
    return;
  }

  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[WRITE_BUFFER_SIZE]);
  auto verifyBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[WRITE_BUFFER_SIZE]);
  if (!buffer || !verifyBuffer) {
    LOG_ERR("FW", "Could not allocate OTA buffer");
    fail(tr(STR_OUT_OF_MEMORY));
    return;
  }

  const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
  esp_ota_handle_t handle = 0;
  esp_err_t result = esp_ota_begin(destination, firmwareSize, &handle);
  if (result != ESP_OK) {
    LOG_ERR("FW", "esp_ota_begin failed: %s", esp_err_to_name(result));
    file.close();
    failWithDetail(tr(STR_FIRMWARE_WRITE_FAILED), shortErrorName(result));
    return;
  }

  bool writeFailed = false;
  while (writtenBytes < firmwareSize) {
    const size_t wanted = std::min(WRITE_BUFFER_SIZE, firmwareSize - writtenBytes);
    const int bytesRead = file.read(buffer.get(), wanted);
    if (bytesRead <= 0) {
      LOG_ERR("FW", "Unexpected end of update file");
      result = ESP_ERR_INVALID_SIZE;
      writeFailed = true;
      break;
    }

    result = esp_ota_write(handle, buffer.get(), static_cast<size_t>(bytesRead));
    if (result != ESP_OK) {
      LOG_ERR("FW", "esp_ota_write failed: %s", esp_err_to_name(result));
      writeFailed = true;
      break;
    }

    // Verify each chunk immediately. This separates a flash-write fault from
    // an invalid or intermittently corrupted file read on the MicroSD card.
    result = esp_partition_read(destination, writtenBytes, verifyBuffer.get(), static_cast<size_t>(bytesRead));
    if (result != ESP_OK || std::memcmp(buffer.get(), verifyBuffer.get(), static_cast<size_t>(bytesRead)) != 0) {
      const size_t mismatch = result == ESP_OK
                                  ? firstMismatch(buffer.get(), verifyBuffer.get(), static_cast<size_t>(bytesRead))
                                  : 0;
      char detail[48];
      std::snprintf(detail, sizeof(detail), "FLASH_VERIFY @ 0x%06lx",
                    static_cast<unsigned long>(writtenBytes + mismatch));
      LOG_ERR("FW", "%s (%s)", detail, esp_err_to_name(result));
      esp_ota_abort(handle);
      file.close();
      failWithDetail(tr(STR_FIRMWARE_WRITE_FAILED), detail);
      return;
    }

    writtenBytes += static_cast<size_t>(bytesRead);
    // Do not refresh the e-paper panel while streaming from MicroSD. On the X4
    // those operations contend for hardware resources; refreshing here can
    // interrupt the SD read and leave the OTA image incomplete. The updating
    // screen drawn before this loop remains visible until completion.
    delay(1);
  }

  if (writeFailed) {
    esp_ota_abort(handle);
    file.close();
    failWithDetail(tr(STR_FIRMWARE_WRITE_FAILED), shortErrorName(result));
    return;
  }

  // Read the SD file a second time and compare it with app1. The first pass was
  // compared immediately after every write; a difference here therefore points
  // to an unstable SD read rather than to esp_ota_end().
  if (!file.seekSet(0)) {
    esp_ota_abort(handle);
    file.close();
    failWithDetail(tr(STR_INVALID_FIRMWARE), "SD_REWIND_FAILED");
    return;
  }

  size_t verifyOffset = 0;
  while (verifyOffset < firmwareSize) {
    const size_t wanted = std::min(WRITE_BUFFER_SIZE, firmwareSize - verifyOffset);
    const int bytesRead = file.read(buffer.get(), wanted);
    if (bytesRead != static_cast<int>(wanted)) {
      esp_ota_abort(handle);
      file.close();
      failWithDetail(tr(STR_INVALID_FIRMWARE), "SD_SECOND_READ_FAILED");
      return;
    }

    result = esp_partition_read(destination, verifyOffset, verifyBuffer.get(), wanted);
    if (result != ESP_OK || std::memcmp(buffer.get(), verifyBuffer.get(), wanted) != 0) {
      const size_t mismatch = result == ESP_OK ? firstMismatch(buffer.get(), verifyBuffer.get(), wanted) : 0;
      char detail[48];
      std::snprintf(detail, sizeof(detail), "SD_READ_CHANGED @ 0x%06lx",
                    static_cast<unsigned long>(verifyOffset + mismatch));
      LOG_ERR("FW", "%s (%s)", detail, esp_err_to_name(result));
      esp_ota_abort(handle);
      file.close();
      failWithDetail(tr(STR_INVALID_FIRMWARE), detail);
      return;
    }
    verifyOffset += wanted;
    delay(1);
  }

  file.close();
  result = esp_ota_end(handle);
  if (result != ESP_OK) {
    LOG_ERR("FW", "esp_ota_end rejected image: %s", esp_err_to_name(result));
    failWithDetail(tr(STR_INVALID_FIRMWARE), shortErrorName(result));
    return;
  }

  result = esp_ota_set_boot_partition(destination);
  if (result != ESP_OK) {
    LOG_ERR("FW", "Could not select OTA partition: %s", esp_err_to_name(result));
    failWithDetail(tr(STR_FIRMWARE_WRITE_FAILED), shortErrorName(result));
    return;
  }

  if (Storage.exists(APPLIED_UPDATE_PATH)) {
    Storage.remove(APPLIED_UPDATE_PATH);
  }
  if (!Storage.rename(UPDATE_PATH, APPLIED_UPDATE_PATH)) {
    LOG_ERR("FW", "Could not rename installed update file");
  }

  LOG_INF("FW", "Firmware update complete; restarting");
  state = State::SUCCESS;
  requestUpdateAndWait();
  delay(1500);
  ESP.restart();
}

void SdFirmwareUpdateActivity::fail(const char* message) {
  errorMessage = message;
  errorDetail.clear();
  state = State::FAILED;
  requestUpdate();
}

void SdFirmwareUpdateActivity::failWithDetail(const char* message, const char* detail) {
  errorMessage = message;
  errorDetail = detail ? detail : "";
  state = State::FAILED;
  requestUpdate();
}

void SdFirmwareUpdateActivity::leaveUpdater() {
  if (detectedAtBoot) {
    onGoHome();
  } else {
    finish();
  }
}

bool SdFirmwareUpdateActivity::preventAutoSleep() {
  return state == State::VALIDATING || state == State::UPDATING;
}

void SdFirmwareUpdateActivity::loop() {
  if (state == State::FAILED &&
      (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
       mappedInput.wasPressed(MappedInputManager::Button::Confirm))) {
    leaveUpdater();
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SD_FIRMWARE_UPDATE));

  if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
  } else if (state == State::UPDATING) {
    const int percent = firmwareSize > 0 ? static_cast<int>((writtenBytes * 100) / firmwareSize) : 0;
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, top + lineHeight + metrics.verticalSpacing,
             pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        percent, 100);
    renderer.drawCenteredText(UI_10_FONT_ID,
                              top + lineHeight + metrics.verticalSpacing + metrics.progressBarHeight + lineHeight,
                              tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF));
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, tr(STR_RESTARTING_HINT));
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top - lineHeight, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + metrics.verticalSpacing, errorMessage.c_str());
    if (!errorDetail.empty()) {
      const auto detail = renderer.truncatedText(UI_10_FONT_ID, errorDetail.c_str(),
                                                 pageWidth - metrics.contentSidePadding * 2);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing * 2, detail.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // No render is requested while MicroSD is being read or app1 is being
  // written, so the panel and SD never contend during the update.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
