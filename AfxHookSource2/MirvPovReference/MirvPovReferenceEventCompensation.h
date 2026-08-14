#pragma once

#include "../MirvPovEventCompensation.h"

namespace live_hud::event_compensation {

using GrenadeKind = MirvPovEventCompensation::GrenadeKind;
using RadarSoundKind = MirvPovEventCompensation::RadarSoundKind;
using DeathBannerResolution = MirvPovEventCompensation::DeathBannerResolution;
using RadarSoundSpec = MirvPovEventCompensation::RadarSoundSpec;

inline int kill_cash_reward(std::string_view weapon) noexcept {
    return MirvPovEventCompensation::kill_cash_reward(weapon);
}
inline GrenadeKind grenade_kind(std::string_view weapon) noexcept {
    return MirvPovEventCompensation::grenade_kind(weapon);
}
inline const char* grenade_localization_token(GrenadeKind kind) noexcept {
    return MirvPovEventCompensation::grenade_localization_token(kind);
}
inline RadarSoundSpec radar_sound_from_event(
    std::string_view event_name, std::string_view weapon = {},
    bool silenced = false) noexcept {
    return MirvPovEventCompensation::radar_sound_from_event(event_name, weapon,
                                                             silenced);
}
inline bool native_movement_sound_needs_presentation_repair(
    int radius, float duration, bool is_footstep) noexcept {
    return MirvPovEventCompensation::native_movement_sound_needs_presentation_repair(
        radius, duration, is_footstep);
}
inline bool native_generic_footstep_needs_max(
    int radius, float duration, bool is_footstep) noexcept {
    return MirvPovEventCompensation::native_generic_footstep_needs_max(
        radius, duration, is_footstep);
}
inline bool native_sound_covers_event(std::uint64_t queued_stamp,
                                      std::uint64_t current_stamp,
                                      std::uint64_t last_native_stamp) noexcept {
    return MirvPovEventCompensation::native_sound_covers_event(
        queued_stamp, current_stamp, last_native_stamp);
}
inline DeathBannerResolution resolve_death_banner(
    std::uint64_t death_stamp, std::uint64_t current_stamp,
    std::uint64_t last_killer_stamp, std::uint64_t pair_window) noexcept {
    return MirvPovEventCompensation::resolve_death_banner(
        death_stamp, current_stamp, last_killer_stamp, pair_window);
}

} // namespace live_hud::event_compensation
