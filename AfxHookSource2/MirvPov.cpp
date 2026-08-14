#include "stdafx.h"

#include "MirvPov.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovContext.h"
#include "MirvPovEventCompensation.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../shared/AfxConsole.h"

#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

extern HMODULE g_h_engine2Dll;
extern HMODULE g_H_ClientDll;
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;
extern SOURCESDK::CS2::ICvar* SOURCESDK::CS2::g_pCVar;

namespace MirvPov {
namespace {

namespace pov = MirvPovContext;

constexpr std::uint32_t kExpectedClientSize = 0x027B8000;
constexpr std::uint32_t kExpectedClientTimeDateStamp = 0x6A7CE4FB;
constexpr std::uint32_t kExpectedEngine2Size = 0x00962000;
constexpr std::uint32_t kExpectedEngine2TimeDateStamp = 0x6A7CE4F8;

// All RVAs and prologues below come from the pinned reference build:
// CS2 Steam client dated 2026-08-13.
constexpr std::uint32_t kHltvCameraRva = 0x209AC90;
constexpr std::uint32_t kHltvPrimaryTargetOffset = 0x3C;
constexpr std::uint32_t kEngine2DemoPlayerObjectRva = 0x68C268;
constexpr std::uint32_t kEngine2DemoPlayerPlayingOffset = 0x1230;
constexpr std::uint32_t kEngine2DemoPlayerSkipToTickOffset = 0x208;
constexpr std::uint32_t kPawnGetPlayerSlotRva = 0x900910;
constexpr std::uint32_t kSlotControllerRva = 0x927F60;
constexpr std::uint32_t kSlotPawnRva = 0x927FA0;
constexpr std::uint32_t kIsObserverOrDeadRva = 0x899A80;

constexpr std::uint32_t kRadarTransactionRva = 0xE28280;
constexpr std::uint32_t kRadarTransactionVtableSlotRva = 0x1B76E68;
constexpr std::uint32_t kBroadcastModePredicateRva = 0x732610;
constexpr std::uint32_t kBroadcastModeObjectPtrRva = 0x2319C58;
constexpr std::size_t kBroadcastModeVtableIndex = 0x98 / sizeof(void*);
constexpr std::uint32_t kEngineToClientObjectPtrRva = 0x23A49F0;
constexpr std::size_t kEngineToClientIsPlayingDemoVtableIndex =
    0x150 / sizeof(void*);
constexpr std::uint32_t kRadarModeRva = 0xE21D30;
constexpr std::uint32_t kRadarUpdateRva = 0xE355D0;
constexpr std::uint32_t kRadarLocalTransformRva = 0xE36100;
constexpr std::uint32_t kOverheadRva = 0xE28810;
constexpr std::uint32_t kTeamCounterRva = 0xE289F0;
constexpr std::uint32_t kVoiceRva = 0xE28A20;
constexpr std::uint32_t kServerVoiceRva = 0xAED960;
constexpr std::uint32_t kVoiceStateGetRva = 0xBA75D0;
constexpr std::uint32_t kVoiceActivityRva = 0xAE5500;
constexpr std::uint32_t kVoiceUpdateSpeakerStatusRva = 0xBB8A60;
constexpr std::uint32_t kMoneyRva = 0xE29470;
constexpr std::uint32_t kGameplayRva = 0xC81720;
constexpr std::uint32_t kDeathPostProcessRva = 0xCA59C0;
constexpr std::uint32_t kDamageMessageRva = 0xE011F0;
constexpr std::uint32_t kDamageDirectionRva = 0xDF6CA0;
constexpr std::uint32_t kDamageDirectionCallRva = 0xE012FB;
constexpr std::uint32_t kDeathPanelEventRva = 0xE04210;
constexpr std::uint32_t kLastKillerRva = 0xC216C0;
constexpr std::uint32_t kDeathPanelSummaryRva = 0xE08AD0;
constexpr std::uint32_t kDeathPanelShowRva = 0xE04E40;
constexpr std::uint32_t kDeathPanelHideRva = 0xE01BF0;
constexpr std::uint32_t kRadioRva = 0x1110360;
constexpr std::uint32_t kSayText2Rva = 0x1110CA0;
constexpr std::uint32_t kHudRootRva = 0xE0D430;
constexpr std::uint32_t kSpecPlayerRva = 0xE0C590;
constexpr std::uint32_t kRenderGraphRva = 0x11405E0;
constexpr std::uint32_t kLiveFlashRva = 0x1132230;
constexpr std::uint32_t kSpectatorToolsRva = 0xC78600;
constexpr std::uint32_t kGetHudPlayerRva = 0xC11F70;
constexpr std::uint32_t kGetHudAliveRva = 0xC12520;
constexpr std::uint32_t kHudTeamRelationshipRva = 0x899980;
constexpr std::uint32_t kBuyZonePredicateRva = 0x899440;
constexpr std::uint32_t kEntityPlayerIdRva = 0x1513EF0;
constexpr std::uint32_t kEntityAbsOriginRva = 0x219F80;
constexpr std::uint32_t kPawnPreviousHelmetOffset = 0x14EF;
constexpr std::uint32_t kPawnArmorValueOffset = 0x1CA4;
constexpr std::uint32_t kEmitHurtFeedbackRva = 0x847DB0;
constexpr std::uint32_t kDamageIndicatorVisibleRva = 0xE085B0;
constexpr std::uint32_t kFindHudElementRva = 0xDFC710;
constexpr std::uint32_t kPushNoticeRva = 0xE36B50;
constexpr std::uint32_t kLocalizationInterfaceRva = 0x25CD598;
constexpr std::uint32_t kGameEventDispatchRva = 0x998070;
constexpr std::uint32_t kGameEventDispatchVtableSlotRva = 0x1AD4A28;
constexpr std::uint32_t kRadarSoundSubmitRva = 0xE360A0;
constexpr std::uint32_t kRadarSoundCreateRva = 0xE242F0;
constexpr std::uint32_t kRadarSoundFrameUpdateRva = 0xE4A610;
constexpr std::uint32_t kRadarSoundSnippetUpdateRva = 0xE3A550;
constexpr std::uint32_t kRadarSoundEmitCallRva = 0xBA4F1A;
constexpr std::uint32_t kRadarSoundCreateCallRva = 0xE28336;
constexpr std::uint32_t kRadarSoundSnippetUpdateCallRva = 0xE4A72F;
constexpr std::uint32_t kEventFieldHashRva = 0x224F30;
constexpr std::uint32_t kFilterPlayerEntityRva = 0x7F84B0;
constexpr std::uint32_t kPlayerPawnEventVtableSlotRva = 0x1B2A0C8;
constexpr std::uint32_t kPlayerPawnFireGameEventRva = 0xC0BE40;
constexpr std::uint32_t kPlayerPawnEventListenerOffset = 0x13E0;
constexpr std::uint32_t kEventDeathUseridSeed = 0x31415920;
constexpr std::uint32_t kEventUseridSeed = 0x572DEA01;
constexpr std::uint32_t kEventAttackerSeed = 0xEDE4F213;
constexpr std::uint32_t kEventHeadshotSeed = 0x3141592E;
constexpr std::uint32_t kEventWeaponSeed = 0x3E03DAFA;
constexpr std::uint32_t kEventSilencedSeed = 0x3141592E;
constexpr std::uint32_t kEventDamageHealthSeed = 0x3141592C;
constexpr std::uint32_t kEventUseridHashOffset = 4;
constexpr std::size_t kEventUseridHashLength = 2;
constexpr std::size_t kEventPlayerSlotVtableIndex = 0x78 / sizeof(void*);
constexpr std::size_t kEventPlayerEntityVtableIndex = 0x88 / sizeof(void*);
constexpr std::uint32_t kPawnControllerOffset = 0x13D0;
constexpr std::uint32_t kPawnOriginalControllerOffset = 0x1478;
constexpr std::uint32_t kPawnInBuyZoneOffset = 0x1500;
constexpr std::uint32_t kControllerPlayerNameOffset = 0x6F4;
constexpr std::uint32_t kPawnLastPlaceNameOffset = 0x14DC;
constexpr std::uint64_t kDeathPovLatchMs = 2000;
constexpr std::uint64_t kDeathSummaryPairWindowMs = 120;
constexpr std::uint64_t kRadarSoundNativePriorityMs = 40;
constexpr std::size_t kMaxPendingRadarSounds = 64;

constexpr std::uint8_t kSlotPrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0x83, 0xF9, 0xFF};
constexpr std::uint8_t kIsObserverPrologue[] = {
    0x8B, 0x91, 0xD0, 0x13, 0x00, 0x00, 0x83, 0xFA, 0xFF};
constexpr std::uint8_t kPawnSlotPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kRadarModePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18};
constexpr std::uint8_t kRadarUpdatePrologue[] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kRadarLocalTransformPrologue[] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53};
constexpr std::uint8_t kBroadcastModePredicatePrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D,
    0x3D, 0x76, 0xBE, 0x01};
constexpr std::uint8_t kOverheadPrologue[] = {
    0x41, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
constexpr std::uint8_t kSixBytePrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kMoneyPrologue[] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kSavedRdxPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::uint8_t kDeathPanelHidePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::uint8_t kFiveArgPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kOneArgPrologue[] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kHudRootPrologue[] = {
    0x40, 0x55, 0x53, 0x41, 0x54};
constexpr std::uint8_t kSpecPlayerPrologue[] = {
    0x40, 0x53, 0x56, 0x57, 0x41, 0x54};
constexpr std::uint8_t kDamageMessagePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10};
constexpr std::uint8_t kDeathPostProcessPrologue[] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56};
constexpr std::uint8_t kShowPrologue[] = {
    0x40, 0x57, 0x41, 0x56, 0x48, 0x81,
    0xEC, 0xA8, 0x00, 0x00, 0x00};
constexpr std::uint8_t kSpectatorToolsPrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr std::uint8_t kGetHudPlayerPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC9};
constexpr std::uint8_t kRenderGraphPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr std::uint8_t kLiveFlashPrologue[] = {
    0x48, 0x89, 0x6C, 0x24, 0x10};
constexpr std::uint8_t kRelationshipPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57};
constexpr std::uint8_t kBuyZonePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
constexpr std::uint8_t kEventFieldHashPrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0x45, 0x8B};
constexpr std::uint8_t kFilterPlayerEntityPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kEntityPlayerIdPrologue[] = {
    0x48, 0x83, 0xEC, 0x08};
constexpr std::uint8_t kEmitHurtFeedbackPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
constexpr std::uint8_t kEntityAbsOriginPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kDamageIndicatorVisiblePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
constexpr std::uint8_t kFindHudElementPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kPushNoticePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};
constexpr std::uint8_t kGameEventDispatchPrologue[] = {
    0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
constexpr std::uint8_t kRadarSoundSubmitPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40};
constexpr std::uint8_t kVoiceActivityPrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0x89, 0x4C, 0x24, 0x30};
constexpr std::uint32_t kVoiceShouldDrawRva = 0xE3EDA0;
constexpr std::uint8_t kVoiceShouldDrawPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
constexpr std::uint8_t kDamageDirectionCallBytes[] = {
    0xE8, 0xA0, 0x59, 0xFF, 0xFF};
constexpr std::uint8_t kRadarSoundEmitCallBytes[] = {
    0xE8, 0x81, 0x11, 0x29, 0x00};
constexpr std::uint8_t kRadarSoundCreateCallBytes[] = {
    0xE8, 0xB5, 0xBF, 0xFF, 0xFF};
constexpr std::uint8_t kRadarSoundSnippetUpdateCallBytes[] = {
    0xE8, 0x1C, 0xFE, 0xFE, 0xFF};

#pragma pack(push, 1)
struct AbsJump {
    std::uint8_t mov_rax[2]{0x48, 0xB8};
    std::uint64_t target = 0;
    std::uint8_t jmp_rax[2]{0xFF, 0xE0};
};
#pragma pack(pop)

struct EntryHook {
    std::uint8_t* entry = nullptr;
    std::uint8_t* allocation = nullptr;
    std::uint8_t* stub = nullptr;
    void* trampoline = nullptr;
    std::uint8_t saved[16]{};
    std::size_t stolen = 0;
};

struct RelCallHook {
    std::uint8_t* call = nullptr;
    std::uint8_t* stub = nullptr;
    std::uint8_t saved[5]{};
};

using SlotFn = void*(__fastcall*)(int);
using IsObserverFn = bool(__fastcall*)(void*);
using PawnSlotFn = int*(__fastcall*)(void*, int*);
using OneArgFn = std::uint64_t(__fastcall*)(std::uintptr_t);
using OneArgVoidFn = void(__fastcall*)(std::uintptr_t);
using TwoArgFn = std::uint64_t(__fastcall*)(std::uintptr_t, std::uintptr_t);
using TwoArgVoidFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t);
using FiveArgVoidFn = void(__fastcall*)(std::uintptr_t, std::uintptr_t,
                                         std::uintptr_t, std::uintptr_t,
                                         std::uintptr_t);
using RadarTransactionFn = void(__fastcall*)(std::uintptr_t, std::uint8_t);
using HudGetterFn = void*(__fastcall*)();
using BoolNoArgFn = bool(__fastcall*)();
using BoolOneArgFn = bool(__fastcall*)(void*);
using UIntOneArgFn = unsigned int(__fastcall*)(void*);
using HudTeamRelationshipFn = bool(__fastcall*)(void*, unsigned int);
using BuyZonePredicateFn = bool(__fastcall*)(void*, bool);
using PlayerPawnEventFn = void(__fastcall*)(void*, void*);
using DamageDirectionFn = void(__fastcall*)(std::uintptr_t, const float*, void*);
using EntityPlayerIdFn = int*(__fastcall*)(void*, int*);
using EmitHurtFeedbackFn = void(__fastcall*)(void*, void*, const char*);
using EntityAbsOriginFn = const float*(__fastcall*)(void*);
using DamageIndicatorVisibleFn = void(__fastcall*)(void*, bool);
using PushNoticeFn = std::int64_t(__fastcall*)(void*, char*, unsigned,
                                               std::uint8_t*);
using FindHudElementFn = void*(__fastcall*)(const char*);
using GameEventDispatchFn = bool(__fastcall*)(void*, void*);
using RadarSoundSubmitFn = void(__fastcall*)(void*, int, float, bool);
using RadarSoundCreateFn = void*(__fastcall*)(void*, int, int, float, bool);
using RadarSoundFrameUpdateFn = void(__fastcall*)(void*);
using RadarSoundSnippetUpdateFn = void(__fastcall*)(void*, void*);
using DeathPanelSummaryFn = std::uint64_t(__fastcall*)(
    std::uintptr_t, int, int, int, int, int, int);
using VoiceStateGetFn = void*(__fastcall*)();
using VoiceActivityFn = float(__fastcall*)(int);
using VoiceUpdateSpeakerStatusFn = std::int64_t(__fastcall*)(
    void*, unsigned, int, std::uint8_t);

