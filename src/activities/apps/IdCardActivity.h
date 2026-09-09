#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class IdCardActivity final : public Activity {
 public:
  explicit IdCardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("IdCard", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { CARD_DISPLAY, EDIT_SELECT };

  static constexpr int FIELD_COUNT = 9;
  static constexpr const char* CONFIG_PATH = "/biscuit/id_card.txt";
  static constexpr const char* CARD_IMAGE_PATH = "/biscuit/id_card.bmp";
  static constexpr const char* WALLPAPER_PATH = "/sleep.bmp";
  static constexpr const char* PHOTO_CACHE_PATH = "/.crosspoint/id-card-photo.bmp";

  struct CardInfo {
    std::string firstName;
    std::string lastName;
    std::string phone;
    std::string email;
    std::string company;
    std::string companyId;
    std::string companyEmail;
    std::string companyPosition;
    std::string department;
    std::string photo;
  };

  State state = CARD_DISPLAY;
  ButtonNavigator buttonNavigator;
  int fieldIndex = 0;
  CardInfo info;
  std::string preparedPhotoPath;
  std::string statusMessage;

  static const char* fieldLabel(int index);
  std::string& fieldValue(int index);
  const std::string& fieldValue(int index) const;
  void loadFromSd();
  bool saveToSd() const;
  bool preparePhoto();
  bool saveCardAndSetWallpaper();
  void renderCard(bool withControls) const;
  void renderEditSelect() const;
};
