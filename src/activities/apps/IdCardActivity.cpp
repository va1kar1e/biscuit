#include "IdCardActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <PngToBmpConverter.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr size_t MAX_CONFIG_BYTES = 2048;

std::string trim(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

void appendConfigLine(String& output, const char* key, const std::string& value) {
  output += key;
  output += '=';
  output += value.c_str();
  output += '\n';
}
}  // namespace

const char* IdCardActivity::fieldLabel(const int index) {
  switch (index) {
    case 0: return "First name";
    case 1: return "Last name";
    case 2: return "Phone";
    case 3: return "Email";
    case 4: return "Company";
    case 5: return "Company ID";
    case 6: return "Company Email";
    case 7: return "Company Position";
    case 8: return "Department";
    default: return "";
  }
}

std::string& IdCardActivity::fieldValue(const int index) {
  switch (index) {
    case 0: return info.firstName;
    case 1: return info.lastName;
    case 2: return info.phone;
    case 3: return info.email;
    case 4: return info.company;
    case 5: return info.companyId;
    case 6: return info.companyEmail;
    case 7: return info.companyPosition;
    case 8: return info.department;
    default: return info.firstName;
  }
}

const std::string& IdCardActivity::fieldValue(const int index) const {
  return const_cast<IdCardActivity*>(this)->fieldValue(index);
}

void IdCardActivity::loadFromSd() {
  info = {};
  char buffer[MAX_CONFIG_BYTES + 1] = {};
  const size_t bytes = Storage.readFileToBuffer(CONFIG_PATH, buffer, sizeof(buffer), MAX_CONFIG_BYTES);
  if (bytes == 0) {
    saveToSd();
    return;
  }

  std::string contents(buffer, bytes);
  size_t start = 0;
  while (start < contents.size()) {
    const size_t end = contents.find('\n', start);
    const std::string line = trim(contents.substr(start, end == std::string::npos ? std::string::npos : end - start));
    start = end == std::string::npos ? contents.size() : end + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = trim(line.substr(0, equals));
    const std::string value = trim(line.substr(equals + 1));
    if (key == "first_name") info.firstName = value;
    else if (key == "last_name") info.lastName = value;
    // Keep accepting the original `position` key so existing cards migrate
    // without requiring a manual config edit.
    else if (key == "position" || key == "company_position") info.companyPosition = value;
    else if (key == "email") info.email = value;
    else if (key == "phone") info.phone = value;
    else if (key == "company") info.company = value;
    else if (key == "company_id") info.companyId = value;
    else if (key == "company_email") info.companyEmail = value;
    else if (key == "department") info.department = value;
    else if (key == "photo") info.photo = value;
  }
}

bool IdCardActivity::saveToSd() const {
  Storage.ensureDirectoryExists("/biscuit");
  String output;
  output.reserve(512);
  output += "# Biscuit ID Card (UTF-8)\n";
  appendConfigLine(output, "first_name", info.firstName);
  appendConfigLine(output, "last_name", info.lastName);
  appendConfigLine(output, "phone", info.phone);
  appendConfigLine(output, "email", info.email);
  appendConfigLine(output, "company", info.company);
  appendConfigLine(output, "company_id", info.companyId);
  appendConfigLine(output, "company_email", info.companyEmail);
  appendConfigLine(output, "company_position", info.companyPosition);
  appendConfigLine(output, "department", info.department);
  appendConfigLine(output, "photo", info.photo);
  return Storage.writeFile(CONFIG_PATH, output);
}