std::atomic<bool> g_requested{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_install_failed{false};
HMODULE g_client = nullptr;
bool g_client_initialized = false;
std::atomic<bool> g_build_mismatch_logged{false};
std::atomic<bool> g_build_cache_valid{false};
std::atomic<bool> g_build_cache_result{false};
bool g_pending_logged = false;

enum class RuntimePoint : std::size_t {
    slot_pawn,
    slot_controller,
    is_observer,
    radar_sound_submit,
    radar_sound_create,
    radar_sound_snippet_update,
    game_event_dispatch,
    radar_transaction,
    radar_mode,
    radar_update,
    radar_local_transform,
    player_overhead,
    team_counter,
    voice,
    voice_should_draw,
    server_voice,
    money,
    gameplay,
    death_postprocess,
    damage_message,
    damage_direction,
    death_panel_event,
    last_killer,
    death_panel_summary,
    death_panel_show,
    death_panel_hide,
    radio,
    say_text2,
    hud_root,
    spec_player,
    live_flash,
    render_graph,
    spectator_tools,
    player_pawn_event,
    get_hud_player,
    get_hud_alive,
    team_relationship,
    buy_zone,
    is_playing_demo,
    broadcast_mode,
    count
};

constexpr const char* kRuntimePointNames[] = {
    "slot_pawn", "slot_controller", "is_observer", "radar_sound_submit",
    "radar_sound_create", "radar_sound_snippet_update",
    "game_event_dispatch", "radar_transaction", "radar_mode",
    "radar_update", "radar_local_transform", "player_overhead",
    "team_counter", "voice", "voice_should_draw", "server_voice", "money",
    "gameplay", "death_postprocess", "damage_message", "damage_direction",
    "death_panel_event", "last_killer", "death_panel_summary",
    "death_panel_show", "death_panel_hide", "radio", "say_text2", "hud_root",
    "spec_player", "live_flash", "render_graph", "spectator_tools",
    "player_pawn_event", "get_hud_player", "get_hud_alive",
    "team_relationship", "buy_zone", "is_playing_demo", "broadcast_mode"};

std::array<std::atomic<std::uint64_t>,
           static_cast<std::size_t>(RuntimePoint::count)>
    g_runtime_point_calls{};
std::atomic<std::uint64_t> g_identity_frame_calls{0};
std::atomic<std::uint64_t> g_identity_valid_frames{0};
std::atomic<std::uint64_t> g_identity_no_target_frames{0};
std::atomic<std::uint64_t> g_identity_controller_fallbacks{0};
std::atomic<std::uint64_t> g_identity_invalid_team_frames{0};
std::atomic<int> g_last_hltv_primary_index{-1};
std::atomic<void*> g_last_logged_followed{nullptr};

void note_runtime_point(RuntimePoint point) noexcept {
    const auto index = static_cast<std::size_t>(point);
    const auto calls =
        g_runtime_point_calls[index].fetch_add(1, std::memory_order_relaxed) + 1;
    if (calls == 1) {
        advancedfx::Message("[mirv_pov] runtime hook hit=%s\n",
                            kRuntimePointNames[index]);
    }
}

EntryHook g_slot_pawn{};
EntryHook g_slot_controller{};
EntryHook g_is_observer{};

EntryHook g_radar_mode{};
EntryHook g_radar_update{};
EntryHook g_radar_local_transform{};
EntryHook g_overhead{};
EntryHook g_team_counter{};
EntryHook g_voice{};
EntryHook g_voice_should_draw{};
EntryHook g_server_voice{};
EntryHook g_money{};
EntryHook g_gameplay{};
EntryHook g_death_postprocess{};
EntryHook g_damage_message{};
EntryHook g_death_panel_event{};
EntryHook g_last_killer{};
EntryHook g_death_panel_summary{};
EntryHook g_death_panel_show{};
EntryHook g_death_panel_hide{};
EntryHook g_radio{};
EntryHook g_say_text2{};
EntryHook g_hud_root{};
EntryHook g_spec_player{};
EntryHook g_render_graph{};
EntryHook g_live_flash{};
EntryHook g_spectator_tools{};
EntryHook g_get_hud_player{};
EntryHook g_get_hud_alive{};
EntryHook g_relationship{};
EntryHook g_buy_zone{};
RelCallHook g_damage_direction_call{};
RelCallHook g_radar_sound_emit_call{};
RelCallHook g_radar_sound_create_call{};
RelCallHook g_radar_sound_snippet_update_call{};

void** g_radar_transaction_slot = nullptr;
RadarTransactionFn g_radar_transaction_original = nullptr;
void** g_is_playing_demo_slot = nullptr;
BoolOneArgFn g_is_playing_demo_original = nullptr;
void** g_broadcast_mode_slot = nullptr;
UIntOneArgFn g_broadcast_mode_original = nullptr;
DamageDirectionFn g_damage_direction_original = nullptr;
EntityPlayerIdFn g_entity_player_id_original = nullptr;
EmitHurtFeedbackFn g_emit_hurt_feedback = nullptr;
EntityAbsOriginFn g_entity_abs_origin = nullptr;
DamageIndicatorVisibleFn g_damage_indicator_visible = nullptr;
PushNoticeFn g_push_notice = nullptr;
FindHudElementFn g_find_hud_element = nullptr;
void** g_game_event_dispatch_slot = nullptr;
GameEventDispatchFn g_game_event_dispatch_original = nullptr;
RadarSoundSubmitFn g_radar_sound_submit_original = nullptr;
RadarSoundCreateFn g_radar_sound_create_original = nullptr;
RadarSoundFrameUpdateFn g_radar_sound_frame_update_original = nullptr;
RadarSoundSnippetUpdateFn g_radar_sound_snippet_update_original = nullptr;
void** g_player_pawn_event_slot = nullptr;
PlayerPawnEventFn g_player_pawn_event_original = nullptr;
int* g_tv_voice_indices = nullptr;
int* g_tv_voice_indices_high = nullptr;
int g_tv_voice_original = 0;
int g_tv_voice_original_high = 0;
bool g_tv_voice_original_valid = false;
bool g_tv_voice_original_high_valid = false;
std::uint64_t g_voice_mask_generation = 0;
std::uint64_t g_voice_mask_seek_epoch = 0;
std::uint32_t g_voice_mask_low = 0;
std::uint32_t g_voice_mask_high = 0;
int g_voice_mask_team = 0;
unsigned g_voice_mask_retry_frames = 0;
std::uint32_t g_voice_audio_adapted_low = 0;
std::uint32_t g_voice_audio_adapted_high = 0;
std::atomic<std::uint64_t> g_death_pov_latch_stamp{0};
std::atomic<std::uint64_t> g_combat_latch_generation{0};
std::atomic<std::uint64_t> g_combat_latch_stamp{0};
std::atomic<bool> g_last_killer_damage_armed{false};

struct PendingRadarSound {
    void* pawn = nullptr;
    MirvPovEventCompensation::RadarSoundSpec spec{};
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

SRWLOCK g_pending_radar_sound_lock = SRWLOCK_INIT;
std::array<PendingRadarSound, kMaxPendingRadarSounds> g_pending_radar_sounds{};
std::size_t g_pending_radar_sound_count = 0;
std::array<std::atomic<std::uint64_t>, 5> g_native_radar_sound_stamp{};
std::array<std::atomic<std::uint64_t>, 5> g_native_radar_sound_generation{};
thread_local bool g_radar_sound_transaction_active = false;
thread_local void* g_radar_sound_created_hud = nullptr;
thread_local void* g_radar_sound_created_snippet = nullptr;
thread_local bool g_radar_sound_created_snippet_updated = false;
thread_local void* g_radar_sound_max_snippet = nullptr;
thread_local bool g_radar_sound_max_armed = false;

SRWLOCK g_pending_damage_lock = SRWLOCK_INIT;
PendingDamageFeedback g_pending_damage{};
std::atomic<std::uint64_t> g_native_damage_stamp{0};

SRWLOCK g_pending_death_banner_lock = SRWLOCK_INIT;
PendingDeathBanner g_pending_death_banner{};
PendingLastKillerDamage g_pending_last_killer_damage{};

void queue_radar_sound_event(void* event);
void queue_damage_feedback_event(std::uintptr_t event);
void flush_pending_radar_sounds();
void flush_pending_damage_feedback();
void flush_pending_death_banner();
void refresh_voice_receive_mask();

std::uint8_t* module_base(HMODULE module) {
    return reinterpret_cast<std::uint8_t*>(module);
}

bool read_pe_fingerprint(HMODULE module, std::uint32_t* timestamp,
                         std::uint32_t* image_size) {
    if (!module || !timestamp || !image_size) {
        return false;
    }
    const auto* base = module_base(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return false;
    }
    *timestamp = nt->FileHeader.TimeDateStamp;
    *image_size = nt->OptionalHeader.SizeOfImage;
    return true;
}

bool supported_build(HMODULE client, HMODULE engine2) {
    if (!client || !engine2) {
        return false;
    }
    if (g_build_cache_valid.load(std::memory_order_acquire)) {
        return g_build_cache_result.load(std::memory_order_relaxed);
    }
    std::uint32_t client_timestamp = 0;
    std::uint32_t client_size = 0;
    std::uint32_t engine_timestamp = 0;
    std::uint32_t engine_size = 0;
    const bool valid =
        read_pe_fingerprint(client, &client_timestamp, &client_size) &&
        read_pe_fingerprint(engine2, &engine_timestamp, &engine_size);
    const bool matches =
        valid && client_timestamp == kExpectedClientTimeDateStamp &&
        client_size == kExpectedClientSize &&
        engine_timestamp == kExpectedEngine2TimeDateStamp &&
        engine_size == kExpectedEngine2Size;
    if (!matches && !g_build_mismatch_logged.exchange(true)) {
        advancedfx::Warning(
            "[mirv_pov] unsupported build: client ts=0x%08X size=0x%08X "
            "engine2 ts=0x%08X size=0x%08X\n",
            client_timestamp, client_size, engine_timestamp, engine_size);
    }
    g_build_cache_result.store(matches, std::memory_order_relaxed);
    g_build_cache_valid.store(true, std::memory_order_release);
    return matches;
}

bool demo_is_skipping() {
    // Match the reference's engine2 demo-player gate. During goto/seek CS2
    // tears down entity storage while the outer client frame continues to run.
    if (g_client && g_h_engine2Dll &&
        supported_build(g_client, g_h_engine2Dll)) {
        __try {
            auto* demo = module_base(g_h_engine2Dll) +
                         kEngine2DemoPlayerObjectRva;
            const bool playing =
                *reinterpret_cast<const std::uint8_t*>(
                    demo + kEngine2DemoPlayerPlayingOffset) != 0;
            std::int32_t skip_to_tick = -1;
            std::memcpy(&skip_to_tick,
                        demo + kEngine2DemoPlayerSkipToTickOffset,
                        sizeof(skip_to_tick));
            if (playing && skip_to_tick != -1) {
                return true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return true;
        }
    }
    if (!g_pEngineToClient) {
        return false;
    }
    auto* demo = g_pEngineToClient->GetDemoFile();
    return demo && demo->IsPlayingDemo() && demo->IsDemoPaused();
}

void log_message(const char* value) {
    if (advancedfx::Message) {
        advancedfx::Message("[mirv_pov] %s\n", value);
    }
}

void* alloc_near(void* near_address, std::size_t size) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    std::uintptr_t granularity = info.dwAllocationGranularity;
    if (!granularity) {
        granularity = 0x10000;
    }
    const auto center = reinterpret_cast<std::uintptr_t>(near_address);
    const auto low =
        reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
    const auto high =
        reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
    constexpr std::uintptr_t kSpan = 0x70000000ull;

    auto try_address = [&](std::uintptr_t address) -> void* {
        if (address < low || address > high - size) {
            return nullptr;
        }
        address &= ~(granularity - 1);
        return VirtualAlloc(reinterpret_cast<void*>(address), size,
                            MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
    };
    for (std::uintptr_t delta = granularity; delta < kSpan;
         delta += granularity) {
        if (void* value = try_address(center + delta)) {
            return value;
        }
        if (center > delta) {
            if (void* value = try_address(center - delta)) {
                return value;
            }
        }
    }
    return nullptr;
}

bool write_rel_jump(std::uint8_t* from, const void* to) {
    const auto rel = reinterpret_cast<const std::uint8_t*>(to) - (from + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) {
        return false;
    }
    from[0] = 0xE9;
    *reinterpret_cast<std::int32_t*>(from + 1) =
        static_cast<std::int32_t>(rel);
    return true;
}

void write_abs_jump(std::uint8_t* at, const void* to) {
    AbsJump jump{};
    jump.target = reinterpret_cast<std::uint64_t>(to);
    std::memcpy(at, &jump, sizeof(jump));
}

bool install_entry(HMODULE module, std::uint32_t rva,
                   const std::uint8_t* expected, std::size_t stolen,
                   const void* replacement, EntryHook& hook,
                   const char* tag) {
    if (!module || !expected || !replacement || stolen < 5 || stolen > 16) {
        return false;
    }
    auto* entry = module_base(module) + rva;
    if (std::memcmp(entry, expected, stolen) != 0) {
        advancedfx::Warning(
            "[mirv_pov] %s prologue mismatch rva=0x%X actual=%02X %02X %02X %02X %02X expected=%02X %02X %02X %02X %02X\n",
            tag, rva, static_cast<unsigned>(entry[0]),
            static_cast<unsigned>(entry[1]), static_cast<unsigned>(entry[2]),
            static_cast<unsigned>(entry[3]), static_cast<unsigned>(entry[4]),
            static_cast<unsigned>(expected[0]),
            static_cast<unsigned>(expected[1]),
            static_cast<unsigned>(expected[2]),
            static_cast<unsigned>(expected[3]),
            static_cast<unsigned>(expected[4]));
        return false;
    }

    auto* allocation =
        reinterpret_cast<std::uint8_t*>(alloc_near(entry, 64));
    if (!allocation) {
        advancedfx::Warning("[mirv_pov] %s near allocation failed\n", tag);
        return false;
    }
    std::memcpy(hook.saved, entry, stolen);
    std::memcpy(allocation, hook.saved, stolen);
    if (!write_rel_jump(allocation + stolen, entry + stolen)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return false;
    }
    auto* stub = allocation + 32;
    write_abs_jump(stub, replacement);

    DWORD old_protect = 0;
    if (!VirtualProtect(entry, stolen, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return false;
    }
    const bool jumped = write_rel_jump(entry, stub);
    if (jumped) {
        for (std::size_t index = 5; index < stolen; ++index) {
            entry[index] = 0x90;
        }
        FlushInstructionCache(GetCurrentProcess(), entry, stolen);
        FlushInstructionCache(GetCurrentProcess(), allocation, 64);
    }
    VirtualProtect(entry, stolen, old_protect, &old_protect);
    if (!jumped) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return false;
    }
    hook.entry = entry;
    hook.allocation = allocation;
    hook.stub = stub;
    hook.trampoline = allocation;
    hook.stolen = stolen;
    advancedfx::Message("[mirv_pov] hook installed tag=%s rva=0x%X stolen=%zu\n",
                        tag, rva, stolen);
    return true;
}

void restore_entry(EntryHook& hook) noexcept {
    if (hook.entry && hook.stolen) {
        DWORD old_protect = 0;
        if (VirtualProtect(hook.entry, hook.stolen, PAGE_EXECUTE_READWRITE,
                           &old_protect)) {
            std::memcpy(hook.entry, hook.saved, hook.stolen);
            FlushInstructionCache(GetCurrentProcess(), hook.entry, hook.stolen);
            VirtualProtect(hook.entry, hook.stolen, old_protect, &old_protect);
        }
    }
    if (hook.allocation) {
        VirtualFree(hook.allocation, 0, MEM_RELEASE);
    }
    hook = {};
}

bool install_rel_call(HMODULE module, std::uint32_t rva,
                      const std::uint8_t expected[5], const void* replacement,
                      RelCallHook& hook, const char* tag) {
    if (!module || !expected || !replacement) {
        return false;
    }
    auto* call = module_base(module) + rva;
    if (std::memcmp(call, expected, 5) != 0) {
        advancedfx::Warning("[mirv_pov] %s call bytes mismatch\n", tag);
        return false;
    }
    auto* stub = reinterpret_cast<std::uint8_t*>(alloc_near(call, 32));
    if (!stub) {
        return false;
    }
    write_abs_jump(stub, replacement);
    const auto rel = stub - (call + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) {
        VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    std::uint8_t patch[5]{0xE8, 0, 0, 0, 0};
    *reinterpret_cast<std::int32_t*>(patch + 1) =
        static_cast<std::int32_t>(rel);
    std::memcpy(hook.saved, call, 5);
    hook.call = call;
    hook.stub = stub;

    DWORD old_protect = 0;
    if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        hook = {};
        return false;
    }
    std::memcpy(call, patch, 5);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    FlushInstructionCache(GetCurrentProcess(), stub, sizeof(AbsJump));
    VirtualProtect(call, 5, old_protect, &old_protect);
    advancedfx::Message("[mirv_pov] call hook installed tag=%s rva=0x%X\n",
                        tag, rva);
    return true;
}

void restore_rel_call(RelCallHook& hook) noexcept {
    if (hook.call) {
        DWORD old_protect = 0;
        if (VirtualProtect(hook.call, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
            bool owned = hook.call[0] == 0xE8;
            if (owned) {
                const auto rel = *reinterpret_cast<std::int32_t*>(hook.call + 1);
                owned = hook.call + 5 + rel == hook.stub;
            }
            if (owned) {
                std::memcpy(hook.call, hook.saved, 5);
                FlushInstructionCache(GetCurrentProcess(), hook.call, 5);
            }
            VirtualProtect(hook.call, 5, old_protect, &old_protect);
        }
    }
    if (hook.stub) {
        VirtualFree(hook.stub, 0, MEM_RELEASE);
    }
    hook = {};
}

void* entity_from_entry_index(int index) {
    if (!g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex ||
        index <= 0) {
        return nullptr;
    }
    return g_GetEntityFromIndex(*g_pEntityList, index);
}

void* entity_from_handle(std::uint32_t handle) {
    if (!handle || handle == 0xFFFFFFFFu || handle == 0xFFFFFFFEu) {
        return nullptr;
    }
    return entity_from_entry_index(static_cast<int>(handle & 0x7FFFu));
}

void* controller_from_pawn(void* pawn) {
    if (!pawn || !g_client_initialized) {
        return nullptr;
    }
    __try {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(pawn);
        const std::uint32_t handles[] = {
            *reinterpret_cast<const std::uint32_t*>(
                bytes + kPawnOriginalControllerOffset),
            *reinterpret_cast<const std::uint32_t*>(
                bytes + kPawnControllerOffset)};
        for (const auto handle : handles) {
            if (void* controller = entity_from_handle(handle)) {
                return controller;
            }
        }
        CEntityInstance* instance = reinterpret_cast<CEntityInstance*>(pawn);
        const auto handle = instance->GetPlayerControllerHandle();
        return entity_from_handle(static_cast<std::uint32_t>(handle.ToInt()));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* pawn_from_controller(void* controller) {
    if (!controller || !g_client_initialized) {
        return nullptr;
    }
    __try {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(controller);
        // CCSPlayerController has separate active and observer pawn handles.
        // HLTV follows the former in normal playback, but the base controller
        // handle is the stable fallback during a demo rebind.
        const std::uint32_t handles[] = {0x914, 0x6BC, 0x918};
        for (const auto offset : handles) {
            if (void* pawn = entity_from_handle(
                    *reinterpret_cast<const std::uint32_t*>(bytes + offset))) {
                return pawn;
            }
        }
        CEntityInstance* instance =
            reinterpret_cast<CEntityInstance*>(controller);
        const auto handle = instance->GetPlayerPawnHandle();
        return entity_from_handle(static_cast<std::uint32_t>(handle.ToInt()));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int entity_team(void* entity) {
    if (!entity) {
        return 0;
    }
    __try {
        return reinterpret_cast<CEntityInstance*>(entity)->GetTeam();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* hltv_primary_entity() {
    if (!g_client) {
        return nullptr;
    }
    __try {
        auto* camera = module_base(g_client) + kHltvCameraRva;
        const int index =
            *reinterpret_cast<const std::int32_t*>(
                camera + kHltvPrimaryTargetOffset);
        g_last_hltv_primary_index.store(index, std::memory_order_relaxed);
        return entity_from_entry_index(index);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* followed_pawn() {
    void* entity = hltv_primary_entity();
    if (entity) {
        __try {
            CEntityInstance* instance = reinterpret_cast<CEntityInstance*>(entity);
            if (instance->IsPlayerController()) {
                if (void* pawn = pawn_from_controller(entity)) {
                    return pawn;
                }
            }
            if (instance->IsPlayerPawn()) {
                return entity;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // HLTV playback can leave the primary target at -1 while the observer
    // controller still carries the active or observer pawn handle. The
    // reference resolves this fallback before declaring identity unavailable.
    const auto original =
        reinterpret_cast<SlotFn>(g_slot_controller.trampoline);
    if (original) {
        for (const int slot : {0, -1}) {
            if (void* controller = original(slot)) {
                if (void* pawn = pawn_from_controller(controller)) {
                    g_identity_controller_fallbacks.fetch_add(
                        1, std::memory_order_relaxed);
                    return pawn;
                }
            }
        }
    }
    return nullptr;
}

bool same_player(void* left, void* right) {
    if (!left || !right) {
        return false;
    }
    if (left == right) {
        return true;
    }
    void* left_controller = controller_from_pawn(left);
    void* right_controller = controller_from_pawn(right);
    if (left_controller && left_controller == right_controller) {
        return true;
    }
    __try {
        const auto* left_bytes = reinterpret_cast<const std::uint8_t*>(left);
        const auto* right_bytes = reinterpret_cast<const std::uint8_t*>(right);
        const auto left_controller_handle =
            *reinterpret_cast<const std::uint32_t*>(left_bytes + kPawnControllerOffset);
        const auto right_controller_handle =
            *reinterpret_cast<const std::uint32_t*>(right_bytes + kPawnControllerOffset);
        if (left_controller_handle && left_controller_handle != 0xFFFFFFFFu &&
            left_controller_handle == right_controller_handle) {
            return true;
        }
        const auto left_original_handle =
            *reinterpret_cast<const std::uint32_t*>(left_bytes +
                                                    kPawnOriginalControllerOffset);
        const auto right_original_handle =
            *reinterpret_cast<const std::uint32_t*>(right_bytes +
                                                    kPawnOriginalControllerOffset);
        return left_original_handle && left_original_handle != 0xFFFFFFFFu &&
               left_original_handle == right_original_handle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* event_name(void* event) {
    if (!event) {
        return nullptr;
    }
    __try {
        auto** vtable = *reinterpret_cast<void***>(event);
        if (!vtable || !vtable[1]) {
            return nullptr;
        }
        using EventNameFn = const char*(__fastcall*)(void*);
        return reinterpret_cast<EventNameFn>(vtable[1])(event);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const char* event_string_field(void* event, const char* field,
                               const char* hash_text, int hash_length,
                               std::uint32_t seed) {
    if (!g_client || !event || !field || !hash_text || hash_length <= 0) {
        return nullptr;
    }
    __try {
        using HashFn = int(__fastcall*)(const char*, int, std::uint32_t);
        using GetStringFn = const char*(__fastcall*)(void*, void*, const char*);
        auto** vtable = *reinterpret_cast<void***>(event);
        if (!vtable || !vtable[0x50 / sizeof(void*)]) {
            return nullptr;
        }
        auto hash = reinterpret_cast<HashFn>(
            module_base(g_client) + kEventFieldHashRva);
        auto get_string = reinterpret_cast<GetStringFn>(
            vtable[0x50 / sizeof(void*)]);
        alignas(16) std::uint8_t key[24]{};
        *reinterpret_cast<int*>(key) = hash(hash_text, hash_length, seed);
        *reinterpret_cast<int*>(key + 4) = -1;
        *reinterpret_cast<const char**>(key + 8) = field;
        const char* value = get_string(event, key, "");
        return value && value[0] ? value : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int event_int_field(void* event, const char* field, int length,
                    std::uint32_t seed) {
    if (!g_client || !event || !field || length <= 0) {
        return 0;
    }
    __try {
        using HashFn = int(__fastcall*)(const char*, int, std::uint32_t);
        using GetIntFn = std::int64_t(__fastcall*)(void*, void*);
        auto** vtable = *reinterpret_cast<void***>(event);
        if (!vtable || !vtable[0x38 / sizeof(void*)]) {
            return 0;
        }
        auto hash = reinterpret_cast<HashFn>(
            module_base(g_client) + kEventFieldHashRva);
        auto get_int = reinterpret_cast<GetIntFn>(
            vtable[0x38 / sizeof(void*)]);
        alignas(16) std::uint8_t key[24]{};
        *reinterpret_cast<int*>(key) = hash(field, length, seed);
        *reinterpret_cast<int*>(key + 4) = -1;
        *reinterpret_cast<const char**>(key + 8) = field;
        return static_cast<int>(get_int(event, key));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* event_player_field(void* event, const char* field, const char* hash_text,
                         int hash_length, std::uint32_t seed) {
    if (!g_client || !event || !field || !hash_text || hash_length <= 0) {
        return nullptr;
    }
    __try {
        using HashFn = int(__fastcall*)(const char*, int, std::uint32_t);
        using GetEntityFn = void*(__fastcall*)(void*, void*);
        using FilterFn = void*(__fastcall*)(void*);
        auto hash = reinterpret_cast<HashFn>(
            module_base(g_client) + kEventFieldHashRva);
        auto** vtable = *reinterpret_cast<void***>(event);
        if (!vtable || !vtable[kEventPlayerEntityVtableIndex]) {
            return nullptr;
        }
        auto get_entity = reinterpret_cast<GetEntityFn>(
            vtable[kEventPlayerEntityVtableIndex]);
        auto filter = reinterpret_cast<FilterFn>(
            module_base(g_client) + kFilterPlayerEntityRva);
        if (std::memcmp(module_base(g_client) + kFilterPlayerEntityRva,
                        kFilterPlayerEntityPrologue,
                        sizeof(kFilterPlayerEntityPrologue)) != 0) {
            return nullptr;
        }
        alignas(16) std::uint8_t key[24]{};
        *reinterpret_cast<int*>(key) = hash(hash_text, hash_length, seed);
        *reinterpret_cast<int*>(key + 4) = -1;
        *reinterpret_cast<const char**>(key + 8) = field;
        void* entity = get_entity(event, key);
        return entity && filter ? filter(entity) : entity;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool event_player_slot_matches_pawn(void* event, void* pawn) {
    if (!event || !pawn || !g_client) {
        return false;
    }
    __try {
        auto* get_slot = reinterpret_cast<PawnSlotFn>(
            module_base(g_client) + kPawnGetPlayerSlotRva);
        int pawn_slot = -1;
        get_slot(pawn, &pawn_slot);
        if (pawn_slot < 0 || pawn_slot >= 64) {
            return false;
        }
        auto** vtable = *reinterpret_cast<void***>(event);
        if (!vtable || !vtable[kEventPlayerSlotVtableIndex]) {
            return false;
        }
        using GetEventSlotFn = void(__fastcall*)(void*, int*, void*);
        auto get_event_slot = reinterpret_cast<GetEventSlotFn>(
            vtable[kEventPlayerSlotVtableIndex]);
        using HashFn = int(__fastcall*)(const char*, int, std::uint32_t);
        auto hash = reinterpret_cast<HashFn>(
            module_base(g_client) + kEventFieldHashRva);
        static constexpr char kUserid[] = "userid";
        alignas(16) std::uint8_t key[24]{};
        *reinterpret_cast<int*>(key) =
            hash(kUserid + kEventUseridHashOffset,
                 static_cast<int>(kEventUseridHashLength), kEventUseridSeed);
        *reinterpret_cast<int*>(key + 4) = -1;
        *reinterpret_cast<const char**>(key + 8) = kUserid;
        int event_slot = -1;
        get_event_slot(event, &event_slot, key);
        return event_slot == pawn_slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool event_matches_followed_victim(void* event, void* pawn) {
    if (!event || !pawn || !g_client) {
        return false;
    }
    const char* name = event_name(event);
    if (!name || std::strcmp(name, "player_death") != 0) {
        return false;
    }
    const auto event_pawn =
        event_player_field(event, "userid", "userid", 6,
                           kEventDeathUseridSeed);
    return event_pawn == pawn;
}

bool damage_message_targets_follow(std::uintptr_t message,
                                   const pov::Snapshot& followed) {
    if (!message || !followed.pawn || !followed.controller ||
        !g_entity_player_id_original) {
        return false;
    }
    __try {
        const int message_player_id =
            *reinterpret_cast<const int*>(message + 84);
        if (message_player_id < 0) {
            return true;
        }
        int followed_player_id = -1;
        g_entity_player_id_original(followed.pawn, &followed_player_id);
        return followed_player_id < 0 ||
               followed_player_id == message_player_id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool pawn_is_dead(void* pawn) {
    if (!pawn) {
        return false;
    }
    __try {
        return *reinterpret_cast<const std::int32_t*>(
                   reinterpret_cast<const std::uint8_t*>(pawn) + 0x13FC) == 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool event_field_truthy(void* event, const char* field, int length,
                        std::uint32_t seed) {
    if (!g_client || !event || !field || length <= 0) {
        return false;
    }
    __try {
        using HashFn = int(__fastcall*)(const char*, int, std::uint32_t);
        using GetIntFn = std::int64_t(__fastcall*)(void*, void*);
        auto hash = reinterpret_cast<HashFn>(
            module_base(g_client) + kEventFieldHashRva);
        auto** vtable = *reinterpret_cast<void***>(event);
        if (!vtable) {
            return false;
        }
        alignas(16) std::uint8_t key[24]{};
        *reinterpret_cast<int*>(key) = hash(field, length, seed);
        *reinterpret_cast<int*>(key + 4) = -1;
        *reinterpret_cast<const char**>(key + 8) = field;
        // player_death's native notice path reads headshot through vtable +0x38.
        auto get_int = reinterpret_cast<GetIntFn>(vtable[0x38 / sizeof(void*)]);
        return get_int ? get_int(event, key) != 0 : false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int voice_speaker_slot(std::uintptr_t message) noexcept {
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
        return client_index >= 1 && client_index <= 64 ? client_index - 1
                                                        : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool voice_slot_allowed(int slot) noexcept {
    if (slot < 0 || slot >= 64 || g_voice_mask_team == 0) {
        return false;
    }
    const auto mask = slot < 32 ? g_voice_mask_low : g_voice_mask_high;
    return (mask & (1u << (slot & 31))) != 0;
}

void refresh_voice_receive_mask() {
    if (!SOURCESDK::CS2::g_pCVar || !g_client) {
        return;
    }

    __try {
        if (!g_tv_voice_indices) {
            auto low_handle =
                SOURCESDK::CS2::g_pCVar->FindConVar(
                    "tv_listen_voice_indices", false);
            auto high_handle =
                SOURCESDK::CS2::g_pCVar->FindConVar(
                    "tv_listen_voice_indices_h", false);
            auto* low = low_handle.IsValid()
                            ? SOURCESDK::CS2::g_pCVar->GetCvar(low_handle.Get())
                            : nullptr;
            auto* high = high_handle.IsValid()
                             ? SOURCESDK::CS2::g_pCVar->GetCvar(high_handle.Get())
                             : nullptr;
            if (!low) {
                return;
            }
            g_tv_voice_indices = &low->m_Value.m_i32Value;
            g_tv_voice_indices_high = high ? &high->m_Value.m_i32Value
                                            : nullptr;
            g_tv_voice_original = *g_tv_voice_indices;
            g_tv_voice_original_valid = true;
            if (g_tv_voice_indices_high) {
                g_tv_voice_original_high = *g_tv_voice_indices_high;
                g_tv_voice_original_high_valid = true;
            }
        }

        const auto followed = pov::snapshot();
        const bool valid_team = followed.team == 2 || followed.team == 3;
        if (!valid_team) {
            if (g_voice_mask_team != 0 || g_voice_mask_low != 0 ||
                g_voice_mask_high != 0) {
                InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                        g_tv_voice_indices), 0);
                if (g_tv_voice_indices_high) {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                            g_tv_voice_indices_high), 0);
                }
            }
            g_voice_mask_low = 0;
            g_voice_mask_high = 0;
            g_voice_mask_team = 0;
            g_voice_mask_generation = followed.generation;
            return;
        }

        if (followed.generation == g_voice_mask_generation &&
            followed.team == g_voice_mask_team &&
            ++g_voice_mask_retry_frames < 60) {
            return;
        }
        g_voice_mask_retry_frames = 0;

        std::uint32_t low_mask = 0;
        std::uint32_t high_mask = 0;
        const int highest = GetHighestEntityIndex();
        for (int index = 1; index <= highest; ++index) {
            void* controller = entity_from_entry_index(index);
            if (!controller) {
                continue;
            }
            CEntityInstance* instance =
                reinterpret_cast<CEntityInstance*>(controller);
            if (!instance->IsPlayerController()) {
                continue;
            }
            void* pawn = pawn_from_controller(controller);
            if (!pawn || entity_team(pawn) != followed.team) {
                continue;
            }
            int slot = -1;
            auto get_slot = reinterpret_cast<PawnSlotFn>(
                module_base(g_client) + kPawnGetPlayerSlotRva);
            if (!get_slot) {
                continue;
            }
            get_slot(pawn, &slot);
            if (slot < 0 || slot >= 64) {
                continue;
            }
            if (slot < 32) {
                low_mask |= 1u << slot;
            } else {
                high_mask |= 1u << (slot - 32);
            }
        }
        if (followed.slot >= 0 && followed.slot < 64) {
            if (followed.slot < 32) {
                low_mask |= 1u << followed.slot;
            } else {
                high_mask |= 1u << (followed.slot - 32);
            }
        }

        InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                g_tv_voice_indices),
                            static_cast<LONG>(low_mask));
        if (g_tv_voice_indices_high) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                    g_tv_voice_indices_high),
                                static_cast<LONG>(high_mask));
        }
        g_voice_mask_low = low_mask;
        g_voice_mask_high = high_mask;
        g_voice_mask_team = followed.team;
        g_voice_mask_generation = followed.generation;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tv_voice_indices = nullptr;
        g_tv_voice_indices_high = nullptr;
        g_tv_voice_original_valid = false;
        g_tv_voice_original_high_valid = false;
    }
}

void restore_voice_receive_mask() noexcept {
    __try {
        if (g_tv_voice_indices && g_tv_voice_original_valid) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                    g_tv_voice_indices),
                                static_cast<LONG>(g_tv_voice_original));
        }
        if (g_tv_voice_indices_high && g_tv_voice_original_high_valid) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(
                                    g_tv_voice_indices_high),
                                static_cast<LONG>(g_tv_voice_original_high));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_tv_voice_indices = nullptr;
    g_tv_voice_indices_high = nullptr;
    g_tv_voice_original_valid = false;
    g_tv_voice_original_high_valid = false;
    g_voice_mask_generation = 0;
    g_voice_mask_low = 0;
    g_voice_mask_high = 0;
    g_voice_mask_team = 0;
    g_voice_mask_retry_frames = 0;
    g_voice_audio_adapted_low = 0;
    g_voice_audio_adapted_high = 0;
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
        auto* base = module_base(g_client);
        const auto get_state = reinterpret_cast<VoiceStateGetFn>(
            base + kVoiceStateGetRva);
        const auto get_activity = reinterpret_cast<VoiceActivityFn>(
            base + kVoiceActivityRva);
        const auto update_speaker = reinterpret_cast<VoiceUpdateSpeakerStatusFn>(
            base + kVoiceUpdateSpeakerStatusRva);
        auto* state = reinterpret_cast<std::uint8_t*>(get_state());
        if (!state) {
            return;
        }

        for (unsigned half = 0; half < 2; ++half) {
            const unsigned first_slot = half == 0 ? 0u : 32u;
            const std::uint32_t team_mask = half == 0 ? team_low : team_high;
            std::uint32_t& adapted_mask = half == 0
                                               ? g_voice_audio_adapted_low
                                               : g_voice_audio_adapted_high;
            const unsigned native_offset = half == 0 ? 148u : 152u;
            const std::uint32_t native_bits =
                *reinterpret_cast<const std::uint32_t*>(state + native_offset);

            for (unsigned bit_index = 0; bit_index < 32; ++bit_index) {
                const std::uint32_t bit = 1u << bit_index;
                const unsigned slot = first_slot + bit_index;
                const bool selected = (team_mask & bit) != 0;
                const float activity =
                    selected ? get_activity(static_cast<int>(slot)) : 0.0f;
                const bool talking = selected && activity > 0.0f;
                const bool adapted = (adapted_mask & bit) != 0;
                const bool native_speaking = (native_bits & bit) != 0;

                if (!selected && native_speaking) {
                    update_speaker(state, slot, -1, 0);
                    adapted_mask &= ~bit;
                    continue;
                }
                if ((!talking && !adapted) ||
                    (talking && adapted && native_speaking)) {
                    continue;
                }

                update_speaker(state, slot, -1, talking ? 1 : 0);
                if (talking) {
                    adapted_mask |= bit;
                } else {
                    adapted_mask &= ~bit;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_voice_audio_adapted_low = 0;
        g_voice_audio_adapted_high = 0;
    }
}

const char* localized_token(const char* token) {
    if (!g_client || !token || !token[0]) {
        return nullptr;
    }
    __try {
        void* localize = *reinterpret_cast<void**>(
            module_base(g_client) + kLocalizationInterfaceRva);
        auto** vtable = localize ? *reinterpret_cast<void***>(localize) : nullptr;
        if (!vtable || !vtable[120 / sizeof(void*)]) {
            return nullptr;
        }
        using LocalizeFn = const char*(__fastcall*)(void*, const char*);
        const char* value = reinterpret_cast<LocalizeFn>(
            vtable[120 / sizeof(void*)])(localize, token);
        return value && value[0] ? value : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

const char* player_name_from_pawn(void* pawn) {
    void* controller = controller_from_pawn(pawn);
    if (!controller) {
        return nullptr;
    }
    __try {
        const char* name = reinterpret_cast<const char*>(
            reinterpret_cast<const std::uint8_t*>(controller) +
            kControllerPlayerNameOffset);
        for (std::size_t index = 0; index < 128; ++index) {
            if (name[index] == '\0') {
                return index == 0 ? nullptr : name;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

const char* localized_place_name(void* pawn, char* key,
                                 std::size_t key_size) {
    if (!pawn || !key || key_size < 3) {
        return nullptr;
    }
    __try {
        const char* place = reinterpret_cast<const char*>(
            reinterpret_cast<const std::uint8_t*>(pawn) +
            kPawnLastPlaceNameOffset);
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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool push_native_notice(const char* message, unsigned slot = 0xFFFFFFFFu) {
    if (!message || !message[0] || !g_find_hud_element || !g_push_notice) {
        return false;
    }
    pov::Scope scope(pov::Domain::communications);
    void* voice_element = g_find_hud_element("CCSGO_HudVoiceStatus");
    if (!voice_element) {
        return false;
    }
    void* voice_hud = reinterpret_cast<std::uint8_t*>(voice_element) - 0x20;
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
    if (!event || demo_is_skipping()) {
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
            event, "attacker", "cker", 4, kEventAttackerSeed);
        void* victim = event_player_field(
            event, "userid", "userid", 6, kEventDeathUseridSeed);
        if (attacker != followed.pawn || !victim || victim == followed.pawn ||
            entity_team(victim) == followed.team) {
            return;
        }
        const char* weapon = event_string_field(
            event, "weapon", "on", 2, kEventWeaponSeed);
        const int reward = MirvPovEventCompensation::kill_cash_reward(
            weapon ? weapon : "");
        char amount[16]{};
        std::snprintf(amount, sizeof(amount), "%d", reward);
        char message[256]{};
        const char* localized = localized_token(
            "#Player_Cash_Award_Killed_Enemy_Generic");
        if (!substitute_first_parameter(localized, amount, message,
                                        sizeof(message))) {
            std::snprintf(message, sizeof(message), "\x01+$%d\x01", reward);
        }
        push_native_notice(message);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool is_zh_cn_radio_locale() {
    static constexpr char kChineseFlash[] =
        "\xE9\x97\xAA\xE5\x85\x89\xE9\x9C\x87\xE6\x92\BC\xE5\xBC\xB9";
    const char* flash = localized_token("#SFUI_TitlesTXT_Flashbang");
    return flash && std::strcmp(flash, kChineseFlash) == 0;
}

const char* grenade_notice_text(MirvPovEventCompensation::GrenadeKind kind,
                                char* fallback, std::size_t fallback_size) {
    if (is_zh_cn_radio_locale()) {
        switch (kind) {
        case MirvPovEventCompensation::GrenadeKind::flashbang:
            return "\x0B\xE5\xB0\x8F\xE5\xBF\x83\xE9\x97\xAA\xE5\x85\x89\xE5\xBC\xB9\xEF\xBC\x81";
        case MirvPovEventCompensation::GrenadeKind::smoke:
            return "\x05\xE7\x83\x9F\xE9\x9B\xBE\xE5\xBC\xB9\xEF\xBC\x81";
        case MirvPovEventCompensation::GrenadeKind::high_explosive:
            return "\x0F\xE9\xAB\x98\xE7\x88\x86\xE6\x89\x8B\xE9\x9B\xB7\xEF\xBC\x81";
        case MirvPovEventCompensation::GrenadeKind::incendiary:
            return "\x10\xE7\x87\x83\xE7\x83\xA7\xE5\xBC\xB9\xEF\xBC\x81";
        case MirvPovEventCompensation::GrenadeKind::decoy:
            return "\x08\xE8\xAF\xB1\xE9\xA5\xB5\xE5\xBC\xB9\xEF\xBC\x81";
        case MirvPovEventCompensation::GrenadeKind::none:
            return nullptr;
        }
    }
    const char* localized = localized_token(
        MirvPovEventCompensation::grenade_localization_token(kind));
    if (!localized) {
        return nullptr;
    }
    if (kind == MirvPovEventCompensation::GrenadeKind::flashbang && fallback &&
        fallback_size > 4) {
        std::snprintf(fallback, fallback_size, "\x0B%s!", localized);
        return fallback;
    }
    return localized;
}

void adapt_grenade_throw_notice(void* listener, void* event) {
    if (!listener || !event || demo_is_skipping()) {
        return;
    }
    __try {
        const char* name = event_name(event);
        if (!name || std::strcmp(name, "weapon_fire") != 0) {
            return;
        }
        auto* pawn = reinterpret_cast<std::uint8_t*>(listener) -
                     kPlayerPawnEventListenerOffset;
        if (!event_player_slot_matches_pawn(event, pawn)) {
            return;
        }
        const char* weapon = event_string_field(
            event, "weapon", "on", 2, kEventWeaponSeed);
        const auto kind = MirvPovEventCompensation::grenade_kind(
            weapon ? weapon : "");
        if (kind == MirvPovEventCompensation::GrenadeKind::none) {
            return;
        }
        const auto followed = pov::snapshot();
        const int thrower_team = entity_team(pawn);
        if (!followed.pawn || (followed.team != 2 && followed.team != 3) ||
            thrower_team != followed.team) {
            return;
        }
        const char* player_name = player_name_from_pawn(pawn);
        if (!player_name) {
            player_name = "\xE9\x98\x9F\xE5\x8F\x8B";
        }
        const char* team_prefix = localized_token(
            thrower_team == 3 ? "#game_radio_team_prefix_3"
                              : "#game_radio_team_prefix_2");
        if (!team_prefix) {
            team_prefix = thrower_team == 3 ? "[CT] " : "[T] ";
        }
        char phrase_buffer[192]{};
        const char* phrase = grenade_notice_text(
            kind, phrase_buffer, sizeof(phrase_buffer));
        if (!phrase) {
            return;
        }
        char place_key[32]{};
        const char* place = localized_place_name(
            pawn, place_key, sizeof(place_key));
        char message[384]{};
        if (place && place[0]) {
            std::snprintf(message, sizeof(message),
                          " %s\x03%s\x04\xEF\xB9\xAB%s\x01: %s",
                          team_prefix, player_name, place, phrase);
        } else {
            std::snprintf(message, sizeof(message), " %s\x03%s\x01: %s",
                          team_prefix, player_name, phrase);
        }
        unsigned slot = 0xFFFFFFFFu;
        int pawn_slot = -1;
        auto get_slot = reinterpret_cast<PawnSlotFn>(
            module_base(g_client) + kPawnGetPlayerSlotRva);
        get_slot(pawn, &pawn_slot);
        if (pawn_slot >= 0 && pawn_slot < 64) {
            slot = static_cast<unsigned>(pawn_slot);
        }
        push_native_notice(message, slot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::size_t radar_sound_kind_index(
    MirvPovEventCompensation::RadarSoundKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < 5 ? index : 0;
}

MirvPovEventCompensation::RadarSoundKind native_radar_sound_kind(
    int radius, bool is_footstep) noexcept {
    using Kind = MirvPovEventCompensation::RadarSoundKind;
    if (is_footstep) {
        return Kind::footstep;
    }
    if (radius >= 580 && radius <= 620) {
        return Kind::scope;
    }
    if (radius > 0 && radius < 800) {
        return Kind::utility;
    }
    return Kind::weapon;
}

void clear_pending_radar_sounds() noexcept {
    AcquireSRWLockExclusive(&g_pending_radar_sound_lock);
    g_pending_radar_sound_count = 0;
    ReleaseSRWLockExclusive(&g_pending_radar_sound_lock);
}

void queue_radar_sound_event(void* event) {
    if (!event || demo_is_skipping()) {
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
        void* actor = event_player_field(
            event, "userid", "id", 2, kEventUseridSeed);
        if (actor != followed.pawn) {
            return;
        }
        const char* weapon = nullptr;
        if (std::strcmp(name, "weapon_fire") == 0) {
            weapon = event_string_field(
                event, "weapon", "on", 2, kEventWeaponSeed);
        }
        const bool silenced = std::strcmp(name, "weapon_fire") == 0 &&
                              event_field_truthy(
                                  event, "silenced", 8, kEventSilencedSeed);
        const auto spec = MirvPovEventCompensation::radar_sound_from_event(
            name, weapon ? weapon : "", silenced);
        if (!spec) {
            return;
        }

        PendingRadarSound pending{};
        pending.pawn = actor;
        pending.spec = spec;
        pending.generation = followed.generation;
        pending.stamp = GetTickCount64();
        std::snprintf(pending.event_name, sizeof(pending.event_name), "%s",
                      name);
        std::snprintf(pending.weapon, sizeof(pending.weapon), "%s",
                      weapon ? weapon : "");
        AcquireSRWLockExclusive(&g_pending_radar_sound_lock);
        if (g_pending_radar_sound_count == kMaxPendingRadarSounds) {
            std::memmove(g_pending_radar_sounds.data(),
                         g_pending_radar_sounds.data() + 1,
                         sizeof(PendingRadarSound) *
                             (kMaxPendingRadarSounds - 1));
            --g_pending_radar_sound_count;
        }
        g_pending_radar_sounds[g_pending_radar_sound_count++] = pending;
        ReleaseSRWLockExclusive(&g_pending_radar_sound_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void queue_damage_feedback_event(std::uintptr_t event_address) {
    if (!event_address || demo_is_skipping()) {
        return;
    }
    __try {
        void* event = reinterpret_cast<void*>(event_address);
        const char* name = event_name(event);
        if (!name || std::strcmp(name, "player_hurt") != 0) {
            return;
        }
        const auto followed = pov::snapshot();
        void* victim = event_player_field(
            event, "userid", "userid", 6, kEventDeathUseridSeed);
        if (!followed.pawn || !followed.controller ||
            victim != followed.pawn) {
            return;
        }
        void* attacker = event_player_field(
            event, "attacker", "cker", 4, kEventAttackerSeed);
        if (!attacker || attacker == victim) {
            return;
        }
        PendingDamageFeedback pending{};
        pending.victim = victim;
        pending.attacker = attacker;
        pending.generation = followed.generation;
        pending.stamp = GetTickCount64();
        pending.damage = event_int_field(
            event, "dmg_health", 10, kEventDamageHealthSeed);
        AcquireSRWLockExclusive(&g_pending_damage_lock);
        g_pending_damage = pending;
        ReleaseSRWLockExclusive(&g_pending_damage_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool read_damage_indicator_strengths(std::uintptr_t hud,
                                     std::array<float, 4>& values) noexcept {
    if (!hud) {
        return false;
    }
    __try {
        const auto* source = reinterpret_cast<const float*>(hud + 0x60);
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] = source[index];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool build_damage_feedback_input(const PendingDamageFeedback& pending,
                                 std::uintptr_t* hud_out,
                                 std::uint8_t message[160], int* id_out,
                                 float source_out[3]) {
    if (!hud_out || !message || !id_out || !source_out ||
        !g_entity_abs_origin || !g_entity_player_id_original ||
        !g_find_hud_element) {
        return false;
    }
    __try {
        const float* source = g_entity_abs_origin(pending.attacker);
        if (!source) {
            return false;
        }
        source_out[0] = source[0];
        source_out[1] = source[1];
        source_out[2] = source[2];
        auto* vector_message = message + 96;
        *reinterpret_cast<float*>(vector_message + 0x18) = source[0];
        *reinterpret_cast<float*>(vector_message + 0x1C) = source[1];
        *reinterpret_cast<float*>(vector_message + 0x20) = source[2];
        *reinterpret_cast<const void**>(message + 72) = vector_message;
        *reinterpret_cast<int*>(message + 80) = pending.damage > 0
                                                    ? pending.damage
                                                    : 1;
        g_entity_player_id_original(pending.victim, id_out);
        *reinterpret_cast<int*>(message + 84) = *id_out;
        void* element = g_find_hud_element("CCSGO_HudDamageIndicator");
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
        return false;
    }
}

void flush_pending_damage_feedback() {
    PendingDamageFeedback pending{};
    AcquireSRWLockExclusive(&g_pending_damage_lock);
    const auto now = GetTickCount64();
    if (g_pending_damage.victim &&
        now >= g_pending_damage.stamp &&
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
    const auto native_stamp = g_native_damage_stamp.load(
        std::memory_order_acquire);
    if (native_stamp >= pending.stamp && native_stamp - pending.stamp <= 100) {
        return;
    }

    alignas(16) std::uint8_t message[160]{};
    std::uintptr_t hud = 0;
    int player_id = -1;
    float source[3]{};
    if (!build_damage_feedback_input(pending, &hud, message, &player_id,
                                      source)) {
        return;
    }
    const auto original = reinterpret_cast<TwoArgFn>(g_damage_message.trampoline);
    if (!original) {
        return;
    }
    std::array<float, 4> before{};
    std::array<float, 4> after{};
    const bool before_valid = read_damage_indicator_strengths(hud, before);
    pov::Scope scope(pov::Domain::combat_feedback);
    if (g_damage_indicator_visible) {
        g_damage_indicator_visible(reinterpret_cast<void*>(hud + 0x20), true);
    }
    original(hud, reinterpret_cast<std::uintptr_t>(message));
    bool after_valid = read_damage_indicator_strengths(hud, after);
    const bool produced = after_valid &&
                          (after[0] > 0.001f || after[1] > 0.001f ||
                           after[2] > 0.001f || after[3] > 0.001f);
    if (!produced) {
        after_valid = invoke_damage_direction_repair(hud, source,
                                                      pending.victim) &&
                      read_damage_indicator_strengths(hud, after);
    }
    (void)player_id;
    (void)before_valid;
    (void)after_valid;
    g_native_damage_stamp.store(now, std::memory_order_release);
}

bool capture_last_killer_damage(std::uintptr_t message,
                                const pov::Snapshot& followed,
                                std::uint64_t stamp,
                                PendingLastKillerDamage* out) {
    if (!message || !out || !followed.pawn || !followed.controller ||
        !pawn_is_dead(followed.pawn)) {
        return false;
    }
    __try {
        PendingLastKillerDamage captured{};
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
    if (!banner.hud) {
        return;
    }
    const auto original = reinterpret_cast<DeathPanelSummaryFn>(
        g_death_panel_summary.trampoline);
    if (!original) {
        return;
    }
    const std::array<int, 6> zero{};
    const auto& values = summary && summary->valid ? summary->values : zero;
    pov::Scope scope(pov::Domain::combat_feedback);
    original(banner.hud, values[0], values[1], values[2], values[3], values[4],
             values[5]);
}

void flush_pending_death_banner() {
    PendingDeathBanner banner{};
    PendingLastKillerDamage summary{};
    MirvPovEventCompensation::DeathBannerResolution resolution =
        MirvPovEventCompensation::DeathBannerResolution::wait;
    const auto now = GetTickCount64();

    AcquireSRWLockExclusive(&g_pending_death_banner_lock);
    if (g_pending_death_banner.hud) {
        const bool same_generation =
            g_pending_last_killer_damage.valid &&
            g_pending_last_killer_damage.generation ==
                g_pending_death_banner.generation;
        const auto summary_stamp =
            same_generation ? g_pending_last_killer_damage.stamp : 0;
        resolution = MirvPovEventCompensation::resolve_death_banner(
            g_pending_death_banner.stamp, now, summary_stamp,
            kDeathSummaryPairWindowMs);
        if (resolution !=
            MirvPovEventCompensation::DeathBannerResolution::wait) {
            banner = g_pending_death_banner;
            if (same_generation) {
                summary = g_pending_last_killer_damage;
            }
            g_pending_death_banner = {};
            g_pending_last_killer_damage = {};
        }
    } else if (g_pending_last_killer_damage.valid &&
               now >= g_pending_last_killer_damage.stamp &&
               now - g_pending_last_killer_damage.stamp >=
                   kDeathSummaryPairWindowMs) {
        g_pending_last_killer_damage = {};
    }
    ReleaseSRWLockExclusive(&g_pending_death_banner_lock);

    if (resolution ==
            MirvPovEventCompensation::DeathBannerResolution::wait ||
        !banner.hud) {
        return;
    }
    const auto followed = pov::snapshot();
    if (!followed.pawn || !followed.controller ||
        followed.generation != banner.generation ||
        !pawn_is_dead(followed.pawn) || demo_is_skipping()) {
        return;
    }
    const bool replay_native =
        resolution ==
            MirvPovEventCompensation::DeathBannerResolution::native_summary &&
        summary.valid;
    submit_death_panel_summary(banner, replay_native ? &summary : nullptr);
    g_last_killer_damage_armed.store(false, std::memory_order_release);
}

void __fastcall radar_sound_submit_scope(void* source_pawn, int radius,
                                         float duration, bool is_footstep) {
    note_runtime_point(RuntimePoint::radar_sound_submit);
    const auto original = g_radar_sound_submit_original;
    if (!original) {
        return;
    }
    if (demo_is_skipping() || !source_pawn) {
        original(source_pawn, radius, duration, is_footstep);
        return;
    }
    const auto followed = pov::snapshot();
    if (!followed.pawn || source_pawn != followed.pawn) {
        original(source_pawn, radius, duration, is_footstep);
        return;
    }

    const bool repaired =
        MirvPovEventCompensation::native_movement_sound_needs_presentation_repair(
            radius, duration, is_footstep);
    const int effective_radius = repaired ? 1100 : radius;
    const float effective_duration = repaired ? 0.50f : duration;
    const auto kind = repaired
                          ? MirvPovEventCompensation::RadarSoundKind::footstep
                          : native_radar_sound_kind(radius, is_footstep);
    const auto index = radar_sound_kind_index(kind);
    g_native_radar_sound_generation[index].store(
        followed.generation, std::memory_order_relaxed);
    g_native_radar_sound_stamp[index].store(
        GetTickCount64(), std::memory_order_release);
    pov::Scope scope(pov::Domain::player_sound);
    original(source_pawn, effective_radius, effective_duration, is_footstep);
}

void* __fastcall radar_sound_create_scope(void* hud, int player_id, int radius,
                                          float duration, bool is_footstep) {
    note_runtime_point(RuntimePoint::radar_sound_create);
    const auto original = g_radar_sound_create_original;
    void* snippet = original
                        ? original(hud, player_id, radius, duration, is_footstep)
                        : nullptr;
    if (g_radar_sound_transaction_active && snippet) {
        g_radar_sound_created_hud = hud;
        g_radar_sound_created_snippet = snippet;
        g_radar_sound_created_snippet_updated = false;
        if (MirvPovEventCompensation::native_generic_footstep_needs_max(
                radius, duration, is_footstep)) {
            g_radar_sound_max_snippet = snippet;
            g_radar_sound_max_armed = false;
        }
    }
    return snippet;
}

void __fastcall radar_sound_snippet_update_scope(void* hud, void* snippet) {
    note_runtime_point(RuntimePoint::radar_sound_snippet_update);
    const auto original = g_radar_sound_snippet_update_original;
    if (original) {
        original(hud, snippet);
    }
    if (g_radar_sound_transaction_active && snippet &&
        snippet == g_radar_sound_max_snippet && !g_radar_sound_max_armed) {
        __try {
            reinterpret_cast<std::uint8_t*>(snippet)[0x17C] |= 0x02;
            g_radar_sound_max_armed = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (g_radar_sound_transaction_active &&
        snippet == g_radar_sound_created_snippet) {
        g_radar_sound_created_snippet_updated = true;
    }
}

void flush_pending_radar_sounds() {
    if (!g_radar_sound_submit_original) {
        clear_pending_radar_sounds();
        return;
    }
    const auto followed = pov::snapshot();
    const auto now = GetTickCount64();
    PendingRadarSound selected{};
    bool selected_ready = false;

    AcquireSRWLockExclusive(&g_pending_radar_sound_lock);
    std::size_t kept = 0;
    for (std::size_t index = 0; index < g_pending_radar_sound_count; ++index) {
        const auto& pending = g_pending_radar_sounds[index];
        if (!followed.pawn || pending.pawn != followed.pawn ||
            pending.generation != followed.generation) {
            continue;
        }
        if (now < pending.stamp ||
            now - pending.stamp < kRadarSoundNativePriorityMs ||
            selected_ready) {
            g_pending_radar_sounds[kept++] = pending;
            continue;
        }
        const auto sound_kind = radar_sound_kind_index(pending.spec.kind);
        const auto native_stamp = g_native_radar_sound_stamp[sound_kind].load(
            std::memory_order_acquire);
        const auto native_generation =
            g_native_radar_sound_generation[sound_kind].load(
                std::memory_order_relaxed);
        if (native_generation == pending.generation &&
            MirvPovEventCompensation::native_sound_covers_event(
                pending.stamp, now, native_stamp)) {
            continue;
        }
        selected = pending;
        selected_ready = true;
    }
    g_pending_radar_sound_count = kept;
    ReleaseSRWLockExclusive(&g_pending_radar_sound_lock);

    if (!selected_ready) {
        return;
    }
    pov::Scope scope(pov::Domain::player_sound);
    // Keep the event-derived approximation in the generic native slot. The
    // game still creates, positions, animates and expires the radar snippet.
    g_radar_sound_submit_original(selected.pawn, selected.spec.radius,
                                  selected.spec.duration, false);
}

bool __fastcall game_event_dispatch_scope(void* manager, void* event) {
    note_runtime_point(RuntimePoint::game_event_dispatch);
    queue_radar_sound_event(event);
    queue_damage_feedback_event(reinterpret_cast<std::uintptr_t>(event));
    return g_game_event_dispatch_original
               ? g_game_event_dispatch_original(manager, event)
               : false;
}

bool emit_followed_death_feedback(void* event) {
    if (!g_emit_hurt_feedback || !event || demo_is_skipping()) {
        return false;
    }
    __try {
        const auto followed = pov::snapshot();
        if (!followed.pawn) {
            return false;
        }
        void* attacker = event_player_field(event, "attacker", "cker", 4,
                                            kEventAttackerSeed);
        void* victim = event_player_field(event, "userid", "userid", 6,
                                          kEventDeathUseridSeed);
        if (!attacker || !victim || attacker != followed.pawn ||
            attacker == victim || entity_team(victim) == followed.team) {
            return false;
        }
        const bool headshot =
            event_field_truthy(event, "headshot", 8, kEventHeadshotSeed);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(victim);
        const bool helmet = bytes[kPawnPreviousHelmetOffset] != 0;
        const int armor =
            *reinterpret_cast<const int*>(bytes + kPawnArmorValueOffset);
        const char* sound = nullptr;
        if (headshot) {
            sound = helmet ? "Player.DeathHeadShotArmor.AttackerFeedback"
                           : "Player.DeathHeadShot.AttackerFeedback";
        } else {
            sound = armor > 0 ? "Player.DeathBodyArmor.AttackerFeedback"
                              : "Player.DeathBody.AttackerFeedback";
        }
        g_emit_hurt_feedback(victim, nullptr, sound);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void pin_death_pov(const pov::Snapshot& current) {
    if (!current.pawn) {
        return;
    }
    pov::pin(current);
    const auto stamp = GetTickCount64();
    g_death_pov_latch_stamp = stamp;
    g_combat_latch_generation.store(current.generation,
                                    std::memory_order_relaxed);
    g_combat_latch_stamp.store(stamp, std::memory_order_relaxed);
    g_last_killer_damage_armed.store(true, std::memory_order_release);
}

void release_expired_death_pov() {
    const auto stamp = g_death_pov_latch_stamp.load(std::memory_order_acquire);
    if (!stamp) {
        return;
    }
    if (!pov::pinned()) {
        g_death_pov_latch_stamp = 0;
        return;
    }
    const auto now = GetTickCount64();
    if (now >= stamp && now - stamp >= kDeathPovLatchMs) {
        pov::unpin();
        g_death_pov_latch_stamp = 0;
    }
}

void publish_followed_identity() {
    g_identity_frame_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_requested || !g_client_initialized || !g_client ||
        !supported_build(g_client, g_h_engine2Dll) || demo_is_skipping()) {
        if (demo_is_skipping()) {
            pov::invalidate();
        }
        return;
    }
    void* pawn = followed_pawn();
    if (!pawn) {
        const auto no_target = g_identity_no_target_frames.fetch_add(
                                   1, std::memory_order_relaxed) +
                               1;
        if (no_target == 1) {
            advancedfx::Warning(
                "[mirv_pov] followed identity unavailable hltv_index=%d\n",
                g_last_hltv_primary_index.load(std::memory_order_relaxed));
        }
        if (g_last_logged_followed.exchange(nullptr,
                                             std::memory_order_acq_rel)) {
            advancedfx::Message("[mirv_pov] followed identity cleared\n");
        }
        pov::invalidate();
        return;
    }
    const int team = entity_team(pawn);
    // The reference rejects spectator/observer entities (team 1). Publishing
    // one as the followed player makes HUD callers consume an observer pawn.
    if (team != 2 && team != 3) {
        const auto invalid_team_frames =
            g_identity_invalid_team_frames.fetch_add(
                1, std::memory_order_relaxed) +
            1;
        if (g_last_logged_followed.exchange(nullptr,
                                            std::memory_order_acq_rel)) {
            advancedfx::Message("[mirv_pov] followed identity cleared\n");
        }
        if (invalid_team_frames == 1) {
            advancedfx::Warning(
                "[mirv_pov] followed identity rejected team=%d hltv_index=%d\n",
                team,
                g_last_hltv_primary_index.load(std::memory_order_relaxed));
        }
        pov::invalidate();
        return;
    }
    void* controller = controller_from_pawn(pawn);
    if (!controller) {
        const auto no_target = g_identity_no_target_frames.fetch_add(
                                   1, std::memory_order_relaxed) +
                               1;
        if (no_target == 1) {
            advancedfx::Warning(
                "[mirv_pov] followed pawn has no controller hltv_index=%d\n",
                g_last_hltv_primary_index.load(std::memory_order_relaxed));
        }
        pov::invalidate();
        return;
    }

    g_identity_valid_frames.fetch_add(1, std::memory_order_relaxed);
    pov::Snapshot next{};
    next.pawn = pawn;
    next.controller = controller;
    next.team = team;
    next.slot = -1;
    __try {
        auto get_slot = reinterpret_cast<PawnSlotFn>(
            module_base(g_client) + kPawnGetPlayerSlotRva);
        if (get_slot) {
            get_slot(pawn, &next.slot);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        next.slot = -1;
    }

    const auto old = pov::snapshot();
    if (old.pawn != next.pawn || old.controller != next.controller ||
        old.slot != next.slot || old.team != next.team) {
        pov::publish(next);
    }
    if (g_last_logged_followed.exchange(next.pawn,
                                        std::memory_order_acq_rel) != next.pawn) {
        advancedfx::Message(
            "[mirv_pov] followed identity pawn=%p controller=%p slot=%d "
            "team=%d hltv_index=%d\n",
            next.pawn, next.controller, next.slot, next.team,
            g_last_hltv_primary_index.load(std::memory_order_relaxed));
    }
}

void* __fastcall slot_pawn_hook(int slot) {
    note_runtime_point(RuntimePoint::slot_pawn);
    const auto original = reinterpret_cast<SlotFn>(g_slot_pawn.trampoline);
    void* native = original ? original(slot) : nullptr;
    if ((slot != 0 && slot != -1) || !pov::active() || demo_is_skipping()) {
        return native;
    }
    const auto current = pov::snapshot();
    return current.pawn ? current.pawn : native;
}

void* __fastcall slot_controller_hook(int slot) {
    note_runtime_point(RuntimePoint::slot_controller);
    const auto original =
        reinterpret_cast<SlotFn>(g_slot_controller.trampoline);
    void* native = original ? original(slot) : nullptr;
    if ((slot != 0 && slot != -1) || !pov::active() || demo_is_skipping()) {
        return native;
    }
    const auto current = pov::snapshot();
    return current.controller ? current.controller : native;
}

bool __fastcall is_observer_hook(void* pawn) {
    note_runtime_point(RuntimePoint::is_observer);
    const auto original =
        reinterpret_cast<IsObserverFn>(g_is_observer.trampoline);
    const bool native = original ? original(pawn) : false;
    if (!pov::active() || demo_is_skipping()) {
        return native;
    }
    const auto current = pov::snapshot();
    return current.pawn && pawn == current.pawn ? false : native;
}

std::uint64_t call_one_arg(EntryHook& hook, std::uintptr_t value) {
    const auto original = reinterpret_cast<OneArgFn>(hook.trampoline);
    return original ? original(value) : 0;
}

void __fastcall radar_transaction_scope(std::uintptr_t hud,
                                        std::uint8_t active) {
    note_runtime_point(RuntimePoint::radar_transaction);
    const auto original = g_radar_transaction_original;
    if (!original) {
        return;
    }
    if (demo_is_skipping()) {
        clear_pending_radar_sounds();
        AcquireSRWLockExclusive(&g_pending_damage_lock);
        g_pending_damage = {};
        ReleaseSRWLockExclusive(&g_pending_damage_lock);
        AcquireSRWLockExclusive(&g_pending_death_banner_lock);
        g_pending_death_banner = {};
        g_pending_last_killer_damage = {};
        ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
        original(hud, active);
        return;
    }
    pov::Scope scope(pov::Domain::radar);
    refresh_voice_receive_mask();
    flush_pending_radar_sounds();
    const bool owns_sound_transaction = !g_radar_sound_transaction_active;
    if (owns_sound_transaction) {
        g_radar_sound_transaction_active = true;
        g_radar_sound_created_hud = nullptr;
        g_radar_sound_created_snippet = nullptr;
        g_radar_sound_created_snippet_updated = false;
        g_radar_sound_max_snippet = nullptr;
        g_radar_sound_max_armed = false;
    }
    original(hud, active);
    if (owns_sound_transaction && g_radar_sound_created_snippet &&
        !g_radar_sound_created_snippet_updated &&
        g_radar_sound_frame_update_original && g_radar_sound_created_hud) {
        g_radar_sound_frame_update_original(g_radar_sound_created_hud);
    }
    if (owns_sound_transaction) {
        g_radar_sound_transaction_active = false;
        g_radar_sound_created_hud = nullptr;
        g_radar_sound_created_snippet = nullptr;
        g_radar_sound_created_snippet_updated = false;
        g_radar_sound_max_snippet = nullptr;
        g_radar_sound_max_armed = false;
    }
    if (active) {
        flush_pending_damage_feedback();
        flush_pending_death_banner();
    }
}

std::uint64_t __fastcall radar_mode_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::radar_mode);
    if (demo_is_skipping()) {
        return call_one_arg(g_radar_mode, hud);
    }
    pov::Scope scope(pov::Domain::radar);
    return call_one_arg(g_radar_mode, hud);
}

std::uint64_t __fastcall radar_update_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::radar_update);
    if (demo_is_skipping()) {
        return call_one_arg(g_radar_update, hud);
    }
    pov::Scope scope(pov::Domain::radar);
    return call_one_arg(g_radar_update, hud);
}

std::uint64_t __fastcall radar_local_transform_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::radar_local_transform);
    if (demo_is_skipping()) {
        return call_one_arg(g_radar_local_transform, hud);
    }
    pov::Scope scope(pov::Domain::radar);
    return call_one_arg(g_radar_local_transform, hud);
}

std::uint64_t __fastcall overhead_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::player_overhead);
    if (demo_is_skipping()) {
        return call_one_arg(g_overhead, hud);
    }
    pov::Scope scope(pov::Domain::player_overhead);
    return call_one_arg(g_overhead, hud);
}

std::uint64_t __fastcall team_counter_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::team_counter);
    if (demo_is_skipping()) {
        return call_one_arg(g_team_counter, hud);
    }
    pov::Scope scope(pov::Domain::team_counter);
    return call_one_arg(g_team_counter, hud);
}

std::uint64_t __fastcall voice_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::voice);
    if (demo_is_skipping()) {
        return call_one_arg(g_voice, hud);
    }
    pov::Scope scope(pov::Domain::voice);
    refresh_voice_receive_mask();
    adapt_native_voice_speaking_from_audio();
    return call_one_arg(g_voice, hud);
}

bool __fastcall voice_should_draw_scope(void* hud) {
    note_runtime_point(RuntimePoint::voice_should_draw);
    const auto original =
        reinterpret_cast<BoolOneArgFn>(g_voice_should_draw.trampoline);
    if (demo_is_skipping()) {
        return original ? original(hud) : false;
    }
    pov::Scope scope(pov::Domain::voice);
    // The reference lets the native VoiceStatus panel initialize even though
    // the recorded-demo gate starts closed; speaker selection remains native.
    if (original) {
        original(hud);
    }
    return true;
}

std::uint64_t __fastcall server_voice_scope(std::uintptr_t decoder,
                                            std::uintptr_t message) {
    note_runtime_point(RuntimePoint::server_voice);
    const auto original = reinterpret_cast<TwoArgFn>(g_server_voice.trampoline);
    if (demo_is_skipping()) {
        return original ? original(decoder, message) : 0;
    }
    const int slot = voice_speaker_slot(message);
    refresh_voice_receive_mask();
    if (slot >= 0 && g_tv_voice_indices && g_voice_mask_team != 0 &&
        !voice_slot_allowed(slot)) {
        return 0;
    }
    pov::Scope scope(pov::Domain::voice);
    return original ? original(decoder, message) : 0;
}

std::uint64_t __fastcall money_scope(std::uintptr_t hud,
                                     std::uintptr_t visible) {
    note_runtime_point(RuntimePoint::money);
    const auto original = reinterpret_cast<TwoArgFn>(g_money.trampoline);
    if (demo_is_skipping()) {
        return original ? original(hud, visible) : 0;
    }
    pov::Scope scope(pov::Domain::money);
    return original ? original(hud, visible) : 0;
}

void __fastcall gameplay_scope(std::uintptr_t listener,
                               std::uintptr_t event) {
    note_runtime_point(RuntimePoint::gameplay);
    const auto original = reinterpret_cast<TwoArgVoidFn>(g_gameplay.trampoline);
    if (demo_is_skipping()) {
        if (original) {
            original(listener, event);
        }
        return;
    }
    const char* name = event_name(reinterpret_cast<void*>(event));
    const bool is_death_event =
        name && std::strcmp(name, "player_death") == 0;
    const auto current = pov::snapshot();
    const bool followed_death =
        is_death_event && current.pawn &&
        event_matches_followed_victim(reinterpret_cast<void*>(event),
                                      current.pawn);
    if (followed_death) {
        pin_death_pov(current);
        pov::Scope scope(pov::Domain::combat_feedback);
        if (original) {
            original(listener, event);
        }
    } else if (original) {
        original(listener, event);
    }
    // The reference applies this compensation after the complete gameplay
    // dispatch, including when the followed player is the attacker and the
    // victim is another pawn.
    emit_followed_death_feedback(reinterpret_cast<void*>(event));
    adapt_kill_cash_notice(reinterpret_cast<void*>(event));
}

void __fastcall death_postprocess_scope(std::uintptr_t client_mode) {
    note_runtime_point(RuntimePoint::death_postprocess);
    release_expired_death_pov();
    const auto original =
        reinterpret_cast<OneArgVoidFn>(g_death_postprocess.trampoline);
    if (demo_is_skipping()) {
        if (original) {
            original(client_mode);
        }
        return;
    }
    pov::Scope scope(pov::Domain::view_effects |
                     pov::Domain::combat_feedback);
    if (original) {
        original(client_mode);
    }
}

std::uint64_t __fastcall damage_message_scope(std::uintptr_t hud,
                                              std::uintptr_t message) {
    note_runtime_point(RuntimePoint::damage_message);
    const auto original =
        reinterpret_cast<TwoArgFn>(g_damage_message.trampoline);
    if (demo_is_skipping()) {
        return original ? original(hud, message) : 0;
    }
    if (!damage_message_targets_follow(message, pov::snapshot())) {
        return original ? original(hud, message) : 0;
    }
    pov::Scope scope(pov::Domain::combat_feedback);
    if (g_damage_indicator_visible) {
        g_damage_indicator_visible(reinterpret_cast<void*>(hud + 0x20), true);
    }
    const auto result = original ? original(hud, message) : 0;
    g_native_damage_stamp.store(GetTickCount64(), std::memory_order_release);
    return result;
}

void __fastcall damage_direction_scope(std::uintptr_t hud, const float* source,
                                       void* pawn) {
    note_runtime_point(RuntimePoint::damage_direction);
    const auto original = g_damage_direction_original;
    if (!original) {
        return;
    }
    // This is the reference's rel-call boundary inside DamageMessage. The
    // outer transaction already owns combat_feedback when this is followed
    // damage; preserve the native ABI and call it exactly once.
    original(hud, source, pawn);
}

std::uint64_t __fastcall death_panel_event_scope(std::uintptr_t hud,
                                                 std::uintptr_t event) {
    note_runtime_point(RuntimePoint::death_panel_event);
    const auto original =
        reinterpret_cast<TwoArgFn>(g_death_panel_event.trampoline);
    if (demo_is_skipping()) {
        return original ? original(hud, event) : 0;
    }
    const auto current = pov::snapshot();
    if (!current.pawn || !event_matches_followed_victim(
                             reinterpret_cast<void*>(event), current.pawn)) {
        return original ? original(hud, event) : 0;
    }
    pin_death_pov(current);
    pov::Scope scope(pov::Domain::combat_feedback);
    const auto result = original ? original(hud, event) : 0;
    const auto stamp = GetTickCount64();
    PendingDeathBanner banner{hud, current.generation, stamp};
    PendingLastKillerDamage early_summary{};
    bool replay_early_summary = false;
    AcquireSRWLockExclusive(&g_pending_death_banner_lock);
    if (g_pending_last_killer_damage.valid &&
        g_pending_last_killer_damage.generation == current.generation &&
        MirvPovEventCompensation::resolve_death_banner(
            stamp, stamp, g_pending_last_killer_damage.stamp,
            kDeathSummaryPairWindowMs) ==
            MirvPovEventCompensation::DeathBannerResolution::native_summary) {
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
    }
    return result;
}

std::uint64_t __fastcall last_killer_scope(std::uintptr_t message) {
    note_runtime_point(RuntimePoint::last_killer);
    const auto original = reinterpret_cast<OneArgFn>(g_last_killer.trampoline);
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
    const bool accepted = followed.pawn && followed.controller &&
                          followed.generation == armed_generation &&
                          now >= armed_stamp &&
                          now - armed_stamp <= kDeathSummaryPairWindowMs &&
                          pawn_is_dead(followed.pawn) &&
                          g_last_killer_damage_armed.exchange(
                              false, std::memory_order_acq_rel);
    if (!accepted) {
        if (has_captured) {
            AcquireSRWLockExclusive(&g_pending_death_banner_lock);
            g_pending_last_killer_damage = captured;
            ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
        }
        return original(message);
    }
    std::uint64_t result = 0;
    {
        pov::Scope scope(pov::Domain::combat_feedback);
        result = original(message);
    }
    AcquireSRWLockExclusive(&g_pending_death_banner_lock);
    if (g_pending_death_banner.hud &&
        g_pending_death_banner.generation == followed.generation &&
        MirvPovEventCompensation::resolve_death_banner(
            g_pending_death_banner.stamp, now, now,
            kDeathSummaryPairWindowMs) ==
            MirvPovEventCompensation::DeathBannerResolution::native_summary) {
        g_pending_death_banner = {};
        g_pending_last_killer_damage = {};
    } else if (has_captured) {
        g_pending_last_killer_damage = captured;
    }
    ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
    return result;
}

std::uint64_t __fastcall death_panel_summary_scope(std::uintptr_t hud, int a2,
                                                  int a3, int a4, int a5,
                                                  int a6, int a7) {
    note_runtime_point(RuntimePoint::death_panel_summary);
    const auto original =
        reinterpret_cast<DeathPanelSummaryFn>(g_death_panel_summary.trampoline);
    if (demo_is_skipping()) {
        return original ? original(hud, a2, a3, a4, a5, a6, a7) : 0;
    }
    if (!pov::active(pov::Domain::combat_feedback)) {
        return original ? original(hud, a2, a3, a4, a5, a6, a7) : 0;
    }
    pov::Scope scope(pov::Domain::combat_feedback);
    return original ? original(hud, a2, a3, a4, a5, a6, a7) : 0;
}

void __fastcall death_panel_show_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::death_panel_show);
    const auto original =
        reinterpret_cast<OneArgVoidFn>(g_death_panel_show.trampoline);
    if (demo_is_skipping()) {
        if (original) {
            original(hud);
        }
        return;
    }
    if (!pov::active(pov::Domain::combat_feedback)) {
        if (original) {
            original(hud);
        }
        return;
    }
    pov::Scope scope(pov::Domain::combat_feedback);
    if (original) {
        original(hud);
    }
}

void __fastcall death_panel_hide_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::death_panel_hide);
    const auto original =
        reinterpret_cast<OneArgVoidFn>(g_death_panel_hide.trampoline);
    if (demo_is_skipping()) {
        if (original) {
            original(hud);
        }
        return;
    }
    if (!pov::active(pov::Domain::combat_feedback)) {
        if (original) {
            original(hud);
        }
        return;
    }
    pov::Scope scope(pov::Domain::combat_feedback);
    if (original) {
        original(hud);
    }
}

void __fastcall radio_scope(std::uintptr_t slot, std::uintptr_t message) {
    note_runtime_point(RuntimePoint::radio);
    const auto original = reinterpret_cast<TwoArgVoidFn>(g_radio.trampoline);
    if (demo_is_skipping()) {
        if (original) {
            original(slot, message);
        }
        return;
    }
    pov::Scope scope(pov::Domain::communications);
    if (original) {
        original(slot, message);
    }
}

void __fastcall say_text2_scope(std::uintptr_t slot,
                                std::uintptr_t message) {
    note_runtime_point(RuntimePoint::say_text2);
    const auto original = reinterpret_cast<TwoArgVoidFn>(g_say_text2.trampoline);
    if (demo_is_skipping()) {
        if (original) {
            original(slot, message);
        }
        return;
    }
    pov::Scope scope(pov::Domain::communications);
    if (original) {
        original(slot, message);
    }
}

std::uint64_t __fastcall hud_root_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::hud_root);
    if (demo_is_skipping()) {
        return call_one_arg(g_hud_root, hud);
    }
    pov::Scope scope(pov::Domain::hud_presentation);
    return call_one_arg(g_hud_root, hud);
}

std::uint64_t __fastcall spec_player_scope(std::uintptr_t hud) {
    note_runtime_point(RuntimePoint::spec_player);
    if (demo_is_skipping()) {
        return call_one_arg(g_spec_player, hud);
    }
    pov::Scope scope(pov::Domain::hud_presentation);
    return call_one_arg(g_spec_player, hud);
}

void __fastcall live_flash_scope(std::uintptr_t a1, std::uintptr_t a2,
                                 std::uintptr_t a3, std::uintptr_t a4,
                                 std::uintptr_t a5) {
    note_runtime_point(RuntimePoint::live_flash);
    const auto original =
        reinterpret_cast<FiveArgVoidFn>(g_live_flash.trampoline);
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

void __fastcall render_graph_scope(std::uintptr_t a1, std::uintptr_t a2,
                                   std::uintptr_t a3, std::uintptr_t a4,
                                   std::uintptr_t a5) {
    note_runtime_point(RuntimePoint::render_graph);
    const auto original =
        reinterpret_cast<FiveArgVoidFn>(g_render_graph.trampoline);
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

bool __fastcall spectator_tools_scope() {
    note_runtime_point(RuntimePoint::spectator_tools);
    if (pov::active(pov::Domain::view_effects |
                    pov::Domain::player_overhead |
                    pov::Domain::combat_feedback)) {
        return false;
    }
    const auto original =
        reinterpret_cast<BoolNoArgFn>(g_spectator_tools.trampoline);
    return original ? original() : false;
}

void* hud_getter(EntryHook& hook) {
    const auto original = reinterpret_cast<HudGetterFn>(hook.trampoline);
    const auto native = original ? original() : nullptr;
    if (!pov::active() || demo_is_skipping() || native) {
        return native;
    }
    // Match the reference adapter: only provide the followed pawn when the
    // native getter rejected the target. Replacing a valid native result can
    // turn a HUD object/context into a pawn and crash downstream callers.
    return pov::snapshot().pawn;
}

void __fastcall player_pawn_event_scope(void* listener, void* event) {
    note_runtime_point(RuntimePoint::player_pawn_event);
    const auto original = g_player_pawn_event_original;
    if (!original) {
        return;
    }
    if (demo_is_skipping()) {
        original(listener, event);
        return;
    }
    const auto current = pov::snapshot();
    const auto* name = event_name(event);
    const bool combat_event =
        name && (std::strcmp(name, "player_hurt") == 0 ||
                 std::strcmp(name, "player_death") == 0);
    void* pawn = nullptr;
    if (listener) {
        pawn = reinterpret_cast<void*>(
            reinterpret_cast<std::uint8_t*>(listener) -
            kPlayerPawnEventListenerOffset);
    }
    if (combat_event && current.pawn && same_player(pawn, current.pawn) &&
        event_player_slot_matches_pawn(event, pawn)) {
        if (std::strcmp(name, "player_death") == 0) {
            pin_death_pov(current);
        }
        pov::Scope scope(pov::Domain::combat_feedback);
        original(listener, event);
        if (std::strcmp(name, "weapon_fire") == 0) {
            adapt_grenade_throw_notice(listener, event);
        }
        return;
    }
    original(listener, event);
    adapt_grenade_throw_notice(listener, event);
}

void* __fastcall get_hud_player_scope() {
    note_runtime_point(RuntimePoint::get_hud_player);
    return hud_getter(g_get_hud_player);
}

void* __fastcall get_hud_alive_scope() {
    note_runtime_point(RuntimePoint::get_hud_alive);
    if (!demo_is_skipping() &&
        pov::active(pov::Domain::radar | pov::Domain::player_sound)) {
        const auto current = pov::snapshot();
        if (current.pawn) {
            return current.pawn;
        }
    }
    return hud_getter(g_get_hud_alive);
}

bool __fastcall relationship_scope(void* local_pawn,
                                   unsigned int target_handle) {
    note_runtime_point(RuntimePoint::team_relationship);
    const auto original =
        reinterpret_cast<HudTeamRelationshipFn>(g_relationship.trampoline);
    const bool native = original ? original(local_pawn, target_handle) : false;
    if (demo_is_skipping() || !local_pawn ||
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
        target = entity_from_entry_index(static_cast<int>(target_handle & 0x7FFFu));
    }
    const int target_team = entity_team(target);
    if (target_team != 2 && target_team != 3) {
        return native;
    }
    return followed.team != target_team;
}

bool __fastcall buy_zone_scope(void* pawn, bool strict) {
    note_runtime_point(RuntimePoint::buy_zone);
    const auto original =
        reinterpret_cast<BuyZonePredicateFn>(g_buy_zone.trampoline);
    const bool native = original ? original(pawn, strict) : false;
    if (demo_is_skipping() || !pov::active(pov::Domain::money)) {
        return native;
    }
    const auto followed = pov::snapshot();
    if (!followed.pawn || pawn != followed.pawn) {
        return native;
    }
    __try {
        return *reinterpret_cast<const std::uint8_t*>(
                   reinterpret_cast<const std::uint8_t*>(pawn) +
                   kPawnInBuyZoneOffset) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return native;
    }
}

bool __fastcall is_playing_demo_scope(void* engine) {
    note_runtime_point(RuntimePoint::is_playing_demo);
    if (pov::active(pov::Domain::communications |
                    pov::Domain::voice |
                    pov::Domain::combat_feedback)) {
        return false;
    }
    return g_is_playing_demo_original
               ? g_is_playing_demo_original(engine)
               : false;
}

unsigned int __fastcall broadcast_mode_scope(void* mode_provider) {
    note_runtime_point(RuntimePoint::broadcast_mode);
    if (pov::active(pov::Domain::team_counter)) {
        return 0;
    }
    return g_broadcast_mode_original
               ? g_broadcast_mode_original(mode_provider)
               : 0;
}

bool install_radar_transaction() {
    if (!g_client) {
        return false;
    }
    auto** slot = reinterpret_cast<void**>(
        module_base(g_client) + kRadarTransactionVtableSlotRva);
    void* expected = module_base(g_client) + kRadarTransactionRva;
    if (!slot || *slot != expected) {
        advancedfx::Warning("[mirv_pov] radar transaction vtable mismatch\n");
        return false;
    }
    g_radar_transaction_slot = slot;
    g_radar_transaction_original =
        reinterpret_cast<RadarTransactionFn>(expected);
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        g_radar_transaction_slot = nullptr;
        g_radar_transaction_original = nullptr;
        return false;
    }
    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(&radar_transaction_scope), expected);
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    if (observed != expected) {
        g_radar_transaction_slot = nullptr;
        g_radar_transaction_original = nullptr;
        return false;
    }
    return true;
}

void restore_radar_transaction() noexcept {
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

bool install_is_playing_demo_adapter() {
    if (!g_client) {
        return false;
    }
    auto* base = module_base(g_client);
    void* engine = nullptr;
    void** slot = nullptr;
    __try {
        engine = *reinterpret_cast<void**>(base + kEngineToClientObjectPtrRva);
        if (engine) {
            auto** vtable = *reinterpret_cast<void***>(engine);
            if (vtable) {
                slot = &vtable[kEngineToClientIsPlayingDemoVtableIndex];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    if (!slot || !*slot) {
        advancedfx::Warning(
            "[mirv_pov] IsPlayingDemo vtable adapter target missing\n");
        return false;
    }

    void* expected = *slot;
    g_is_playing_demo_slot = slot;
    g_is_playing_demo_original = reinterpret_cast<BoolOneArgFn>(expected);
    MemoryBarrier();

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        g_is_playing_demo_slot = nullptr;
        g_is_playing_demo_original = nullptr;
        return false;
    }
    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(&is_playing_demo_scope), expected);
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    if (observed != expected) {
        g_is_playing_demo_slot = nullptr;
        g_is_playing_demo_original = nullptr;
        advancedfx::Warning(
            "[mirv_pov] IsPlayingDemo vtable adapter replacement raced\n");
        return false;
    }
    return true;
}

bool install_broadcast_mode_adapter() {
    if (!g_client) {
        return false;
    }
    auto* base = module_base(g_client);
    if (std::memcmp(base + kBroadcastModePredicateRva,
                    kBroadcastModePredicatePrologue,
                    sizeof(kBroadcastModePredicatePrologue)) != 0) {
        advancedfx::Warning(
            "[mirv_pov] broadcast mode predicate prologue mismatch\n");
        return false;
    }

    void* provider = nullptr;
    void** slot = nullptr;
    __try {
        provider = *reinterpret_cast<void**>(base + kBroadcastModeObjectPtrRva);
        if (provider) {
            auto** vtable = *reinterpret_cast<void***>(provider);
            if (vtable) {
                slot = &vtable[kBroadcastModeVtableIndex];
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    if (!slot || !*slot) {
        advancedfx::Warning(
            "[mirv_pov] broadcast mode vtable adapter target missing\n");
        return false;
    }

    void* expected = *slot;
    g_broadcast_mode_slot = slot;
    g_broadcast_mode_original = reinterpret_cast<UIntOneArgFn>(expected);
    MemoryBarrier();

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        g_broadcast_mode_slot = nullptr;
        g_broadcast_mode_original = nullptr;
        return false;
    }
    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(&broadcast_mode_scope), expected);
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    if (observed != expected) {
        g_broadcast_mode_slot = nullptr;
        g_broadcast_mode_original = nullptr;
        advancedfx::Warning(
            "[mirv_pov] broadcast mode vtable adapter replacement raced\n");
        return false;
    }
    return true;
}

bool install_player_pawn_event_adapter() {
    if (!g_client) {
        return false;
    }
    auto** slot = reinterpret_cast<void**>(
        module_base(g_client) + kPlayerPawnEventVtableSlotRva);
    const void* expected = module_base(g_client) + kPlayerPawnFireGameEventRva;
    if (!slot || *slot != expected) {
        advancedfx::Warning(
            "[mirv_pov] player pawn event vtable adapter mismatch\n");
        return false;
    }
    g_player_pawn_event_slot = slot;
    g_player_pawn_event_original = reinterpret_cast<PlayerPawnEventFn>(*slot);
    MemoryBarrier();
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        g_player_pawn_event_slot = nullptr;
        g_player_pawn_event_original = nullptr;
        return false;
    }
    void* observed = InterlockedCompareExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(&player_pawn_event_scope),
        const_cast<void*>(expected));
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    if (observed != expected) {
        g_player_pawn_event_slot = nullptr;
        g_player_pawn_event_original = nullptr;
        advancedfx::Warning(
            "[mirv_pov] player pawn event vtable replacement raced\n");
        return false;
    }
    return true;
}

void restore_is_playing_demo_adapter() noexcept {
    if (g_is_playing_demo_slot && g_is_playing_demo_original) {
        DWORD old_protect = 0;
        if (VirtualProtect(g_is_playing_demo_slot, sizeof(void*),
                           PAGE_READWRITE, &old_protect)) {
            InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(g_is_playing_demo_slot),
                reinterpret_cast<void*>(g_is_playing_demo_original),
                reinterpret_cast<void*>(&is_playing_demo_scope));
            VirtualProtect(g_is_playing_demo_slot, sizeof(void*), old_protect,
                           &old_protect);
        }
    }
    g_is_playing_demo_slot = nullptr;
    g_is_playing_demo_original = nullptr;
}

void restore_broadcast_mode_adapter() noexcept {
    if (g_broadcast_mode_slot && g_broadcast_mode_original) {
        DWORD old_protect = 0;
        if (VirtualProtect(g_broadcast_mode_slot, sizeof(void*),
                           PAGE_READWRITE, &old_protect)) {
            InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(g_broadcast_mode_slot),
                reinterpret_cast<void*>(g_broadcast_mode_original),
                reinterpret_cast<void*>(&broadcast_mode_scope));
            VirtualProtect(g_broadcast_mode_slot, sizeof(void*), old_protect,
                           &old_protect);
        }
    }
    g_broadcast_mode_slot = nullptr;
    g_broadcast_mode_original = nullptr;
}

void restore_player_pawn_event_adapter() noexcept {
    if (g_player_pawn_event_slot && g_player_pawn_event_original) {
        DWORD old_protect = 0;
        if (VirtualProtect(g_player_pawn_event_slot, sizeof(void*),
                           PAGE_READWRITE, &old_protect)) {
            InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(g_player_pawn_event_slot),
                reinterpret_cast<void*>(g_player_pawn_event_original),
                reinterpret_cast<void*>(&player_pawn_event_scope));
            VirtualProtect(g_player_pawn_event_slot, sizeof(void*), old_protect,
                           &old_protect);
        }
    }
    g_player_pawn_event_slot = nullptr;
    g_player_pawn_event_original = nullptr;
}

bool install_radar_sound_adapter() {
    if (!g_client) {
        return false;
    }
    auto* base = module_base(g_client);
    g_radar_sound_submit_original = reinterpret_cast<RadarSoundSubmitFn>(
        base + kRadarSoundSubmitRva);
    g_radar_sound_create_original = reinterpret_cast<RadarSoundCreateFn>(
        base + kRadarSoundCreateRva);
    g_radar_sound_frame_update_original =
        reinterpret_cast<RadarSoundFrameUpdateFn>(base + kRadarSoundFrameUpdateRva);
    g_radar_sound_snippet_update_original = reinterpret_cast<RadarSoundSnippetUpdateFn>(
        base + kRadarSoundSnippetUpdateRva);
    if (!install_rel_call(g_client, kRadarSoundEmitCallRva,
                          kRadarSoundEmitCallBytes,
                          reinterpret_cast<const void*>(&radar_sound_submit_scope),
                          g_radar_sound_emit_call, "radar_sound_emit_call") ||
        !install_rel_call(g_client, kRadarSoundCreateCallRva,
                          kRadarSoundCreateCallBytes,
                          reinterpret_cast<const void*>(&radar_sound_create_scope),
                          g_radar_sound_create_call, "radar_sound_create_call") ||
        !install_rel_call(
            g_client, kRadarSoundSnippetUpdateCallRva,
            kRadarSoundSnippetUpdateCallBytes,
            reinterpret_cast<const void*>(&radar_sound_snippet_update_scope),
            g_radar_sound_snippet_update_call,
            "radar_sound_snippet_update_call")) {
        restore_rel_call(g_radar_sound_snippet_update_call);
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

void restore_radar_sound_adapter() noexcept {
    restore_rel_call(g_radar_sound_snippet_update_call);
    restore_rel_call(g_radar_sound_create_call);
    restore_rel_call(g_radar_sound_emit_call);
    g_radar_sound_submit_original = nullptr;
    g_radar_sound_create_original = nullptr;
    g_radar_sound_frame_update_original = nullptr;
    g_radar_sound_snippet_update_original = nullptr;
    clear_pending_radar_sounds();
}

bool install_game_event_dispatch_adapter() {
    if (!g_client) {
        return false;
    }
    auto* base = module_base(g_client);
    auto** slot = reinterpret_cast<void**>(
        base + kGameEventDispatchVtableSlotRva);
    void* expected = base + kGameEventDispatchRva;
    if (!slot || *slot != expected) {
        advancedfx::Warning(
            "[mirv_pov] game event dispatch vtable adapter mismatch\n");
        return false;
    }
    g_game_event_dispatch_slot = slot;
    g_game_event_dispatch_original =
        reinterpret_cast<GameEventDispatchFn>(expected);
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
        advancedfx::Warning(
            "[mirv_pov] game event dispatch replacement raced\n");
        return false;
    }
    return true;
}

void restore_game_event_dispatch_adapter() noexcept {
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

bool prepare_damage_direction() {
    if (!g_client) {
        return false;
    }
    auto* entry = module_base(g_client) + kDamageDirectionRva;
    if (std::memcmp(entry, kSavedRdxPrologue, sizeof(kSavedRdxPrologue)) != 0) {
        advancedfx::Warning("[mirv_pov] damage_direction prologue mismatch\n");
        return false;
    }
    g_damage_direction_original = reinterpret_cast<DamageDirectionFn>(entry);
    return true;
}

bool validate_prologue(HMODULE module, std::uint32_t rva,
                       const std::uint8_t* expected, std::size_t size,
                       const char* tag) {
    if (!module || !expected || size < 4) {
        return false;
    }
    const auto* actual = module_base(module) + rva;
    if (std::memcmp(actual, expected, size) == 0) {
        return true;
    }
    advancedfx::Warning(
        "[mirv_pov] %s validation mismatch rva=0x%X size=%zu "
        "actual=%02X %02X %02X %02X expected=%02X %02X %02X %02X\n",
        tag, rva, size, static_cast<unsigned>(actual[0]),
        static_cast<unsigned>(actual[1]), static_cast<unsigned>(actual[2]),
        static_cast<unsigned>(actual[3]), static_cast<unsigned>(expected[0]),
        static_cast<unsigned>(expected[1]), static_cast<unsigned>(expected[2]),
        static_cast<unsigned>(expected[3]));
    return false;
}

bool validate_event_helpers() {
    if (!g_client) {
        return false;
    }
    bool ok = true;
    ok &= validate_prologue(g_client, kEventFieldHashRva,
                            kEventFieldHashPrologue,
                            sizeof(kEventFieldHashPrologue),
                            "event_field_hash");
    ok &= validate_prologue(g_client, kFilterPlayerEntityRva,
                            kFilterPlayerEntityPrologue,
                            sizeof(kFilterPlayerEntityPrologue),
                            "filter_player_entity");
    ok &= validate_prologue(g_client, kPawnGetPlayerSlotRva,
                            kPawnSlotPrologue, sizeof(kPawnSlotPrologue),
                            "pawn_get_player_slot");
    return ok;
}

bool validate_native_compensation_helpers() {
    if (!g_client) {
        return false;
    }
    bool ok = true;
    ok &= validate_prologue(g_client, kEntityPlayerIdRva,
                            kEntityPlayerIdPrologue,
                            sizeof(kEntityPlayerIdPrologue),
                            "entity_player_id");
    ok &= validate_prologue(g_client, kEmitHurtFeedbackRva,
                            kEmitHurtFeedbackPrologue,
                            sizeof(kEmitHurtFeedbackPrologue),
                            "emit_hurt_feedback");
    ok &= validate_prologue(g_client, kEntityAbsOriginRva,
                            kEntityAbsOriginPrologue,
                            sizeof(kEntityAbsOriginPrologue),
                            "entity_abs_origin");
    ok &= validate_prologue(g_client, kDamageIndicatorVisibleRva,
                            kDamageIndicatorVisiblePrologue,
                            sizeof(kDamageIndicatorVisiblePrologue),
                            "damage_indicator_visible");
    ok &= validate_prologue(g_client, kFindHudElementRva,
                            kFindHudElementPrologue,
                            sizeof(kFindHudElementPrologue),
                            "find_hud_element");
    ok &= validate_prologue(g_client, kPushNoticeRva, kPushNoticePrologue,
                            sizeof(kPushNoticePrologue), "push_notice");
    // HLAE's legacy GameEvents.cpp already detours FireEventClientSide at
    // this entry before the native POV pipeline is initialized. The vtable
    // adapter below validates ownership and keeps that detour in the call
    // chain, so a raw prologue check here would reject a valid HLAE state.
    ok &= validate_prologue(g_client, kRadarSoundSubmitRva,
                            kRadarSoundSubmitPrologue,
                            sizeof(kRadarSoundSubmitPrologue),
                            "radar_sound_submit");
    ok &= validate_prologue(g_client, kVoiceActivityRva,
                            kVoiceActivityPrologue,
                            sizeof(kVoiceActivityPrologue),
                            "voice_activity");
    return ok;
}

bool install_identity() {
    bool ok = true;
    ok &= install_entry(g_client, kSlotPawnRva, kSlotPrologue,
                        sizeof(kSlotPrologue),
                        reinterpret_cast<const void*>(&slot_pawn_hook),
                        g_slot_pawn, "slot_pawn");
    ok &= install_entry(g_client, kSlotControllerRva, kSlotPrologue,
                        sizeof(kSlotPrologue),
                        reinterpret_cast<const void*>(&slot_controller_hook),
                        g_slot_controller, "slot_controller");
    ok &= install_entry(g_client, kIsObserverOrDeadRva, kIsObserverPrologue,
                        sizeof(kIsObserverPrologue),
                        reinterpret_cast<const void*>(&is_observer_hook),
                        g_is_observer, "is_observer");
    return ok;
}

bool install_pipeline() {
    bool ok = true;
#define INSTALL_ENTRY(RVA, PROLOGUE, FN, HOOK, TAG) \
    ok &= install_entry(g_client, RVA, PROLOGUE, sizeof(PROLOGUE), \
                        reinterpret_cast<const void*>(&FN), HOOK, TAG)
    ok &= install_radar_transaction();
    ok &= prepare_damage_direction();
    ok &= validate_event_helpers();
    ok &= validate_native_compensation_helpers();
    if (ok) {
        auto* base = module_base(g_client);
        g_entity_player_id_original =
            reinterpret_cast<EntityPlayerIdFn>(base + kEntityPlayerIdRva);
        g_emit_hurt_feedback =
            reinterpret_cast<EmitHurtFeedbackFn>(base + kEmitHurtFeedbackRva);
        g_entity_abs_origin =
            reinterpret_cast<EntityAbsOriginFn>(base + kEntityAbsOriginRva);
        g_damage_indicator_visible = reinterpret_cast<DamageIndicatorVisibleFn>(
            base + kDamageIndicatorVisibleRva);
        g_push_notice =
            reinterpret_cast<PushNoticeFn>(base + kPushNoticeRva);
        g_find_hud_element =
            reinterpret_cast<FindHudElementFn>(base + kFindHudElementRva);
    }
    if (!ok) {
        g_entity_player_id_original = nullptr;
        g_emit_hurt_feedback = nullptr;
        g_entity_abs_origin = nullptr;
        g_damage_indicator_visible = nullptr;
        g_push_notice = nullptr;
        g_find_hud_element = nullptr;
    }
    INSTALL_ENTRY(kRadarModeRva, kRadarModePrologue, radar_mode_scope,
                  g_radar_mode, "radar_mode");
    INSTALL_ENTRY(kRadarUpdateRva, kRadarUpdatePrologue, radar_update_scope,
                  g_radar_update, "radar_update");
    INSTALL_ENTRY(kRadarLocalTransformRva, kRadarLocalTransformPrologue,
                  radar_local_transform_scope, g_radar_local_transform,
                  "radar_local_transform");
    INSTALL_ENTRY(kOverheadRva, kOverheadPrologue, overhead_scope, g_overhead,
                  "player_overhead");
    INSTALL_ENTRY(kTeamCounterRva, kSixBytePrologue, team_counter_scope,
                  g_team_counter, "team_counter");
    INSTALL_ENTRY(kVoiceRva, kSixBytePrologue, voice_scope, g_voice, "voice");
    INSTALL_ENTRY(kVoiceShouldDrawRva, kVoiceShouldDrawPrologue,
                  voice_should_draw_scope, g_voice_should_draw,
                  "voice_should_draw");
    INSTALL_ENTRY(kServerVoiceRva, kOneArgPrologue, server_voice_scope,
                  g_server_voice, "server_voice");
    INSTALL_ENTRY(kMoneyRva, kMoneyPrologue, money_scope, g_money, "money");
    INSTALL_ENTRY(kGameplayRva, kOneArgPrologue, gameplay_scope, g_gameplay,
                  "gameplay");
    INSTALL_ENTRY(kDeathPostProcessRva, kDeathPostProcessPrologue,
                  death_postprocess_scope, g_death_postprocess,
                  "death_postprocess");
    INSTALL_ENTRY(kDamageMessageRva, kDamageMessagePrologue, damage_message_scope,
                  g_damage_message, "damage_message");
    ok &= install_rel_call(g_client, kDamageDirectionCallRva,
                           kDamageDirectionCallBytes,
                           reinterpret_cast<const void*>(&damage_direction_scope),
                           g_damage_direction_call, "damage_direction_call");
    INSTALL_ENTRY(kDeathPanelEventRva, kOneArgPrologue,
                  death_panel_event_scope, g_death_panel_event,
                  "death_panel_event");
    INSTALL_ENTRY(kLastKillerRva, kSavedRdxPrologue, last_killer_scope,
                  g_last_killer, "last_killer");
    INSTALL_ENTRY(kDeathPanelSummaryRva, kSixBytePrologue,
                  death_panel_summary_scope, g_death_panel_summary,
                  "death_panel_summary");
    INSTALL_ENTRY(kDeathPanelShowRva, kShowPrologue, death_panel_show_scope,
                  g_death_panel_show, "death_panel_show");
    INSTALL_ENTRY(kDeathPanelHideRva, kDeathPanelHidePrologue,
                  death_panel_hide_scope,
                  g_death_panel_hide, "death_panel_hide");
    INSTALL_ENTRY(kRadioRva, kSavedRdxPrologue, radio_scope, g_radio, "radio");
    INSTALL_ENTRY(kSayText2Rva, kOneArgPrologue, say_text2_scope, g_say_text2,
                  "say_text2");
    INSTALL_ENTRY(kHudRootRva, kHudRootPrologue, hud_root_scope, g_hud_root,
                  "hud_root");
    INSTALL_ENTRY(kSpecPlayerRva, kSpecPlayerPrologue, spec_player_scope,
                  g_spec_player, "spec_player");
    INSTALL_ENTRY(kLiveFlashRva, kLiveFlashPrologue, live_flash_scope,
                  g_live_flash, "live_flash");
    INSTALL_ENTRY(kRenderGraphRva, kRenderGraphPrologue, render_graph_scope,
                  g_render_graph, "render_graph");
    INSTALL_ENTRY(kSpectatorToolsRva, kSpectatorToolsPrologue,
                  spectator_tools_scope, g_spectator_tools, "spectator_tools");
    INSTALL_ENTRY(kGetHudPlayerRva, kGetHudPlayerPrologue,
                  get_hud_player_scope, g_get_hud_player, "get_hud_player");
    INSTALL_ENTRY(kGetHudAliveRva, kSixBytePrologue, get_hud_alive_scope,
                  g_get_hud_alive, "get_hud_alive");
    INSTALL_ENTRY(kHudTeamRelationshipRva, kRelationshipPrologue,
                  relationship_scope, g_relationship, "team_relationship");
    INSTALL_ENTRY(kBuyZonePredicateRva, kBuyZonePrologue, buy_zone_scope,
                  g_buy_zone, "buy_zone");
    ok &= install_is_playing_demo_adapter();
    ok &= install_broadcast_mode_adapter();
    ok &= install_player_pawn_event_adapter();
    ok &= install_radar_sound_adapter();
    ok &= install_game_event_dispatch_adapter();
#undef INSTALL_ENTRY
    return ok;
}

void restore_pipeline() noexcept {
    restore_rel_call(g_damage_direction_call);
    restore_game_event_dispatch_adapter();
    restore_radar_sound_adapter();
    restore_radar_transaction();
    restore_broadcast_mode_adapter();
    restore_is_playing_demo_adapter();
    restore_player_pawn_event_adapter();
#define RESTORE(HOOK) restore_entry(HOOK)
    RESTORE(g_buy_zone);
    RESTORE(g_relationship);
    RESTORE(g_get_hud_alive);
    RESTORE(g_get_hud_player);
    RESTORE(g_spectator_tools);
    RESTORE(g_render_graph);
    RESTORE(g_live_flash);
    RESTORE(g_spec_player);
    RESTORE(g_hud_root);
    RESTORE(g_say_text2);
    RESTORE(g_radio);
    RESTORE(g_death_panel_hide);
    RESTORE(g_death_panel_show);
    RESTORE(g_death_panel_summary);
    RESTORE(g_last_killer);
    RESTORE(g_death_panel_event);
    RESTORE(g_damage_message);
    RESTORE(g_death_postprocess);
    RESTORE(g_gameplay);
    RESTORE(g_money);
    RESTORE(g_server_voice);
    RESTORE(g_voice);
    RESTORE(g_voice_should_draw);
    RESTORE(g_team_counter);
    RESTORE(g_overhead);
    RESTORE(g_radar_local_transform);
    RESTORE(g_radar_update);
    RESTORE(g_radar_mode);
#undef RESTORE
    g_damage_direction_original = nullptr;
    g_entity_player_id_original = nullptr;
    g_emit_hurt_feedback = nullptr;
    g_entity_abs_origin = nullptr;
    g_damage_indicator_visible = nullptr;
    g_push_notice = nullptr;
    g_find_hud_element = nullptr;
    g_death_pov_latch_stamp = 0;
    g_combat_latch_generation = 0;
    g_combat_latch_stamp = 0;
    g_last_killer_damage_armed = false;
    AcquireSRWLockExclusive(&g_pending_damage_lock);
    g_pending_damage = {};
    ReleaseSRWLockExclusive(&g_pending_damage_lock);
    AcquireSRWLockExclusive(&g_pending_death_banner_lock);
    g_pending_death_banner = {};
    g_pending_last_killer_damage = {};
    ReleaseSRWLockExclusive(&g_pending_death_banner_lock);
    restore_voice_receive_mask();
}

void restore_identity() noexcept {
    restore_entry(g_is_observer);
    restore_entry(g_slot_controller);
    restore_entry(g_slot_pawn);
}

bool install_all() {
    if (g_installed || g_install_failed || !g_client_initialized ||
        !g_client || !g_h_engine2Dll) {
        return false;
    }
    if (!supported_build(g_client, g_h_engine2Dll)) {
        g_install_failed = true;
        return false;
    }
    const bool identity_ok = install_identity();
    const bool pipeline_ok = install_pipeline();
    if (!identity_ok || !pipeline_ok) {
        g_install_failed = true;
        log_message("install failed; all hooks rolled back");
        restore_pipeline();
        restore_identity();
        pov::invalidate();
        g_entity_player_id_original = nullptr;
        g_emit_hurt_feedback = nullptr;
        g_entity_abs_origin = nullptr;
        g_damage_indicator_visible = nullptr;
        g_push_notice = nullptr;
        g_find_hud_element = nullptr;
        return false;
    }
    g_installed = true;
    log_message("native POV pipeline installed");
    return true;
}

void print_status() {
    advancedfx::Message(
        "[mirv_pov] requested=%d installed=%d client_initialized=%d client=%p\n",
        g_requested.load() ? 1 : 0, g_installed.load() ? 1 : 0,
        g_client_initialized ? 1 : 0, g_client);
    const auto followed = pov::snapshot();
    advancedfx::Message(
        "[mirv_pov] identity pawn=%p controller=%p slot=%d team=%d "
        "generation=%llu hltv_index=%d frames=%llu valid=%llu no_target=%llu "
        "invalid_team=%llu controller_fallback=%llu\n",
        followed.pawn, followed.controller, followed.slot, followed.team,
        static_cast<unsigned long long>(followed.generation),
        g_last_hltv_primary_index.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(
            g_identity_frame_calls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_identity_valid_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_identity_no_target_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_identity_invalid_team_frames.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_identity_controller_fallbacks.load(std::memory_order_relaxed)));
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(RuntimePoint::count); ++index) {
        const auto calls =
            g_runtime_point_calls[index].load(std::memory_order_relaxed);
        if (calls != 0) {
            advancedfx::Message("[mirv_pov] runtime %s calls=%llu\n",
                                kRuntimePointNames[index],
                                static_cast<unsigned long long>(calls));
        }
    }
}

CON_COMMAND(mirv_pov, "Use the followed demo player for native HUD transactions.")
{
    const int count = args->ArgC();
    if (count < 2 || _stricmp(args->ArgV(1), "status") == 0) {
        print_status();
        return;
    }
    if (_stricmp(args->ArgV(1), "1") == 0 ||
        _stricmp(args->ArgV(1), "on") == 0) {
        g_requested = true;
        g_install_failed = false;
        if (!install_all()) {
            advancedfx::Warning(
                "[mirv_pov] enable pending or refused; see console output\n");
        }
        print_status();
        return;
    }
    if (_stricmp(args->ArgV(1), "0") == 0 ||
        _stricmp(args->ArgV(1), "off") == 0) {
        g_requested = false;
        g_install_failed = false;
        g_last_logged_followed = nullptr;
        RemoveHooks();
        print_status();
        return;
    }
    advancedfx::Message("[mirv_pov] usage: mirv_pov [1|0|status]\n");
}

} // namespace

void OnClientLoaded(HMODULE client) {
    g_client = client;
    g_install_failed = false;
    g_last_logged_followed = nullptr;
    g_build_cache_valid.store(false, std::memory_order_release);
    g_build_cache_result.store(false, std::memory_order_relaxed);
    g_build_mismatch_logged.store(false, std::memory_order_relaxed);
    if (g_requested && !g_client_initialized && !g_pending_logged) {
        g_pending_logged = true;
        log_message("client loaded; enable after client init");
    }
}

void OnClientInit() {
    g_client_initialized = true;
    g_install_failed = false;
    g_pending_logged = false;
    if (g_requested) {
        install_all();
    }
}

void OnClientShutdown() {
    RemoveHooks();
    g_install_failed = false;
    g_last_logged_followed = nullptr;
    g_client_initialized = false;
    g_client = nullptr;
    g_build_cache_valid.store(false, std::memory_order_release);
    g_build_cache_result.store(false, std::memory_order_relaxed);
}

void OnFrameStage(SOURCESDK::CS2::ClientFrameStage_t stage) {
    if (!g_requested) {
        return;
    }
    if (!g_installed) {
        install_all();
    }
    if (stage == SOURCESDK::CS2::FRAME_RENDER_PASS) {
        publish_followed_identity();
    }
}

bool Requested() {
    return g_requested;
}

bool Installed() {
    return g_installed;
}

void RemoveHooks() {
    if (!g_installed.exchange(false)) {
        pov::invalidate();
        g_entity_player_id_original = nullptr;
        g_emit_hurt_feedback = nullptr;
        g_entity_abs_origin = nullptr;
        g_damage_indicator_visible = nullptr;
        g_push_notice = nullptr;
        g_find_hud_element = nullptr;
        return;
    }
    restore_pipeline();
    restore_identity();
    pov::invalidate();
    log_message("native POV pipeline removed");
}

} // namespace MirvPov
