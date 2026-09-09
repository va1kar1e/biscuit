#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <memory>
#include <new>

#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t MIN_FIRMWARE_SIZE = 1024;
constexpr size_t WRITE_BUFFER_SIZE = 4096;
constexpr unsigned int PROGRESS_STEP_PERCENT = 5;
}  // namespace

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  state = State::VALIDATING;
  requestUpdateAndWait();

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
  lastRenderedPercent = 101;
  requestUpdateAndWait();
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
  if (!buffer) {
    LOG_ERR("FW", "Could not allocate OTA buffer");
    fail(tr(STR_OUT_OF_MEMORY));
    return;
  }

  const esp_partition_t* destination = esp_ota_get_next_update_partition(nullptr);
  esp_ota_handle_t handle = 0;
  esp_err_t result = esp_ota_begin(destination, firmwareSize, &handle);
  if (result != ESP_OK) {
    LOG_ERR("FW", "esp_ota_begin failed: %s", esp_err_to_name(result));
    fail(tr(STR_FIRMWARE_WRITE_FAILED));
    return;
  }

  bool writeFailed = false;
  while (writtenBytes < firmwareSize) {
    const size_t wanted = std::min(WRITE_BUFFER_SIZE, firmwareSize - writtenBytes);
    const int bytesRead = file.read(buffer.get(), wanted);
    if (bytesRead <= 0) {
      LOG_ERR("FW", "Unexpected end of update file");
      writeFailed = true;
      break;
    }

    result = esp_ota_write(handle, buffer.get(), static_cast<size_t>(bytesRead));
    if (result != ESP_OK) {
      LOG_ERR("FW", "esp_ota_write failed: %s", esp_err_to_name(result));
      writeFailed = true;
      break;
    }

    writtenBytes += static_cast<size_t>(bytesRead);
    const unsigned int percent = static_cast<unsigned int>((writtenBytes * 100) / firmwareSize);
    if (lastRenderedPercent == 101 || percent >= lastRenderedPercent + PROGRESS_STEP_PERCENT || percent == 100) {
      lastRenderedPercent = percent;
      requestUpdate(true);
    }
    delay(1);
  }

  if (writeFailed) {
    esp_ota_abort(handle);
    fail(tr(STR_FIRMWARE_WRITE_FAILED));
    return;
  }

  result = esp_ota_end(handle);
  if (result != ESP_OK) {
    LOG_ERR("FW", "esp_ota_end rejected image: %s", esp_err_to_name(result));
    fail(tr(STR_INVALID_FIRMWARE));
    return;
  }

  result = esp_ota_set_boot_partition(destination);
  if (result != ESP_OK) {
    LOG_ERR("FW", "Could not select OTA partition: %s", esp_err_to_name(result));
    fail(tr(STR_FIRMWARE_WRITE_FAILED));
    return;
  }

  file.close();
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
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