bool IdCardActivity::preparePhoto() {
  preparedPhotoPath.clear();
  std::string sourcePath = info.photo;
  if (sourcePath.empty()) {
    constexpr const char* candidates[] = {
        "/biscuit/id_photo.bmp", "/biscuit/id_photo.jpg",
        "/biscuit/id_photo.jpeg", "/biscuit/id_photo.png"};
    for (const char* candidate : candidates) {
      if (Storage.exists(candidate)) {
        sourcePath = candidate;
        break;
      }
    }
  }
  if (sourcePath.empty() || !Storage.exists(sourcePath.c_str())) return false;

  if (!FsHelpers::hasJpgExtension(sourcePath) && !FsHelpers::hasPngExtension(sourcePath)) {
    preparedPhotoPath = sourcePath;
    return true;
  }

  FsFile source;
  if (!Storage.openFileForRead("IDC", sourcePath, source)) return false;
  Storage.ensureDirectoryExists("/.crosspoint");
  if (Storage.exists(PHOTO_CACHE_PATH)) Storage.remove(PHOTO_CACHE_PATH);
  FsFile cache;
  if (!Storage.openFileForWrite("IDC", PHOTO_CACHE_PATH, cache)) {
    source.close();
    return false;
  }

  constexpr int PHOTO_WIDTH = 180;
  constexpr int PHOTO_HEIGHT = 220;
  const bool converted = FsHelpers::hasJpgExtension(sourcePath)
      ? JpegToBmpConverter::jpegFileToBmpStreamWithSize(source, cache, PHOTO_WIDTH, PHOTO_HEIGHT)
      : PngToBmpConverter::pngFileToBmpStreamWithSize(source, cache, PHOTO_WIDTH, PHOTO_HEIGHT);
  cache.close();
  source.close();
  if (!converted) {
    Storage.remove(PHOTO_CACHE_PATH);
    return false;
  }
  preparedPhotoPath = PHOTO_CACHE_PATH;
  return true;
}

void IdCardActivity::onEnter() {
  Activity::onEnter();
  loadFromSd();
  preparePhoto();
  state = CARD_DISPLAY;
  fieldIndex = 0;
  statusMessage.clear();
  requestUpdate();
}

void IdCardActivity::loop() {
  if (state == CARD_DISPLAY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = EDIT_SELECT;
      fieldIndex = 0;
      statusMessage.clear();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      loadFromSd();
      const bool photoReady = preparePhoto();
      statusMessage = photoReady ? "Config and photo reloaded" : "Config reloaded; no photo";
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      statusMessage = saveCardAndSetWallpaper() ? "Saved and set as wallpaper" : "Could not save wallpaper";
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    state = CARD_DISPLAY;
    requestUpdate();
    return;
  }
  buttonNavigator.onNext([this] {
    fieldIndex = ButtonNavigator::nextIndex(fieldIndex, FIELD_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    fieldIndex = ButtonNavigator::previousIndex(fieldIndex, FIELD_COUNT);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int index = fieldIndex;
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, fieldLabel(index), fieldValue(index), 96),
        [this, index](const ActivityResult& result) {
          if (!result.isCancelled) {
            fieldValue(index) = std::get<KeyboardResult>(result.data).text;
            statusMessage = saveToSd() ? "Saved to id_card.txt" : "Could not save config";
          }
        });
  }
}

bool IdCardActivity::saveCardAndSetWallpaper() {
  renderCard(false);
  const uint8_t* framebuffer = renderer.getFrameBuffer();
  const bool cardSaved = ScreenshotUtil::saveFramebufferAsBmp(
      CARD_IMAGE_PATH, framebuffer, HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT);
  if (!cardSaved) return false;
  const bool wallpaperSaved = ScreenshotUtil::saveFramebufferAsBmp(
      WALLPAPER_PATH, framebuffer, HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT);
  if (!wallpaperSaved) return false;
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  return SETTINGS.saveToFile();
}

void IdCardActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  if (state == CARD_DISPLAY) renderCard(true);
  else renderEditSelect();
  renderer.displayBuffer();
}

