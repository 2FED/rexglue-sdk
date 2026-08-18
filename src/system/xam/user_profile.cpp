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

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <sstream>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

namespace rex {
namespace system {
namespace xam {

namespace {

constexpr std::array<uint8_t, 8> kPersistentSettingMagic = {'R', 'E', 'X', 'P', 'R', 'O', 'F', '1'};
constexpr size_t kPersistentSettingHeaderSize = 20;
constexpr size_t kMaxPersistentSettingPayload = 64 * 1024;

void StoreLe32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t LoadLe32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) | (static_cast<uint32_t>(source[3]) << 24);
}

std::filesystem::path PersistentSettingPath(const std::filesystem::path& root,
                                            uint32_t setting_id) {
  return root / fmt::format("{:08X}.bin", setting_id);
}

void RecoverPersistentSettingFile(const std::filesystem::path& file_path) {
  std::filesystem::path temporary_path = file_path;
  temporary_path += ".tmp";
  std::filesystem::path backup_path = file_path;
  backup_path += ".bak";
  std::error_code ec;
  if (std::filesystem::exists(file_path, ec)) {
    std::filesystem::remove(temporary_path, ec);
    ec.clear();
    std::filesystem::remove(backup_path, ec);
    return;
  }
  ec.clear();
  if (std::filesystem::exists(backup_path, ec)) {
    ec.clear();
    std::filesystem::rename(backup_path, file_path, ec);
  }
  ec.clear();
  std::filesystem::remove(temporary_path, ec);
}

}  // namespace

UserProfile::UserProfile(uint32_t signin_state) : signin_state_(signin_state == 2 ? 2 : 1) {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."
  xuid_ = 0xB13EBABEBABEBABE;
  name_ = "User";
  REXSYS_INFO("XAM_USER_SIGNIN_CONFIG v=1 state={} mode={}", signin_state_,
              signin_state_ == 2 ? "online-compatible" : "local");

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

  if (setting->is_set && ((setting->is_title_specific() && kernel_state_) ||
                          (!setting->is_title_specific() && !persistence_root_.empty()))) {
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

void UserProfile::SetPersistenceRoot(const std::filesystem::path& root) {
  persistence_root_ = root;
  if (persistence_root_.empty()) {
    return;
  }
  for (const auto& setting : setting_list_) {
    if (!setting->is_title_specific()) {
      LoadPersistentSetting(setting.get());
    }
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
    setting->is_set = false;
    if (setting->type == Setting::Type::BINARY) {
      static_cast<BinarySetting*>(setting)->value.clear();
    }
    auto content_dir = kernel_state_->content_manager()->ResolveGameUserContentPath();
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "rb");
    if (file) {
      fseek(file, 0, SEEK_END);
      const long input_file_size_long = ftell(file);
      fseek(file, 0, SEEK_SET);

      Setting::Key key{};
      key.value = setting->setting_id;
      if (input_file_size_long < 0 || static_cast<uint64_t>(input_file_size_long) > key.size) {
        fclose(file);
        setting->is_set = false;
        if (setting->type == Setting::Type::BINARY) {
          static_cast<BinarySetting*>(setting)->value.clear();
        }
        REXSYS_WARN("Ignoring invalid title profile setting {:08X}", setting->setting_id);
      } else {
        const uint32_t input_file_size = static_cast<uint32_t>(input_file_size_long);
        std::vector<uint8_t> serialized_data(input_file_size);
        const size_t bytes_read = fread(serialized_data.data(), 1, serialized_data.size(), file);
        fclose(file);
        if (bytes_read != serialized_data.size() || !setting->Deserialize(serialized_data)) {
          setting->is_set = false;
          if (setting->type == Setting::Type::BINARY) {
            static_cast<BinarySetting*>(setting)->value.clear();
          }
          REXSYS_WARN("Ignoring invalid title profile setting {:08X}", setting->setting_id);
        }
      }
    }
    setting->loaded_title_id = kernel_state_->title_id();
  } else {
    LoadPersistentSetting(setting);
  }
}

void UserProfile::SaveSetting(UserProfile::Setting* setting) {
  if (setting->is_title_specific()) {
    auto serialized_setting = setting->Serialize();
    auto content_dir = kernel_state_->content_manager()->ResolveGameUserContentPath();
    std::filesystem::create_directories(content_dir);
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    Setting::Key key{};
    key.value = setting->setting_id;
    if (serialized_setting.size() > key.size) {
      REXSYS_WARN("Refusing oversized title profile setting {:08X}", setting->setting_id);
      return;
    }
    auto file = rex::filesystem::OpenFile(file_path, "wb");
    if (!file) {
      REXSYS_WARN("Unable to write title profile setting {:08X}", setting->setting_id);
      return;
    }
    const size_t bytes_written =
        fwrite(serialized_setting.data(), 1, serialized_setting.size(), file);
    fclose(file);
    if (bytes_written != serialized_setting.size()) {
      REXSYS_WARN("Short write for title profile setting {:08X}", setting->setting_id);
    }
  } else {
    SavePersistentSetting(setting);
  }
}

void UserProfile::LoadPersistentSetting(UserProfile::Setting* setting) {
  if (persistence_root_.empty() || setting->is_title_specific()) {
    return;
  }
  const auto file_path = PersistentSettingPath(persistence_root_, setting->setting_id);
  RecoverPersistentSettingFile(file_path);
  std::error_code ec;
  const uintmax_t file_size = std::filesystem::file_size(file_path, ec);
  if (ec) {
    return;
  }
  if (file_size < kPersistentSettingHeaderSize ||
      file_size > kPersistentSettingHeaderSize + kMaxPersistentSettingPayload) {
    REXSYS_WARN("Ignoring invalid persistent profile setting {:08X}", setting->setting_id);
    return;
  }
  std::vector<uint8_t> serialized(static_cast<size_t>(file_size));
  std::ifstream file(file_path, std::ios::binary);
  if (!file || !file.read(reinterpret_cast<char*>(serialized.data()),
                          static_cast<std::streamsize>(serialized.size()))) {
    REXSYS_WARN("Unable to read persistent profile setting {:08X}", setting->setting_id);
    return;
  }
  const uint32_t stored_id = LoadLe32(serialized.data() + 8);
  const uint32_t stored_type = LoadLe32(serialized.data() + 12);
  const uint32_t payload_size = LoadLe32(serialized.data() + 16);
  if (!std::equal(kPersistentSettingMagic.begin(), kPersistentSettingMagic.end(),
                  serialized.begin()) ||
      stored_id != setting->setting_id || stored_type != static_cast<uint32_t>(setting->type) ||
      payload_size != serialized.size() - kPersistentSettingHeaderSize) {
    REXSYS_WARN("Ignoring mismatched persistent profile setting {:08X}", setting->setting_id);
    return;
  }
  std::vector<uint8_t> payload(serialized.begin() + kPersistentSettingHeaderSize, serialized.end());
  Setting::Key key{};
  key.value = setting->setting_id;
  if ((setting->type == Setting::Type::WSTRING || setting->type == Setting::Type::BINARY) &&
      payload.size() > key.size) {
    REXSYS_WARN("Ignoring oversized persistent profile setting {:08X}", setting->setting_id);
    return;
  }
  if (!setting->Deserialize(payload)) {
    REXSYS_WARN("Ignoring malformed persistent profile setting {:08X}", setting->setting_id);
  }
}

void UserProfile::SavePersistentSetting(UserProfile::Setting* setting) {
  if (persistence_root_.empty() || setting->is_title_specific() || !setting->is_set) {
    return;
  }
  const std::vector<uint8_t> payload = setting->Serialize();
  Setting::Key key{};
  key.value = setting->setting_id;
  if (payload.size() > kMaxPersistentSettingPayload ||
      ((setting->type == Setting::Type::WSTRING || setting->type == Setting::Type::BINARY) &&
       payload.size() > key.size)) {
    REXSYS_WARN("Refusing oversized persistent profile setting {:08X}", setting->setting_id);
    return;
  }
  std::vector<uint8_t> serialized(kPersistentSettingHeaderSize + payload.size());
  std::copy(kPersistentSettingMagic.begin(), kPersistentSettingMagic.end(), serialized.begin());
  StoreLe32(serialized.data() + 8, setting->setting_id);
  StoreLe32(serialized.data() + 12, static_cast<uint32_t>(setting->type));
  StoreLe32(serialized.data() + 16, static_cast<uint32_t>(payload.size()));
  std::copy(payload.begin(), payload.end(), serialized.begin() + kPersistentSettingHeaderSize);

  std::error_code ec;
  std::filesystem::create_directories(persistence_root_, ec);
  if (ec) {
    REXSYS_WARN("Unable to create persistent profile directory");
    return;
  }
  const auto file_path = PersistentSettingPath(persistence_root_, setting->setting_id);
  std::filesystem::path temporary_path = file_path;
  temporary_path += ".tmp";
  std::filesystem::path backup_path = file_path;
  backup_path += ".bak";
  {
    std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(serialized.data()),
                             static_cast<std::streamsize>(serialized.size()))) {
      REXSYS_WARN("Unable to write persistent profile setting {:08X}", setting->setting_id);
      return;
    }
  }
  std::filesystem::remove(backup_path, ec);
  ec.clear();
  if (std::filesystem::exists(file_path, ec)) {
    ec.clear();
    std::filesystem::rename(file_path, backup_path, ec);
    if (ec) {
      std::filesystem::remove(temporary_path, ec);
      REXSYS_WARN("Unable to stage persistent profile setting {:08X}", setting->setting_id);
      return;
    }
  }
  ec.clear();
  std::filesystem::rename(temporary_path, file_path, ec);
  if (ec) {
    std::error_code restore_ec;
    if (std::filesystem::exists(backup_path, restore_ec)) {
      std::filesystem::rename(backup_path, file_path, restore_ec);
    }
    REXSYS_WARN("Unable to commit persistent profile setting {:08X}", setting->setting_id);
    return;
  }
  std::filesystem::remove(backup_path, ec);
}

}  // namespace xam
}  // namespace system
}  // namespace rex
