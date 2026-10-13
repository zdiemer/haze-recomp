#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Shortcut data is UTF-8. Paths at the Windows boundary remain UTF-16.
struct HazeSteamTarget {
    std::filesystem::path launcher,working_directory,icon;
    std::string arguments;
    std::filesystem::path artwork_directory; // local PS3_GAME directory, never downloaded
};
struct HazeSteamPlan {
    std::string before,after;
    uint32_t appid=0;
    bool created=false;
};
// Pure transformation. Unsupported/corrupt input fails without producing a plan.
// A missing shortcuts.vdf is represented by an empty input; callers must reject
// an existing zero-byte file instead of treating corruption as an empty library.
bool haze_steam_plan(const std::string& input,const HazeSteamTarget& target,
                     HazeSteamPlan& plan,std::string& message);

struct HazeSteamProfile {
    std::filesystem::path steam_root,config_directory;
    std::string account_id;
    std::string persona_name; // optional cached display name; never used as identity
};
// Optional, bounded loginusers.vdf metadata. Invalid data leaves all names empty;
// only existing profiles with matching public individual Steam IDs gain labels.
bool haze_steam_profile_names(const std::string& input,std::vector<HazeSteamProfile>& profiles);
struct HazeSteamResult {
    bool success=false,changed=false;
    uint32_t appid=0;
    std::filesystem::path backup;
    std::string message;
};
struct HazeSteamArtworkResult {
    unsigned installed=0,preserved=0;
    std::string warning;
};
// Caller holds the same closed-client/account locks as shortcut installation.
// Artwork is independently atomic and create-only; existing customization wins.
HazeSteamArtworkResult haze_steam_artwork(const std::filesystem::path& config_directory,
    uint32_t appid,const std::filesystem::path& source_directory);
// Discovery never chooses among profiles. The menu must present the account ID
// and obtain an explicit selection before writing, even if only one was found.
std::vector<HazeSteamProfile> haze_steam_profiles(std::string& message);
HazeSteamResult haze_steam_install(const HazeSteamProfile& profile,const HazeSteamTarget& target);
