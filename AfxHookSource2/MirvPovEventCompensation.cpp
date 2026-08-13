#include "stdafx.h"

#include "MirvPovEventCompensation.h"

namespace MirvPovEventCompensation {
namespace {

std::string_view without_weapon_prefix(std::string_view weapon) noexcept {
    constexpr std::string_view prefix = "weapon_";
    if (weapon.size() >= prefix.size() &&
        weapon.substr(0, prefix.size()) == prefix) {
        weapon.remove_prefix(prefix.size());
    }
    return weapon;
}

bool is_utility(std::string_view weapon) noexcept {
    weapon = without_weapon_prefix(weapon);
    return weapon == "flashbang" || weapon == "smokegrenade" ||
           weapon == "hegrenade" || weapon == "incgrenade" ||
           weapon == "molotov" || weapon == "decoy";
}

bool is_gun(std::string_view weapon) noexcept {
    weapon = without_weapon_prefix(weapon);
    if (weapon.empty() || is_utility(weapon) || weapon == "c4" ||
        weapon == "taser" || weapon == "bayonet" ||
        weapon.find("knife") != std::string_view::npos) {
        return false;
    }
    return true;
}

bool is_heavy_loud_weapon(std::string_view weapon) noexcept {
    weapon = without_weapon_prefix(weapon);
    return weapon == "awp" || weapon == "ssg08" || weapon == "g3sg1" ||
           weapon == "scar20" || weapon == "negev" || weapon == "m249";
}

} // namespace

int kill_cash_reward(std::string_view weapon) noexcept {
    weapon = without_weapon_prefix(weapon);
    if (weapon.find("knife") != std::string_view::npos ||
        weapon == "bayonet") {
        return 1500;
    }
    if (weapon == "nova" || weapon == "xm1014" || weapon == "mag7" ||
        weapon == "sawedoff") {
        return 900;
    }
    if (weapon == "mp9" || weapon == "mac10" || weapon == "mp7" ||
        weapon == "mp5sd" || weapon == "ump45" || weapon == "bizon") {
        return 600;
    }
    if (weapon == "awp" || weapon == "cz75a" || weapon == "taser") {
        return 100;
    }
    return 300;
}

GrenadeKind grenade_kind(std::string_view weapon) noexcept {
    weapon = without_weapon_prefix(weapon);
    if (weapon == "flashbang") {
        return GrenadeKind::flashbang;
    }
    if (weapon == "smokegrenade") {
        return GrenadeKind::smoke;
    }
    if (weapon == "hegrenade") {
        return GrenadeKind::high_explosive;
    }
    if (weapon == "incgrenade" || weapon == "molotov") {
        return GrenadeKind::incendiary;
    }
    if (weapon == "decoy") {
        return GrenadeKind::decoy;
    }
    return GrenadeKind::none;
}

const char* grenade_localization_token(GrenadeKind kind) noexcept {
    switch (kind) {
    case GrenadeKind::flashbang:
        return "#SFUI_TitlesTXT_Flashbang";
    case GrenadeKind::smoke:
        return "#SFUI_TitlesTXT_Smoke_in_the_hole";
    case GrenadeKind::high_explosive:
        return "#SFUI_TitlesTXT_Fire_in_the_hole";
    case GrenadeKind::incendiary:
        return "#SFUI_TitlesTXT_Incendiary_in_the_hole";
    case GrenadeKind::decoy:
        return "#SFUI_TitlesTXT_Decoy_in_the_hole";
    case GrenadeKind::none:
        return nullptr;
    }
    return nullptr;
}

RadarSoundSpec radar_sound_from_event(std::string_view event_name,
                                      std::string_view weapon,
                                      bool silenced) noexcept {
    if (event_name == "player_footstep") {
        return {RadarSoundKind::footstep, 1100, 0.5f, true};
    }
    if (event_name == "player_jump") {
        return {RadarSoundKind::utility, 204, 0.1f, false};
    }
    if (event_name == "weapon_zoom") {
        return {RadarSoundKind::scope, 597, 0.1f, false};
    }
    if (event_name != "weapon_fire") {
        return {};
    }
    if (is_utility(weapon)) {
        return {RadarSoundKind::utility, 700, 0.16f, false};
    }
    if (!is_gun(weapon)) {
        return {};
    }

    weapon = without_weapon_prefix(weapon);
    if (silenced || weapon.find("silencer") != std::string_view::npos) {
        return {RadarSoundKind::weapon, 800, 0.1f, false};
    }
    if (is_heavy_loud_weapon(weapon)) {
        return {RadarSoundKind::weapon, 1400, 0.1f, false};
    }
    return {RadarSoundKind::weapon, 1100, 0.1f, false};
}

bool native_movement_sound_needs_presentation_repair(
    int radius, float duration, bool is_footstep) noexcept {
    return !is_footstep && radius == 548 && duration >= 0.08f &&
           duration <= 0.12f;
}

bool native_generic_footstep_needs_max(int radius, float duration,
                                       bool is_footstep) noexcept {
    return !is_footstep && radius == 1100 && duration >= 0.48f &&
           duration <= 0.52f;
}

bool native_sound_covers_event(std::uint64_t queued_stamp,
                               std::uint64_t current_stamp,
                               std::uint64_t last_native_stamp) noexcept {
    constexpr std::uint64_t kNativeLeadTolerance = 25;
    const auto window_start = queued_stamp > kNativeLeadTolerance
                                  ? queued_stamp - kNativeLeadTolerance
                                  : 0;
    return last_native_stamp >= window_start &&
           last_native_stamp <= current_stamp;
}

DeathBannerResolution resolve_death_banner(
    std::uint64_t death_stamp, std::uint64_t current_stamp,
    std::uint64_t last_killer_stamp, std::uint64_t pair_window) noexcept {
    if (!death_stamp || current_stamp < death_stamp) {
        return DeathBannerResolution::wait;
    }
    if (last_killer_stamp) {
        const auto delta = last_killer_stamp >= death_stamp
                               ? last_killer_stamp - death_stamp
                               : death_stamp - last_killer_stamp;
        if (delta <= pair_window) {
            return DeathBannerResolution::native_summary;
        }
    }
    if (current_stamp - death_stamp >= pair_window) {
        return DeathBannerResolution::zero_summary;
    }
    return DeathBannerResolution::wait;
}

} // namespace MirvPovEventCompensation
