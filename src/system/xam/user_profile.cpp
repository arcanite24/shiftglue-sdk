/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <sstream>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

REXCVAR_DEFINE_STRING(user_name, "User", "Kernel",
                      "Gamertag the title shows for the signed-in profile (up to 15 letters, "
                      "digits and spaces); the save directory does not depend on it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(user_xuid, "B13EBABEBABEBABE", "Kernel",
                      "Offline XUID of the signed-in profile, 16 hex digits. Titles may sign "
                      "saves with it, and content lives under a folder of this name, so saves "
                      "copied from another emulator profile need that profile's XUID")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex {
namespace system {
namespace xam {

std::string UserProfile::SanitizeGamertag(std::string_view requested) {
  // Xbox 360 gamertags: 1 to 15 characters, letters, digits and spaces, not
  // starting with a digit or space.
  std::string name;
  for (char c : requested) {
    const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    const bool digit = c >= '0' && c <= '9';
    if ((letter || ((digit || c == ' ') && !name.empty())) && name.size() < 15) {
      name.push_back(c);
    }
  }
  while (!name.empty() && name.back() == ' ') {
    name.pop_back();
  }
  return name.empty() ? std::string("User") : name;
}

uint64_t UserProfile::ParseXuid(std::string_view requested) {
  constexpr uint64_t kDefault = 0xB13EBABEBABEBABE;
  // Accept exactly 16 hex digits, optionally prefixed with 0x. Zero and XUIDs
  // carrying the 0x00C0000000000000 bits (refused by titles, see below) fall
  // back to the default rather than locking the player out.
  if (requested.size() == 18 && requested[0] == '0' && (requested[1] == 'x' || requested[1] == 'X')) {
    requested.remove_prefix(2);
  }
  if (requested.size() != 16) {
    if (!requested.empty()) REXSYS_WARN("user_xuid must be 16 hex digits; using the default");
    return kDefault;
  }
  uint64_t value = 0;
  for (char c : requested) {
    uint64_t digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else {
      REXSYS_WARN("user_xuid must be 16 hex digits; using the default");
      return kDefault;
    }
    value = (value << 4) | digit;
  }
  if (!value || (value & 0x00C0000000000000ull)) {
    REXSYS_WARN("user_xuid {:016X} is not a usable offline XUID; using the default", value);
    return kDefault;
  }
  return value;
}

UserProfile::UserProfile() {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."
  xuid_ = ParseXuid(REXCVAR_GET(user_xuid));
  name_ = SanitizeGamertag(REXCVAR_GET(user_name));

  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=gfwl+live&start=195
  // https://github.com/arkem/py360/blob/master/py360/constants.py
  // XPROFILE_GAMER_YAXIS_INVERSION
  AddSetting(std::make_unique<Int32Setting>(0x10040002, 0));
  // XPROFILE_OPTION_CONTROLLER_VIBRATION
  AddSetting(std::make_unique<Int32Setting>(0x10040003, 3));
  // XPROFILE_GAMERCARD_ZONE
  AddSetting(std::make_unique<Int32Setting>(0x10040004, 0));
  // XPROFILE_GAMERCARD_REGION
  AddSetting(std::make_unique<Int32Setting>(0x10040005, 0));
  // XPROFILE_GAMERCARD_CRED
  AddSetting(std::make_unique<Int32Setting>(0x10040006, 0xFA));
  // XPROFILE_GAMERCARD_REP
  AddSetting(std::make_unique<FloatSetting>(0x5004000B, 0.0f));
  // XPROFILE_OPTION_VOICE_MUTED
  AddSetting(std::make_unique<Int32Setting>(0x1004000C, 0));
  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
  AddSetting(std::make_unique<Int32Setting>(0x1004000D, 0));
  // XPROFILE_OPTION_VOICE_VOLUME
  AddSetting(std::make_unique<Int32Setting>(0x1004000E, 0x64));
  // XPROFILE_GAMERCARD_MOTTO
  AddSetting(std::make_unique<UnicodeSetting>(0x402C0011, u""));
  // XPROFILE_GAMERCARD_TITLES_PLAYED
  AddSetting(std::make_unique<Int32Setting>(0x10040012, 1));
  // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040013, 0));
  // XPROFILE_GAMER_DIFFICULTY
  AddSetting(std::make_unique<Int32Setting>(0x10040015, 0));
  // XPROFILE_GAMER_CONTROL_SENSITIVITY
  AddSetting(std::make_unique<Int32Setting>(0x10040018, 0));
  // Preferred color 1
  AddSetting(std::make_unique<Int32Setting>(0x1004001D, 0xFFFF0000u));
  // Preferred color 2
  AddSetting(std::make_unique<Int32Setting>(0x1004001E, 0xFF00FF00u));
  // XPROFILE_GAMER_ACTION_AUTO_AIM
  AddSetting(std::make_unique<Int32Setting>(0x10040022, 1));
  // XPROFILE_GAMER_ACTION_AUTO_CENTER
  AddSetting(std::make_unique<Int32Setting>(0x10040023, 0));
  // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040024, 0));
  // XPROFILE_GAMER_RACE_TRANSMISSION
  AddSetting(std::make_unique<Int32Setting>(0x10040026, 0));
  // XPROFILE_GAMER_RACE_CAMERA_LOCATION
  AddSetting(std::make_unique<Int32Setting>(0x10040027, 0));
  // XPROFILE_GAMER_RACE_BRAKE_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040028, 0));
  // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040029, 0));
  // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040038, 0));
  // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040039, 0));

  // If we set this, games will try to get it.
  // XPROFILE_GAMERCARD_PICTURE_KEY
  AddSetting(std::make_unique<UnicodeSetting>(0x4064000F, u"gamercard_picture_key"));

  // XPROFILE_TITLE_SPECIFIC1
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFF));
  // XPROFILE_TITLE_SPECIFIC2
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFE));
  // XPROFILE_TITLE_SPECIFIC3
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFD));
}

