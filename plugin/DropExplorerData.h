#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace DropExplorer {

enum class Campaign : uint8_t {
    All = 0,
    Prophecies,
    Factions,
    Nightfall,
    EyeOfTheNorth,
    EliteCore
};

inline const char* GetCampaignName(Campaign c) {
    switch (c) {
        case Campaign::Prophecies: return "Prophecies";
        case Campaign::Factions: return "Factions";
        case Campaign::Nightfall: return "Nightfall";
        case Campaign::EyeOfTheNorth: return "Eye of the North";
        case Campaign::EliteCore: return "Elite / Core";
        default: return "All";
    }
}

enum class DropRarity : uint8_t {
    Common = 0,
    Uncommon,
    Rare,
    UniqueGreen,
    Exotic,
    Unknown = 255
};

inline const char* GetRarityName(DropRarity r) {
    switch (r) {
        case DropRarity::Common: return "Common";
        case DropRarity::Uncommon: return "Uncommon";
        case DropRarity::Rare: return "Rare";
        case DropRarity::UniqueGreen: return "Green Unique";
        case DropRarity::Exotic: return "Exotic / Ultra";
        default: return "Unknown";
    }
}

struct DropItem {
    std::string name;
    std::string category;
    DropRarity rarity = DropRarity::Unknown;
    float est_chance_pct = -1.0f;
    std::string chance_str = "Unknown";
    std::string stats;
    std::string description;
    std::string notes;
    std::string source_url;
};

struct MobInfo {
    std::string name;
    std::string level_str;
    std::string profession;
    bool is_boss = false;
    std::string boss_skill;
    std::string description;
    std::string movement_notes;
    std::string source_url;
    bool trackable = true;
    std::vector<DropItem> drops;
};

struct ZoneInfo {
    uint32_t map_id = 0;
    uint32_t nearest_outpost_id = 0;
    std::string nearest_outpost_name;
    std::string name;
    Campaign campaign = Campaign::Prophecies;
    std::string region;
    std::vector<MobInfo> mobs;
};

struct ItemLookupResult {
    std::string item_name;
    std::string item_category;
    DropRarity rarity = DropRarity::Unknown;
    float est_chance_pct = -1.0f;
    std::string chance_str = "Unknown";
    std::string stats;
    std::string description;
    std::string notes;
    std::string source_url;
    std::string mob_name;
    std::string mob_level;
    std::string mob_prof;
    bool mob_is_boss = false;
    std::string zone_name;
    std::string region;
    Campaign campaign = Campaign::Prophecies;
    uint32_t map_id = 0;
    uint32_t nearest_outpost_id = 0;
    std::string nearest_outpost_name;
};

std::vector<ZoneInfo> GetBuiltinZones();

} // namespace DropExplorer