void IdCardActivity::renderCard(const bool withControls) const {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bottom = withControls ? pageHeight - metrics.buttonHintsHeight : pageHeight;
  constexpr int PAD = 10;
  renderer.drawRoundedRect(PAD, PAD, pageWidth - PAD * 2, bottom - PAD * 2, 2, 12, true);

  renderer.drawCenteredText(PKNAKHONSAWAN_16_FONT_ID, 25, "ID CARD", true, EpdFontFamily::BOLD);
  const int headerSeparatorY = 25 + renderer.getLineHeight(PKNAKHONSAWAN_16_FONT_ID) + 10;
  renderer.fillRect(PAD + 8, headerSeparatorY, pageWidth - (PAD + 8) * 2, 2, true);

  constexpr int PHOTO_X = 24;
  const int PHOTO_Y = headerSeparatorY + 22;
  constexpr int PHOTO_W = 180;
  constexpr int PHOTO_H = 220;
  renderer.drawRect(PHOTO_X, PHOTO_Y, PHOTO_W, PHOTO_H, 2, true);
  bool photoDrawn = false;
  if (!preparedPhotoPath.empty()) {
    FsFile file;
    if (Storage.openFileForRead("IDC", preparedPhotoPath, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderer.drawBitmap(bitmap, PHOTO_X + 4, PHOTO_Y + 4, PHOTO_W - 8, PHOTO_H - 8);
        photoDrawn = true;
      }
      file.close();
    }
  }
  if (!photoDrawn) {
    renderer.drawCenteredText(SMALL_FONT_ID, PHOTO_Y + PHOTO_H / 2 - 7, "PHOTO");
  }

  const int textX = 224;
  const int textW = pageWidth - textX - 24;
  std::string fullName = info.firstName;
  if (!fullName.empty() && !info.lastName.empty()) fullName += ' ';
  fullName += info.lastName;
  const auto nameLines = renderer.wrappedText(PKNAKHONSAWAN_18_FONT_ID, fullName.c_str(), textW, 3,
                                              EpdFontFamily::BOLD);
  // With the position moved into the detail list, use the full portrait-height
  // lane for the name and center the wrapped block beside the photo.
  const int nameLineHeight = renderer.getLineHeight(PKNAKHONSAWAN_18_FONT_ID);
  const int nameHeight = static_cast<int>(nameLines.size()) * nameLineHeight;
  int y = PHOTO_Y + std::max(0, (PHOTO_H - nameHeight) / 2);
  for (const auto& line : nameLines) {
    const int lineWidth = renderer.getTextWidth(PKNAKHONSAWAN_18_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(PKNAKHONSAWAN_18_FONT_ID, textX + (textW - lineWidth) / 2, y,
                      line.c_str(), true, EpdFontFamily::BOLD);
    y += nameLineHeight;
  }

  const int separatorY = PHOTO_Y + PHOTO_H + 24;
  renderer.fillRect(PAD + 8, separatorY, pageWidth - (PAD + 8) * 2, 1, true);
  int rowY = separatorY + 16;
  const int valueX = std::max(142, 40 + renderer.getTextWidth(SMALL_FONT_ID, "DEPARTMENT", EpdFontFamily::BOLD));
  const int valueWidth = pageWidth - valueX - 26;
  const int rowHeight = renderer.getLineHeight(PKNAKHONSAWAN_14_FONT_ID);
  auto drawContact = [&](const char* label, const std::string& value, int maxLines = 1) {
    if (value.empty()) return;
    renderer.drawText(SMALL_FONT_ID, 28, rowY, label, true, EpdFontFamily::BOLD);
    for (const auto& line : renderer.wrappedText(PKNAKHONSAWAN_14_FONT_ID, value.c_str(), valueWidth, maxLines)) {
      renderer.drawText(PKNAKHONSAWAN_14_FONT_ID, valueX, rowY - 3, line.c_str());
      rowY += rowHeight;
    }
  };
  drawContact("PHONE", info.phone);
  drawContact("EMAIL", info.email);
  const bool hasCompanyDetails = !info.company.empty() || !info.companyId.empty() ||
                                 !info.companyEmail.empty() || !info.companyPosition.empty() || !info.department.empty();
  if (hasCompanyDetails && (!info.phone.empty() || !info.email.empty())) {
    renderer.fillRect(PAD + 8, rowY + 4, pageWidth - (PAD + 8) * 2, 1, true);
    rowY += 20;
  }
  drawContact("COMPANY", info.company);
  drawContact("ID", info.companyId);
  drawContact("EMAIL", info.companyEmail);
  drawContact("POSITION", info.companyPosition, 2);
  drawContact("DEPARTMENT", info.department);
  if (withControls && !statusMessage.empty()) {
    renderer.drawCenteredText(SMALL_FONT_ID, bottom - 35,
                              renderer.truncatedText(SMALL_FONT_ID, statusMessage.c_str(), pageWidth - 30).c_str());
  }
  if (withControls) {
    const auto labels = mappedInput.mapLabels("Back", "Edit", "Reload", "Wallpaper");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void IdCardActivity::renderEditSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Edit ID Card");
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listHeight = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listHeight}, FIELD_COUNT, fieldIndex,
               [this](const int index) -> std::string {
                 const std::string& value = fieldValue(index);
                 return std::string(fieldLabel(index)) + ": " + (value.empty() ? "-" : value);
               });
  const auto labels = mappedInput.mapLabels("Back", "Edit", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
