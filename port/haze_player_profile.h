#pragma once
#include <filesystem>
#include <string>

// Display identity is independent of the guest user ID and save-directory name.
bool haze_player_name_valid(const std::string& name,std::string& error);
void haze_player_profile_initialize(const std::filesystem::path& save_root);
std::string haze_player_name();
std::string haze_player_profile_error();
bool haze_player_name_save(const std::string& name,std::string& error);
void haze_player_profile_register();
void haze_player_profile_menu_register();
