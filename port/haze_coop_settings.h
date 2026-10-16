#pragma once
#include <filesystem>
#include <string>

void haze_coop_settings_initialize(const std::filesystem::path& save_root);
std::string haze_coop_server();
std::string haze_coop_settings_error();
bool haze_coop_server_normalize(const std::string& input, bool local_test,
                                std::string& endpoint, std::string& error);
bool haze_coop_server_save(const std::string& input, std::string& error);
