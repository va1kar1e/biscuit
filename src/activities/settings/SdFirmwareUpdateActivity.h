#pragma once

#include <cstddef>
#include <string>

#include "activities/Activity.h"

class SdFirmwareUpdateActivity final : public Activity {
 public:
  static constexpr const char* UPDATE_PATH = "/update.bin";
  static constexpr const char* APPLIED_UPDATE_PATH = "/update.applied.bin";

  explicit SdFirmwareUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool detectedAtBoot = false)
      : Activity("SdFirmwareUpdate", renderer, mappedInput), detectedAtBoot(detectedAtBoot) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State { VALIDATING, CONFIRMING, UPDATING, SUCCESS, FAILED };

  State state = State::VALIDATING;
  bool detectedAtBoot = false;
  size_t firmwareSize = 0;
  size_t writtenBytes = 0;
  std::string errorMessage;
  std::string errorDetail;

  bool validateFirmware();
  void promptConfirmation();
  void onConfirmationResult(const ActivityResult& result);
  void performUpdate();
  void fail(const char* message);
  void failWithDetail(const char* message, const char* detail);
  void leaveUpdater();
};
