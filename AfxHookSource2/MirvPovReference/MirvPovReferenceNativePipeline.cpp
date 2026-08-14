#include "MirvPovReferenceNativePipeline.h"

#include "MirvPovReferenceCompat.h"
#include "MirvPovReferenceDetour.h"
#include "MirvPovReferenceEventCompensation.h"
#include "MirvPovReferenceHooks.h"
#include "MirvPovReferenceIdentity.h"
#include "MirvPovReferencePov.h"
#include "MirvPovReferenceOffsets.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace live_hud::native_pipeline {
namespace {

using detour::EntryHook;
using detour::RelCallHook;
using detour::install_entry;
using detour::install_rel_call;
using detour::restore_entry;
using detour::restore_rel_call;

using OneArgFn = std::uint64_t(__fastcall*)(std::uintptr_t);
using OneArgVoidFn = void(__fastcall*)(std::uintptr_t);
using TwoArgFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t);
using TwoArgVoidFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
using FiveArgVoidFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t,
                                        std::uintptr_t, std::uintptr_t,
                                        std::uintptr_t);
using HudGetterFn = void*(__fastcall*)();
using BoolNoArgFn = bool(__fastcall*)();
using BoolOneArgFn = bool(__fastcall*)(void*);
using UIntOneArgFn = unsigned int(__fastcall*)(void*);
using HudTeamRelationshipFn = bool(__fastcall*)(void*, unsigned int);
using BuyZonePredicateFn = bool(__fastcall*)(void*, bool);
using VoiceStateGetFn = void*(__fastcall*)();
using VoiceActivityFn = float(__fastcall*)(int);
using VoiceUpdateSpeakerStatusFn = std::int64_t(__fastcall*)(
    void*, unsigned, int, std::uint8_t);
using EmitHurtFeedbackFn = void(__fastcall*)(void*, void*, const char*);
using PushNoticeFn = std::int64_t(__fastcall*)(void*, char*, unsigned,
                                               std::uint8_t*);
using FindHudElementFn = void*(__fastcall*)(const char*);
using PlayerPawnEventFn = void(__fastcall*)(void*, void*);
using RadarTransactionFn = void(__fastcall*)(std::uintptr_t, std::uint8_t);
using RadarSoundSubmitFn = void(__fastcall*)(void*, int, float, bool);
using RadarSoundCreateFn = void*(__fastcall*)(void*, int, int, float, bool);
using RadarSoundFrameUpdateFn = void(__fastcall*)(void*);
using RadarSoundSnippetUpdateFn = void(__fastcall*)(void*, void*);
using DamageDirectionFn = void(__fastcall*)(std::uintptr_t, const float*,
                                             void*);
using DeathPanelDamageSummaryFn = std::uint64_t(__fastcall*)(
    std::uintptr_t, int, int, int, int, int, int);
using DamageIndicatorVisibleFn = void(__fastcall*)(void*, bool);
using GameEventDispatchFn = bool(__fastcall*)(void*, void*);
using CvarIteratorFirstFn = void(__fastcall*)(void*, std::uint64_t*);
using CvarIteratorNextFn = void(__fastcall*)(void*, std::uint64_t*,
                                             std::uint64_t);
using CvarByIndexFn = void*(__fastcall*)(void*, std::uint64_t);
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
using ExecuteClientCommandFn = void(__fastcall*)(void*, int, const char*, int,
                                                  double, std::int64_t);

EntryHook g_radar_mode_update{};
EntryHook g_radar_update{};
EntryHook g_radar_local_transform{};
EntryHook g_player_overhead_update{};
EntryHook g_team_counter_update{};
EntryHook g_voice_update{};
EntryHook g_server_voice_submit{};
EntryHook g_money_update{};
EntryHook g_gameplay_event{};
EntryHook g_death_postprocess_update{};
EntryHook g_damage_message{};
EntryHook g_death_panel_event{};
EntryHook g_death_panel_show{};
EntryHook g_death_panel_hide{};
EntryHook g_last_killer_damage{};
EntryHook g_radio_text{};
EntryHook g_say_text2{};
EntryHook g_hud_root_update{};
EntryHook g_spec_player_update{};
EntryHook g_live_flash_submit{};
EntryHook g_render_graph{};
EntryHook g_get_hud_player{};
EntryHook g_get_hud_alive{};
EntryHook g_spectator_tools{};
EntryHook g_voice_should_draw{};
EntryHook g_hud_team_relationship{};
EntryHook g_buy_zone_predicate{};

RelCallHook g_radar_sound_emit_call{};
RelCallHook g_radar_sound_create_call{};
RelCallHook g_radar_sound_snippet_update_call{};
RelCallHook g_damage_direction_call{};
RadarSoundSubmitFn g_radar_sound_submit_original = nullptr;
RadarSoundCreateFn g_radar_sound_create_original = nullptr;
RadarSoundFrameUpdateFn g_radar_sound_frame_update_original = nullptr;
RadarSoundSnippetUpdateFn g_radar_sound_snippet_update_original = nullptr;
DamageDirectionFn g_damage_direction_original = nullptr;
thread_local bool g_radar_sound_transaction_active = false;
thread_local void* g_radar_sound_created_hud = nullptr;
thread_local void* g_radar_sound_created_snippet = nullptr;
thread_local bool g_radar_sound_created_snippet_updated = false;
thread_local void* g_radar_sound_max_snippet = nullptr;
thread_local bool g_radar_sound_max_armed = false;
void** g_game_event_dispatch_slot = nullptr;
GameEventDispatchFn g_game_event_dispatch_original = nullptr;

void** g_is_playing_demo_slot = nullptr;
BoolOneArgFn g_is_playing_demo_original = nullptr;
void** g_broadcast_mode_slot = nullptr;
UIntOneArgFn g_broadcast_mode_original = nullptr;
void** g_radar_transaction_slot = nullptr;
RadarTransactionFn g_radar_transaction_original = nullptr;
HMODULE g_client = nullptr;
EmitHurtFeedbackFn g_emit_hurt_feedback = nullptr;
PushNoticeFn g_push_notice = nullptr;
FindHudElementFn g_find_hud_element = nullptr;
DeathPanelDamageSummaryFn g_death_panel_damage_summary = nullptr;
DamageIndicatorVisibleFn g_damage_indicator_visible = nullptr;
void** g_player_pawn_event_slot = nullptr;
PlayerPawnEventFn g_player_pawn_event_original = nullptr;
bool g_kill_reward_enabled = true;
bool g_throw_notice_enabled = true;
bool g_player_sound_enabled = true;
int* g_tv_voice_indices = nullptr;
int* g_tv_voice_indices_high = nullptr;
int g_tv_voice_original = 0;
int g_tv_voice_original_high = 0;
bool g_tv_voice_original_valid = false;
bool g_tv_voice_original_high_valid = false;
std::uint32_t g_voice_mask_low = 0;
std::uint32_t g_voice_mask_high = 0;
int g_voice_mask_team = -1;
std::uint32_t g_voice_cvar_retry_frames = 0;
std::uint64_t g_voice_roster_generation = 0;
std::uint64_t g_voice_roster_seek_epoch = 0;
std::uint32_t g_voice_roster_retry_frames = 0;
unsigned g_voice_roster_selected = 0;
std::uint32_t g_follow_bootstrap_frames = 0;
std::uint32_t g_follow_bootstrap_attempts = 0;
bool g_follow_bootstrap_complete = false;
std::atomic<std::uint64_t> g_seek_rebuild_epoch{0};
std::uint64_t g_seek_rebuild_seen_epoch = 0;
std::atomic<std::uint64_t> g_death_feedback_count{0};
std::atomic<std::uint64_t> g_kill_reward_count{0};
std::atomic<std::uint64_t> g_throw_event_count{0};
std::atomic<std::uint64_t> g_throw_candidate_count{0};
std::atomic<std::uint64_t> g_throw_notice_count{0};
std::atomic<std::uint64_t> g_throw_team_drop_count{0};
std::atomic<std::uint64_t> g_notice_sink_probe_count{0};
std::atomic<bool> g_radar_mode_seen{false};
std::atomic<bool> g_radar_local_transform_seen{false};
std::atomic<bool> g_radar_transaction_seen{false};
std::atomic<bool> g_radar_sound_identity_seen{false};
std::atomic<bool> g_player_overhead_seen{false};
std::atomic<bool> g_voice_live_seen{false};
std::atomic<bool> g_server_voice_live_seen{false};
std::atomic<bool> g_voice_draw_gate_seen{false};
std::atomic<std::uint64_t> g_team_relationship_adapted{0};
std::atomic<std::uint64_t> g_buy_zone_adapted{0};
std::uint64_t g_voice_state_generation = 0;
std::uint32_t g_voice_state_frames = 0;
std::uint32_t g_voice_audio_adapted_low = 0;
std::uint32_t g_voice_audio_adapted_high = 0;
std::atomic<std::uint64_t> g_voice_audio_adapt_count{0};
std::atomic<std::uint64_t> g_voice_packet_team_drops{0};
std::atomic<bool> g_hud_presentation_seen{false};
std::atomic<bool> g_live_flash_seen{false};
std::atomic<std::uint64_t> g_damage_message_count{0};
std::atomic<std::uint64_t> g_damage_visibility_count{0};
std::atomic<std::uint64_t> g_damage_direction_count{0};
std::atomic<std::uint64_t> g_death_event_count{0};
std::atomic<std::uint64_t> g_last_killer_damage_count{0};
std::atomic<std::uint64_t> g_death_camera_count{0};
std::atomic<std::uint64_t> g_freeze_resource_probe_count{0};
std::atomic<std::uint64_t> g_death_postprocess_active_count{0};
std::atomic<std::uint64_t> g_death_postprocess_zero_count{0};
std::atomic<std::uint64_t> g_death_timing_probe_count{0};
std::atomic<std::uint64_t> g_combat_reject_count{0};
std::atomic<std::uintptr_t> g_death_panel_visible_hud{0};
std::atomic<std::uint64_t> g_death_panel_visible_since{0};

struct PendingRadarSound {
  void* pawn = nullptr;
  event_compensation::RadarSoundSpec spec{};
  std::uint64_t generation = 0;
  std::uint64_t stamp = 0;
  char event_name[24]{};
  char weapon[48]{};
};

struct PendingDamageFeedback {
  void* victim = nullptr;
  void* attacker = nullptr;
  std::uint64_t generation = 0;
  std::uint64_t stamp = 0;
  int damage = 0;
};

struct PendingDeathBanner {
  std::uintptr_t hud = 0;
  std::uint64_t generation = 0;
  std::uint64_t stamp = 0;
};

struct PendingLastKillerDamage {
  std::array<int, 6> values{};
  std::uint64_t generation = 0;
  std::uint64_t stamp = 0;
  bool valid = false;
};

constexpr std::size_t kMaxPendingRadarSounds = 64;
constexpr std::uint64_t kRadarSoundNativePriorityMs = 40;
constexpr std::uint64_t kDeathBannerPairWindowMs = 120;
constexpr std::uint64_t kDeathPovLatchMs = 2000;
SRWLOCK g_pending_radar_sound_lock = SRWLOCK_INIT;
std::array<PendingRadarSound, kMaxPendingRadarSounds> g_pending_radar_sounds{};
std::size_t g_pending_radar_sound_count = 0;
std::array<std::atomic<std::uint64_t>, 5> g_native_radar_sound_stamp{};
std::array<std::atomic<std::uint64_t>, 5> g_native_radar_sound_generation{};
std::atomic<std::uint64_t> g_native_radar_sound_count{0};
std::atomic<std::uint64_t> g_fallback_radar_sound_count{0};
std::atomic<std::uint64_t> g_deduped_radar_sound_count{0};
std::atomic<std::uint64_t> g_dropped_radar_sound_count{0};
std::atomic<std::uint64_t> g_consumed_radar_sound_count{0};
std::atomic<std::uint64_t> g_updated_radar_sound_count{0};
std::atomic<std::uint64_t> g_repaired_radar_sound_count{0};
std::atomic<std::uint64_t> g_missed_radar_sound_update_count{0};
std::uint64_t g_radar_sound_cvar_seek_epoch =
    std::numeric_limits<std::uint64_t>::max();
std::atomic<std::uint64_t> g_combat_latch_generation{0};
std::atomic<std::uint64_t> g_combat_latch_stamp{0};
std::atomic<bool> g_last_killer_damage_armed{false};
SRWLOCK g_pending_damage_lock = SRWLOCK_INIT;
PendingDamageFeedback g_pending_damage{};
std::atomic<std::uint64_t> g_native_damage_stamp{0};
std::atomic<std::uint64_t> g_damage_fallback_count{0};
std::atomic<std::uint64_t> g_damage_deduped_count{0};
SRWLOCK g_pending_death_banner_lock = SRWLOCK_INIT;
PendingDeathBanner g_pending_death_banner{};
PendingLastKillerDamage g_pending_last_killer_damage{};
std::atomic<std::uint64_t> g_native_last_killer_damage_stamp{0};
std::atomic<std::uint64_t> g_death_banner_fallback_count{0};
std::atomic<std::uint64_t> g_death_banner_deduped_count{0};

const char* event_name(void* event);
void* event_player_field(void* event, const char* field,
                         const char* hash_text, int hash_length,
                         unsigned seed);
const char* event_string_field(void* event, const char* field,
                               const char* hash_text, int hash_length,
                               unsigned seed);
bool event_field_truthy(void* event, const char* field, int name_length,
                        unsigned seed);
void queue_damage_feedback_event(std::uintptr_t event);
void flush_pending_damage_feedback();
void flush_pending_death_banner();

bool read_damage_indicator_strengths(
    std::uintptr_t hud, std::array<float, 4>& strengths) noexcept {
  __try {
    const auto* values = reinterpret_cast<const float*>(hud + 0x60);
    for (std::size_t index = 0; index < strengths.size(); ++index) {
      strengths[index] = values[index];
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void log_client_mode_postprocess_resources(std::uintptr_t listener) noexcept {
  const auto count =
      g_freeze_resource_probe_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > 12) {
    return;
  }
  __try {
    auto* base = reinterpret_cast<std::uint8_t*>(
        listener - offsets::kClientModeEventListenerOffset);
    char detail[512]{};
    std::snprintf(
        detail, sizeof(detail),
        "freeze_resource_probe=%llu ct_resource=%p ct_control=%p "
        "t_resource=%p t_control=%p phase1=%p/%p phase1_low=%p/%p "
        "phase2=%p/%p weights=%.3f/%.3f/%.3f ready=0x%02X",
        static_cast<unsigned long long>(count),
        *reinterpret_cast<void**>(base + offsets::kClientModeFreezeCtResource),
        *reinterpret_cast<void**>(base + offsets::kClientModeFreezeCtControl),
        *reinterpret_cast<void**>(base + offsets::kClientModeFreezeTResource),
        *reinterpret_cast<void**>(base + offsets::kClientModeFreezeTControl),
        *reinterpret_cast<void**>(base +
                                  offsets::kClientModeDeathPhase1Resource),
        *reinterpret_cast<void**>(base +
                                  offsets::kClientModeDeathPhase1Control),
        *reinterpret_cast<void**>(
            base + offsets::kClientModeDeathPhase1LowResource),
        *reinterpret_cast<void**>(
            base + offsets::kClientModeDeathPhase1LowControl),
        *reinterpret_cast<void**>(base +
                                  offsets::kClientModeDeathPhase2Resource),
        *reinterpret_cast<void**>(base +
                                  offsets::kClientModeDeathPhase2Control),
        static_cast<double>(*reinterpret_cast<float*>(
            base + offsets::kClientModeDeathPhase1Weight)),
        static_cast<double>(*reinterpret_cast<float*>(
            base + offsets::kClientModeDeathPhase1LowWeight)),
        static_cast<double>(*reinterpret_cast<float*>(
            base + offsets::kClientModeDeathPhase2Weight)),
        base[offsets::kClientModeFreezeStateReady]);
    log_kv("pov_combat", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "freeze_resource_probe_seh");
  }
}

void log_death_postprocess_weights(std::uintptr_t client_mode) noexcept {
  __try {
    auto* base = reinterpret_cast<std::uint8_t*>(client_mode);
    const float phase1 = *reinterpret_cast<float*>(
        base + offsets::kClientModeDeathPhase1Weight);
    const float phase1_low = *reinterpret_cast<float*>(
        base + offsets::kClientModeDeathPhase1LowWeight);
    const float phase2 = *reinterpret_cast<float*>(
        base + offsets::kClientModeDeathPhase2Weight);
    const bool active = phase1 > 0.001f || phase1_low > 0.001f ||
                        phase2 > 0.001f;
    const bool death_panel_visible =
        g_death_panel_visible_hud.load(std::memory_order_acquire) != 0;
    if (!active && !death_panel_visible) {
      return;
    }
    auto& counter = active ? g_death_postprocess_active_count
                           : g_death_postprocess_zero_count;
    const auto count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 120) {
      char detail[192]{};
      std::snprintf(detail, sizeof(detail),
                    "death_postprocess_%s=%llu phase1=%.3f "
                    "phase1_low=%.3f phase2=%.3f",
                    active ? "active" : "zero",
                    static_cast<unsigned long long>(count),
                    static_cast<double>(phase1),
                    static_cast<double>(phase1_low),
                    static_cast<double>(phase2));
      log_kv("pov_combat", detail);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "death_postprocess_probe_seh");
  }
}

void log_death_timing_cvars() noexcept {
  const auto count =
      g_death_timing_probe_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > 4) {
    return;
  }
  HMODULE tier0 = GetModuleHandleA("tier0.dll");
  auto create_interface = tier0 ? reinterpret_cast<CreateInterfaceFn>(
                                      GetProcAddress(tier0, "CreateInterface"))
                                : nullptr;
  if (!create_interface) {
    log_kv("pov_combat", "death_timing_cvars_interface_unavailable");
    return;
  }

  __try {
    void* cvar_interface = create_interface("VEngineCvar007", nullptr);
    auto** vtable = cvar_interface
                        ? *reinterpret_cast<void***>(cvar_interface)
                        : nullptr;
    if (!vtable || !vtable[12] || !vtable[13] || !vtable[41]) {
      log_kv("pov_combat", "death_timing_cvars_vtable_unavailable");
      return;
    }
    auto first = reinterpret_cast<CvarIteratorFirstFn>(vtable[12]);
    auto next = reinterpret_cast<CvarIteratorNextFn>(vtable[13]);
    auto by_index = reinterpret_cast<CvarByIndexFn>(vtable[41]);
    constexpr const char* kNames[4] = {
        "spec_freeze_time", "spec_freeze_traveltime",
        "spec_freeze_time_lock", "spec_freeze_deathanim_time"};
    float values[4]{};
    unsigned found = 0;
    std::uint64_t iterator = 0;
    first(cvar_interface, &iterator);
    for (std::size_t visited = 0;
         iterator != 0xFFFFFFFFull && visited < 16384 && found != 0xF;
         ++visited) {
      void* cvar = by_index(cvar_interface, iterator);
      if (cvar) {
        const char* name = *reinterpret_cast<const char**>(cvar);
        for (unsigned index = 0; name && index < 4; ++index) {
          if ((found & (1u << index)) == 0 &&
              std::strcmp(name, kNames[index]) == 0) {
            values[index] = *reinterpret_cast<const float*>(
                reinterpret_cast<const std::uint8_t*>(cvar) + 0x58);
            found |= 1u << index;
          }
        }
      }
      next(cvar_interface, &iterator, iterator);
    }
    char detail[256]{};
    std::snprintf(detail, sizeof(detail),
                  "death_timing_cvars=%llu found=0x%X freeze=%.3f "
                  "travel=%.3f lock=%.3f deathanim=%.3f",
                  static_cast<unsigned long long>(count), found,
                  static_cast<double>(values[0]),
                  static_cast<double>(values[1]),
                  static_cast<double>(values[2]),
                  static_cast<double>(values[3]));
    log_kv("pov_combat", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "death_timing_cvars_seh");
  }
}

