#pragma once

#include <functional>
#include <string>

#include "../Activity.h"
#include "MappedInputManager.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preserveScreenOnSleep() const override { return true; }

 private:
  static constexpr const char* IMAGE_CACHE_PATH = "/.image-viewer-cache.bmp";

  std::string filePath;
  bool ownsCache = false;

  bool renderCurrentImage();
  bool prepareDisplayFile(std::string& displayPath);
  bool findAdjacentImage(bool forward, std::string& adjacentPath) const;
  void showError(const char* message);
};
