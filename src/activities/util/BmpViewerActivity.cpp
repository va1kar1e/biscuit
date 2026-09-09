#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include <cctype>
#include <cstring>
#include <string_view>

#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
bool isImageFile(const std::string_view name) {
  return FsHelpers::hasBmpExtension(name) || FsHelpers::hasJpgExtension(name) || FsHelpers::hasPngExtension(name);
}

// Natural filename comparison keeps image1, image2, image10 in reading order.
int naturalCompare(const std::string_view left, const std::string_view right) {
  size_t leftPos = 0;
  size_t rightPos = 0;

  while (leftPos < left.size() && rightPos < right.size()) {
    const auto leftChar = static_cast<unsigned char>(left[leftPos]);
    const auto rightChar = static_cast<unsigned char>(right[rightPos]);
    if (std::isdigit(leftChar) && std::isdigit(rightChar)) {
      const size_t leftRunStart = leftPos;
      const size_t rightRunStart = rightPos;
      while (leftPos < left.size() && left[leftPos] == '0') leftPos++;
      while (rightPos < right.size() && right[rightPos] == '0') rightPos++;

      size_t leftEnd = leftPos;
      size_t rightEnd = rightPos;
      while (leftEnd < left.size() && std::isdigit(static_cast<unsigned char>(left[leftEnd]))) leftEnd++;
      while (rightEnd < right.size() && std::isdigit(static_cast<unsigned char>(right[rightEnd]))) rightEnd++;

      const size_t leftDigits = leftEnd - leftPos;
      const size_t rightDigits = rightEnd - rightPos;
      if (leftDigits != rightDigits) return leftDigits < rightDigits ? -1 : 1;

      for (size_t index = 0; index < leftDigits; index++) {
        if (left[leftPos + index] != right[rightPos + index]) {
          return left[leftPos + index] < right[rightPos + index] ? -1 : 1;
        }
      }

      // Equal numeric values: use the shorter digit run as a stable tie-break.
      const size_t leftRunLength = leftEnd - leftRunStart;
      const size_t rightRunLength = rightEnd - rightRunStart;
      if (leftRunLength != rightRunLength) return leftRunLength < rightRunLength ? -1 : 1;
      leftPos = leftEnd;
      rightPos = rightEnd;
      continue;
    }

    const int foldedLeft = std::tolower(leftChar);
    const int foldedRight = std::tolower(rightChar);
    if (foldedLeft != foldedRight) return foldedLeft < foldedRight ? -1 : 1;
    leftPos++;
    rightPos++;
  }

  if (leftPos == left.size() && rightPos == right.size()) return 0;
  return leftPos == left.size() ? -1 : 1;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::showError(const char* message) {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

bool BmpViewerActivity::prepareDisplayFile(std::string& displayPath) {
  if (ownsCache && Storage.exists(IMAGE_CACHE_PATH)) {
    Storage.remove(IMAGE_CACHE_PATH);
    ownsCache = false;
  }

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

bool BmpViewerActivity::findAdjacentImage(const bool forward, std::string& adjacentPath) const {
  const size_t slash = filePath.find_last_of('/');
  const std::string directoryPath = slash == std::string::npos || slash == 0 ? "/" : filePath.substr(0, slash);
  const std::string currentName = slash == std::string::npos ? filePath : filePath.substr(slash + 1);

  FsFile directory = Storage.open(directoryPath.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return false;
  }

  std::string directCandidate;
  std::string wrapCandidate;
  char name[500];
  directory.rewindDirectory();
  for (FsFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    entry.getName(name, sizeof(name));
    entry.close();

    const std::string_view entryName{name};
    if (entryName.empty() || entryName.front() == '.' || !isImageFile(entryName)) continue;

    const int relative = naturalCompare(entryName, currentName);
    if (relative == 0) continue;

    if (forward) {
      if (relative > 0 && (directCandidate.empty() || naturalCompare(entryName, directCandidate) < 0)) {
        directCandidate.assign(entryName);
      }
      if (wrapCandidate.empty() || naturalCompare(entryName, wrapCandidate) < 0) {
        wrapCandidate.assign(entryName);
      }
    } else {
      if (relative < 0 && (directCandidate.empty() || naturalCompare(entryName, directCandidate) > 0)) {
        directCandidate.assign(entryName);
      }
      if (wrapCandidate.empty() || naturalCompare(entryName, wrapCandidate) > 0) {
        wrapCandidate.assign(entryName);
      }
    }
  }
  directory.close();

  const std::string& candidate = directCandidate.empty() ? wrapCandidate : directCandidate;
  if (candidate.empty() || naturalCompare(candidate, currentName) == 0) return false;

  adjacentPath = directoryPath;
  if (adjacentPath.back() != '/') adjacentPath += '/';
  adjacentPath += candidate;
  return true;
}

bool BmpViewerActivity::renderCurrentImage() {

  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress

  std::string displayPath;
  if (!prepareDisplayFile(displayPath)) {
    showError("Could not decode image");
    return false;
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

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "<", ">");
      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    } else {
      showError("Invalid image file");
      file.close();
      return false;
    }

    file.close();
  } else {
    showError("Could not open image");
    return false;
  }
  return true;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  renderCurrentImage();
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

  const auto pageTurn = ReaderUtils::detectPageTurn(mappedInput);
  if (!pageTurn.prev && !pageTurn.next) return;

  std::string adjacentPath;
  if (findAdjacentImage(pageTurn.next, adjacentPath)) {
    filePath = std::move(adjacentPath);
    LOG_DBG("IMG", "Opening adjacent image: %s", filePath.c_str());
    renderCurrentImage();
  }
}