std::size_t radar_sound_kind_index(
    event_compensation::RadarSoundKind kind) noexcept {
  return static_cast<std::size_t>(kind);
}

event_compensation::RadarSoundKind native_radar_sound_kind(
    int radius, bool is_footstep) noexcept {
  using event_compensation::RadarSoundKind;
  if (is_footstep) {
    return RadarSoundKind::footstep;
  }
  if (radius >= 580 && radius <= 620) {
    return RadarSoundKind::scope;
  }
  if (radius > 0 && radius < 800) {
    return RadarSoundKind::utility;
  }
  return RadarSoundKind::weapon;
}

void clear_pending_radar_sounds() noexcept {
  AcquireSRWLockExclusive(&g_pending_radar_sound_lock);
  g_pending_radar_sound_count = 0;
  ReleaseSRWLockExclusive(&g_pending_radar_sound_lock);
}

void queue_radar_sound_event(void* event) {
  if (!g_player_sound_enabled || !event || demo_is_skipping()) {
    return;
  }
  __try {
    const char* name = event_name(event);
    if (!name || (std::strcmp(name, "player_footstep") != 0 &&
                  std::strcmp(name, "player_jump") != 0 &&
                  std::strcmp(name, "weapon_fire") != 0 &&
                  std::strcmp(name, "weapon_zoom") != 0)) {
      return;
    }
    const auto followed = pov::snapshot();
    if (!followed.pawn || followed.generation == 0) {
      return;
    }
    void* actor = event_player_field(event, "userid", "id", 2,
                                     offsets::kClientEventUseridSeed);
    if (actor != followed.pawn) {
      return;
    }
    const char* weapon = nullptr;
    if (std::strcmp(name, "weapon_fire") == 0) {
      weapon = event_string_field(event, "weapon", "on", 2,
                                  offsets::kClientEventWeaponSeed);
    }
    const bool silenced =
        std::strcmp(name, "weapon_fire") == 0 &&
        event_field_truthy(event, "silenced", 8,
                           offsets::kClientEventSilencedSeed);
    // Exact runtime VSND metadata remains authoritative and suppresses this
    // fallback whenever it is available.
    const auto spec = event_compensation::radar_sound_from_event(
        name, weapon ? weapon : "", silenced);
    if (!spec) {
      return;
    }

    PendingRadarSound pending{};
    pending.pawn = actor;
    pending.spec = spec;
    pending.generation = followed.generation;
    pending.stamp = GetTickCount64();
    std::snprintf(pending.event_name, sizeof(pending.event_name), "%s", name);
    std::snprintf(pending.weapon, sizeof(pending.weapon), "%s",
                  weapon ? weapon : "");

    AcquireSRWLockExclusive(&g_pending_radar_sound_lock);
    if (g_pending_radar_sound_count == kMaxPendingRadarSounds) {
      std::memmove(g_pending_radar_sounds.data(),
                   g_pending_radar_sounds.data() + 1,
                   sizeof(PendingRadarSound) *
                       (kMaxPendingRadarSounds - 1));
      --g_pending_radar_sound_count;
      g_dropped_radar_sound_count.fetch_add(1, std::memory_order_relaxed);
    }
    g_pending_radar_sounds[g_pending_radar_sound_count++] = pending;
    ReleaseSRWLockExclusive(&g_pending_radar_sound_lock);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_sound_event", "queue_seh");
  }
}

void flush_pending_radar_sounds() {
  if (!g_player_sound_enabled || !g_radar_sound_submit_original) {
    clear_pending_radar_sounds();
    return;
  }
  const auto followed = pov::snapshot();
  const auto now = GetTickCount64();
  PendingRadarSound selected{};
  bool selected_ready = false;
  std::array<PendingRadarSound, kMaxPendingRadarSounds> deduped{};
  std::size_t deduped_count = 0;

  AcquireSRWLockExclusive(&g_pending_radar_sound_lock);
  std::size_t kept = 0;
  for (std::size_t index = 0; index < g_pending_radar_sound_count; ++index) {
    const auto& pending = g_pending_radar_sounds[index];
    if (!followed.pawn || pending.pawn != followed.pawn ||
        pending.generation != followed.generation) {
      g_dropped_radar_sound_count.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (now - pending.stamp < kRadarSoundNativePriorityMs) {
      g_pending_radar_sounds[kept++] = pending;
      continue;
    }
    // HudRadar exposes one visible generic sound-ring producer slot per
    // transaction. Submitting footstep+jump together creates both snippets,
    // but the later row replaces the earlier one before its native update.
    // Consume at most one fallback here and retain the rest for later frames.
    if (selected_ready) {
      g_pending_radar_sounds[kept++] = pending;
      continue;
    }

    const auto kind_index = radar_sound_kind_index(pending.spec.kind);
    const auto native_stamp =
        g_native_radar_sound_stamp[kind_index].load(std::memory_order_acquire);
    const auto native_generation = g_native_radar_sound_generation[kind_index]
                                       .load(std::memory_order_relaxed);
    if (native_generation == pending.generation &&
        event_compensation::native_sound_covers_event(
            pending.stamp, now, native_stamp)) {
      deduped[deduped_count++] = pending;
      continue;
    }
    selected = pending;
    selected_ready = true;
  }
  g_pending_radar_sound_count = kept;
  ReleaseSRWLockExclusive(&g_pending_radar_sound_lock);

  for (std::size_t index = 0; index < deduped_count; ++index) {
    const auto& pending = deduped[index];
    const auto kind_index = radar_sound_kind_index(pending.spec.kind);
    const auto native_stamp =
        g_native_radar_sound_stamp[kind_index].load(std::memory_order_acquire);
    const auto count = g_deduped_radar_sound_count.fetch_add(
                           1, std::memory_order_relaxed) +
                       1;
    if (count <= 32) {
      char detail[160]{};
      std::snprintf(detail, sizeof(detail),
                    "deduped=%llu event=%s weapon=%s native_age=%llu",
                    static_cast<unsigned long long>(count),
                    pending.event_name, pending.weapon,
                    static_cast<unsigned long long>(
                        now >= native_stamp ? now - native_stamp : 0));
      log_kv("pov_sound_event", detail);
    }
  }

  if (!selected_ready) {
    return;
  }

  pov::Scope sound_scope(pov::Domain::player_sound);
  // Runtime presentation audit shows the dedicated Step slot is updated but
  // remains visually dormant in Demo POV. Route the event-derived approximation
  // through the visible generic native slot; HudRadar still owns the snippet,
  // geometry, animation and expiry.
  const bool native_step = false;
  g_radar_sound_submit_original(selected.pawn, selected.spec.radius,
                                selected.spec.duration, native_step);
  const auto count = g_fallback_radar_sound_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 32) {
    char detail[192]{};
    std::snprintf(detail, sizeof(detail),
                  "submitted=%llu event=%s weapon=%s radius=%d duration=%.2f "
                  "event_step=%d native_step=%d",
                  static_cast<unsigned long long>(count), selected.event_name,
                  selected.weapon, selected.spec.radius,
                  static_cast<double>(selected.spec.duration),
                  selected.spec.is_footstep ? 1 : 0,
                  native_step ? 1 : 0);
    log_kv("pov_sound_event", detail);
  }
}

void __fastcall radar_sound_submit_scope(void* source_pawn, int radius,
                                         float duration, bool is_footstep) {
  auto original = g_radar_sound_submit_original;
  if (!original) {
    return;
  }
  if (!g_player_sound_enabled || demo_is_skipping()) {
    original(source_pawn, radius, duration, is_footstep);
    return;
  }
  const auto followed = pov::snapshot();
  if (!source_pawn || source_pawn != followed.pawn) {
    original(source_pawn, radius, duration, is_footstep);
    return;
  }

  // Current runtime logs show continuous followed-player movement as a stable
  // 548/0.10 + 204/0.10 pair, while player_jump is the standalone 204 pulse.
  // Restore only the 548 movement/landing carrier to the same presentation
  // profile as the event fallback. Keep it in the visible generic native slot
  // and leave 204 untouched so the working jump circle is not regressed.
  const bool repaired_movement =
      event_compensation::native_movement_sound_needs_presentation_repair(
          radius, duration, is_footstep);
  const int effective_radius = repaired_movement ? 1100 : radius;
  const float effective_duration = repaired_movement ? 0.50f : duration;
  const bool effective_footstep = is_footstep;
  const auto kind = repaired_movement
                        ? event_compensation::RadarSoundKind::footstep
                        : native_radar_sound_kind(radius, effective_footstep);
  const auto kind_index = radar_sound_kind_index(kind);
  const auto stamp = GetTickCount64();
  g_native_radar_sound_generation[kind_index].store(
      followed.generation, std::memory_order_relaxed);
  g_native_radar_sound_stamp[kind_index].store(stamp,
                                               std::memory_order_release);
  const auto count = g_native_radar_sound_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 48) {
    char detail[176]{};
    std::snprintf(detail, sizeof(detail),
                   "accepted=%llu radius=%d/%d duration=%.2f/%.2f "
                   "step=%d/%d movement_repaired=%d gen=%llu",
                   static_cast<unsigned long long>(count), radius,
                   effective_radius, static_cast<double>(duration),
                   static_cast<double>(effective_duration),
                   is_footstep ? 1 : 0, effective_footstep ? 1 : 0,
                   repaired_movement ? 1 : 0,
                   static_cast<unsigned long long>(followed.generation));
    log_kv("pov_sound_native", detail);
  }
  pov::Scope sound_scope(pov::Domain::player_sound);
  original(source_pawn, effective_radius, effective_duration,
           effective_footstep);
}

void* __fastcall radar_sound_create_scope(void* hud, int player_id,
                                          int radius, float duration,
                                          bool is_footstep) {
  auto original = g_radar_sound_create_original;
  void* snippet = original
                      ? original(hud, player_id, radius, duration,
                                 is_footstep)
                      : nullptr;
  if (g_radar_sound_transaction_active && snippet) {
    g_radar_sound_created_hud = hud;
    g_radar_sound_created_snippet = snippet;
    g_radar_sound_created_snippet_updated = false;
    if (event_compensation::native_generic_footstep_needs_max(
            radius, duration, is_footstep)) {
      g_radar_sound_max_snippet = snippet;
      g_radar_sound_max_armed = false;
    }
  }
  const auto followed = pov::snapshot();
  if (g_player_sound_enabled && followed.pawn && !demo_is_skipping()) {
    const auto count = g_consumed_radar_sound_count.fetch_add(
                           1, std::memory_order_relaxed) +
                       1;
    if (count <= 64) {
      __try {
        auto* bytes = reinterpret_cast<std::uint8_t*>(snippet);
        void* panel = snippet ? *reinterpret_cast<void**>(bytes) : nullptr;
        void* child = snippet ? *reinterpret_cast<void**>(bytes + 8) : nullptr;
        const unsigned state =
            snippet ? *reinterpret_cast<unsigned*>(bytes + 0x178) : 0;
        const unsigned flags = snippet ? bytes[0x17C] : 0;
        char detail[256]{};
        std::snprintf(detail, sizeof(detail),
                      "consumed=%llu player=%d radius=%d duration=%.2f "
                      "step=%d snippet=%p panel=%p child=%p state=%u "
                      "flags=0x%02X gen=%llu",
                      static_cast<unsigned long long>(count), player_id,
                      radius, static_cast<double>(duration),
                      is_footstep ? 1 : 0, snippet, panel, child, state, flags,
                      static_cast<unsigned long long>(followed.generation));
        log_kv("pov_sound_render", detail);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        log_kv("pov_sound_render", "create_probe_seh");
      }
    }
  }
  return snippet;
}

