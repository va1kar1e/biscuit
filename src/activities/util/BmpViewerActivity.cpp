#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include "components/UITheme.h"
#include "fontIds.h"

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::showError(const char* message) {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

bool BmpViewerActivity::prepareDisplayFile(std::string& displayPath) {
  if (FsHelpers::hasBmpExtension(filePath)) {
    displayPath = filePath;
    return true;
  }

  if (!FsHelpers::hasJpgExtension(filePath) && !FsHelpers::hasPngExtension(filePath)) {
    LOG_ERR("IMG", "Unsupported image type: %s", filePath.c_str());
    return false;
  }

  // The converters stream decoded rows to an SD-backed BMP. This avoids a
  // second full-screen allocation on the ESP32-C3 and keeps peak heap bounded.
  if (Storage.exists(IMAGE_CACHE_PATH)) {
    Storage.remove(IMAGE_CACHE_PATH);
  }

  FsFile source;
  if (!Storage.openFileForRead("IMG", filePath, source)) {
    LOG_ERR("IMG", "Could not open source image: %s", filePath.c_str());
    return false;
  }

  FsFile cache;
  if (!Storage.openFileForWrite("IMG", IMAGE_CACHE_PATH, cache)) {
    LOG_ERR("IMG", "Could not create image cache");
    source.close();
    return false;
  }

  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  bool converted = false;
  if (FsHelpers::hasJpgExtension(filePath)) {
    converted = JpegToBmpConverter::jpegFileToBmpStreamWithSize(source, cache, width, height);
  } else {
    converted = PngToBmpConverter::pngFileToBmpStreamWithSize(source, cache, width, height);
  }

  cache.close();
  source.close();

  if (!converted) {
    LOG_ERR("IMG", "Image conversion failed: %s", filePath.c_str());
    Storage.remove(IMAGE_CACHE_PATH);
    return false;
  }

  displayPath = IMAGE_CACHE_PATH;
  ownsCache = true;
  return true;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress

  std::string displayPath;
  if (!prepareDisplayFile(displayPath)) {
    showError("Could not decode image");
    return;
  }

  FsFile file;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  if (Storage.openFileForRead("IMG", displayPath, file)) {
    Bitmap bitmap(file, true);

    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    } else {
      showError("Invalid image file");
    }

    file.close();
  } else {
    showError("Could not open image");
  }
}

void BmpViewerActivity::onExit() {
  if (ownsCache && Storage.exists(IMAGE_CACHE_PATH)) {
    Storage.remove(IMAGE_CACHE_PATH);
    ownsCache = false;
  }
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
}
