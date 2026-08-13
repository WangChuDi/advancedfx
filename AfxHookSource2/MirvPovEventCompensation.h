#pragma once

#include <cstdint>
#include <string_view>

namespace MirvPovEventCompensation {

enum class GrenadeKind {
    none,
    flashbang,
    smoke,
    high_explosive,
    incendiary,
    decoy
};

enum class RadarSoundKind {
    none,
    footstep,
    weapon,
    scope,
    utility
};

enum class DeathBannerResolution {
    wait,
    native_summary,
    zero_summary
};

struct RadarSoundSpec {
    RadarSoundKind kind = RadarSoundKind::none;
    int radius = 0;
    float duration = 0.0f;
    bool is_footstep = false;

    explicit operator bool() const noexcept {
        return kind != RadarSoundKind::none && radius > 0 && duration > 0.0f;
    }
};

int kill_cash_reward(std::string_view weapon) noexcept;
GrenadeKind grenade_kind(std::string_view weapon) noexcept;
const char* grenade_localization_token(GrenadeKind kind) noexcept;
RadarSoundSpec radar_sound_from_event(std::string_view event_name,
                                      std::string_view weapon = {},
                                      bool silenced = false) noexcept;
bool native_movement_sound_needs_presentation_repair(
    int radius, float duration, bool is_footstep) noexcept;
bool native_generic_footstep_needs_max(int radius, float duration,
                                       bool is_footstep) noexcept;
bool native_sound_covers_event(std::uint64_t queued_stamp,
                               std::uint64_t current_stamp,
                               std::uint64_t last_native_stamp) noexcept;
DeathBannerResolution resolve_death_banner(
    std::uint64_t death_stamp, std::uint64_t current_stamp,
    std::uint64_t last_killer_stamp, std::uint64_t pair_window) noexcept;

} // namespace MirvPovEventCompensation