void __fastcall radar_sound_snippet_update_scope(void* hud, void* snippet) {
  auto original = g_radar_sound_snippet_update_original;
  if (original) {
    original(hud, snippet);
  }
  const bool arm_native_max =
      g_radar_sound_transaction_active && snippet &&
      snippet == g_radar_sound_max_snippet && !g_radar_sound_max_armed;
  if (arm_native_max) {
    __try {
      // E3A550 normally owns this max condition. Setting it after that native
      // update lets the immediately-following E4A610 branch invoke the game's
      // own player-sound-max trigger on both radar-mode panels exactly once.
      reinterpret_cast<std::uint8_t*>(snippet)[0x17C] |= 0x02;
      g_radar_sound_max_armed = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      log_kv("pov_sound_update", "native_max_arm_seh");
    }
  }
  const bool is_new_snippet =
      g_radar_sound_transaction_active && snippet &&
      snippet == g_radar_sound_created_snippet;
  if (is_new_snippet) {
    g_radar_sound_created_snippet_updated = true;
  }
  const auto count =
      g_updated_radar_sound_count.fetch_add(1, std::memory_order_relaxed) + 1;
  // Always log the first update of a snippet created in the current radar
  // transaction.  A global cap alone can be exhausted by older rows before a
  // delayed fallback footstep reaches the renderer.
  if ((count > 64 && !is_new_snippet && !arm_native_max) || !snippet) {
    return;
  }
  __try {
    auto* bytes = reinterpret_cast<std::uint8_t*>(snippet);
    auto* hud_bytes = reinterpret_cast<std::uint8_t*>(hud);
    const auto* origin = reinterpret_cast<const float*>(bytes + 0x110);
    const auto* projected = reinterpret_cast<const float*>(bytes + 0x128);
    const bool rotated = hud_bytes && hud_bytes[0x60] != 0;
    float map_scale = 0.0f;
    if (hud_bytes) {
      if (rotated) {
        map_scale = *reinterpret_cast<const float*>(hud_bytes + 0x17F8C);
      } else {
        const float denominator =
            *reinterpret_cast<const float*>(hud_bytes + 0x19C);
        if (denominator != 0.0f) {
          map_scale = *reinterpret_cast<const float*>(hud_bytes + 0x1A0) /
                      denominator;
        }
      }
    }
    const float hud_sound_scale =
        hud_bytes ? *reinterpret_cast<const float*>(hud_bytes + 0x1B4) : 0.0f;
    const float effective_scale = map_scale * hud_sound_scale;
    const int radius = *reinterpret_cast<const int*>(bytes + 0x164);
    const int pixel_radius = effective_scale > 0.0f && effective_scale < 1000.0f
                                 ? static_cast<int>(radius * effective_scale)
                                 : 0;
    char detail[512]{};
    std::snprintf(
        detail, sizeof(detail),
        "updated=%llu snippet=%p panel=%p child=%p player=%d state=%u "
        "flags=0x%02X/0x%02X origin=%.1f,%.1f,%.1f "
        "projected=%.3f,%.3f style=%.3f,%.3f opacity=%.3f mask=0x%08X "
        "rotated=%d map_scale=%.6f hud_sound_scale=%.6f "
        "radius=%d pixel_radius=%d diameter=%d new=%d max_armed=%d",
        static_cast<unsigned long long>(count), snippet,
        *reinterpret_cast<void**>(bytes),
        *reinterpret_cast<void**>(bytes + 8),
        *reinterpret_cast<int*>(bytes + 0x15C),
        *reinterpret_cast<unsigned*>(bytes + 0x178), bytes[0x17C],
        bytes[0x17D], static_cast<double>(origin[0]),
        static_cast<double>(origin[1]), static_cast<double>(origin[2]),
        static_cast<double>(projected[0]), static_cast<double>(projected[1]),
        static_cast<double>(*reinterpret_cast<float*>(bytes + 0x134)),
        static_cast<double>(*reinterpret_cast<float*>(bytes + 0x138)),
        static_cast<double>(*reinterpret_cast<float*>(bytes + 0x148)),
        *reinterpret_cast<unsigned*>(bytes + 0x150),
        rotated ? 1 : 0, static_cast<double>(map_scale),
        static_cast<double>(hud_sound_scale), radius, pixel_radius,
        pixel_radius * 2, is_new_snippet ? 1 : 0,
        arm_native_max && g_radar_sound_max_armed ? 1 : 0);
    log_kv("pov_sound_update", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_sound_update", "probe_seh");
  }
}

bool __fastcall game_event_dispatch_scope(void* manager, void* event) {
  // Copy the event fields while the event is alive, but do not expose any POV
  // identity to the native dispatcher. The original owns dispatch/freeing.
  queue_radar_sound_event(event);
  queue_damage_feedback_event(reinterpret_cast<std::uintptr_t>(event));
  return g_game_event_dispatch_original
             ? g_game_event_dispatch_original(manager, event)
             : false;
}

void* event_player_field(void* event, const char* field,
                         const char* hash_text, int hash_length,
                         unsigned seed) {
  if (!g_client || !event || !field || !hash_text) {
    return nullptr;
  }
  auto** vtable = *reinterpret_cast<void***>(event);
  if (!vtable) {
    return nullptr;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  using GetEntityFn = void*(__fastcall*)(void*, void*);
  using FilterPlayerFn = void*(__fastcall*)(void*);
  auto* base = reinterpret_cast<std::uint8_t*>(g_client);
  auto hash = reinterpret_cast<HashFn>(
      base + offsets::kClientEventFieldHashRva);
  auto get_entity = reinterpret_cast<GetEntityFn>(vtable[0x88 / 8]);
  auto filter_player = reinterpret_cast<FilterPlayerFn>(
      base + offsets::kClientFilterPlayerEntRva);
  if (!hash || !get_entity || !filter_player) {
    return nullptr;
  }
  alignas(16) std::uint8_t key[24]{};
  *reinterpret_cast<int*>(key) = hash(hash_text, hash_length, seed);
  *reinterpret_cast<int*>(key + 4) = -1;
  *reinterpret_cast<const char**>(key + 8) = field;
  void* entity = get_entity(event, key);
  return entity ? filter_player(entity) : nullptr;
}

bool event_field_truthy(void* event, const char* field, int name_length,
                        unsigned seed) {
  if (!g_client || !event || !field) {
    return false;
  }
  auto** vtable = *reinterpret_cast<void***>(event);
  if (!vtable) {
    return false;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  using GetIntFn = std::int64_t(__fastcall*)(void*, void*);
  auto* base = reinterpret_cast<std::uint8_t*>(g_client);
  auto hash = reinterpret_cast<HashFn>(
      base + offsets::kClientEventFieldHashRva);
  auto get_int = reinterpret_cast<GetIntFn>(vtable[0x38 / 8]);
  if (!hash || !get_int) {
    return false;
  }
  alignas(16) std::uint8_t key[24]{};
  *reinterpret_cast<int*>(key) = hash(field, name_length, seed);
  *reinterpret_cast<int*>(key + 4) = -1;
  *reinterpret_cast<const char**>(key + 8) = field;
  return get_int(event, key) != 0;
}

int event_int_field(void* event, const char* field, int name_length,
                    unsigned seed) {
  if (!g_client || !event || !field) {
    return 0;
  }
  auto** vtable = *reinterpret_cast<void***>(event);
  if (!vtable) {
    return 0;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  using GetIntFn = std::int64_t(__fastcall*)(void*, void*);
  auto* base = reinterpret_cast<std::uint8_t*>(g_client);
  auto hash = reinterpret_cast<HashFn>(
      base + offsets::kClientEventFieldHashRva);
  auto get_int = reinterpret_cast<GetIntFn>(vtable[0x38 / 8]);
  if (!hash || !get_int) {
    return 0;
  }
  alignas(16) std::uint8_t key[24]{};
  *reinterpret_cast<int*>(key) = hash(field, name_length, seed);
  *reinterpret_cast<int*>(key + 4) = -1;
  *reinterpret_cast<const char**>(key + 8) = field;
  return static_cast<int>(get_int(event, key));
}

bool env_flag_default_on(const char* name) {
  char value[8]{};
  const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  if (length == 0) {
    return true;
  }
  return !(value[0] == '0' && (length == 1 || value[1] == '\0'));
}

const char* event_name(void* event) {
  if (!event) {
    return nullptr;
  }
  auto** vtable = *reinterpret_cast<void***>(event);
  if (!vtable || !vtable[1]) {
    return nullptr;
  }
  using EventNameFn = const char*(__fastcall*)(void*);
  return reinterpret_cast<EventNameFn>(vtable[1])(event);
}

const char* event_string_field(void* event, const char* field,
                               const char* hash_text, int hash_length,
                               unsigned seed) {
  if (!g_client || !event || !field || !hash_text) {
    return nullptr;
  }
  auto** vtable = *reinterpret_cast<void***>(event);
  if (!vtable) {
    return nullptr;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  using GetStringFn = const char*(__fastcall*)(void*, void*, const char*);
  auto* base = reinterpret_cast<std::uint8_t*>(g_client);
  auto hash = reinterpret_cast<HashFn>(
      base + offsets::kClientEventFieldHashRva);
  auto get_string = reinterpret_cast<GetStringFn>(vtable[0x50 / 8]);
  if (!hash || !get_string) {
    return nullptr;
  }
  alignas(16) std::uint8_t key[24]{};
  *reinterpret_cast<int*>(key) = hash(hash_text, hash_length, seed);
  *reinterpret_cast<int*>(key + 4) = -1;
  *reinterpret_cast<const char**>(key + 8) = field;
  const char* value = get_string(event, key, "");
  return value && value[0] ? value : nullptr;
}

int entity_team(void* entity) {
  if (!entity) {
    return 0;
  }
  return *reinterpret_cast<std::uint8_t*>(
      reinterpret_cast<std::uint8_t*>(entity) + offsets::kEntityTeamNum);
}

void* entity_from_handle(std::uint32_t handle) {
  if (!g_client || handle == 0 || handle == 0xFFFFFFFFu ||
      handle == 0xFFFFFFFEu) {
    return nullptr;
  }
  __try {
    auto* base = reinterpret_cast<std::uint8_t*>(g_client);
    // Current client sub_926D60 dereferences this global to obtain the chunk
    // table directly. It does not use the entity-system +0x10 layout used by
    // older MulNX offsets.
    const auto table = *reinterpret_cast<std::uintptr_t*>(
        base + offsets::kClientEntityChunkTable);
    if (!table) {
      return nullptr;
    }
    const unsigned index = handle & 0x7FFFu;
    const auto chunk = *reinterpret_cast<std::uintptr_t*>(
        table + 0x8ull * (index >> 9));
    if (!chunk) {
      return nullptr;
    }
    auto* entry = reinterpret_cast<std::uint8_t*>(
        chunk + 0x70ull * (index & 0x1FFu));
    void* entity = *reinterpret_cast<void**>(entry);
    if (!entity) {
      return nullptr;
    }
    const auto serial = *reinterpret_cast<std::uint32_t*>(entry + 0x10);
    return serial == handle ? entity : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void* entity_from_index(unsigned index) {
  if (!g_client || index > 0x7FFFu) {
    return nullptr;
  }
  __try {
    auto* base = reinterpret_cast<std::uint8_t*>(g_client);
    const auto table = *reinterpret_cast<std::uintptr_t*>(
        base + offsets::kClientEntityChunkTable);
    if (!table) {
      return nullptr;
    }
    const auto chunk = *reinterpret_cast<std::uintptr_t*>(
        table + 0x8ull * (index >> 9));
    if (!chunk) {
      return nullptr;
    }
    auto* entry = reinterpret_cast<std::uint8_t*>(
        chunk + 0x70ull * (index & 0x1FFu));
    return *reinterpret_cast<void**>(entry);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void* controller_from_pawn(void* pawn) {
  if (!pawn) {
    return nullptr;
  }
  auto* bytes = reinterpret_cast<std::uint8_t*>(pawn);
  const std::uint32_t handles[] = {
      *reinterpret_cast<std::uint32_t*>(
          bytes + offsets::kPawnOriginalController),
      *reinterpret_cast<std::uint32_t*>(bytes + offsets::kPawnController),
  };
  for (const auto handle : handles) {
    if (void* controller = entity_from_handle(handle)) {
      return controller;
    }
  }
  return nullptr;
}

const char* player_name_from_pawn(void* pawn) {
  void* controller = controller_from_pawn(pawn);
  if (!controller) {
    return nullptr;
  }
  const char* name = reinterpret_cast<const char*>(
      reinterpret_cast<std::uint8_t*>(controller) +
      offsets::kControllerPlayerName);
  for (std::size_t index = 0; index < 128; ++index) {
    if (name[index] == '\0') {
      return index == 0 ? nullptr : name;
    }
  }
  return nullptr;
}

const char* localized_token(const char* token) {
  if (!g_client || !token || !token[0]) {
    return nullptr;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(g_client);
  void* localize = *reinterpret_cast<void**>(
      base + offsets::kClientLocalizationInterfaceRva);
  if (!localize) {
    return nullptr;
  }
  auto** vtable = *reinterpret_cast<void***>(localize);
  if (!vtable || !vtable[120 / sizeof(void*)]) {
    return nullptr;
  }
  using LocalizeFn = const char*(__fastcall*)(void*, const char*);
  const char* localized = reinterpret_cast<LocalizeFn>(
      vtable[120 / sizeof(void*)])(localize, token);
  return localized && localized[0] ? localized : nullptr;
}

const char* localized_place_name(void* pawn, char* key,
                                 std::size_t key_size) {
  if (!pawn || !key || key_size < 3) {
    return nullptr;
  }
  const char* place = reinterpret_cast<const char*>(
      reinterpret_cast<std::uint8_t*>(pawn) + offsets::kPawnLastPlaceName);
  std::size_t length = 0;
  while (length < 18 && place[length]) {
    ++length;
  }
  if (length == 0 || length == 18 || length + 2 > key_size) {
    return nullptr;
  }
  key[0] = '#';
  std::memcpy(key + 1, place, length);
  key[length + 1] = '\0';
  const char* localized = localized_token(key);
  return localized && localized[0] ? localized : place;
}

void log_notice_sink_probe(void* voice_hud, unsigned slot,
                           std::uint64_t probe) {
  if (probe <= 6) {
    int free_stack = -1;
    int panel_count = -1;
    __try {
      free_stack = *reinterpret_cast<int*>(
          reinterpret_cast<std::uint8_t*>(voice_hud) + 328);
      panel_count = *reinterpret_cast<int*>(
          reinterpret_cast<std::uint8_t*>(voice_hud) + 304);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    char detail[96]{};
    std::snprintf(detail, sizeof(detail),
                  "sink_probe=%llu free=%d panels=%d slot=%u",
                  static_cast<unsigned long long>(probe), free_stack,
                  panel_count, slot);
    log_kv("pov_compensation", detail);
  }
}

bool push_native_notice(const char* message,
                        unsigned slot = 0xFFFFFFFFu) {
  if (!message || !message[0] || !g_find_hud_element || !g_push_notice) {
    log_kv("pov_compensation", "notice_sink_not_ready");
    return false;
  }
  // The translated event becomes a native message only for this bounded call.
  // This also lets any downstream demo predicate observe the same live message
  // context as RadioText/SayText2, without changing global message behavior.
  pov::Scope message_scope(pov::Domain::communications);
  void* voice_element = g_find_hud_element("CCSGO_HudVoiceStatus");
  if (!voice_element) {
    log_kv("pov_compensation", "voice_status_hud_missing");
    return false;
  }
  void* voice_hud = reinterpret_cast<std::uint8_t*>(voice_element) - 0x20;
  const auto probe = g_notice_sink_probe_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  log_notice_sink_probe(voice_hud, slot, probe);
  char copy[384]{};
  std::snprintf(copy, sizeof(copy), "%s", message);
  std::uint8_t flags[4] = {0, 1, 0, 0};
  g_push_notice(voice_hud, copy, slot, flags);
  return true;
}

bool substitute_first_parameter(const char* format, const char* value,
                                char* output, std::size_t output_size) {
  if (!format || !value || !output || output_size == 0) {
    return false;
  }
  const char* marker = std::strstr(format, "%s1");
  if (!marker) {
    return false;
  }
  const std::size_t prefix = static_cast<std::size_t>(marker - format);
  const std::size_t value_length = std::strlen(value);
  const char* suffix = marker + 3;
  const std::size_t suffix_length = std::strlen(suffix);
  if (prefix + value_length + suffix_length + 1 > output_size) {
    return false;
  }
  std::memcpy(output, format, prefix);
  std::memcpy(output + prefix, value, value_length);
  std::memcpy(output + prefix + value_length, suffix, suffix_length + 1);
  return true;
}

void adapt_kill_cash_notice(void* event) {
  if (!g_kill_reward_enabled || !event || demo_is_skipping()) {
    return;
  }
  __try {
    const char* name = event_name(event);
    if (!name || std::strcmp(name, "player_death") != 0) {
      return;
    }
    const auto followed = pov::snapshot();
    if (!followed.pawn || (followed.team != 2 && followed.team != 3)) {
      return;
    }
    void* attacker = event_player_field(
        event, "attacker", "cker", 4,
        offsets::kClientEventAttackerSeed);
    void* victim = event_player_field(
        event, "userid", "userid", 6,
        offsets::kClientDeathUseridSeed);
    if (attacker != followed.pawn || !victim || victim == followed.pawn ||
        entity_team(victim) == followed.team) {
      return;
    }
    const char* weapon = event_string_field(
        event, "weapon", "on", 2, offsets::kClientEventWeaponSeed);
    const int reward = event_compensation::kill_cash_reward(
        weapon ? weapon : "");
    char amount[16]{};
    std::snprintf(amount, sizeof(amount), "%d", reward);
    char message[256]{};
    const char* localized = localized_token(
        "#Player_Cash_Award_Killed_Enemy_Generic");
    if (!substitute_first_parameter(localized, amount, message,
                                    sizeof(message))) {
      std::snprintf(
          message, sizeof(message),
          "\x01"
          "\xE8\xA7\xA3\xE5\x86\xB3\xE4\xB8\x80\xE5\x90\x8D"
          "\xE6\x95\x8C\xE4\xBA\xBA\xE8\x8E\xB7\xE5\xBE\x97 "
          "\x06+$%d\x01",
          reward);
    }
    if (push_native_notice(message)) {
      const auto count = g_kill_reward_count.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
      if (count <= 12) {
        char detail[128]{};
        std::snprintf(detail, sizeof(detail),
                      "shown=%llu reward=%d weapon=%s",
                      static_cast<unsigned long long>(count), reward,
                      weapon ? weapon : "");
        log_kv("pov_kill_reward", detail);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_kill_reward", "adapter_seh");
  }
}

bool event_player_slot_matches_pawn(void* event, void* pawn,
                                    unsigned* slot_out) {
  if (!g_client || !event || !pawn) {
    return false;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(g_client);
  using PawnSlotFn = int*(__fastcall*)(void*, int*);
  auto pawn_slot = reinterpret_cast<PawnSlotFn>(
      base + offsets::kClientPawnGetPlayerSlotRva);
  int own_slot = -1;
  pawn_slot(pawn, &own_slot);
  if (own_slot < 0 || own_slot >= 64) {
    return false;
  }
  auto** vtable = *reinterpret_cast<void***>(event);
  if (!vtable || !vtable[0x78 / sizeof(void*)]) {
    return false;
  }
  using HashFn = int(__fastcall*)(const char*, int, unsigned);
  using GetSlotFn = void(__fastcall*)(void*, int*, void*);
  auto hash = reinterpret_cast<HashFn>(
      base + offsets::kClientEventFieldHashRva);
  auto get_slot = reinterpret_cast<GetSlotFn>(
      vtable[0x78 / sizeof(void*)]);
  static constexpr char kUserid[] = "userid";
  alignas(16) std::uint8_t key[24]{};
  *reinterpret_cast<int*>(key) = hash(
      kUserid + 4, 2, offsets::kClientEventUseridSeed);
  *reinterpret_cast<int*>(key + 4) = -1;
  *reinterpret_cast<const char**>(key + 8) = kUserid;
  int event_slot = -1;
  get_slot(event, &event_slot, key);
  if (event_slot != own_slot) {
    return false;
  }
  if (slot_out) {
    *slot_out = static_cast<unsigned>(event_slot);
  }
  return true;
}

bool is_zh_cn_radio_locale() {
  static constexpr char kChineseFlash[] =
      "\xE9\x97\xAA\xE5\x85\x89\xE9\x9C\x87\xE6\x92\xBC\xE5\xBC\xB9";
  const char* flash = localized_token("#SFUI_TitlesTXT_Flashbang");
  return flash && std::strcmp(flash, kChineseFlash) == 0;
}

const char* grenade_notice_text(event_compensation::GrenadeKind kind,
                                char* fallback,
                                std::size_t fallback_size) {
  if (is_zh_cn_radio_locale()) {
    switch (kind) {
      case event_compensation::GrenadeKind::flashbang:
        return "\x0B"
               "\xE5\xB0\x8F\xE5\xBF\x83\xE9\x97\xAA\xE5\x85\x89"
               "\xE5\xBC\xB9\xEF\xBC\x81";
      case event_compensation::GrenadeKind::smoke:
        return "\x05"
               "\xE7\x83\x9F\xE9\x9B\xBE\xE5\xBC\xB9\xEF\xBC\x81";
      case event_compensation::GrenadeKind::high_explosive:
        return "\x0F"
               "\xE9\xAB\x98\xE7\x88\x86\xE6\x89\x8B\xE9\x9B\xB7"
               "\xEF\xBC\x81";
      case event_compensation::GrenadeKind::incendiary:
        return "\x10"
               "\xE7\x87\x83\xE7\x83\xA7\xE5\xBC\xB9\xEF\xBC\x81";
      case event_compensation::GrenadeKind::decoy:
        return "\x08"
               "\xE8\xAF\xB1\xE9\xA5\xB5\xE5\xBC\xB9\xEF\xBC\x81";
      case event_compensation::GrenadeKind::none:
        return nullptr;
    }
  }
  const char* token = event_compensation::grenade_localization_token(kind);
  const char* localized = localized_token(token);
  if (!localized) {
    return nullptr;
  }
  if (kind == event_compensation::GrenadeKind::flashbang && fallback &&
      fallback_size > 4) {
    std::snprintf(fallback, fallback_size, "\x0B%s!", localized);
    return fallback;
  }
  return localized;
}

void adapt_grenade_throw_notice(void* listener, void* event) {
  g_throw_event_count.fetch_add(1, std::memory_order_relaxed);
  if (!g_throw_notice_enabled || !listener || !event || demo_is_skipping()) {
    return;
  }
  __try {
    const char* name = event_name(event);
    if (!name || std::strcmp(name, "weapon_fire") != 0) {
      return;
    }
    auto* pawn = reinterpret_cast<std::uint8_t*>(listener) -
                 offsets::kPlayerPawnEventListenerOffset;
    unsigned event_slot = 0xFFFFFFFFu;
    if (!event_player_slot_matches_pawn(event, pawn, &event_slot)) {
      return;
    }
    const char* weapon = event_string_field(
        event, "weapon", "on", 2, offsets::kClientEventWeaponSeed);
    const auto kind = event_compensation::grenade_kind(
        weapon ? weapon : "");
    if (kind == event_compensation::GrenadeKind::none) {
      return;
    }
    const auto followed = pov::snapshot();
    const int thrower_team = entity_team(pawn);
    const auto candidate = g_throw_candidate_count.fetch_add(
                               1, std::memory_order_relaxed) +
                           1;
    if (candidate <= 12) {
      char detail[160]{};
      std::snprintf(detail, sizeof(detail),
                    "candidate=%llu slot=%u team=%d follow_team=%d weapon=%s",
                    static_cast<unsigned long long>(candidate), event_slot,
                    thrower_team, followed.team, weapon ? weapon : "");
      log_kv("pov_throw_event", detail);
    }
    if (!followed.pawn || (followed.team != 2 && followed.team != 3) ||
        thrower_team != followed.team) {
      g_throw_team_drop_count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const char* player_name = player_name_from_pawn(pawn);
    if (!player_name) {
      player_name =
          "\xE9\x98\x9F\xE5\x8F\x8B";  // teammate
    }
    const char* team_prefix = localized_token(
        thrower_team == 3 ? "#game_radio_team_prefix_3"
                          : "#game_radio_team_prefix_2");
    if (!team_prefix) {
      team_prefix = thrower_team == 3 ? "[CT] " : "[T] ";
    }
    char phrase_buffer[192]{};
    const char* phrase = grenade_notice_text(kind, phrase_buffer,
                                             sizeof(phrase_buffer));
    if (!phrase) {
      return;
    }
    char place_key[32]{};
    const char* place = localized_place_name(pawn, place_key,
                                             sizeof(place_key));
    char message[384]{};
    if (place && place[0]) {
      std::snprintf(message, sizeof(message),
                    " %s\x03%s\x04\xEF\xB9\xAB%s\x01: %s",
                    team_prefix, player_name, place, phrase);
    } else {
      std::snprintf(message, sizeof(message), " %s\x03%s\x01: %s",
                    team_prefix, player_name, phrase);
    }
    if (push_native_notice(message, event_slot)) {
      const auto count = g_throw_notice_count.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
      if (count <= 16) {
        char detail[192]{};
        std::snprintf(detail, sizeof(detail),
                      "shown=%llu slot=%u team=%d weapon=%s",
                      static_cast<unsigned long long>(count), event_slot,
                      thrower_team, weapon ? weapon : "");
        log_kv("pov_throw_notice", detail);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_throw_notice", "adapter_seh");
  }
}

void __fastcall player_pawn_event_scope(void* listener, void* event) {
  if (g_player_pawn_event_original) {
    g_player_pawn_event_original(listener, event);
  }
  adapt_grenade_throw_notice(listener, event);
}

void adapt_player_death_feedback(void* event) {
  if (!event || !g_emit_hurt_feedback || demo_is_skipping()) {
    return;
  }
  __try {
    auto** vtable = *reinterpret_cast<void***>(event);
    if (!vtable || !vtable[1]) {
      return;
    }
    using EventNameFn = const char*(__fastcall*)(void*);
    const char* name = reinterpret_cast<EventNameFn>(vtable[1])(event);
    if (!name || std::strcmp(name, "player_death") != 0) {
      return;
    }

    const auto followed = pov::snapshot();
    if (!followed.pawn) {
      return;
    }
    void* attacker = event_player_field(
        event, "attacker", "cker", 4,
        offsets::kClientEventAttackerSeed);
    if (attacker != followed.pawn) {
      return;
    }
    void* victim = event_player_field(
        event, "userid", "userid", 6,
        offsets::kClientDeathUseridSeed);
    if (!victim || victim == followed.pawn) {
      return;
    }

    const bool headshot = event_field_truthy(
        event, "headshot", 8, offsets::kClientEventHeadshotSeed);
    auto* victim_bytes = reinterpret_cast<std::uint8_t*>(victim);
    const bool helmet = victim_bytes[offsets::kPawnPreviousHelmet] != 0;
    const int armor = *reinterpret_cast<int*>(
        victim_bytes + offsets::kPawnArmorValue);
    const char* sound = nullptr;
    if (headshot) {
      sound = helmet
                  ? "Player.DeathHeadShotArmor.AttackerFeedback"
                  : "Player.DeathHeadShot.AttackerFeedback";
    } else {
      sound = armor > 0
                  ? "Player.DeathBodyArmor.AttackerFeedback"
                  : "Player.DeathBody.AttackerFeedback";
    }

    // This is the live native input boundary used by CS2 itself: the victim
    // remains the spatial source and the engine owns recipient filtering,
    // attenuation, Steam Audio/reverb and final mixing.
    g_emit_hurt_feedback(victim, nullptr, sound);
    const auto count = g_death_feedback_count.fetch_add(
                           1, std::memory_order_relaxed) +
                       1;
    if (count <= 8) {
      log_kv("pov_feedback",
             headshot ? "native_death_headshot" : "native_death_body");
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_feedback", "native_death_adapter_seh");
  }
}

bool resolve_voice_receive_cvars() {
  if (g_tv_voice_indices) {
    return true;
  }

  HMODULE tier0 = GetModuleHandleA("tier0.dll");
  auto create_interface = tier0 ? reinterpret_cast<CreateInterfaceFn>(
                                      GetProcAddress(tier0, "CreateInterface"))
                                : nullptr;
  if (!create_interface) {
    return false;
  }

  void* cvar_interface = nullptr;
  __try {
    cvar_interface = create_interface("VEngineCvar007", nullptr);
    auto** vtable = cvar_interface
                        ? *reinterpret_cast<void***>(cvar_interface)
                        : nullptr;
    if (!vtable || !vtable[12] || !vtable[13] || !vtable[41]) {
      return false;
    }
    auto first = reinterpret_cast<CvarIteratorFirstFn>(vtable[12]);
    auto next = reinterpret_cast<CvarIteratorNextFn>(vtable[13]);
    auto by_index = reinterpret_cast<CvarByIndexFn>(vtable[41]);

    std::uint64_t iterator = 0;
    first(cvar_interface, &iterator);
    for (std::size_t visited = 0;
         iterator != 0xFFFFFFFFull && visited < 16384;
         ++visited) {
      void* cvar = by_index(cvar_interface, iterator);
      if (cvar) {
        const char* name = *reinterpret_cast<const char**>(cvar);
        if (name && std::strcmp(name, "tv_listen_voice_indices") == 0) {
          g_tv_voice_indices = reinterpret_cast<int*>(
              reinterpret_cast<std::uint8_t*>(cvar) + 0x58);
        } else if (name &&
                   std::strcmp(name, "tv_listen_voice_indices_h") == 0) {
          g_tv_voice_indices_high = reinterpret_cast<int*>(
              reinterpret_cast<std::uint8_t*>(cvar) + 0x58);
        }
      }
      if (g_tv_voice_indices && g_tv_voice_indices_high) {
        break;
      }
      next(cvar_interface, &iterator, iterator);
    }

    if (!g_tv_voice_indices) {
      g_tv_voice_indices_high = nullptr;
      return false;
    }
    g_tv_voice_original = *g_tv_voice_indices;
    g_tv_voice_original_valid = true;
    if (g_tv_voice_indices_high) {
      g_tv_voice_original_high = *g_tv_voice_indices_high;
      g_tv_voice_original_high_valid = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_tv_voice_indices = nullptr;
    g_tv_voice_indices_high = nullptr;
    g_tv_voice_original_valid = false;
    g_tv_voice_original_high_valid = false;
    return false;
  }

  char detail[96]{};
  std::snprintf(detail, sizeof(detail), "cvars_ready low=%p high=%p",
                static_cast<void*>(g_tv_voice_indices),
                static_cast<void*>(g_tv_voice_indices_high));
  log_kv("voice_receive", detail);
  return true;
}

struct PlayerRoster {
  std::uint32_t low[4]{};
  std::uint32_t high[4]{};
  unsigned count[4]{};
  unsigned eligible[4]{};
  unsigned total = 0;
  unsigned highest_index = 0;
};

bool inspect_player_controller(unsigned entity_index, int* team_out,
                               int* slot_out, bool* eligible_out) {
  if (!g_client || !team_out || !slot_out || !eligible_out) {
    return false;
  }
  __try {
    void* controller = entity_from_index(entity_index);
    if (!controller) {
      return false;
    }
    auto* controller_bytes = reinterpret_cast<std::uint8_t*>(controller);
    const auto pawn_handle = *reinterpret_cast<std::uint32_t*>(
        controller_bytes + offsets::kControllerPlayerPawn);
    void* pawn = entity_from_handle(pawn_handle);
    if (!pawn) {
      return false;
    }
    auto* pawn_bytes = reinterpret_cast<std::uint8_t*>(pawn);
    const int team = *reinterpret_cast<std::uint8_t*>(
        pawn_bytes + offsets::kEntityTeamNum);
    if (team != 2 && team != 3) {
      return false;
    }

    // A random entity can have readable bytes at +m_hPlayerPawn. Require the
    // pawn's native controller backlink to resolve to this exact candidate.
    const std::uint32_t controller_handles[] = {
        *reinterpret_cast<std::uint32_t*>(
            pawn_bytes + offsets::kPawnOriginalController),
        *reinterpret_cast<std::uint32_t*>(
            pawn_bytes + offsets::kPawnController),
    };
    bool backlink_matches = false;
    for (const auto handle : controller_handles) {
      if (entity_from_handle(handle) == controller) {
        backlink_matches = true;
        break;
      }
    }
    if (!backlink_matches) {
      return false;
    }

    using PawnSlotFn = int*(__fastcall*)(void*, int*);
    auto get_slot = reinterpret_cast<PawnSlotFn>(
        reinterpret_cast<std::uint8_t*>(g_client) +
        offsets::kClientPawnGetPlayerSlotRva);
    int slot = -1;
    get_slot(pawn, &slot);
    if (slot < 0 || slot >= 64) {
      return false;
    }
    auto** vtable = *reinterpret_cast<void***>(controller);
    using PlayerIteratorEligibilityFn = bool(__fastcall*)(void*);
    auto eligibility = reinterpret_cast<PlayerIteratorEligibilityFn>(
        vtable[1792 / sizeof(void*)]);
    *team_out = team;
    *slot_out = slot;
    // Exact predicate used by CS2's native player iterator (sub_A27ED0).
    // False from the virtual is accepted into radar/top player collections.
    *eligible_out = eligibility && !eligibility(controller);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

PlayerRoster scan_player_roster() {
  PlayerRoster roster{};
  // Player controllers are not guaranteed to occupy entity indices 1..64;
  // after a demo seek their handles are commonly in later chunks. A bounded
  // walk over the game-native chunk table is cheap at identity changes and is
  // validated controller -> pawn -> controller before accepting an entry.
  constexpr unsigned kEntityScanLimit = 4096;
  for (unsigned entity_index = 1; entity_index < kEntityScanLimit;
       ++entity_index) {
    int team = 0;
    int slot = -1;
    bool eligible = false;
    if (!inspect_player_controller(entity_index, &team, &slot, &eligible)) {
      continue;
    }
    const auto bit = 1u << (slot & 31);
    auto& half = slot < 32 ? roster.low[team] : roster.high[team];
    if ((half & bit) != 0) {
      continue;
    }
    half |= bit;
    ++roster.count[team];
    if (eligible) {
      ++roster.eligible[team];
    }
    ++roster.total;
    roster.highest_index = entity_index;
  }
  return roster;
}

bool __fastcall hud_team_relationship_scope(void* local_pawn,
                                              unsigned int target_handle) {
  auto original = reinterpret_cast<HudTeamRelationshipFn>(
      g_hud_team_relationship.trampoline);
  const bool native = original ? original(local_pawn, target_handle) : false;
  if (!local_pawn ||
      !pov::active(pov::Domain::radar | pov::Domain::team_counter |
                   pov::Domain::player_overhead)) {
    return native;
  }

  const auto followed = pov::snapshot();
  if (!followed.pawn || local_pawn != followed.pawn ||
      (followed.team != 2 && followed.team != 3)) {
    return native;
  }
  void* target = entity_from_handle(target_handle);
  if (!target) {
    target = entity_from_index(target_handle & 0x7FFFu);
  }
  const int target_team = entity_team(target);
  if (target_team != 2 && target_team != 3) {
    return native;
  }

  const bool direct_enemy = followed.team != target_team;
  if (direct_enemy != native) {
    const auto adapted = ++g_team_relationship_adapted;
    if (adapted <= 8) {
      char detail[128]{};
      std::snprintf(detail, sizeof(detail),
                    "native=%d direct=%d local_team=%d target_team=%d "
                    "target=0x%08X",
                    native ? 1 : 0, direct_enemy ? 1 : 0, followed.team,
                    target_team, target_handle);
      log_kv("pov_relationship", detail);
    }
  }
  return direct_enemy;
}

bool __fastcall buy_zone_predicate_scope(void* pawn, bool strict) {
  auto original = reinterpret_cast<BuyZonePredicateFn>(
      g_buy_zone_predicate.trampoline);
  const bool native = original ? original(pawn, strict) : false;
  if (!pawn || !pov::active(pov::Domain::money)) {
    return native;
  }
  const auto followed = pov::snapshot();
  if (!followed.pawn || pawn != followed.pawn) {
    return native;
  }
  bool in_buy_zone = native;
  __try {
    in_buy_zone = *reinterpret_cast<const std::uint8_t*>(
                      reinterpret_cast<const std::uint8_t*>(pawn) +
                      offsets::kPawnInBuyZone) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return native;
  }
  if (in_buy_zone != native) {
    const auto adapted = ++g_buy_zone_adapted;
    if (adapted <= 8) {
      char detail[96]{};
      std::snprintf(detail, sizeof(detail),
                    "native=%d replicated=%d strict=%d",
                    native ? 1 : 0, in_buy_zone ? 1 : 0, strict ? 1 : 0);
      log_kv("pov_buy_zone", detail);
    }
  }
  return in_buy_zone;
}

void refresh_team_voice_receive_mask() {
  // The cvar registry can finish after client HUD construction. Resolve on
  // the first radar transaction, then retry only once every few seconds.
  if (!g_tv_voice_indices) {
    if ((g_voice_cvar_retry_frames++ % 240u) != 0u ||
        !resolve_voice_receive_cvars()) {
      return;
    }
  }

  const auto followed = pov::snapshot();
  const bool valid_team = followed.team == 2 || followed.team == 3;
  const auto seek_epoch = g_seek_rebuild_epoch.load(std::memory_order_acquire);
  bool scan_due = followed.generation != g_voice_roster_generation ||
                  seek_epoch != g_voice_roster_seek_epoch;
  if (!scan_due && valid_team && g_voice_roster_selected < 5) {
    scan_due = ++g_voice_roster_retry_frames >= 60;
  }
  if (!scan_due) {
    return;
  }
  g_voice_roster_retry_frames = 0;
  g_voice_roster_generation = followed.generation;
  g_voice_roster_seek_epoch = seek_epoch;

  std::uint32_t low = 0;
  std::uint32_t high = 0;
  unsigned selected = 0;
  PlayerRoster roster{};

  if (valid_team && g_client) {
    roster = scan_player_roster();
    low = roster.low[followed.team];
    high = roster.high[followed.team];
    selected = roster.count[followed.team];

    // Keep the followed speaker addressable even during the short interval
    // in which CS2 has published the chase slot but not rebuilt the table.
    if (followed.slot >= 0 && followed.slot < 64) {
      const auto bit = 1u << (followed.slot & 31);
      auto& half = followed.slot < 32 ? low : high;
      if ((half & bit) == 0) {
        half |= bit;
        ++selected;
      }
    }
  }
  g_voice_roster_selected = selected;

  char roster_detail[192]{};
  std::snprintf(
      roster_detail, sizeof(roster_detail),
      "gen=%llu seek=%llu total=%u t=%u/%u ct=%u/%u last_entity=%u "
      "selected=%u",
      static_cast<unsigned long long>(followed.generation),
      static_cast<unsigned long long>(seek_epoch), roster.total,
      roster.count[2], roster.eligible[2], roster.count[3],
      roster.eligible[3], roster.highest_index, selected);
  log_kv("pov_roster", roster_detail);

  const int team = valid_team ? followed.team : 0;
  if (low == g_voice_mask_low && high == g_voice_mask_high &&
      team == g_voice_mask_team) {
    return;
  }

  __try {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(g_tv_voice_indices),
                        static_cast<LONG>(low));
    if (g_tv_voice_indices_high) {
      InterlockedExchange(
          reinterpret_cast<volatile LONG*>(g_tv_voice_indices_high),
          static_cast<LONG>(high));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("voice_receive", "mask_write_seh");
    return;
  }

  g_voice_mask_low = low;
  g_voice_mask_high = high;
  g_voice_mask_team = team;
  char detail[128]{};
  std::snprintf(detail, sizeof(detail),
                "team=%d low=0x%08X high=0x%08X slots=%u", team, low,
                high, selected);
  log_kv("voice_receive", detail);
}

void log_native_voice_state(const pov::Snapshot& followed) {
  if (!g_client) {
    return;
  }
  __try {
    auto get_state = reinterpret_cast<VoiceStateGetFn>(
        reinterpret_cast<std::uint8_t*>(g_client) +
        offsets::kClientVoiceStateGetRva);
    auto* state = reinterpret_cast<std::uint8_t*>(get_state());
    if (!state) {
      return;
    }
    const auto speaking_low = *reinterpret_cast<std::uint32_t*>(state + 148);
    const auto speaking_high = *reinterpret_cast<std::uint32_t*>(state + 152);
    const auto audible_low = *reinterpret_cast<std::uint32_t*>(state + 156);
    const auto audible_high = *reinterpret_cast<std::uint32_t*>(state + 160);
    char detail[144]{};
    std::snprintf(detail, sizeof(detail),
                  "gen=%llu speaking=%08X/%08X audible=%08X/%08X",
                  static_cast<unsigned long long>(followed.generation),
                  speaking_low, speaking_high, audible_low, audible_high);
    log_kv("pov_voice_state", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_voice_state", "read_seh");
  }
}

void adapt_native_voice_speaking_from_audio() {
  if (!g_client) {
    return;
  }

  const auto followed = pov::snapshot();
  const bool valid_team = followed.team == 2 || followed.team == 3;
  const bool roster_ready = valid_team && g_voice_mask_team == followed.team;
  const std::uint32_t team_low = roster_ready ? g_voice_mask_low : 0;
  const std::uint32_t team_high = roster_ready ? g_voice_mask_high : 0;

  __try {
    auto* base = reinterpret_cast<std::uint8_t*>(g_client);
    auto get_state = reinterpret_cast<VoiceStateGetFn>(
        base + offsets::kClientVoiceStateGetRva);
    auto get_activity = reinterpret_cast<VoiceActivityFn>(
        base + offsets::kClientVoiceActivityRva);
    auto update_speaker = reinterpret_cast<VoiceUpdateSpeakerStatusFn>(
        base + offsets::kClientVoiceUpdateSpeakerStatusRva);
    auto* state = reinterpret_cast<std::uint8_t*>(get_state());
    if (!state) {
      return;
    }

    auto reconcile_half = [&](unsigned first_slot, std::uint32_t team_mask,
                              std::uint32_t& adapted_mask) {
      for (unsigned bit_index = 0; bit_index < 32; ++bit_index) {
        const auto bit = 1u << bit_index;
        const unsigned slot = first_slot + bit_index;
        const bool selected = (team_mask & bit) != 0;
        const float activity = selected ? get_activity(static_cast<int>(slot))
                                        : 0.0f;
        const bool talking = selected && activity > 0.0f;
        const bool adapted = (adapted_mask & bit) != 0;
        const auto native_offset = first_slot == 0 ? 148u : 152u;
        const auto native_bits =
            *reinterpret_cast<const std::uint32_t*>(state + native_offset);
        const bool native_speaking = (native_bits & bit) != 0;
        // A packet can cross a POV/team edge after the native live branch has
        // published its speaking bit. Never leave a non-selected slot visible.
        if (!selected && native_speaking) {
          update_speaker(state, slot, -1, 0);
          adapted_mask &= ~bit;
          continue;
        }
        if ((!talking && !adapted) ||
            (talking && adapted && native_speaking)) {
          continue;
        }

        update_speaker(state, slot, -1, talking ? 1u : 0u);
        if (talking) {
          adapted_mask |= bit;
        } else {
          adapted_mask &= ~bit;
        }

        const auto count = ++g_voice_audio_adapt_count;
        if (count <= 32) {
          char detail[128]{};
          std::snprintf(detail, sizeof(detail),
                        "count=%llu slot=%u talking=%d activity=%.3f team=%d",
                        static_cast<unsigned long long>(count), slot,
                        talking ? 1 : 0, static_cast<double>(activity),
                        followed.team);
          log_kv("pov_voice_audio", detail);
        }
      }
    };

    reconcile_half(0, team_low, g_voice_audio_adapted_low);
    reconcile_half(32, team_high, g_voice_audio_adapted_high);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_voice_audio", "adapter_seh");
  }
}

bool execute_client_command(const char* command) {
  if (!g_client || !command || !command[0]) {
    return false;
  }
  __try {
    void* engine = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(g_client) +
        offsets::kClientEngineToClientPtrRva);
    auto** vtable = engine ? *reinterpret_cast<void***>(engine) : nullptr;
    if (!vtable ||
        !vtable[offsets::kEngineToClientExecuteCommandVtableIndex]) {
      return false;
    }
    auto execute = reinterpret_cast<ExecuteClientCommandFn>(
        vtable[offsets::kEngineToClientExecuteCommandVtableIndex]);
    execute(engine, 0, command, 1, 0.0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void maybe_rebuild_hud_after_seek() {
  const auto epoch = g_seek_rebuild_epoch.load(std::memory_order_acquire);
  if (epoch == 0 || epoch == g_seek_rebuild_seen_epoch) {
    return;
  }
  const auto followed = pov::snapshot();
  if (!followed.pawn || (followed.team != 2 && followed.team != 3)) {
    return;
  }
  g_seek_rebuild_seen_epoch = epoch;
  // Queued reload commands execute after the transaction scope has exited and
  // rebuild the panels against the HLTV/free-camera identity. Runtime proved
  // that this collapses an initially correct roster to self-only. The shared
  // relation and identity inputs are now adapted every native HUD transaction,
  // so no asynchronous panel reconstruction is required.
  char detail[80]{};
  std::snprintf(detail, sizeof(detail),
                "native_reload_bypassed epoch=%llu scoped_inputs=1",
                static_cast<unsigned long long>(epoch));
  log_kv("pov_seek", detail);
}

void maybe_bootstrap_initial_follow() {
  if (g_follow_bootstrap_complete) {
    return;
  }
  if (pov::snapshot().pawn) {
    g_follow_bootstrap_complete = true;
    return;
  }
  if (++g_follow_bootstrap_frames < 120) {
    return;
  }
  g_follow_bootstrap_frames = 0;
  if (g_follow_bootstrap_attempts >= 4) {
    return;
  }
  ++g_follow_bootstrap_attempts;
  log_kv("pov_bootstrap", execute_client_command("spec_next")
                              ? "spec_next_requested"
                              : "spec_next_failed");
}

void __fastcall radar_transaction_scope(std::uintptr_t hud,
                                        std::uint8_t active) {
  auto original = g_radar_transaction_original;
  if (demo_is_skipping()) {
    clear_pending_radar_sounds();
    if (original) {
      original(hud, active);
    }
    return;
  }

  pov::Scope scope(pov::Domain::radar);
  // Demo seek can restore spectator radar cvars after the launch command has
  // run. Submit the live settings in the owning game-thread transaction before
  // native relationship and icon-style consumers execute.
  force_teammate_colors_no_letters();
  if (g_player_sound_enabled) {
    const auto seek_epoch =
        g_seek_rebuild_epoch.load(std::memory_order_acquire);
    if (seek_epoch != g_radar_sound_cvar_seek_epoch) {
      execute_client_command("snd_disable_radar_visualize 0");
      g_radar_sound_cvar_seek_epoch = seek_epoch;
    }
  }
  if (!g_radar_transaction_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "radar_transaction_live_context");
  }
  // Submit one matured event fallback immediately before HudRadar consumes its
  // native 28-byte queue.  Submitting after the transaction left the record
  // parked until a later Demo frame, where the live-local gate/reset could
  // discard it before the snippet factory ran.
  if (active && g_player_sound_enabled) {
    flush_pending_radar_sounds();
  }
  const bool owns_sound_probe = !g_radar_sound_transaction_active;
  if (owns_sound_probe) {
    g_radar_sound_transaction_active = true;
    g_radar_sound_created_hud = nullptr;
    g_radar_sound_created_snippet = nullptr;
    g_radar_sound_created_snippet_updated = false;
    g_radar_sound_max_snippet = nullptr;
    g_radar_sound_max_armed = false;
  }
  if (original) {
    original(hud, active);
  }
  if (owns_sound_probe && g_radar_sound_created_snippet &&
      !g_radar_sound_created_snippet_updated &&
      g_radar_sound_frame_update_original && g_radar_sound_created_hud) {
    // The Demo branch can consume a sound row yet omit the normal sound-loop
    // pass that follows live HudRadar input. Re-enter the complete native
    // frame updater once, still inside the owning radar identity transaction;
    // the E3A420 call wrapper above proves whether the row reached Panorama.
    g_radar_sound_frame_update_original(g_radar_sound_created_hud);
    if (g_radar_sound_created_snippet_updated) {
      const auto repaired = g_repaired_radar_sound_count.fetch_add(
                                1, std::memory_order_relaxed) +
                            1;
      if (repaired <= 32) {
        char detail[112]{};
        std::snprintf(detail, sizeof(detail),
                      "native_frame_repaired=%llu snippet=%p",
                      static_cast<unsigned long long>(repaired),
                      g_radar_sound_created_snippet);
        log_kv("pov_sound_update", detail);
      }
    } else {
      const auto missed = g_missed_radar_sound_update_count.fetch_add(
                              1, std::memory_order_relaxed) +
                          1;
      if (missed <= 32) {
        char detail[112]{};
        std::snprintf(detail, sizeof(detail),
                      "native_frame_no_match=%llu snippet=%p",
                      static_cast<unsigned long long>(missed),
                      g_radar_sound_created_snippet);
        log_kv("pov_sound_update", detail);
      }
    }
  }
  if (owns_sound_probe) {
    g_radar_sound_transaction_active = false;
    g_radar_sound_created_hud = nullptr;
    g_radar_sound_created_snippet = nullptr;
    g_radar_sound_created_snippet_updated = false;
    g_radar_sound_max_snippet = nullptr;
    g_radar_sound_max_armed = false;
  }
  if (active) {
    refresh_team_voice_receive_mask();
    maybe_bootstrap_initial_follow();
    maybe_rebuild_hud_after_seek();
    flush_pending_damage_feedback();
    flush_pending_death_banner();
  }
}

void __fastcall radar_update_scope(std::uintptr_t hud) {
  auto original = reinterpret_cast<OneArgVoidFn>(g_radar_update.trampoline);
  if (demo_is_skipping()) {
    if (original) {
      original(hud);
    }
    return;
  }
  pov::Scope scope(pov::Domain::radar);
  if (original) {
    original(hud);
  }
}

std::uint64_t __fastcall radar_mode_update_scope(std::uintptr_t hud) {
  auto original =
      reinterpret_cast<OneArgFn>(g_radar_mode_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : 0;
  }
  pov::Scope scope(pov::Domain::radar);
  if (!g_radar_mode_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "radar_mode_live_context");
  }
  return original ? original(hud) : 0;
}

std::uint64_t __fastcall radar_local_transform_scope(std::uintptr_t hud) {
  auto original =
      reinterpret_cast<OneArgFn>(g_radar_local_transform.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : 0;
  }
  pov::Scope scope(pov::Domain::radar);
  if (!g_radar_local_transform_seen.exchange(true,
                                              std::memory_order_relaxed)) {
    log_kv("pov_runtime", "radar_local_transform_live_context");
  }
  return original ? original(hud) : 0;
}

std::uint64_t __fastcall player_overhead_update_scope(std::uintptr_t hud) {
  auto original =
      reinterpret_cast<OneArgFn>(g_player_overhead_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : 0;
  }
  pov::Scope scope(pov::Domain::player_overhead);
  if (!g_player_overhead_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "player_overhead_live_context");
  }
  return original ? original(hud) : 0;
}

std::uint64_t __fastcall team_counter_update_scope(std::uintptr_t hud) {
  auto original =
      reinterpret_cast<OneArgFn>(g_team_counter_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : 0;
  }
  pov::Scope scope(pov::Domain::team_counter);
  return original ? original(hud) : 0;
}

std::uint64_t __fastcall voice_update_scope(std::uintptr_t hud_callback) {
  auto original = reinterpret_cast<OneArgFn>(g_voice_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud_callback) : 0;
  }
  pov::Scope scope(pov::Domain::voice);
  if (!g_voice_live_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "voice_live_context");
  }
  // Some recorded-demo voice decoders publish native per-player audio
  // activity but bypass the live packet path that toggles CVoiceStatus's
  // speaking bit. Bridge those two native inputs before the stock HUD update;
  // speaker identity, level/timeout and Panorama rendering remain native.
  adapt_native_voice_speaking_from_audio();
  const auto result = original ? original(hud_callback) : 0;
  const auto followed = pov::snapshot();
  const bool generation_edge = followed.generation != g_voice_state_generation;
  if (generation_edge) {
    g_voice_state_generation = followed.generation;
    g_voice_state_frames = 0;
  }
  if (followed.pawn && (generation_edge || ++g_voice_state_frames >= 120)) {
    g_voice_state_frames = 0;
    log_native_voice_state(followed);
  }
  return result;
}

int server_voice_speaker_slot(std::uintptr_t message) noexcept {
  if (!message) {
    return -1;
  }
  __try {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(message);
    const int flags = *reinterpret_cast<const int*>(bytes + 0x40);
    int client_index = -1;
    if ((flags & 0x80) != 0) {
      client_index = *reinterpret_cast<const int*>(bytes + 0x6C);
    } else if ((flags & 0x40) != 0) {
      const int encoded = *reinterpret_cast<const int*>(bytes + 0x68);
      if (encoded != -1) {
        client_index = encoded + 1;
      }
    }
    return client_index >= 1 && client_index <= 64 ? client_index - 1 : -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

std::uint64_t __fastcall server_voice_submit_scope(std::uintptr_t decoder,
                                                    std::uintptr_t message) {
  auto original =
      reinterpret_cast<TwoArgFn>(g_server_voice_submit.trampoline);
  if (demo_is_skipping()) {
    return original ? original(decoder, message) : 0;
  }
  // AED960's receive-mask check exists only on its Demo branch. The scoped
  // IsPlayingDemo=false adaptation selects the native live activity path, so
  // reapply the same zero-based speaker-bit decision before entering it.
  const int speaker_slot = server_voice_speaker_slot(message);
  const auto followed = pov::snapshot();
  const bool mask_ready = (followed.team == 2 || followed.team == 3) &&
                          g_voice_mask_team == followed.team;
  std::uint32_t mask = 0;
  if (speaker_slot >= 0) {
    mask = speaker_slot < 32 ? g_voice_mask_low : g_voice_mask_high;
  }
  const bool selected = mask_ready && speaker_slot >= 0 &&
                        (mask & (1u << (speaker_slot & 31))) != 0;
  if (!selected) {
    const auto dropped = ++g_voice_packet_team_drops;
    if (dropped <= 24) {
      char detail[112]{};
      std::snprintf(detail, sizeof(detail),
                    "count=%llu slot=%d team=%d mask_team=%d",
                    static_cast<unsigned long long>(dropped), speaker_slot,
                    followed.team, g_voice_mask_team);
      log_kv("pov_voice_drop", detail);
    }
    return 0;
  }
  pov::Scope scope(pov::Domain::voice);
  if (!g_server_voice_live_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "server_voice_live_context");
  }
  return original ? original(decoder, message) : 0;
}

std::uint64_t __fastcall money_update_scope(std::uintptr_t hud,
                                            std::uintptr_t visible) {
  auto original = reinterpret_cast<TwoArgFn>(g_money_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud, visible) : 0;
  }
  pov::Scope scope(pov::Domain::money);
  return original ? original(hud, visible) : 0;
}

// Keep SEH in leaf readers with no C++ objects that need unwinding.  The
// transaction wrappers below can then use normal RAII for pov::Scope.
bool followed_victim_event(std::uintptr_t event, bool arm_death_banner) {
  if (!event) {
    return false;
  }
  __try {
    const char* name = event_name(reinterpret_cast<void*>(event));
    if (!name || std::strcmp(name, "player_death") != 0) {
      return false;
    }
    const auto followed = pov::snapshot();
    void* victim = event_player_field(
        reinterpret_cast<void*>(event), "userid", "userid", 6,
        offsets::kClientDeathUseridSeed);
    const bool matched = followed.pawn && followed.controller &&
                         victim == followed.pawn;
    if (matched && arm_death_banner) {
      g_combat_latch_generation.store(followed.generation,
                                      std::memory_order_relaxed);
      g_combat_latch_stamp.store(GetTickCount64(),
                                 std::memory_order_relaxed);
      pov::pin(followed);
      g_last_killer_damage_armed.store(true, std::memory_order_release);
      log_kv("pov_combat", "death_pov_latched_ms=2000");
    }
    return matched;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "death_gate_seh");
    return false;
  }
}

bool damage_targets_follow(std::uintptr_t message,
                           const pov::Snapshot& followed,
                           int* message_player_id,
                           int* followed_player_id) {
  __try {
    *message_player_id = *reinterpret_cast<const int*>(message + 84);
    if (*message_player_id >= 0 && g_client) {
      using EntityPlayerIdFn = int*(__fastcall*)(void*, int*);
      auto player_id = reinterpret_cast<EntityPlayerIdFn>(
          reinterpret_cast<std::uint8_t*>(g_client) +
          offsets::kClientEntityPlayerIdRva);
      player_id(followed.pawn, followed_player_id);
      if (*followed_player_id >= 0 &&
          *followed_player_id != *message_player_id) {
        return false;
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool pawn_is_native_dead(void* pawn) {
  __try {
    return pawn &&
           *reinterpret_cast<const int*>(
               reinterpret_cast<const std::uint8_t*>(pawn) +
               offsets::kPawnPlayerState) == 4;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool capture_last_killer_damage(std::uintptr_t message,
                                const pov::Snapshot& followed,
                                std::uint64_t stamp,
                                PendingLastKillerDamage* out) {
  if (!message || !out || !followed.pawn || !followed.controller ||
      !pawn_is_native_dead(followed.pawn)) {
    return false;
  }
  __try {
    PendingLastKillerDamage captured{};
    // C216C0 maps the wire fields to E089A0 in this exact order:
    // +54, +50, +4C, +48, +58, +5C.
    captured.values = {
        *reinterpret_cast<const int*>(message + 0x54),
        *reinterpret_cast<const int*>(message + 0x50),
        *reinterpret_cast<const int*>(message + 0x4C),
        *reinterpret_cast<const int*>(message + 0x48),
        *reinterpret_cast<const int*>(message + 0x58),
        *reinterpret_cast<const int*>(message + 0x5C),
    };
    captured.generation = followed.generation;
    captured.stamp = stamp;
    captured.valid = true;
    *out = captured;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void submit_death_panel_summary(const PendingDeathBanner& banner,
                                const PendingLastKillerDamage* summary) {
  if (!banner.hud || !g_death_panel_damage_summary) {
    return;
  }
  const std::array<int, 6> zero{};
  const auto& values = summary && summary->valid ? summary->values : zero;
  pov::Scope scope(pov::Domain::combat_feedback);
  g_death_panel_damage_summary(
      banner.hud, values[0], values[1], values[2], values[3], values[4],
      values[5]);
}

void flush_pending_death_banner() {
  PendingDeathBanner banner{};
  PendingLastKillerDamage summary{};
  auto resolution = event_compensation::DeathBannerResolution::wait;
  const auto now = GetTickCount64();

  AcquireSRWLockExclusive(&g_pending_death_banner_lock);
  if (g_pending_death_banner.hud) {
    const bool same_generation =
        g_pending_last_killer_damage.valid &&
        g_pending_last_killer_damage.generation ==
            g_pending_death_banner.generation;
    const auto summary_stamp = same_generation
                                   ? g_pending_last_killer_damage.stamp
                                   : 0;
    resolution = event_compensation::resolve_death_banner(
        g_pending_death_banner.stamp, now, summary_stamp,
        kDeathBannerPairWindowMs);
    if (resolution != event_compensation::DeathBannerResolution::wait) {
      banner = g_pending_death_banner;
      if (same_generation) {
        summary = g_pending_last_killer_damage;
      }
      g_pending_death_banner = {};
      g_pending_last_killer_damage = {};
    }
  }
  ReleaseSRWLockExclusive(&g_pending_death_banner_lock);

  if (resolution == event_compensation::DeathBannerResolution::wait) {
    return;
  }
  const auto followed = pov::snapshot();
  if (!followed.pawn || !followed.controller ||
      followed.generation != banner.generation ||
      !pawn_is_native_dead(followed.pawn) || demo_is_skipping()) {
    return;
  }

  const bool replay_native =
      resolution == event_compensation::DeathBannerResolution::native_summary &&
      summary.valid;
  submit_death_panel_summary(banner, replay_native ? &summary : nullptr);
  g_last_killer_damage_armed.store(false, std::memory_order_release);
  const auto count = g_death_banner_fallback_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 16) {
    log_kv("pov_combat", replay_native
                              ? "death_banner_native_summary_replayed"
                              : "death_banner_zero_summary_fallback");
  }
}

void queue_damage_feedback_event(std::uintptr_t event) {
  if (!event || demo_is_skipping()) {
    return;
  }
  __try {
    const char* name = event_name(reinterpret_cast<void*>(event));
    if (!name || std::strcmp(name, "player_hurt") != 0) {
      return;
    }
    const auto followed = pov::snapshot();
    void* victim = event_player_field(
        reinterpret_cast<void*>(event), "userid", "userid", 6,
        offsets::kClientDeathUseridSeed);
    if (!followed.pawn || !followed.controller || victim != followed.pawn) {
      return;
    }
    void* attacker = event_player_field(
        reinterpret_cast<void*>(event), "attacker", "cker", 4,
        offsets::kClientEventAttackerSeed);
    if (!attacker || attacker == victim) {
      return;
    }
    PendingDamageFeedback pending{};
    pending.victim = victim;
    pending.attacker = attacker;
    pending.generation = followed.generation;
    pending.stamp = GetTickCount64();
    pending.damage = event_int_field(
        reinterpret_cast<void*>(event), "dmg_health", 10,
        offsets::kClientEventDamageHealthSeed);
    AcquireSRWLockExclusive(&g_pending_damage_lock);
    g_pending_damage = pending;
    ReleaseSRWLockExclusive(&g_pending_damage_lock);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "damage_queue_seh");
  }
}

bool build_damage_feedback_input(const PendingDamageFeedback& pending,
                                 std::uintptr_t* hud_out,
                                 std::uint8_t message[160], int* id_out,
                                 float source_out[3]) {
  __try {
    using AbsOriginFn = const float*(__fastcall*)(void*);
    auto abs_origin = reinterpret_cast<AbsOriginFn>(
        reinterpret_cast<std::uint8_t*>(g_client) +
        offsets::kClientEntityAbsOriginRva);
    const float* source = abs_origin(pending.attacker);
    if (!source) {
      return false;
    }
    source_out[0] = source[0];
    source_out[1] = source[1];
    source_out[2] = source[2];
    // E010C0 consumes +0x48 as a protobuf-style vector message and reads the
    // actual xyz values from +0x18/+0x1c/+0x20.  Pointing +0x48 directly at
    // AbsOrigin made those reads land past the raw float[3], yielding a zero
    // direction and an early return from the native indicator calculator.
    auto* vector_message = message + 96;
    *reinterpret_cast<float*>(vector_message + 0x18) = source[0];
    *reinterpret_cast<float*>(vector_message + 0x1c) = source[1];
    *reinterpret_cast<float*>(vector_message + 0x20) = source[2];
    *reinterpret_cast<const void**>(message + 72) = vector_message;
    *reinterpret_cast<int*>(message + 80) = pending.damage > 0
                                                ? pending.damage
                                                : 1;
    using EntityPlayerIdFn = int*(__fastcall*)(void*, int*);
    auto player_id = reinterpret_cast<EntityPlayerIdFn>(
        reinterpret_cast<std::uint8_t*>(g_client) +
        offsets::kClientEntityPlayerIdRva);
    player_id(pending.victim, id_out);
    *reinterpret_cast<int*>(message + 84) = *id_out;
    void* element = g_find_hud_element
                        ? g_find_hud_element("CCSGO_HudDamageIndicator")
                        : nullptr;
    if (!element) {
      return false;
    }
    *hud_out = reinterpret_cast<std::uintptr_t>(
        reinterpret_cast<std::uint8_t*>(element) - 0x20);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool invoke_damage_direction_repair(std::uintptr_t hud,
                                    const float source[3],
                                    void* victim) noexcept {
  if (!g_damage_direction_original || !hud || !source || !victim) {
    return false;
  }
  __try {
    g_damage_direction_original(hud, source, victim);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "damage_direction_repair_seh");
    return false;
  }
}

void flush_pending_damage_feedback() {
  PendingDamageFeedback pending{};
  AcquireSRWLockExclusive(&g_pending_damage_lock);
  const auto now = GetTickCount64();
  if (g_pending_damage.victim &&
      now - g_pending_damage.stamp >= kRadarSoundNativePriorityMs) {
    pending = g_pending_damage;
    g_pending_damage = {};
  }
  ReleaseSRWLockExclusive(&g_pending_damage_lock);
  if (!pending.victim) {
    return;
  }

  const auto followed = pov::snapshot();
  if (!followed.pawn || followed.pawn != pending.victim ||
      followed.generation != pending.generation || demo_is_skipping()) {
    return;
  }
  const auto native_stamp =
      g_native_damage_stamp.load(std::memory_order_acquire);
  if (native_stamp >= pending.stamp && native_stamp - pending.stamp <= 100) {
    const auto count = g_damage_deduped_count.fetch_add(
                           1, std::memory_order_relaxed) +
                       1;
    if (count <= 16) {
      log_kv("pov_combat", "damage_event_deduped_native");
    }
    return;
  }

  // Rebuild only the stable fields consumed by E010C0: protobuf-vector pointer
  // at +0x48, positive damage at +0x50, and explicit victim player id at
  // +0x54. The vector payload lives at +0x60 and stores xyz at its native
  // +0x18/+0x1c/+0x20 offsets. The native Damage handler owns all direction
  // math and Panorama output.
  alignas(16) std::uint8_t message[160]{};
  std::uintptr_t hud = 0;
  int id = -1;
  float source[3]{};
  if (!build_damage_feedback_input(pending, &hud, message, &id, source)) {
    log_kv("pov_combat", "damage_input_build_failed");
    return;
  }
  auto original = reinterpret_cast<TwoArgFn>(g_damage_message.trampoline);
  if (!original) {
    return;
  }
  std::array<float, 4> strengths_before{};
  std::array<float, 4> strengths_after{};
  const bool strengths_before_valid =
      read_damage_indicator_strengths(hud, strengths_before);
  pov::Scope scope(pov::Domain::combat_feedback);
  if (g_damage_indicator_visible) {
    g_damage_indicator_visible(reinterpret_cast<void*>(hud + 0x20), true);
    const auto visible = g_damage_visibility_count.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
    if (visible <= 24) {
      log_kv("pov_combat", "damage_indicator_visibility_native");
    }
  }
  original(hud, reinterpret_cast<std::uintptr_t>(message));
  bool strengths_after_valid =
      read_damage_indicator_strengths(hud, strengths_after);
  const bool message_produced_direction =
      strengths_after_valid &&
      (strengths_after[0] > 0.001f || strengths_after[1] > 0.001f ||
       strengths_after[2] > 0.001f || strengths_after[3] > 0.001f);
  bool direction_repaired = false;
  if (!message_produced_direction) {
    // E011F0 can reject an event-derived Damage message at a mode/player-id
    // gate before its E012FB call.  Use that exact audited callee and ABI with
    // the stable attacker AbsOrigin captured above and immutable POV victim.
    // The game still owns view projection and all four Panorama strengths.
    direction_repaired =
        invoke_damage_direction_repair(hud, source, pending.victim);
    if (direction_repaired) {
      strengths_after_valid =
          read_damage_indicator_strengths(hud, strengths_after);
    }
  }
  const auto count = g_damage_fallback_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 24) {
    char detail[320]{};
    std::snprintf(detail, sizeof(detail),
                  "damage_event_native=%llu damage=%d player=%d repaired=%d "
                  "strengths_valid=%d/%d before=%.3f,%.3f,%.3f,%.3f "
                  "after=%.3f,%.3f,%.3f,%.3f",
                  static_cast<unsigned long long>(count), pending.damage, id,
                  direction_repaired ? 1 : 0,
                  strengths_before_valid ? 1 : 0,
                  strengths_after_valid ? 1 : 0,
                  static_cast<double>(strengths_before[0]),
                  static_cast<double>(strengths_before[1]),
                  static_cast<double>(strengths_before[2]),
                  static_cast<double>(strengths_before[3]),
                  static_cast<double>(strengths_after[0]),
                  static_cast<double>(strengths_after[1]),
                  static_cast<double>(strengths_after[2]),
                  static_cast<double>(strengths_after[3]));
    log_kv("pov_combat", detail);
  }
}

void __fastcall death_postprocess_update_scope(std::uintptr_t client_mode) {
  auto original = reinterpret_cast<OneArgVoidFn>(
      g_death_postprocess_update.trampoline);
  const auto now = GetTickCount64();
  const auto latch_stamp =
      g_combat_latch_stamp.load(std::memory_order_relaxed);
  if (pov::pinned() && latch_stamp && now >= latch_stamp &&
      now - latch_stamp >= kDeathPovLatchMs) {
    pov::unpin();
    log_kv("pov_combat", "death_pov_latch_released");
  }
  const auto followed = pov::snapshot();
  if (!original || demo_is_skipping() || !followed.pawn) {
    if (original) {
      original(client_mode);
    }
    return;
  }

  // CA59C0 owns the complete native death-cam phase state machine. Its local
  // pawn and Demo/HLTV predicates must observe the same immutable POV identity
  // as C81720; scoping only player_death leaves all phase weights at zero.
  {
    pov::Scope scope(pov::Domain::combat_feedback);
    original(client_mode);
  }
  log_death_postprocess_weights(client_mode);
}

void __fastcall gameplay_event_scope(std::uintptr_t listener,
                                     std::uintptr_t event) {
  auto original = reinterpret_cast<TwoArgVoidFn>(g_gameplay_event.trampoline);
  const bool followed_death =
      !demo_is_skipping() && followed_victim_event(event, true);

  // Keep every non-death event, especially round_start/round_end during a
  // seek, on real Demo identity. C81720 is invoked through every Pawn's +8
  // listener subobject, whose address is not guaranteed to equal the entity-
  // system Pawn base published by the immutable POV snapshot. The native
  // function performs its own userid/player-slot rejection before reaching
  // any HUD identity consumer, so scoping every invocation of this one current-
  // POV player_death event remains narrow: only the victim listener reaches
  // CS2's death-camera/red-view branch.
  if (original) {
    if (followed_death) {
      // Keep the postprocess probe budget per death.  A two-second phase at a
      // high frame rate otherwise consumes the global cap and makes a later
      // direct-black death indistinguishable in the log.
      g_death_postprocess_active_count.store(0, std::memory_order_relaxed);
      g_death_postprocess_zero_count.store(0, std::memory_order_relaxed);
      pov::Scope scope(pov::Domain::combat_feedback);
      original(listener, event);
      const auto count = g_death_camera_count.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
      if (count <= 12) {
        log_kv("pov_combat", "native_player_death_transaction");
      }
      log_death_timing_cvars();
      log_client_mode_postprocess_resources(listener);
    } else {
      original(listener, event);
    }
  }
  // The two Demo-only compensations consume the event and the immutable POV
  // snapshot directly.  They do not need to remap any identity getter while
  // the native event dispatcher is running.
  if (demo_is_skipping()) {
    return;
  }
  adapt_player_death_feedback(reinterpret_cast<void*>(event));
  adapt_kill_cash_notice(reinterpret_cast<void*>(event));
}

void __fastcall damage_direction_scope(std::uintptr_t hud,
                                       const float* source, void* pawn) {
  auto original = g_damage_direction_original;
  std::array<float, 4> before{};
  std::array<float, 4> after{};
  const bool tracked = pov::active(pov::Domain::combat_feedback);
  const bool before_valid = tracked && read_damage_indicator_strengths(hud, before);
  if (original) {
    original(hud, source, pawn);
  }
  if (!tracked) {
    return;
  }
  const bool after_valid = read_damage_indicator_strengths(hud, after);
  const auto followed = pov::snapshot();
  const auto count =
      g_damage_direction_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count > 32) {
    return;
  }
  __try {
    const float x = source ? source[0] : 0.0f;
    const float y = source ? source[1] : 0.0f;
    const float z = source ? source[2] : 0.0f;
    char detail[384]{};
    std::snprintf(
        detail, sizeof(detail),
        "damage_direction_native=%llu hud=%p source=%.1f,%.1f,%.1f "
        "pawn=%p followed=%p same=%d valid=%d/%d "
        "before=%.3f,%.3f,%.3f,%.3f after=%.3f,%.3f,%.3f,%.3f",
        static_cast<unsigned long long>(count),
        reinterpret_cast<void*>(hud), static_cast<double>(x),
        static_cast<double>(y), static_cast<double>(z), pawn, followed.pawn,
        pawn && pawn == followed.pawn ? 1 : 0, before_valid ? 1 : 0,
        after_valid ? 1 : 0, static_cast<double>(before[0]),
        static_cast<double>(before[1]), static_cast<double>(before[2]),
        static_cast<double>(before[3]), static_cast<double>(after[0]),
        static_cast<double>(after[1]), static_cast<double>(after[2]),
        static_cast<double>(after[3]));
    log_kv("pov_combat", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "damage_direction_probe_seh");
  }
}

std::uint64_t __fastcall damage_message_scope(std::uintptr_t hud,
                                              std::uintptr_t message) {
  auto original = reinterpret_cast<TwoArgFn>(g_damage_message.trampoline);
  if (!original || !message || demo_is_skipping()) {
    return original ? original(hud, message) : 0;
  }

  const auto followed = pov::snapshot();
  bool accepted = followed.pawn && followed.controller;
  int message_player_id = -1;
  int followed_player_id = -1;
  accepted = accepted && damage_targets_follow(
                             message, followed, &message_player_id,
                             &followed_player_id);
  if (!accepted) {
    const auto rejected = g_combat_reject_count.fetch_add(
                              1, std::memory_order_relaxed) +
                          1;
    if (rejected <= 20) {
      char detail[112]{};
      std::snprintf(detail, sizeof(detail),
                    "damage_drop=%llu msg_player=%d follow_player=%d",
                    static_cast<unsigned long long>(rejected),
                    message_player_id, followed_player_id);
      log_kv("pov_combat", detail);
    }
    return original(hud, message);
  }

  std::array<float, 4> strengths_before{};
  std::array<float, 4> strengths_after{};
  const bool strengths_before_valid =
      read_damage_indicator_strengths(hud, strengths_before);

  pov::Scope scope(pov::Domain::combat_feedback);
  if (g_damage_indicator_visible) {
    g_damage_indicator_visible(reinterpret_cast<void*>(hud + 0x20), true);
    const auto visible = g_damage_visibility_count.fetch_add(
                             1, std::memory_order_relaxed) +
                         1;
    if (visible <= 24) {
      log_kv("pov_combat", "damage_indicator_visibility_native");
    }
  }
  const auto result = original(hud, message);
  const bool strengths_after_valid =
      read_damage_indicator_strengths(hud, strengths_after);
  g_native_damage_stamp.store(GetTickCount64(), std::memory_order_release);
  const auto count = g_damage_message_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 32) {
    char detail[320]{};
    std::snprintf(detail, sizeof(detail),
                  "damage_native=%llu msg_player=%d follow_player=%d "
                  "strengths_valid=%d/%d before=%.3f,%.3f,%.3f,%.3f "
                  "after=%.3f,%.3f,%.3f,%.3f",
                  static_cast<unsigned long long>(count), message_player_id,
                  followed_player_id, strengths_before_valid ? 1 : 0,
                  strengths_after_valid ? 1 : 0,
                  static_cast<double>(strengths_before[0]),
                  static_cast<double>(strengths_before[1]),
                  static_cast<double>(strengths_before[2]),
                  static_cast<double>(strengths_before[3]),
                  static_cast<double>(strengths_after[0]),
                  static_cast<double>(strengths_after[1]),
                  static_cast<double>(strengths_after[2]),
                  static_cast<double>(strengths_after[3]));
    log_kv("pov_combat", detail);
  }
  return result;
}

void __fastcall death_panel_show_scope(std::uintptr_t hud) {
  auto original = reinterpret_cast<OneArgVoidFn>(g_death_panel_show.trampoline);
  const bool tracked = pov::active(pov::Domain::combat_feedback);
  if (original) {
    original(hud);
  }
  if (!tracked || !hud) {
    return;
  }
  const auto now = GetTickCount64();
  g_death_panel_visible_hud.store(hud, std::memory_order_release);
  g_death_panel_visible_since.store(now, std::memory_order_release);
  __try {
    char detail[160]{};
    std::snprintf(detail, sizeof(detail),
                  "death_panel_shown_native hud=%p visible=%u armed=%u",
                  reinterpret_cast<void*>(hud),
                  static_cast<unsigned>(
                      *reinterpret_cast<const std::uint8_t*>(hud + 0x1A0)),
                  static_cast<unsigned>(
                      *reinterpret_cast<const std::uint8_t*>(hud + 0x1C0)));
    log_kv("pov_combat", detail);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    log_kv("pov_combat", "death_panel_show_probe_seh");
  }
}

void __fastcall death_panel_hide_scope(std::uintptr_t hud) {
  auto original = reinterpret_cast<OneArgVoidFn>(g_death_panel_hide.trampoline);
  const auto tracked_hud =
      g_death_panel_visible_hud.load(std::memory_order_acquire);
  const auto shown_at =
      g_death_panel_visible_since.load(std::memory_order_acquire);
  if (original) {
    original(hud);
  }
  if (!hud || hud != tracked_hud || !shown_at) {
    return;
  }
  g_death_panel_visible_hud.store(0, std::memory_order_release);
  g_death_panel_visible_since.store(0, std::memory_order_release);
  const auto now = GetTickCount64();
  char detail[160]{};
  std::snprintf(detail, sizeof(detail),
                "death_panel_hidden_native hud=%p elapsed_ms=%llu",
                reinterpret_cast<void*>(hud),
                static_cast<unsigned long long>(now - shown_at));
  log_kv("pov_combat", detail);
}

std::uint64_t __fastcall death_panel_event_scope(std::uintptr_t hud,
                                                  std::uintptr_t event) {
  auto original = reinterpret_cast<TwoArgFn>(g_death_panel_event.trampoline);
  if (!original || !event || demo_is_skipping()) {
    return original ? original(hud, event) : 0;
  }
  const auto followed = pov::snapshot();
  const bool accepted = followed_victim_event(event, true);
  if (!accepted) {
    return original(hud, event);
  }

  pov::Scope scope(pov::Domain::combat_feedback);
  const auto result = original(hud, event);
  const auto stamp = GetTickCount64();

  // player_death populates killer/weapon text but deliberately hides the
  // panel until a damage summary arrives. Pair a real summary that arrived
  // just before this event; otherwise hold the populated panel for a short
  // window so a just-after native message can finish the same transaction.
  PendingDeathBanner banner{hud, followed.generation, stamp};
  PendingLastKillerDamage early_summary{};
  bool replay_early_summary = false;
  AcquireSRWLockExclusive(&g_pending_death_banner_lock);
  if (g_pending_last_killer_damage.valid &&
      g_pending_last_killer_damage.generation == followed.generation &&
      event_compensation::resolve_death_banner(
          stamp, stamp, g_pending_last_killer_damage.stamp,
          kDeathBannerPairWindowMs) ==
          event_compensation::DeathBannerResolution::native_summary) {
    early_summary = g_pending_last_killer_damage;
    g_pending_last_killer_damage = {};
    g_pending_death_banner = {};
    replay_early_summary = true;
  } else {
    g_pending_death_banner = banner;
  }
  ReleaseSRWLockExclusive(&g_pending_death_banner_lock);

  if (replay_early_summary) {
    submit_death_panel_summary(banner, &early_summary);
    g_last_killer_damage_armed.store(false, std::memory_order_release);
    const auto replayed = g_death_banner_fallback_count.fetch_add(
                              1, std::memory_order_relaxed) +
                          1;
    if (replayed <= 16) {
      log_kv("pov_combat", "death_banner_early_summary_replayed");
    }
  }
  const auto count = g_death_event_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 12) {
    log_kv("pov_combat", "death_panel_event_native");
  }
  return result;
}

std::uint64_t __fastcall last_killer_damage_scope(std::uintptr_t message) {
  auto original = reinterpret_cast<OneArgFn>(
      g_last_killer_damage.trampoline);
  if (!original || !message || demo_is_skipping()) {
    return original ? original(message) : 0;
  }
  const auto followed = pov::snapshot();
  const auto armed_generation =
      g_combat_latch_generation.load(std::memory_order_relaxed);
  const auto armed_stamp =
      g_combat_latch_stamp.load(std::memory_order_relaxed);
  const auto now = GetTickCount64();
  PendingLastKillerDamage captured{};
  const bool has_captured =
      capture_last_killer_damage(message, followed, now, &captured);
  bool accepted = followed.pawn && followed.controller &&
                   followed.generation == armed_generation &&
                   now >= armed_stamp &&
                   now - armed_stamp <= kDeathBannerPairWindowMs &&
                   pawn_is_native_dead(followed.pawn) &&
                  g_last_killer_damage_armed.exchange(
                      false, std::memory_order_acq_rel);
  if (!accepted) {
    if (has_captured) {
      AcquireSRWLockExclusive(&g_pending_death_banner_lock);
      g_pending_last_killer_damage = captured;
      ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
      log_kv("pov_combat", "last_killer_damage_waiting_for_death");
    }
    return original(message);
  }

  std::uint64_t result = 0;
  {
    pov::Scope scope(pov::Domain::combat_feedback);
    result = original(message);
  }
  g_native_last_killer_damage_stamp.store(now, std::memory_order_release);

  // If player_death already populated the panel, this native handler has just
  // completed it and the pending fallback must be cancelled. If the gameplay
  // listener armed first but DeathPanel has not consumed player_death yet,
  // retain the six native fields so that event can replay them after it resets
  // and fills the banner content.
  bool completed_pending_banner = false;
  AcquireSRWLockExclusive(&g_pending_death_banner_lock);
  if (g_pending_death_banner.hud &&
      g_pending_death_banner.generation == followed.generation &&
      event_compensation::resolve_death_banner(
          g_pending_death_banner.stamp, now, now,
          kDeathBannerPairWindowMs) ==
          event_compensation::DeathBannerResolution::native_summary) {
    g_pending_death_banner = {};
    g_pending_last_killer_damage = {};
    completed_pending_banner = true;
  } else if (has_captured) {
    g_pending_last_killer_damage = captured;
  }
  ReleaseSRWLockExclusive(&g_pending_death_banner_lock);

  if (completed_pending_banner) {
    g_death_banner_deduped_count.fetch_add(1, std::memory_order_relaxed);
  }
  const auto count = g_last_killer_damage_count.fetch_add(
                         1, std::memory_order_relaxed) +
                     1;
  if (count <= 12) {
    log_kv("pov_combat", completed_pending_banner
                              ? "last_killer_damage_completed_banner"
                              : "last_killer_damage_before_death_panel");
  }
  return result;
}

void __fastcall radio_text_scope(std::uintptr_t split_screen_slot,
                                 std::uintptr_t message) {
  auto original = reinterpret_cast<TwoArgVoidFn>(g_radio_text.trampoline);
  pov::Scope scope(pov::Domain::communications);
  if (original) {
    original(split_screen_slot, message);
  }
}

void __fastcall say_text2_scope(std::uintptr_t split_screen_slot,
                                std::uintptr_t message) {
  auto original = reinterpret_cast<TwoArgVoidFn>(g_say_text2.trampoline);
  pov::Scope scope(pov::Domain::communications);
  if (original) {
    original(split_screen_slot, message);
  }
}

std::uint64_t __fastcall hud_root_update_scope(std::uintptr_t hud) {
  auto original = reinterpret_cast<OneArgFn>(g_hud_root_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : 0;
  }
  pov::Scope scope(pov::Domain::hud_presentation);
  if (!g_hud_presentation_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "hud_presentation_live_context");
  }
  return original ? original(hud) : 0;
}

std::uint64_t __fastcall spec_player_update_scope(std::uintptr_t hud) {
  auto original =
      reinterpret_cast<OneArgFn>(g_spec_player_update.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : 0;
  }
  pov::Scope scope(pov::Domain::hud_presentation);
  return original ? original(hud) : 0;
}

void __fastcall live_flash_submit_scope(std::uintptr_t a1, std::uintptr_t a2,
                                        std::uintptr_t a3, std::uintptr_t a4,
                                        std::uintptr_t a5) {
  auto original =
      reinterpret_cast<FiveArgVoidFn>(g_live_flash_submit.trampoline);
  if (demo_is_skipping()) {
    if (original) {
      original(a1, a2, a3, a4, a5);
    }
    return;
  }
  pov::Scope scope(pov::Domain::view_effects);
  if (!g_live_flash_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_runtime", "live_flash_submit_context");
  }
  if (original) {
    original(a1, a2, a3, a4, a5);
  }
}

void __fastcall render_graph_scope(std::uintptr_t a1, std::uintptr_t a2,
                                   std::uintptr_t a3, std::uintptr_t a4,
                                   std::uintptr_t a5) {
  auto original = reinterpret_cast<FiveArgVoidFn>(g_render_graph.trampoline);
  if (demo_is_skipping()) {
    if (original) {
      original(a1, a2, a3, a4, a5);
    }
    return;
  }
  pov::Scope scope(pov::Domain::view_effects);
  if (original) {
    original(a1, a2, a3, a4, a5);
  }
}

void* hud_player_adapter(EntryHook& hook) {
  auto original = reinterpret_cast<HudGetterFn>(hook.trampoline);
  void* native = original ? original() : nullptr;
  if (!pov::active() || demo_is_skipping() || native) {
    return native;
  }
  // The original getter calls the scoped slot->pawn adapter first. This
  // fallback is only for native helpers that reject a valid HLTV chase pawn
  // after resolving it (for example an observer-only IsAlive branch).
  return pov::snapshot().pawn;
}

void* __fastcall get_hud_player_scope() {
  return hud_player_adapter(g_get_hud_player);
}

void* __fastcall get_hud_alive_scope() {
  if (pov::active(pov::Domain::player_sound | pov::Domain::radar) &&
      !demo_is_skipping()) {
    // EmitSound may run outside the game/HUD thread, while HudRadar's
    // per-frame sound updater runs inside the radar transaction. Both ends
    // must resolve the same immutable POV pawn: preserving a non-null Demo
    // observer here makes E4A4E0 see a different player id and immediately
    // hide/reset the snippet that E241C0 created on the previous frame.
    const auto followed = pov::snapshot();
    if (followed.pawn) {
      if (pov::active(pov::Domain::radar) &&
          !g_radar_sound_identity_seen.exchange(true,
                                                 std::memory_order_relaxed)) {
        log_kv("pov_sound", "radar_update_followed_identity");
      }
      return followed.pawn;
    }
  }
  return hud_player_adapter(g_get_hud_alive);
}

bool __fastcall spectator_tools_scope() {
  if (pov::active(pov::Domain::view_effects |
                  pov::Domain::player_overhead |
                  pov::Domain::combat_feedback)) {
    return false;
  }
  auto original =
      reinterpret_cast<BoolNoArgFn>(g_spectator_tools.trampoline);
  return original ? original() : false;
}

bool __fastcall voice_should_draw_scope(void* hud) {
  auto original =
      reinterpret_cast<BoolOneArgFn>(g_voice_should_draw.trampoline);
  if (demo_is_skipping()) {
    return original ? original(hud) : false;
  }
  pov::Scope scope(pov::Domain::voice);
  const bool native = original ? original(hud) : false;
  if (!g_voice_draw_gate_seen.exchange(true, std::memory_order_relaxed)) {
    log_kv("pov_voice", native ? "native_display_gate_open"
                               : "native_display_gate_bootstrapped");
  }
  // VoiceStatus is still responsible for speaker detection, player lookup,
  // timeout and Panorama painting. The gate is evaluated while a 5E demo is
  // still in free camera, before a followed identity exists, and is not
  // revisited later. Allow its native panel to initialize at that first call;
  // the receive mask remains empty until a valid followed team is published.
  return true;
}

bool __fastcall is_playing_demo_scope(void* engine) {
  if (pov::active(pov::Domain::communications | pov::Domain::voice |
                  pov::Domain::combat_feedback)) {
    return false;
  }
  return g_is_playing_demo_original
             ? g_is_playing_demo_original(engine)
             : false;
}

unsigned int __fastcall broadcast_mode_scope(void* mode_provider) {
  if (pov::active(pov::Domain::team_counter)) {
    return 0;
  }
  return g_broadcast_mode_original
             ? g_broadcast_mode_original(mode_provider)
             : 0;
}

bool install_radar_transaction_adapter(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto** slot = reinterpret_cast<void**>(
      base + offsets::kClientRadarTransactionVtableSlotRva);
  void* expected = base + offsets::kClientRadarTransactionUpdateRva;
  if (*slot != expected) {
    log_kv("pov_boundary", "radar_transaction_vtable_mismatch");
    return false;
  }

  // Publish the callable target before making the wrapper reachable. The same
  // ordering rule applies here as to executable entry hooks: a render thread
  // may enter immediately after the atomic slot replacement.
  g_radar_transaction_slot = slot;
  g_radar_transaction_original =
      reinterpret_cast<RadarTransactionFn>(expected);
  MemoryBarrier();

  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    g_radar_transaction_slot = nullptr;
    g_radar_transaction_original = nullptr;
    log_kv("pov_boundary", "radar_transaction_protect_failed");
    return false;
  }
  void* observed = InterlockedCompareExchangePointer(
      reinterpret_cast<void* volatile*>(slot),
      reinterpret_cast<void*>(&radar_transaction_scope), expected);
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  if (observed != expected) {
    g_radar_transaction_slot = nullptr;
    g_radar_transaction_original = nullptr;
    log_kv("pov_boundary", "radar_transaction_replace_raced");
    return false;
  }
  log_kv("pov_boundary", "radar_transaction_adapter_ok");
  return true;
}

void restore_radar_transaction_adapter() {
  if (g_radar_transaction_slot && g_radar_transaction_original) {
    DWORD old_protect = 0;
    if (VirtualProtect(g_radar_transaction_slot, sizeof(void*),
                       PAGE_READWRITE, &old_protect)) {
      InterlockedCompareExchangePointer(
          reinterpret_cast<void* volatile*>(g_radar_transaction_slot),
          reinterpret_cast<void*>(g_radar_transaction_original),
          reinterpret_cast<void*>(&radar_transaction_scope));
      VirtualProtect(g_radar_transaction_slot, sizeof(void*), old_protect,
                     &old_protect);
    }
  }
  g_radar_transaction_slot = nullptr;
  g_radar_transaction_original = nullptr;
}

void restore_voice_receive_cvars() {
  __try {
    if (g_tv_voice_indices && g_tv_voice_original_valid) {
      InterlockedExchange(
          reinterpret_cast<volatile LONG*>(g_tv_voice_indices),
          static_cast<LONG>(g_tv_voice_original));
    }
    if (g_tv_voice_indices_high && g_tv_voice_original_high_valid) {
      InterlockedExchange(
          reinterpret_cast<volatile LONG*>(g_tv_voice_indices_high),
          static_cast<LONG>(g_tv_voice_original_high));
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  g_tv_voice_indices = nullptr;
  g_tv_voice_indices_high = nullptr;
  g_tv_voice_original = 0;
  g_tv_voice_original_high = 0;
  g_tv_voice_original_valid = false;
  g_tv_voice_original_high_valid = false;
  g_voice_mask_low = 0;
  g_voice_mask_high = 0;
  g_voice_mask_team = -1;
  g_voice_cvar_retry_frames = 0;
  g_voice_roster_generation = 0;
  g_voice_roster_seek_epoch = 0;
  g_voice_roster_retry_frames = 0;
  g_voice_roster_selected = 0;
}

bool install_broadcast_mode_adapter(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  void* provider = *reinterpret_cast<void**>(
      base + offsets::kClientBroadcastModeObjectPtrRva);
  if (!provider) {
    log_kv("pov_boundary", "broadcast_mode_provider_missing");
    return false;
  }
  auto** vtable = *reinterpret_cast<void***>(provider);
  if (!vtable) {
    return false;
  }
  void** slot = &vtable[offsets::kBroadcastModeVtableIndex];
  if (!*slot) {
    return false;
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    return false;
  }
  g_broadcast_mode_original = reinterpret_cast<UIntOneArgFn>(*slot);
  *slot = reinterpret_cast<void*>(&broadcast_mode_scope);
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  g_broadcast_mode_slot = slot;
  log_kv("pov_boundary", "broadcast_mode_adapter_ok");
  return true;
}

bool install_is_playing_demo_adapter(HMODULE client) {
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  void* engine = *reinterpret_cast<void**>(
      base + offsets::kClientEngineToClientPtrRva);
  if (!engine) {
    log_kv("pov_boundary", "is_playing_demo_engine_missing");
    return false;
  }
  auto** vtable = *reinterpret_cast<void***>(engine);
  if (!vtable) {
    return false;
  }
  void** slot =
      &vtable[offsets::kEngineToClientIsPlayingDemoVtableIndex];
  if (!*slot) {
    return false;
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    return false;
  }
  g_is_playing_demo_original = reinterpret_cast<BoolOneArgFn>(*slot);
  *slot = reinterpret_cast<void*>(&is_playing_demo_scope);
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  g_is_playing_demo_slot = slot;
  log_kv("pov_boundary", "is_playing_demo_adapter_ok");
  return true;
}

bool install_player_pawn_event_adapter(HMODULE client) {
  if (!g_throw_notice_enabled) {
    log_kv("pov_compensation", "throw_notice_disabled_env");
    return true;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto** slot = reinterpret_cast<void**>(
      base + offsets::kClientPlayerPawnEventVtableSlotRva);
  void* expected = base + offsets::kClientPlayerPawnFireGameEventRva;
  if (*slot != expected) {
    log_kv("pov_boundary", "player_pawn_event_vtable_mismatch");
    return false;
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    log_kv("pov_boundary", "player_pawn_event_protect_failed");
    return false;
  }
  g_player_pawn_event_original =
      reinterpret_cast<PlayerPawnEventFn>(expected);
  *slot = reinterpret_cast<void*>(&player_pawn_event_scope);
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  g_player_pawn_event_slot = slot;
  log_kv("pov_boundary", "player_pawn_event_adapter_ok");
  return true;
}

void restore_player_pawn_event_adapter() {
  if (g_player_pawn_event_slot && g_player_pawn_event_original) {
    DWORD old_protect = 0;
    if (VirtualProtect(g_player_pawn_event_slot, sizeof(void*), PAGE_READWRITE,
                       &old_protect)) {
      if (*g_player_pawn_event_slot ==
          reinterpret_cast<void*>(&player_pawn_event_scope)) {
        *g_player_pawn_event_slot =
            reinterpret_cast<void*>(g_player_pawn_event_original);
      }
      VirtualProtect(g_player_pawn_event_slot, sizeof(void*), old_protect,
                     &old_protect);
    }
  }
  g_player_pawn_event_slot = nullptr;
  g_player_pawn_event_original = nullptr;
}

bool install_radar_sound_adapter(HMODULE client) {
  if (!g_player_sound_enabled) {
    log_kv("pov_sound", "disabled_env");
    return true;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  g_radar_sound_submit_original = reinterpret_cast<RadarSoundSubmitFn>(
      base + offsets::kClientRadarSoundSubmitRva);
  g_radar_sound_create_original = reinterpret_cast<RadarSoundCreateFn>(
      base + offsets::kClientRadarSoundCreateRva);
  g_radar_sound_frame_update_original =
      reinterpret_cast<RadarSoundFrameUpdateFn>(
          base + offsets::kClientRadarSoundFrameUpdateRva);
  g_radar_sound_snippet_update_original =
      reinterpret_cast<RadarSoundSnippetUpdateFn>(
          base + offsets::kClientRadarSoundSnippetUpdateRva);
  MemoryBarrier();
  if (!install_rel_call(client, offsets::kClientRadarSoundEmitCallRva,
                        offsets::kClientRadarSoundEmitCallBytes,
                        reinterpret_cast<const void*>(&radar_sound_submit_scope),
                        g_radar_sound_emit_call, "radar_sound_emit_call")) {
    g_radar_sound_submit_original = nullptr;
    g_radar_sound_create_original = nullptr;
    g_radar_sound_frame_update_original = nullptr;
    g_radar_sound_snippet_update_original = nullptr;
    return false;
  }
  if (!install_rel_call(
          client, offsets::kClientRadarSoundCreateCallRva,
          offsets::kClientRadarSoundCreateCallBytes,
          reinterpret_cast<const void*>(&radar_sound_create_scope),
          g_radar_sound_create_call, "radar_sound_create_call")) {
    restore_rel_call(g_radar_sound_emit_call);
    g_radar_sound_submit_original = nullptr;
    g_radar_sound_create_original = nullptr;
    g_radar_sound_frame_update_original = nullptr;
    g_radar_sound_snippet_update_original = nullptr;
    return false;
  }
  if (!install_rel_call(
          client, offsets::kClientRadarSoundSnippetUpdateCallRva,
          offsets::kClientRadarSoundSnippetUpdateCallBytes,
          reinterpret_cast<const void*>(&radar_sound_snippet_update_scope),
          g_radar_sound_snippet_update_call,
          "radar_sound_snippet_update_call")) {
    restore_rel_call(g_radar_sound_create_call);
    restore_rel_call(g_radar_sound_emit_call);
    g_radar_sound_submit_original = nullptr;
    g_radar_sound_create_original = nullptr;
    g_radar_sound_frame_update_original = nullptr;
    g_radar_sound_snippet_update_original = nullptr;
    return false;
  }
  return true;
}

void restore_radar_sound_adapter() {
  restore_rel_call(g_radar_sound_snippet_update_call);
  restore_rel_call(g_radar_sound_create_call);
  restore_rel_call(g_radar_sound_emit_call);
  g_radar_sound_submit_original = nullptr;
  g_radar_sound_create_original = nullptr;
  g_radar_sound_frame_update_original = nullptr;
  g_radar_sound_snippet_update_original = nullptr;
  clear_pending_radar_sounds();
}

bool install_game_event_dispatch_adapter(HMODULE client) {
  if (!g_player_sound_enabled) {
    return true;
  }
  auto* base = reinterpret_cast<std::uint8_t*>(client);
  auto** slot = reinterpret_cast<void**>(
      base + offsets::kClientGameEventDispatchVtableSlotRva);
  void* expected = base + offsets::kClientGameEventDispatchRva;
  if (*slot != expected) {
    log_kv("pov_boundary", "game_event_dispatch_vtable_mismatch");
    return false;
  }
  g_game_event_dispatch_slot = slot;
  g_game_event_dispatch_original =
      reinterpret_cast<GameEventDispatchFn>(expected);
  MemoryBarrier();
  DWORD old_protect = 0;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
    g_game_event_dispatch_slot = nullptr;
    g_game_event_dispatch_original = nullptr;
    return false;
  }
  void* observed = InterlockedCompareExchangePointer(
      reinterpret_cast<void* volatile*>(slot),
      reinterpret_cast<void*>(&game_event_dispatch_scope), expected);
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  if (observed != expected) {
    g_game_event_dispatch_slot = nullptr;
    g_game_event_dispatch_original = nullptr;
    log_kv("pov_boundary", "game_event_dispatch_replace_raced");
    return false;
  }
  log_kv("pov_boundary", "game_event_dispatch_adapter_ok");
  return true;
}

void restore_game_event_dispatch_adapter() {
  if (g_game_event_dispatch_slot && g_game_event_dispatch_original) {
    DWORD old_protect = 0;
    if (VirtualProtect(g_game_event_dispatch_slot, sizeof(void*),
                       PAGE_READWRITE, &old_protect)) {
      InterlockedCompareExchangePointer(
          reinterpret_cast<void* volatile*>(g_game_event_dispatch_slot),
          reinterpret_cast<void*>(g_game_event_dispatch_original),
          reinterpret_cast<void*>(&game_event_dispatch_scope));
      VirtualProtect(g_game_event_dispatch_slot, sizeof(void*), old_protect,
                     &old_protect);
    }
  }
  g_game_event_dispatch_slot = nullptr;
  g_game_event_dispatch_original = nullptr;
}

template <std::size_t N>
bool install_one(HMODULE client, std::uint32_t rva,
                 const std::uint8_t (&expected)[N], const void* replacement,
                 EntryHook& hook, const char* tag) {
  return install_entry(client, rva, expected, N, replacement, hook, tag);
}

template <std::size_t N>
bool validate_native_target(HMODULE client, std::uint32_t rva,
                            const std::uint8_t (&expected)[N],
                            const char* tag) {
  auto* entry = reinterpret_cast<std::uint8_t*>(client) + rva;
  if (std::memcmp(entry, expected, N) == 0) {
    return true;
  }
  char detail[96]{};
  std::snprintf(detail, sizeof(detail), "%s_prologue_mismatch", tag);
  log_kv("pov_boundary", detail);
  return false;
}

bool hlae_game_event_entry_is_chained(HMODULE client) noexcept {
  if (!client) {
    return false;
  }
  const auto* entry = reinterpret_cast<const std::uint8_t*>(client) +
                      offsets::kClientGameEventDispatchRva;
  // AfxHookSource2's existing GameEvents bridge owns this entry through
  // Detours. The vtable adapter below must chain through that entry instead of
  // rejecting the already-installed E9 as a native prologue mismatch.
  return entry[0] == 0xE9;
}

}  // namespace

void note_seek_end() noexcept {
  pov::unpin();
  clear_pending_radar_sounds();
  AcquireSRWLockExclusive(&g_pending_damage_lock);
  g_pending_damage = {};
  ReleaseSRWLockExclusive(&g_pending_damage_lock);
  AcquireSRWLockExclusive(&g_pending_death_banner_lock);
  g_pending_death_banner = {};
  g_pending_last_killer_damage = {};
  ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
  g_native_last_killer_damage_stamp.store(0, std::memory_order_relaxed);
  g_last_killer_damage_armed.store(false, std::memory_order_relaxed);
  g_combat_latch_generation.store(0, std::memory_order_relaxed);
  g_combat_latch_stamp.store(0, std::memory_order_relaxed);
  g_seek_rebuild_epoch.fetch_add(1, std::memory_order_release);
}

bool install(HMODULE client) {
  if (g_radar_update.entry) {
    return true;
  }

  g_client = client;
  g_emit_hurt_feedback = reinterpret_cast<EmitHurtFeedbackFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientEmitHurtFeedbackSoundRva);
  g_push_notice = reinterpret_cast<PushNoticeFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientPushNoticeRva);
  g_find_hud_element = reinterpret_cast<FindHudElementFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientFindHudElementRva);
  g_death_panel_damage_summary = reinterpret_cast<DeathPanelDamageSummaryFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientDeathPanelDamageSummaryRva);
  g_damage_indicator_visible = reinterpret_cast<DamageIndicatorVisibleFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientDamageIndicatorVisibleRva);
  g_damage_direction_original = reinterpret_cast<DamageDirectionFn>(
      reinterpret_cast<std::uint8_t*>(client) +
      offsets::kClientDamageDirectionRva);
  g_kill_reward_enabled = env_flag_default_on("LIVE_HUD_KILL_REWARD");
  g_throw_notice_enabled = env_flag_default_on("LIVE_HUD_THROW_NOTICE");
  g_player_sound_enabled = env_flag_default_on("LIVE_HUD_PLAYER_SOUND");
  char compensation_flags[128]{};
  std::snprintf(compensation_flags, sizeof(compensation_flags),
                 "kill_reward=%d throw_notice=%d player_sound=%d "
                 "source=demo_events",
                 g_kill_reward_enabled ? 1 : 0,
                 g_throw_notice_enabled ? 1 : 0,
                 g_player_sound_enabled ? 1 : 0);
  log_kv("pov_compensation", compensation_flags);

  bool ok = validate_native_target(
      client, offsets::kClientEmitHurtFeedbackSoundRva,
      offsets::kClientEmitHurtFeedbackSoundPrologue,
      "emit_hurt_feedback");
  ok &= validate_native_target(
      client, offsets::kClientEventFieldHashRva,
      offsets::kClientEventFieldHashPrologue, "event_field_hash");
  ok &= validate_native_target(
      client, offsets::kClientFilterPlayerEntRva,
      offsets::kClientFilterPlayerEntPrologue, "filter_player_entity");
  if (g_kill_reward_enabled || g_throw_notice_enabled) {
    ok &= validate_native_target(
        client, offsets::kClientPushNoticeRva,
        offsets::kClientPushNoticePrologue, "push_notice_sink");
  }
  ok &= validate_native_target(
      client, offsets::kClientFindHudElementRva,
      offsets::kClientFindHudElementPrologue, "find_hud_element");
  ok &= validate_native_target(
      client, offsets::kClientDeathPanelDamageSummaryRva,
      offsets::kClientDeathPanelDamageSummaryPrologue,
      "death_panel_damage_summary");
  ok &= validate_native_target(
      client, offsets::kClientDamageIndicatorVisibleRva,
      offsets::kClientDamageIndicatorVisiblePrologue,
      "damage_indicator_visible");
  ok &= validate_native_target(
      client, offsets::kClientDamageDirectionRva,
      offsets::kClientDamageDirectionPrologue, "damage_direction");
  ok &= validate_native_target(
      client, offsets::kClientDeathPanelShowRva,
      offsets::kClientDeathPanelShowPrologue, "death_panel_show");
  ok &= validate_native_target(
      client, offsets::kClientDeathPanelHideRva,
      offsets::kClientDeathPanelHidePrologue, "death_panel_hide");
  ok &= validate_native_target(
      client, offsets::kClientPawnGetPlayerSlotRva,
      offsets::kClientPawnGetPlayerSlotPrologue, "pawn_get_player_slot");
  ok &= validate_native_target(
      client, offsets::kClientEntityPlayerIdRva,
      offsets::kClientEntityPlayerIdPrologue, "entity_player_id");
  ok &= validate_native_target(
      client, offsets::kClientEntityAbsOriginRva,
      offsets::kClientEntityAbsOriginPrologue, "entity_abs_origin");
  ok &= validate_native_target(
      client, offsets::kClientBroadcastModePredicateRva,
      offsets::kClientBroadcastModePredicatePrologue,
      "broadcast_mode_predicate");
  if (g_player_sound_enabled) {
    ok &= validate_native_target(
        client, offsets::kClientRadarSoundSubmitRva,
        offsets::kClientRadarSoundSubmitPrologue, "radar_sound_submit");
    if (!hlae_game_event_entry_is_chained(client)) {
      ok &= validate_native_target(
          client, offsets::kClientGameEventDispatchRva,
          offsets::kClientGameEventDispatchPrologue, "game_event_dispatch");
    } else {
      log_kv("pov_boundary", "game_event_dispatch_hlae_chain");
    }
  }
  ok &= install_one(client, offsets::kClientGetHudPlayerRva,
                    offsets::kClientGetHudPlayerPrologue,
                    reinterpret_cast<const void*>(&get_hud_player_scope),
                    g_get_hud_player, "get_hud_player");
  ok &= install_one(client, offsets::kClientGetHudAlivePawnRva,
                    offsets::kClientGetHudPlayerPrologue,
                    reinterpret_cast<const void*>(&get_hud_alive_scope),
                    g_get_hud_alive, "get_hud_alive");
  ok &= install_one(client, offsets::kClientSpectatorToolsPredicateRva,
                    offsets::kClientSpectatorToolsPredicatePrologue,
                    reinterpret_cast<const void*>(&spectator_tools_scope),
                    g_spectator_tools, "spectator_tools");
  ok &= install_one(client, offsets::kClientVoiceShouldDrawRva,
                    offsets::kClientVoiceShouldDrawPrologue,
                    reinterpret_cast<const void*>(&voice_should_draw_scope),
                    g_voice_should_draw, "voice_should_draw");
  ok &= install_one(
      client, offsets::kClientHudTeamRelationshipRva,
      offsets::kClientHudTeamRelationshipPrologue,
      reinterpret_cast<const void*>(&hud_team_relationship_scope),
      g_hud_team_relationship, "hud_team_relationship");
  ok &= install_one(client, offsets::kClientBuyZonePredicateRva,
                    offsets::kClientBuyZonePredicatePrologue,
                    reinterpret_cast<const void*>(&buy_zone_predicate_scope),
                    g_buy_zone_predicate, "buy_zone_predicate");

  ok &= install_radar_transaction_adapter(client);
  ok &= install_one(client, offsets::kClientRadarModeUpdateRva,
                    offsets::kClientRadarModeUpdatePrologue,
                    reinterpret_cast<const void*>(&radar_mode_update_scope),
                    g_radar_mode_update, "radar_mode_update");
  ok &= install_one(client, offsets::kClientRadarUpdateRva,
                    offsets::kClientRadarUpdatePrologue,
                    reinterpret_cast<const void*>(&radar_update_scope),
                    g_radar_update, "radar_update");
  ok &= install_one(
      client, offsets::kClientRadarLocalTransformRva,
      offsets::kClientRadarLocalTransformPrologue,
      reinterpret_cast<const void*>(&radar_local_transform_scope),
      g_radar_local_transform, "radar_local_transform");
  ok &= install_one(client, offsets::kClientPlayerOverheadUpdateRva,
                    offsets::kClientPlayerOverheadUpdatePrologue,
                    reinterpret_cast<const void*>(&player_overhead_update_scope),
                    g_player_overhead_update, "player_overhead_update");
  ok &= install_one(client, offsets::kClientTeamCounterUpdateDispatchRva,
                    offsets::kClientTeamCounterUpdateDispatchPrologue,
                    reinterpret_cast<const void*>(&team_counter_update_scope),
                    g_team_counter_update, "team_counter_update");
  ok &= install_one(client, offsets::kClientVoiceUpdateBoundaryRva,
                    offsets::kClientVoiceUpdateBoundaryPrologue,
                    reinterpret_cast<const void*>(&voice_update_scope),
                    g_voice_update, "voice_update");
  ok &= install_one(client, offsets::kClientServerVoiceSubmitRva,
                    offsets::kClientServerVoiceSubmitPrologue,
                    reinterpret_cast<const void*>(&server_voice_submit_scope),
                    g_server_voice_submit, "server_voice_submit");
  ok &= install_one(client, offsets::kClientMoneyUpdateBoundaryRva,
                    offsets::kClientMoneyUpdateBoundaryPrologue,
                    reinterpret_cast<const void*>(&money_update_scope),
                    g_money_update, "money_update");
  ok &= install_one(client, offsets::kClientGameplayEventDispatchRva,
                    offsets::kClientGameplayEventDispatchPrologue,
                    reinterpret_cast<const void*>(&gameplay_event_scope),
                    g_gameplay_event, "gameplay_event");
  ok &= install_one(
      client, offsets::kClientDeathPostProcessUpdateRva,
      offsets::kClientDeathPostProcessUpdatePrologue,
      reinterpret_cast<const void*>(&death_postprocess_update_scope),
      g_death_postprocess_update, "death_postprocess_update");
  ok &= install_one(client, offsets::kClientDamageMessageHandlerRva,
                    offsets::kClientDamageMessageHandlerPrologue,
                    reinterpret_cast<const void*>(&damage_message_scope),
                    g_damage_message, "damage_message");
  ok &= install_rel_call(
      client, offsets::kClientDamageDirectionCallRva,
      offsets::kClientDamageDirectionCallBytes,
      reinterpret_cast<const void*>(&damage_direction_scope),
      g_damage_direction_call, "damage_direction_call");
  ok &= install_one(client, offsets::kClientDeathPanelEventRva,
                    offsets::kClientDeathPanelEventPrologue,
                    reinterpret_cast<const void*>(&death_panel_event_scope),
                    g_death_panel_event, "death_panel_event");
  ok &= install_one(client, offsets::kClientDeathPanelShowRva,
                    offsets::kClientDeathPanelShowPrologue,
                    reinterpret_cast<const void*>(&death_panel_show_scope),
                    g_death_panel_show, "death_panel_show");
  ok &= install_one(client, offsets::kClientDeathPanelHideRva,
                    offsets::kClientDeathPanelHidePrologue,
                    reinterpret_cast<const void*>(&death_panel_hide_scope),
                    g_death_panel_hide, "death_panel_hide");
  ok &= install_one(
      client, offsets::kClientLastKillerDamageHandlerRva,
      offsets::kClientLastKillerDamageHandlerPrologue,
      reinterpret_cast<const void*>(&last_killer_damage_scope),
      g_last_killer_damage, "last_killer_damage");
  ok &= install_one(client, offsets::kClientRadioTextHandlerRva,
                    offsets::kClientRadioTextHandlerPrologue,
                    reinterpret_cast<const void*>(&radio_text_scope),
                    g_radio_text, "radio_text");
  ok &= install_one(client, offsets::kClientSayText2HandlerRva,
                    offsets::kClientSayText2HandlerPrologue,
                    reinterpret_cast<const void*>(&say_text2_scope),
                    g_say_text2, "say_text2");
  ok &= install_one(client, offsets::kClientHudRootUpdateRva,
                    offsets::kClientHudRootUpdatePrologue,
                    reinterpret_cast<const void*>(&hud_root_update_scope),
                    g_hud_root_update, "hud_root_update");
  ok &= install_one(client, offsets::kClientSpecPlayerUpdateRva,
                    offsets::kClientSpecPlayerUpdatePrologue,
                    reinterpret_cast<const void*>(&spec_player_update_scope),
                    g_spec_player_update, "spec_player_update");
  ok &= install_one(client, offsets::kClientLiveFlashSubmitRva,
                    offsets::kClientLiveFlashSubmitPrologue,
                    reinterpret_cast<const void*>(&live_flash_submit_scope),
                    g_live_flash_submit, "live_flash_submit");
  ok &= install_one(client, offsets::kClientRenderGraphBuildRva,
                    offsets::kClientRenderGraphBuildPrologue,
                    reinterpret_cast<const void*>(&render_graph_scope),
                    g_render_graph, "render_graph");
  ok &= install_is_playing_demo_adapter(client);
  ok &= install_broadcast_mode_adapter(client);
  ok &= install_radar_sound_adapter(client);
  ok &= install_game_event_dispatch_adapter(client);
  // Grenade notices intentionally reuse the first-version, field-tested
  // weapon_fire listener in identity.cpp. Keeping that exception outside the
  // native data scopes avoids two listeners competing for the same vtable.

  if (!ok) {
    log_kv("pov_pipeline", "native_boundaries_failed_rollback");
    restore();
    return false;
  }
  log_kv("pov_pipeline", "native_boundaries_ok");
  return true;
}

void restore() {
  restore_rel_call(g_damage_direction_call);
  restore_game_event_dispatch_adapter();
  restore_radar_sound_adapter();
  restore_radar_transaction_adapter();
  restore_voice_receive_cvars();
  restore_player_pawn_event_adapter();

  if (g_broadcast_mode_slot && g_broadcast_mode_original) {
    DWORD old_protect = 0;
    if (VirtualProtect(g_broadcast_mode_slot, sizeof(void*), PAGE_READWRITE,
                       &old_protect)) {
      if (*g_broadcast_mode_slot ==
          reinterpret_cast<void*>(&broadcast_mode_scope)) {
        *g_broadcast_mode_slot =
            reinterpret_cast<void*>(g_broadcast_mode_original);
      }
      VirtualProtect(g_broadcast_mode_slot, sizeof(void*), old_protect,
                     &old_protect);
    }
  }
  g_broadcast_mode_slot = nullptr;
  g_broadcast_mode_original = nullptr;

  if (g_is_playing_demo_slot && g_is_playing_demo_original) {
    DWORD old_protect = 0;
    if (VirtualProtect(g_is_playing_demo_slot, sizeof(void*), PAGE_READWRITE,
                       &old_protect)) {
      if (*g_is_playing_demo_slot ==
          reinterpret_cast<void*>(&is_playing_demo_scope)) {
        *g_is_playing_demo_slot =
            reinterpret_cast<void*>(g_is_playing_demo_original);
      }
      VirtualProtect(g_is_playing_demo_slot, sizeof(void*), old_protect,
                     &old_protect);
    }
  }
  g_is_playing_demo_slot = nullptr;
  g_is_playing_demo_original = nullptr;
  g_emit_hurt_feedback = nullptr;
  g_push_notice = nullptr;
  g_find_hud_element = nullptr;
  g_death_panel_damage_summary = nullptr;
  g_damage_indicator_visible = nullptr;
  g_damage_direction_original = nullptr;
  g_client = nullptr;
  g_death_feedback_count.store(0, std::memory_order_relaxed);
  g_kill_reward_count.store(0, std::memory_order_relaxed);
  g_throw_event_count.store(0, std::memory_order_relaxed);
  g_throw_candidate_count.store(0, std::memory_order_relaxed);
  g_throw_notice_count.store(0, std::memory_order_relaxed);
  g_throw_team_drop_count.store(0, std::memory_order_relaxed);
  g_notice_sink_probe_count.store(0, std::memory_order_relaxed);
  g_radar_mode_seen.store(false, std::memory_order_relaxed);
  g_radar_local_transform_seen.store(false, std::memory_order_relaxed);
  g_radar_transaction_seen.store(false, std::memory_order_relaxed);
  g_radar_sound_identity_seen.store(false, std::memory_order_relaxed);
  g_player_overhead_seen.store(false, std::memory_order_relaxed);
  g_voice_live_seen.store(false, std::memory_order_relaxed);
  g_server_voice_live_seen.store(false, std::memory_order_relaxed);
  g_voice_draw_gate_seen.store(false, std::memory_order_relaxed);
  g_team_relationship_adapted.store(0, std::memory_order_relaxed);
  g_buy_zone_adapted.store(0, std::memory_order_relaxed);
  g_voice_state_generation = 0;
  g_voice_state_frames = 0;
  g_voice_audio_adapted_low = 0;
  g_voice_audio_adapted_high = 0;
  g_voice_audio_adapt_count.store(0, std::memory_order_relaxed);
  g_voice_packet_team_drops.store(0, std::memory_order_relaxed);
  g_hud_presentation_seen.store(false, std::memory_order_relaxed);
  g_live_flash_seen.store(false, std::memory_order_relaxed);
  g_damage_message_count.store(0, std::memory_order_relaxed);
  g_damage_visibility_count.store(0, std::memory_order_relaxed);
  g_damage_direction_count.store(0, std::memory_order_relaxed);
  g_death_event_count.store(0, std::memory_order_relaxed);
  g_last_killer_damage_count.store(0, std::memory_order_relaxed);
  g_death_camera_count.store(0, std::memory_order_relaxed);
  g_freeze_resource_probe_count.store(0, std::memory_order_relaxed);
  g_death_postprocess_active_count.store(0, std::memory_order_relaxed);
  g_death_postprocess_zero_count.store(0, std::memory_order_relaxed);
  g_death_timing_probe_count.store(0, std::memory_order_relaxed);
  g_combat_reject_count.store(0, std::memory_order_relaxed);
  g_death_panel_visible_hud.store(0, std::memory_order_relaxed);
  g_death_panel_visible_since.store(0, std::memory_order_relaxed);
  g_damage_fallback_count.store(0, std::memory_order_relaxed);
  g_damage_deduped_count.store(0, std::memory_order_relaxed);
  g_native_damage_stamp.store(0, std::memory_order_relaxed);
  AcquireSRWLockExclusive(&g_pending_damage_lock);
  g_pending_damage = {};
  ReleaseSRWLockExclusive(&g_pending_damage_lock);
  g_native_last_killer_damage_stamp.store(0, std::memory_order_relaxed);
  g_death_banner_fallback_count.store(0, std::memory_order_relaxed);
  g_death_banner_deduped_count.store(0, std::memory_order_relaxed);
  AcquireSRWLockExclusive(&g_pending_death_banner_lock);
  g_pending_death_banner = {};
  g_pending_last_killer_damage = {};
  ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
  g_native_radar_sound_count.store(0, std::memory_order_relaxed);
  g_fallback_radar_sound_count.store(0, std::memory_order_relaxed);
  g_deduped_radar_sound_count.store(0, std::memory_order_relaxed);
  g_dropped_radar_sound_count.store(0, std::memory_order_relaxed);
  g_consumed_radar_sound_count.store(0, std::memory_order_relaxed);
  g_updated_radar_sound_count.store(0, std::memory_order_relaxed);
  g_repaired_radar_sound_count.store(0, std::memory_order_relaxed);
  g_missed_radar_sound_update_count.store(0, std::memory_order_relaxed);
  for (auto& stamp : g_native_radar_sound_stamp) {
    stamp.store(0, std::memory_order_relaxed);
  }
  for (auto& generation : g_native_radar_sound_generation) {
    generation.store(0, std::memory_order_relaxed);
  }
  g_radar_sound_cvar_seek_epoch = std::numeric_limits<std::uint64_t>::max();
  g_follow_bootstrap_frames = 0;
  g_follow_bootstrap_attempts = 0;
  g_follow_bootstrap_complete = false;
  g_seek_rebuild_epoch.store(0, std::memory_order_relaxed);
  g_seek_rebuild_seen_epoch = 0;
  g_combat_latch_generation.store(0, std::memory_order_relaxed);
  g_combat_latch_stamp.store(0, std::memory_order_relaxed);
  g_last_killer_damage_armed.store(false, std::memory_order_relaxed);
  g_radar_sound_transaction_active = false;
  g_radar_sound_created_hud = nullptr;
  g_radar_sound_created_snippet = nullptr;
  g_radar_sound_created_snippet_updated = false;
  g_radar_sound_max_snippet = nullptr;
  g_radar_sound_max_armed = false;

  restore_entry(g_render_graph);
  restore_entry(g_live_flash_submit);
  restore_entry(g_spec_player_update);
  restore_entry(g_hud_root_update);
  restore_entry(g_say_text2);
  restore_entry(g_radio_text);
  restore_entry(g_last_killer_damage);
  restore_entry(g_death_panel_hide);
  restore_entry(g_death_panel_show);
  restore_entry(g_death_panel_event);
  restore_entry(g_damage_message);
  restore_entry(g_death_postprocess_update);
  restore_entry(g_gameplay_event);
  restore_entry(g_money_update);
  restore_entry(g_voice_update);
  restore_entry(g_server_voice_submit);
  restore_entry(g_team_counter_update);
  restore_entry(g_player_overhead_update);
  restore_entry(g_radar_local_transform);
  restore_entry(g_radar_update);
  restore_entry(g_radar_mode_update);
  restore_entry(g_buy_zone_predicate);
  restore_entry(g_hud_team_relationship);
  restore_entry(g_voice_should_draw);
  restore_entry(g_spectator_tools);
  restore_entry(g_get_hud_alive);
  restore_entry(g_get_hud_player);
  pov::invalidate();
}

}  // namespace live_hud::native_pipeline