void UserProfile::AddSetting(std::unique_ptr<Setting> setting) {
  Setting* previous_setting = setting.get();
  std::swap(settings_[setting->setting_id], previous_setting);

  if (setting->is_set && setting->is_title_specific()) {
    SaveSetting(setting.get());
  }

  if (previous_setting) {
    // replace: swap out the old setting from the owning list
    for (auto vec_it = setting_list_.begin(); vec_it != setting_list_.end(); ++vec_it) {
      if (vec_it->get() == previous_setting) {
        vec_it->swap(setting);
        break;
      }
    }
  } else {
    // new setting: add to the owning list
    setting_list_.push_back(std::move(setting));
  }
}

UserProfile::Setting* UserProfile::GetSetting(uint32_t setting_id) {
  const auto& it = settings_.find(setting_id);
  if (it == settings_.end()) {
    return nullptr;
  }
  UserProfile::Setting* setting = it->second;
  if (setting->is_title_specific()) {
    // If what we have loaded in memory isn't for the title that is running
    // right now, then load it from disk.
    if (kernel_state_->title_id() != setting->loaded_title_id) {
      LoadSetting(setting);
    }
  }
  return setting;
}

void UserProfile::LoadSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto content_dir = kernel_state_->content_manager()->ResolveGameUserContentPath();
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "rb");
    if (file) {
      fseek(file, 0, SEEK_END);
      uint32_t input_file_size = static_cast<uint32_t>(ftell(file));
      fseek(file, 0, SEEK_SET);

      std::vector<uint8_t> serialized_data(input_file_size);
      fread(serialized_data.data(), 1, serialized_data.size(), file);
      fclose(file);
      setting->Deserialize(serialized_data);
      setting->loaded_title_id = kernel_state_->title_id();
    }
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to load unsupported profile setting from disk");
  }
}

void UserProfile::SaveSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto serialized_setting = setting->Serialize();
    auto content_dir = kernel_state_->content_manager()->ResolveGameUserContentPath();
    std::filesystem::create_directories(content_dir);
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "wb");
    fwrite(serialized_setting.data(), 1, serialized_setting.size(), file);
    fclose(file);
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to save unsupported profile setting to disk");
  }
}

}  // namespace xam
}  // namespace system
}  // namespace rex
