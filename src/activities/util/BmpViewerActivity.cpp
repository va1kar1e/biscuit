#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

#include "CrossPointSettings.h"
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

uint32_t fnv1aUpdate(uint32_t hash, const uint8_t* data, const size_t length) {
  for (size_t index = 0; index < length; index++) {
    hash ^= data[index];
    hash *= 16777619u;
  }
  return hash;
}

uint32_t sourceFingerprint(FsFile& source, const std::string_view path) {
  uint32_t hash = fnv1aUpdate(2166136261u, reinterpret_cast<const uint8_t*>(path.data()), path.size());
  const size_t sourceSize = source.fileSize();
  hash = fnv1aUpdate(hash, reinterpret_cast<const uint8_t*>(&sourceSize), sizeof(sourceSize));

  uint8_t sample[64];
  source.seekSet(0);
  const int headRead = source.read(sample, sizeof(sample));
  if (headRead > 0) hash = fnv1aUpdate(hash, sample, static_cast<size_t>(headRead));

  if (sourceSize > sizeof(sample)) {
    source.seekSet(sourceSize - sizeof(sample));
    const int tailRead = source.read(sample, sizeof(sample));
    if (tailRead > 0) hash = fnv1aUpdate(hash, sample, static_cast<size_t>(tailRead));
  }
  source.seekSet(0);
  return hash;
}

bool isValidBmpCache(const std::string& path) {
  FsFile cache;
  if (!Storage.openFileForRead("IMG", path, cache)) return false;
  Bitmap bitmap(cache, true);
  const bool valid = bitmap.parseHeaders() == BmpReaderError::Ok;
  cache.close();
  return valid;
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
  if (FsHelpers::hasBmpExtension(filePath)) {
    displayPath = filePath;
    return true;
  }

  if (!FsHelpers::hasJpgExtension(filePath) && !FsHelpers::hasPngExtension(filePath)) {
    LOG_ERR("IMG", "Unsupported image type: %s", filePath.c_str());
    return false;
  }

  FsFile source;
  if (!Storage.openFileForRead("IMG", filePath, source)) {
    LOG_ERR("IMG", "Could not open source image: %s", filePath.c_str());
    return false;
  }

  // Keep a persistent, per-source BMP cache. The old implementation reused one
  // temporary file and deleted it on every page turn, forcing JPG/PNG decoding
  // each time the reader moved back to an image it had already displayed.
  Storage.ensureDirectoryExists(IMAGE_CACHE_DIR);
  const uint32_t fingerprint = sourceFingerprint(source, filePath);
  char cacheName[96];
  std::snprintf(cacheName, sizeof(cacheName), "%s/%08lx-%dx%d.bmp", IMAGE_CACHE_DIR,
                static_cast<unsigned long>(fingerprint), renderer.getScreenWidth(), renderer.getScreenHeight());
  const std::string cachePath{cacheName};

  if (Storage.exists(cachePath.c_str())) {
    if (isValidBmpCache(cachePath)) {
      source.close();
      displayPath = cachePath;
      LOG_DBG("IMG", "Using cached image: %s", cachePath.c_str());
      return true;
    }
    Storage.remove(cachePath.c_str());
  }

  // The converters stream decoded rows to an SD-backed BMP. This avoids a
  // second full-screen allocation on the ESP32-C3 and keeps peak heap bounded.
  const std::string temporaryPath = cachePath + ".tmp";
  if (Storage.exists(temporaryPath.c_str())) Storage.remove(temporaryPath.c_str());
  FsFile cache;
  if (!Storage.openFileForWrite("IMG", temporaryPath, cache)) {
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
    Storage.remove(temporaryPath.c_str());
    return false;
  }

  if (!Storage.rename(temporaryPath.c_str(), cachePath.c_str())) {
    LOG_ERR("IMG", "Could not finalize image cache: %s", cachePath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }

  displayPath = cachePath;
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

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_WALLPAPER), "<", ">");
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

bool BmpViewerActivity::setCurrentAsWallpaper() {
  constexpr const char* WALLPAPER_PATH = "/sleep.bmp";
  constexpr const char* TEMP_WALLPAPER_PATH = "/.sleep-wallpaper.tmp";
  constexpr size_t COPY_BUFFER_SIZE = 4096;

  std::string displayPath;
  if (!prepareDisplayFile(displayPath)) return false;

  FsFile source;
  if (!Storage.openFileForRead("IMG", displayPath, source)) return false;

  if (Storage.exists(TEMP_WALLPAPER_PATH)) Storage.remove(TEMP_WALLPAPER_PATH);
  FsFile destination;
  if (!Storage.openFileForWrite("IMG", TEMP_WALLPAPER_PATH, destination)) {
    source.close();
    return false;
  }

  auto buffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[COPY_BUFFER_SIZE]);
  bool copied = buffer != nullptr;
  const size_t sourceSize = source.fileSize();
  size_t copiedBytes = 0;
  while (copied && copiedBytes < sourceSize) {
    const size_t wanted = std::min(COPY_BUFFER_SIZE, sourceSize - copiedBytes);
    const int bytesRead = source.read(buffer.get(), wanted);
    if (bytesRead != static_cast<int>(wanted) || destination.write(buffer.get(), wanted) != wanted) {
      copied = false;
      break;
    }
    copiedBytes += wanted;
  }
  destination.flush();
  destination.close();
  source.close();

  if (!copied) {
    Storage.remove(TEMP_WALLPAPER_PATH);
    return false;
  }

  if (Storage.exists(WALLPAPER_PATH)) Storage.remove(WALLPAPER_PATH);
  if (!Storage.rename(TEMP_WALLPAPER_PATH, WALLPAPER_PATH)) {
    Storage.remove(TEMP_WALLPAPER_PATH);
    return false;
  }

  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("IMG", "Wallpaper saved but sleep-screen setting could not be persisted");
    return false;
  }
  LOG_INF("IMG", "Wallpaper set from: %s", filePath.c_str());
  return true;
}

bool BmpViewerActivity::preserveScreenOnSleep() const {
  // A selected wallpaper uses the existing Custom sleep-screen pipeline. For
  // other modes retain OTA6's behavior of pinning the currently viewed image.
  return SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
}

void BmpViewerActivity::onExit() {
  // Caches intentionally survive leaving the viewer so reopening an image is
  // fast. Settings > Clear Cache removes the whole image cache directory.
  if (Storage.exists(LEGACY_CACHE_PATH)) Storage.remove(LEGACY_CACHE_PATH);
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

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    GUI.drawPopup(renderer, setCurrentAsWallpaper() ? tr(STR_WALLPAPER_SET) : tr(STR_WALLPAPER_FAILED));
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
