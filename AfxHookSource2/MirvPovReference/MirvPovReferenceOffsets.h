#pragma once
#include <cstddef>
#include <cstdint>

// Fixed RVAs / layout for the researched CS2 build only.
// Measure SizeOfImage + TimeDateStamp from the machine's engine2.dll
// (dumpbin /headers or the build_id helper) and replace the placeholder
// fingerprint constants before first real inject.
namespace offsets {
inline constexpr const char* kEngine2Name = "engine2.dll";
inline constexpr const char* kClientName = "client.dll";

// Every client RVA below is pinned to this Steam client build.  engine2 can
// remain unchanged across a client-only update, so its fingerprint alone is
// not a sufficient safety gate.
inline constexpr std::uint32_t kExpectedClientSize = 0x027B8000;
inline constexpr std::uint32_t kExpectedClientTimeDateStamp = 0x6A7CE4FB;

// From IDA research session (engine2 image base 0x180000000 style):
// HLTV_FilterOrBufferNetMessage @ 0x18004D860 -> RVA 0x4D860
inline constexpr std::uint32_t kEngine2FilterOrBufferRva = 0x4D860;

// cmp [rax+0x2C3538],0 / je early-out -- NOP these sites to keep net path.
// FilterOrBuffer @ 0x18004D897: 0F 84 71 03 00 00
inline constexpr std::uint32_t kEngine2FilterIsHltvJeRva = 0x4D897;
inline constexpr std::uint8_t kEngine2FilterIsHltvJeBytes[6] = {
    0x0F, 0x84, 0x71, 0x03, 0x00, 0x00};

// Second HLTV-looking is_hltv je @ 0x7B642 was tested with clear and caused
// crash; DO NOT enable as default. Kept here for research notes only.
inline constexpr std::uint32_t kEngine2AltIsHltvJeRva = 0x7B642;
inline constexpr std::uint8_t kEngine2AltIsHltvJeBytes[2] = {0x74, 0x39};

// qword* clientstate global used by FilterOrBuffer / IsHLTVOrReplay.
// 2026-08-13 IsHLTVOrReplay @ 0x75ED0 resolves this slot to RVA 0x90D490.
inline constexpr std::uint32_t kEngine2ClientStatePtrRva = 0x90D490;

// Continue block after IsSpecialMode early-out (docs only; MVP uses JE NOP).
inline constexpr std::uint32_t kEngine2FilterOrBufferContinueRva = 0x4D8AA;

// clientstate + is_hltv (HUD/mode switch), from ProcessServerInfo
inline constexpr std::uintptr_t kClientStateIsHltv = 0x2C3538;

// CDemoPlayer singleton object.  The 2026-08-13 constructor @ 0x2DC10 stores
// ??_7CDemoPlayer@@6B@ (RVA 0x52DB28) at object RVA 0x68C268; the +0x20 member
// at 0x68C288 is not the CDemoPlayer base.
inline constexpr std::uint32_t kEngine2DemoPlayerObjRva = 0x68C268;
// vtable+0x58 getter: byte playing flag
inline constexpr std::uintptr_t kDemoPlayerPlaying = 0x1230;
// m_nSkipToTick; IsSkipping = playing && (skip != -1); vtable+0x70
inline constexpr std::uintptr_t kDemoPlayerSkipToTick = 0x208;
// demo_gototick early-out gate: vtable+0x120 reads this byte; non-zero => skip seek
inline constexpr std::uintptr_t kDemoPlayerGotoBlocked = 0x18A5;

// Optional anchors (documented; unused by MVP hooks unless needed later)
inline constexpr std::uintptr_t kClientStatePlayerSlot = 0xF8;
inline constexpr std::uint32_t kEngine2ProcessServerInfoRva = 0x6A900;
inline constexpr std::uint32_t kEngine2ProcessServerInfoApplyRva = 0x841C0;
inline constexpr std::uint32_t kEngine2IsSkippingRva = 0x29220;
inline constexpr std::uint32_t kEngine2SkipToTickRva = 0x29260;
inline constexpr std::uint32_t kEngine2DemoGotoBlockedGetterRva = 0x24810;
inline constexpr std::uint32_t kEngine2DemoGotoBlockedSetterRva = 0x24800;

// Measured from the installed Steam engine2.dll (2026-08-13):
inline constexpr std::uint32_t kExpectedEngine2Size = 0x00962000;           // SizeOfImage
inline constexpr std::uint32_t kExpectedEngine2TimeDateStamp = 0x6A7CE4F8;  // PE TimeDateStamp

// Source2EngineToClient_IsHLTVOrReplay @ 0x180075ED0 -> RVA 0x75ED0
// HUD/radar gate (vtable+0x2B0). FilterOrBuffer does NOT call this — it reads
// clientstate+0x2C3538 directly — so lying here keeps the net path on is_hltv.
inline constexpr std::uint32_t kEngine2IsHltvOrReplayRva = 0x75ED0;
inline constexpr std::uint8_t kEngine2IsHltvOrReplayPrologue[] = {
    0x48, 0x8B, 0x05, 0xB9, 0x75, 0x89, 0x00};  // mov rax, [rip+clientstate]
// Whole-function force-false (breaks demo camera — research only).
inline constexpr std::uint8_t kEngine2IsHltvOrReplayLie[] = {0x31, 0xC0, 0xC3};
// Hook site overwrite size (AbsJump). Trampoline is rebuilt — do not memcpy
// the RIP-relative prologue into a far page.
inline constexpr std::size_t kEngine2IsHltvOrReplayStolen = 12;
// Around HudRadar_UpdateSquareLayout, collect call [reg+0x2B0] sites.
inline constexpr std::uint32_t kClientRadarAllowRadius = 0x20000;
inline constexpr std::uint8_t kClientCallVtable2B0[] = {0xFF, 0x90, 0xB0, 0x02,
                                                       0x00, 0x00};

// client.dll radar experiment (HudRadar_UpdateSquareLayout):
// Two sites: mov rax,[rcx] / call qword ptr [rax+0x2B0]  (IsHLTVOrReplay)
// Steam builds drift; runtime scanner finds the unique pair with gap 0xFC.
inline constexpr std::uint8_t kClientCallIsHltvOrReplay[] = {
    0x48, 0x8B, 0x01, 0xFF, 0x90, 0xB0, 0x02, 0x00, 0x00};
// Replace only the CALL (last 6 bytes) with xor eax,eax; nop*4
inline constexpr std::uint8_t kClientForceAlZero[] = {0x31, 0xC0, 0x90, 0x90,
                                                     0x90, 0x90};
// Research IDA gap 0xE20687-0xE2058B; Steam client 0xE20AD4-0xE209D8 — both 0xFC.
inline constexpr std::uint32_t kClientRadarCallPairGap = 0xFC;

// Schema (MulNX cs2_dumper client_dll) — identity remap for LIVE_HUD_PIPELINE.
// dwEntityList: client.dll global; entity resolve: chunk = list+8*(idx>>9)+0x10,
// entry = chunk+0x70*(idx&0x1FF).
// Game-native entity chunk table (from sub_926D60 mov r9,[rip]); not dwEntityList.
inline constexpr std::uint32_t kClientEntityChunkTable = 0x21D5FE0;
inline constexpr std::uint32_t kClientDwEntityList = 0x2554080;
inline constexpr std::uint32_t kClientDwLocalPlayerController = 0x2383DA0;
inline constexpr std::uint32_t kClientDwLocalPlayerPawn = 0x23A9438;
// CCSGameRules* global (same dump family as dwLocalPlayerPawn).
inline constexpr std::uint32_t kClientDwGameRules = 0x23A8BD8;
// C_CSGameRules::m_bFreezePeriod — schema binder in this build uses +0x158
// (s2v sometimes lists 0x40 for other builds; verify against local client).
inline constexpr std::uintptr_t kGameRulesFreezePeriod = 0x40;
// Brief post-freeze money strip (seconds of curtime after freeze clears).
inline constexpr float kMoneyRevealGraceSec = 2.5f;
// C_HLTVCamera singleton (RTTI/vftable instance); +0x3C = primary target entindex
// (written by hltv_chase / SetPrimaryTarget path). Demo follow uses this, NOT
// m_hObserverTarget (which stays 0xFFFFFFFF in HLTV playback).
inline constexpr std::uint32_t kClientHltvCamera = 0x209AC90;
inline constexpr std::uintptr_t kHltvCameraPrimaryTarget = 0x3C;
// Slot helper at client RVA 0x926D60 returns PAWN (controller[slot]->m_hPawn),
// not the controller. Identity remap must treat the hook result as a pawn.
inline constexpr std::uintptr_t kControllerPawnHandle = 0x6BC;       // CBasePlayerController::m_hPawn
inline constexpr std::uintptr_t kControllerPlayerPawn = 0x914;     // CCSPlayerController::m_hPlayerPawn
inline constexpr std::uintptr_t kControllerObserverPawn = 0x918;   // CCSPlayerController::m_hObserverPawn
// CBasePlayerController::m_iszPlayerName (CNetworkStringBase<128>).
inline constexpr std::uintptr_t kControllerPlayerName = 0x6F4;
inline constexpr std::uintptr_t kPawnObserverServices = 0x1220;      // C_BasePlayerPawn::m_pObserverServices
inline constexpr std::uintptr_t kObserverTargetHandle = 0x4C;        // CPlayer_ObserverServices::m_hObserverTarget
inline constexpr std::uintptr_t kPawnController = 0x13d0;            // C_BasePlayerPawn::m_hController
inline constexpr std::uintptr_t kPawnOriginalController = 0x1478;    // C_CSPlayerPawn::m_hOriginalController
inline constexpr std::uintptr_t kPawnLastPlaceName = 0x14DC;         // CCSPlayerPawn::m_szLastPlaceName[18]
inline constexpr std::uint32_t kClientSlotControllerRva = 0x927F60;
// This helper's slot is a split-screen/local-client slot, not a network player
// slot. Its backing global starts at dwLocalPlayerController and must never be
// walked as a 64-player controller array.
// client+0xB11860 calls slot->controller only as a local-exists guard, then
// resolves an unrelated global player id. Returning a followed controller to
// this indirect HUD consumer bypasses its null guard and crashes at +0xB118A4
// (`cmp [rax+0x8c], bl`) when that global id is not ready.
inline constexpr std::uint32_t kClientControllerExistsGuardLo = 0xB11860;
inline constexpr std::uint32_t kClientControllerExistsGuardHi = 0xB11969;
inline constexpr std::uint32_t kClientSlotPawnRva = 0x927FA0;
// Pawn→IsObserverOrDeadSpectating (HudRadar icon style). Reads m_hController
// at +0x13D0 then observer state. Steam RVA confirmed via icon-site callees.
inline constexpr std::uint32_t kClientIsObserverOrDeadRva = 0x899A80;
inline constexpr std::uint8_t kClientIsObserverOrDeadPrologue[] = {
    0x8B, 0x91, 0xD0, 0x13, 0x00, 0x00,
    0x83, 0xFA, 0xFF};  // mov edx,[rcx+0x13D0]; cmp edx,-1
inline constexpr std::size_t kClientIsObserverOrDeadStolen = 9;  // through cmp edx,-1
// C_BaseEntity::m_iTeamNum (schema explorer 2026-08).
inline constexpr std::uintptr_t kEntityTeamNum = 0x3E7;
// CCSGO_HudRadar dirty-bit setter (cl_reload_hud neighbor): forces icon rebuild
// after follow-target team changes (T↔CT).
inline constexpr std::uint32_t kClientHudRadarMarkDirtyRva = 0xE4DF20;
inline constexpr std::uint8_t kClientHudRadarMarkDirtyPrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D};
// FindHudElement("CCSGO_HudRadar") — same helper the dirty setter uses.
inline constexpr std::uint32_t kClientFindHudElementRva = 0xDFC710;
inline constexpr std::uint32_t kClientHudRadarNameRva = 0x1B75800;
// HudRadar player-icon array (UpdatePlayerIcon @ 0xE34220).
inline constexpr std::uintptr_t kHudRadarPlayerLastIndex = 0x340;
inline constexpr std::uintptr_t kHudRadarPlayerArray = 0x358;
inline constexpr std::uintptr_t kHudRadarPlayerStride = 0x180;
inline constexpr std::uintptr_t kHudRadarIconFlags = 0x17C;
inline constexpr std::uintptr_t kHudRadarIconFlash = 0x17D;
inline constexpr std::uintptr_t kHudRadarIconStyle = 0x16C;  // SetPlayerIconStyle cache
inline constexpr std::uintptr_t kHudRadarIconPos = 0x110;
inline constexpr std::uintptr_t kHudRadarIconTimeA = 0x140;
inline constexpr std::uintptr_t kHudRadarIconTimeB = 0x144;
inline constexpr std::uintptr_t kHudRadarSpotBits = 0x17770;

// CCSGO_HudRadar_SetPlayerIconStyle @ 0xE3ACA0:
// if (IsObserver || IsHLTV) → demo team-color+number branch (bpl=1).
// NOP the two JNEs so live team-compare path always runs.
inline constexpr std::uint32_t kClientIconStyleObsJneRva = 0xE3C085;
inline constexpr std::uint8_t kClientIconStyleObsJneBytes[6] = {
    0x0F, 0x85, 0x07, 0x01, 0x00, 0x00};  // jne demo_style
inline constexpr std::uint32_t kClientIconStyleHltvJneRva = 0xE3C09D;
inline constexpr std::uint8_t kClientIconStyleHltvJneBytes[6] = {
    0x0F, 0x85, 0xEF, 0x00, 0x00, 0x00};  // jne demo_style
// Do NOT NOP the "style unchanged" je @ 0xE3AE9F: falling through always calls
// apply(0), which clears icon panels and causes freeze-time radar fade/flicker.

// HudRadar icon *paint* flags (@ ~0xE3CF78): if (IsObserver || IsHLTV)
// → `or ebx, 1` (demo number/letter bit). Separate from SetPlayerIconStyle.
// Seek briefly disables IsHLTV/IsObserver lies → numbers flash unless NOPed.
inline constexpr std::uint32_t kClientIconPaintObsJneRva = 0xE3E33C;
inline constexpr std::uint8_t kClientIconPaintObsJneBytes[2] = {0x75, 0x40};
inline constexpr std::uint32_t kClientIconPaintHltvJneRva = 0xE3E350;
inline constexpr std::uint8_t kClientIconPaintHltvJneBytes[2] = {0x75, 0x2C};

// r_spectator_flashbang_opacity ConVar static (float value @ obj+0x58).
inline constexpr std::uint32_t kClientSpectatorFlashOpacityCvarRva = 0x2438908;
// C_CSPlayerPawn::m_entitySpottedState (client schema) + EntitySpottedState_t.
inline constexpr std::uintptr_t kPawnEntitySpottedState = 0x1C60;
inline constexpr std::uintptr_t kSpottedStateSpotted = 0x8;
inline constexpr std::uintptr_t kSpottedStateMask = 0xC;

// HudRadar UpdatePlayerIcon enemy path @ 0xE3479D (after teammate je-show).
// Old jmp-to-0xE3491C was NOT a hide — that path still paints icons.
// Patch: E9 to a near stub that clears icon visible/flash then jmp 0xE34C60
// (next player). Orig insn: mov rax,[rbp+0x80] (7 bytes).
inline constexpr std::uint32_t kClientRadarEnemyHideRva = 0xE35B4D;
inline constexpr std::uint8_t kClientRadarEnemyHideOrig[7] = {
    0x48, 0x8B, 0x85, 0x80, 0x00, 0x00, 0x00};
inline constexpr std::uint32_t kClientRadarIconNextRva = 0xE36010;

// player_death @ C81D5B: rsi=victim pawn, rbx=attacker-side ent, r12=event.
// At C81E07: cmp victim, GetLocalPlayerPawn(); je deathcam.
// Kill confirm UI.KillCard.1 is server-only; demo never delivers it.
// Adapter: CALL observe stub over the 5-byte cmp/je — queue KillCard when
// attacker==follow, then run original cmp/je (deathcam gates untouched).
inline constexpr std::uint32_t kClientKillSoundCmpJeRva = 0xC83047;
inline constexpr std::uint8_t kClientKillSoundCmpJeBytes[5] = {
    0x48, 0x3B, 0xF0, 0x74, 0x10};  // cmp rsi, rax ; je deathcam
inline constexpr std::uint32_t kClientKillSoundAfterCmpRva = 0xC8304C;
inline constexpr std::uint32_t kClientKillSoundPlayRva = 0xC8305C;  // deathcam
inline constexpr std::uint32_t kClientKillSoundSkipRva = 0xC839A5;
// Play UI sound on a pawn: ba36e0(pawn, "UI.KillCard.1") — same helper as
// UI.DeathMatch.Revenge @ C0B62F.
inline constexpr std::uint32_t kClientPlayUiSoundRva = 0xBA4920;
// Event field hash helper + player filter used by death handlers.
inline constexpr std::uint32_t kClientEventFieldHashRva = 0x224F30;
inline constexpr std::uint8_t kClientEventFieldHashPrologue[6] = {
    0x48, 0x83, 0xEC, 0x28, 0x45, 0x8B};
inline constexpr std::uint32_t kClientFilterPlayerEntRva = 0x7F84B0;
inline constexpr std::uint8_t kClientFilterPlayerEntPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
inline constexpr std::uint32_t kClientPawnGetPlayerSlotRva = 0x900910;
inline constexpr std::uint8_t kClientPawnGetPlayerSlotPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
// Entity-system player id helper used by CCSUsrMsg_Damage itself when the
// message carries an explicit victim id (HLTV/special-mode branch).
inline constexpr std::uint32_t kClientEntityPlayerIdRva = 0x1513EF0;
inline constexpr std::uint8_t kClientEntityPlayerIdPrologue[4] = {
    0x48, 0x83, 0xEC, 0x08};
// CGameEntitySystem absolute-origin getter used by the native damage indicator
// itself to turn a source world position into directional arc strengths.
inline constexpr std::uint32_t kClientEntityAbsOriginRva = 0x219F80;
inline constexpr std::uint8_t kClientEntityAbsOriginPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};

// Shared live local-relative team/visibility predicate consumed by HudRadar,
// TeamCounter and overhead player IDs.
inline constexpr std::uint32_t kClientHudTeamRelationshipRva = 0x899980;
inline constexpr std::uint8_t kClientHudTeamRelationshipPrologue[6] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x57};

// Native buy-zone eligibility input used by CCSGO_HudMoney::Update.
inline constexpr std::uint32_t kClientBuyZonePredicateRva = 0x899440;
inline constexpr std::uint8_t kClientBuyZonePredicatePrologue[6] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};

// Pipeline V2 transaction boundaries. These are native HUD/message/render
// entry points; hooks only establish a scoped live-POV data context and then
// invoke the original function unchanged.
// HudRadar's vtable callback owns the complete update transaction: local/team
// preparation, transform, mode refresh, player records and entity records.
// Scoping the callback is essential for demos that initialize in free camera
// and acquire their first followed player after the radar object exists.
inline constexpr std::uint32_t kClientRadarTransactionUpdateRva = 0xE28280;
inline constexpr std::uint32_t kClientRadarTransactionVtableSlotRva =
    0x1B76E68;
// HudRadar's native mode refresh resolves GetHudAlivePawn and caches that
// pawn's observer mode before updating player records. In Demo playback a
// stale observer value makes the later player loop skip only the followed
// slot's position. Run this producer in the same live radar context so the
// engine itself refreshes the cache from the followed live-style pawn.
inline constexpr std::uint32_t kClientRadarModeUpdateRva = 0xE21D30;
inline constexpr std::uint8_t kClientRadarModeUpdatePrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x18};
inline constexpr std::uint32_t kClientRadarUpdateRva = 0xE355D0;
inline constexpr std::uint8_t kClientRadarUpdatePrologue[5] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
// HudRadar's player loop above does not own the local map transform. IDA:
// E35FD0 resolves slot 0 again, reads that pawn's world origin and writes the
// radar center/transform state. It must share the radar transaction or the
// followed-player marker stays at a cached spawn position while teammates
// continue to move.
inline constexpr std::uint32_t kClientRadarLocalTransformRva = 0xE36100;
inline constexpr std::uint8_t kClientRadarLocalTransformPrologue[5] = {
    0x48, 0x8B, 0xC4, 0x55, 0x53};
// Full teammate overhead transaction. E286E0 owns the name/equipment,
// visibility and color passes; E2CE70 beneath it consumes GetHudAlivePawn,
// sv_teamid_overhead, cl_teamid_overhead_mode and spectator-tools state.
inline constexpr std::uint32_t kClientPlayerOverheadUpdateRva = 0xE28810;
inline constexpr std::uint8_t kClientPlayerOverheadUpdatePrologue[9] = {
    0x41, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
inline constexpr std::uint32_t kClientTeamCounterUpdateDispatchRva = 0xE289F0;
inline constexpr std::uint8_t kClientTeamCounterUpdateDispatchPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
// TeamCounter's live-vs-broadcast selector calls an interface method at +0x98.
// V2 adapts that shared mode value only during the native TeamCounter update.
inline constexpr std::uint32_t kClientBroadcastModePredicateRva = 0x732610;
inline constexpr std::uint8_t kClientBroadcastModePredicatePrologue[11] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x0D,
    0x3D, 0x76, 0xBE, 0x01};
inline constexpr std::uint32_t kClientBroadcastModeObjectPtrRva = 0x2319C58;
inline constexpr std::size_t kBroadcastModeVtableIndex = 0x98 / sizeof(void*);
inline constexpr std::uint32_t kClientVoiceUpdateBoundaryRva = 0xE28A20;
inline constexpr std::uint8_t kClientVoiceUpdateBoundaryPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
inline constexpr std::uint32_t kClientMoneyUpdateBoundaryRva = 0xE29470;
inline constexpr std::uint8_t kClientMoneyUpdateBoundaryPrologue[6] = {
    0x40, 0x57, 0x48, 0x83, 0xEC, 0x20};
inline constexpr std::uint32_t kClientGameplayEventDispatchRva = 0xC81720;
inline constexpr std::uint8_t kClientGameplayEventDispatchPrologue[5] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
// CA59C0 is ClientMode's per-frame native post-processing transaction.  It
// derives the death-cam phase weights from GetLocalPlayerPawn/HLTV state and
// submits all seven native controls through the post-process manager.  Demo
// POV must scope this whole function, not just the player_death event handler.
inline constexpr std::uint32_t kClientDeathPostProcessUpdateRva = 0xCA59C0;
inline constexpr std::uint8_t kClientDeathPostProcessUpdatePrologue[7] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56};
// C81720 is the ClientMode event listener registered from base+8.  Its
// constructor loads CT/T freeze correction plus seven effect resources and
// their post-process controls into the following fields.
inline constexpr std::uintptr_t kClientModeEventListenerOffset = 0x8;
inline constexpr std::uintptr_t kClientModeFreezeCtResource = 0x70;
inline constexpr std::uintptr_t kClientModeFreezeCtControl = 0x78;
inline constexpr std::uintptr_t kClientModeFreezeTResource = 0x88;
inline constexpr std::uintptr_t kClientModeFreezeTControl = 0x90;
inline constexpr std::uintptr_t kClientModeDeathPhase1Resource = 0xB8;
inline constexpr std::uintptr_t kClientModeDeathPhase1Control = 0xF0;
inline constexpr std::uintptr_t kClientModeDeathPhase1LowResource = 0xC0;
inline constexpr std::uintptr_t kClientModeDeathPhase1LowControl = 0xF8;
inline constexpr std::uintptr_t kClientModeDeathPhase2Resource = 0xC8;
inline constexpr std::uintptr_t kClientModeDeathPhase2Control = 0x100;
inline constexpr std::uintptr_t kClientModeDeathPhase1Weight = 0x120;
inline constexpr std::uintptr_t kClientModeDeathPhase1LowWeight = 0x124;
inline constexpr std::uintptr_t kClientModeDeathPhase2Weight = 0x128;
// Set by ClientMode initialization after the freeze-view resources are ready;
// it is not a per-death red-screen activation latch.
inline constexpr std::uintptr_t kClientModeFreezeStateReady = 0x164;

// Native live combat-feedback transactions.  These functions still own all
// direction math, Panorama state, killer/weapon formatting and freeze-camera
// selection; Pipeline V2 only supplies the followed player's local identity
// while each complete native transaction is executing.
// CGameMessageDelegateHook<CCSUsrMsg_Damage_t>::Dispatch callback.  The
// message contains the server/demo damage origin and victim player id.
inline constexpr std::uint32_t kClientDamageMessageHandlerRva = 0xE011F0;
inline constexpr std::uint8_t kClientDamageMessageHandlerPrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x10};
// E010C0 passes its decoded source xyz and current HUD pawn through this exact
// CALL to the native four-direction strength calculator.  The wrapper is
// diagnostic-only and forwards the call unchanged.
inline constexpr std::uint32_t kClientDamageDirectionRva = 0xDF6CA0;
inline constexpr std::uint8_t kClientDamageDirectionPrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
inline constexpr std::uint32_t kClientDamageDirectionCallRva = 0xE012FB;
inline constexpr std::uint8_t kClientDamageDirectionCallBytes[5] = {
    0xE8, 0xA0, 0x59, 0xFF, 0xFF};
// Native HudElement visibility dispatch for CCSGO_HudDamageIndicator.  The
// subobject lives at HudDamageIndicator+0x20.  Passing true removes the
// Panorama `Damage--Hidden` class through E0A480 and resets the four native
// direction strengths before E010C0 fills them.
inline constexpr std::uint32_t kClientDamageIndicatorVisibleRva = 0xE085B0;
inline constexpr std::uint8_t kClientDamageIndicatorVisiblePrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
// CCSGO_HudDeathPanel's dedicated player_death handler.  It consumes the
// event's victim, attacker, weapon/item id and domination/revenge fields.
inline constexpr std::uint32_t kClientDeathPanelEventRva = 0xE04210;
inline constexpr std::uint8_t kClientDeathPanelEventPrologue[5] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
// SendLastKillerDamageToClient delegate.  It passes the native damage summary
// into CCSGO_HudDeathPanel and is the live trigger which unhides the banner.
inline constexpr std::uint32_t kClientLastKillerDamageHandlerRva = 0xC216C0;
inline constexpr std::uint8_t kClientLastKillerDamageHandlerPrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
// Native HudDeathPanel damage-summary input called by C216C0.  Zero summary
// values are valid and still trigger the already-populated killer banner.
inline constexpr std::uint32_t kClientDeathPanelDamageSummaryRva = 0xE08AD0;
inline constexpr std::uint8_t kClientDeathPanelDamageSummaryPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
// Native DeathPanel visibility state machine. E04D10 shows/fills the already
// populated panel and E01AC0 hides/resets it. Wrapping both gives the actual
// lifetime instead of confusing CSS fade-in or message-pair windows for it.
inline constexpr std::uint32_t kClientDeathPanelShowRva = 0xE04E40;
inline constexpr std::uint8_t kClientDeathPanelShowPrologue[11] = {
    0x40, 0x57, 0x41, 0x56, 0x48, 0x81,
    0xEC, 0xA8, 0x00, 0x00, 0x00};
inline constexpr std::uint32_t kClientDeathPanelHideRva = 0xE01BF0;
inline constexpr std::uint8_t kClientDeathPanelHidePrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
// CCSPlayerPawn state read by HudDeathPanel (4 == dead in this build).
inline constexpr std::uintptr_t kPawnPlayerState = 0x13FC;
// Player-owned combat visual state touched/read around C0BE40.  These are
// logged after the native hurt/death callback.  +0x1CA0 is touched by the
// player_death event branch, while +0x1CC8 is written on the player_hurt
// hitgroup==1 path for the attacker; neither is named as a red-screen latch.
inline constexpr std::uintptr_t kPawnLastDamageTime = 0x14CC;
inline constexpr std::uintptr_t kPawnDeathEventState = 0x1CA0;
inline constexpr std::uintptr_t kPawnHeadshotEventTime = 0x1CC8;

// Native attacker-feedback emitter. It creates a CPASAttenuationFilter around
// the victim/source pawn and submits the named sound through CS2's sound
// system, preserving 3D attenuation and environmental processing.
inline constexpr std::uint32_t kClientEmitHurtFeedbackSoundRva = 0x847DB0;
inline constexpr std::uint8_t kClientEmitHurtFeedbackSoundPrologue[23] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
    0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00};
inline constexpr std::uintptr_t kPawnPreviousHelmet = 0x14EF;
inline constexpr std::uintptr_t kPawnArmorValue = 0x1CA4;

// Native RadioText and SayText2 handlers. Both consult the engine-to-client
// IsPlayingDemo virtual (+0x150); V2 adapts that predicate only while these
// handlers are active, so ChatPrintf/PushNotice remain completely native.
inline constexpr std::uint32_t kClientRadioTextHandlerRva = 0x1110360;
inline constexpr std::uint8_t kClientRadioTextHandlerPrologue[5] = {
    0x48, 0x89, 0x5C, 0x24, 0x08};
inline constexpr std::uint32_t kClientSayText2HandlerRva = 0x1110CA0;
inline constexpr std::uint8_t kClientSayText2HandlerPrologue[5] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
inline constexpr std::uint32_t kClientEngineToClientPtrRva = 0x23A49F0;
inline constexpr std::size_t kEngineToClientExecuteCommandVtableIndex = 51;
inline constexpr std::size_t kEngineToClientIsPlayingDemoVtableIndex =
    0x150 / sizeof(void*);

// Top-level gameplay-HUD presentation updates. The root updater derives
// HUD--localplayer--spectator / HUD--spectating-target from the local pawn's
// team and observer mode; the SpecPlayer updater uses the same values to show
// the demo player card. Scoped identity makes both consumers select CS2's
// normal live health/armor/ammo presentation without touching Panorama.
inline constexpr std::uint32_t kClientHudRootUpdateRva = 0xE0D430;
inline constexpr std::uint8_t kClientHudRootUpdatePrologue[5] = {
    0x40, 0x55, 0x53, 0x41, 0x54};
inline constexpr std::uint32_t kClientSpecPlayerUpdateRva = 0xE0C590;
inline constexpr std::uint8_t kClientSpecPlayerUpdatePrologue[6] = {
    0x40, 0x53, 0x56, 0x57, 0x41, 0x54};

// Main CSGO render-graph build. The scoped spectator-tools predicate adapter
// selects the normal/live FlashbangOverlay submission, while the central HUD
// pawn getters below provide the followed pawn's native flash state.
inline constexpr std::uint32_t kClientRenderGraphBuildRva = 0x11405E0;
inline constexpr std::uint8_t kClientRenderGraphBuildPrologue[10] = {
    0x48, 0x89, 0x5C, 0x24, 0x18,
    0x48, 0x89, 0x4C, 0x24, 0x08};
// The normal/live flash submission is a separate render callback that executes
// before the main graph's spectator-only second pass. It owns the first
// spectator-tools predicate and calls FlashbangOverlay @ 1140030. Scope this
// callback as well as the main graph; otherwise the live pass is skipped before
// the main-graph scope begins and suppressing the spectator pass yields no wash.
inline constexpr std::uint32_t kClientLiveFlashSubmitRva = 0x1132230;
inline constexpr std::uint8_t kClientLiveFlashSubmitPrologue[5] = {
    0x48, 0x89, 0x6C, 0x24, 0x10};
inline constexpr std::uint32_t kClientSpectatorToolsPredicateRva = 0xC78600;
inline constexpr std::uint8_t kClientSpectatorToolsPredicatePrologue[9] = {
    0x48, 0x83, 0xEC, 0x28, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF};

inline constexpr std::uint8_t kClientGetHudPlayerPrologue[8] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC9};
inline constexpr std::size_t kClientGetHudPlayerStolen = 8;
// Valve hashes "attacker" as suffix "cker"/len=4 with this seed; keybuf still
// stores the full "attacker" name pointer (see death handlers @ C0B09A).
inline constexpr unsigned kClientEventAttackerSeed = 0xEDE4F213u;
// player_death resolves userid with hash("userid", 6, 0x31415920).
inline constexpr unsigned kClientDeathUseridSeed = 0x31415920u;
// weapon_fire resolves its owner through the "userid" entity field. Valve's
// key hash uses suffix "id"/len=2 while the key buffer keeps "userid".
inline constexpr unsigned kClientEventUseridSeed = 0x572DEA01u;
// weapon_fire string field; hash suffix "on"/len=2, key name "weapon".
inline constexpr unsigned kClientEventWeaponSeed = 0x3E03DAFAu;
// weapon_fire `silenced`; normal full-name hash seed is len(8)^0x31415926.
inline constexpr unsigned kClientEventSilencedSeed = 0x3141592Eu;
// player_death "headshot": hash full name len=8 with this seed; GetInt @ vt+0x38
// (death-notice @ E017DA). Not the userid seed 0x31415920.
inline constexpr unsigned kClientEventHeadshotSeed = 0x3141592Eu;
// player_hurt "dmg_health": full-name len 10, verified in the game's own
// player_hurt consumer @ E2BA28.
inline constexpr unsigned kClientEventDamageHealthSeed = 0x3141592Cu;

// Native live radar-sound producer. EmitSoundByHandle resolves the exact VSND
// public distance-volume radius, duration and .Step flag, then calls E35F70.
// We replace only this existing CALL so unwind/prologue metadata stays native.
inline constexpr std::uint32_t kClientRadarSoundEmitCallRva = 0xBA4F1A;
inline constexpr std::uint8_t kClientRadarSoundEmitCallBytes[5] = {
    0xE8, 0x81, 0x11, 0x29, 0x00};
inline constexpr std::uint32_t kClientRadarSoundSubmitRva = 0xE360A0;
inline constexpr std::uint8_t kClientRadarSoundSubmitPrologue[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x40};
// HudRadar consumes queued sound rows through this native snippet factory.
// The wrapper is diagnostic-only: it forwards every argument/return value and
// proves that a reconstructed .Step record reached the real renderer.
inline constexpr std::uint32_t kClientRadarSoundCreateCallRva = 0xE28336;
inline constexpr std::uint8_t kClientRadarSoundCreateCallBytes[5] = {
    0xE8, 0xB5, 0xBF, 0xFF, 0xFF};
inline constexpr std::uint32_t kClientRadarSoundCreateRva = 0xE242F0;
// E4A4E0 owns the per-frame RadarPlayerSoundSnippet loop.  Demo HUD routing
// can consume/create a row without running that loop in the same owning radar
// transaction.  The call below is wrapped so V2 can prove whether the new row
// reached the native geometry/Panorama updater; if it did not, V2 invokes the
// complete E4A4E0 loop once before leaving that same radar transaction.
inline constexpr std::uint32_t kClientRadarSoundFrameUpdateRva = 0xE4A610;
inline constexpr std::uint32_t kClientRadarSoundSnippetUpdateRva = 0xE3A550;
inline constexpr std::uint32_t kClientRadarSoundSnippetUpdateCallRva =
    0xE4A72F;
inline constexpr std::uint8_t kClientRadarSoundSnippetUpdateCallBytes[5] = {
    0xE8, 0x1C, 0xFE, 0xFE, 0xFF};

// CGameEventManager::FireEventClientSide is vtable slot 8 in the pinned
// client. It mirrors all registered events, including player_footstep and
// weapon_zoom that the per-pawn weapon_fire listener does not receive.
inline constexpr std::uint32_t kClientGameEventDispatchRva = 0x998070;
inline constexpr std::uint8_t kClientGameEventDispatchPrologue[10] = {
    0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30};
inline constexpr std::uint32_t kClientGameEventDispatchVtableSlotRva =
    0x1AD4A28;

// Every C_CSPlayerPawn owns an IGameEventListener2 subobject at +0x13E0 and
// registers it for weapon_fire, player_hurt, player_death and related player
// events. Vtable slot 1 is FireGameEvent. Replacing this data pointer preserves
// the native listener/unwind path. V2 scopes only the followed victim's native
// hurt/death call, then uses the same wrapper after-native for the derived
// grenade radio notice that .dem files do not contain as RadioText.
inline constexpr std::uintptr_t kPlayerPawnEventListenerOffset = 0x13E0;
inline constexpr std::uint32_t kClientPlayerPawnEventVtableSlotRva = 0x1B2A0C8;
inline constexpr std::uint32_t kClientPlayerPawnFireGameEventRva = 0xC0BE40;

// Bottom-left gameplay notices (RadioText / TextMsg → ChatPrintf → PushNotice).
// Slot→entity used for team prefix / mute gate (@ RadioText).
inline constexpr std::uint32_t kClientSlotEntityRva = 0xA74560;
// CCSGO_HudVoiceStatus::PushNotice — final print into chat + voice panels.
inline constexpr std::uint32_t kClientPushNoticeRva = 0xE36B50;
inline constexpr std::uint8_t kClientPushNoticePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20};  // 15 bytes
inline constexpr std::size_t kClientPushNoticeStolen = 15;
// Native HUD lookup used to obtain CCSGO_HudVoiceStatus before submitting a
// derived event notice through PushNotice.
inline constexpr std::uint8_t kClientFindHudElementPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
// ILocalize* used by RadioText; vtable+0x78 resolves a #token to UTF-8.
inline constexpr std::uint32_t kClientLocalizationInterfaceRva = 0x25CD598;
// RadioText mute-gate early-out after BAB620: test al,al @ EA05; jne @ EA07.
inline constexpr std::uint32_t kClientRadioMuteJneRva = 0x1110957;
inline constexpr std::uint8_t kClientRadioMuteJneBytes[6] = {
    0x0F, 0x85, 0xF0, 0x01, 0x00, 0x00};
// ChatPrintf IsPlayingDemo suppress: jne skip when demo flag @ +0x72 set.
inline constexpr std::uint32_t kClientChatDemoJneRva = 0x110DB37;
inline constexpr std::uint8_t kClientChatDemoJneBytes[6] = {
    0x0F, 0x85, 0xC4, 0x00, 0x00, 0x00};
// SayText2 handler repeats the same demo suppress before it can call
// ChatPrintf.  Leaving this gate intact means the downstream patch and
// PushNotice hook never see SayText2 messages (identity_diag chat=0/0).
inline constexpr std::uint32_t kClientSayTextDemoJneRva = 0x1110CD7;
inline constexpr std::uint8_t kClientSayTextDemoJneBytes[6] = {
    0x0F, 0x85, 0x22, 0x07, 0x00, 0x00};

// CCSGO_HudVoiceStatus visibility/update. Demo playback leaves the panel hidden
// even when recorded voice is decoded. sub_E288F0 ends with a tail jump to the
// large updater after its own stack has been fully restored. Hook that dispatch
// jump instead of relocating the updater's seven-push prologue: the latter has
// Windows unwind metadata tied to its original address and crashed on the first
// live call. The wrapper skips seek/unready-panel frames and SEH-fuses a bad
// native update. Team filtering replaces two existing CALL instructions, where
// the compiler already permits volatile GPR/XMM clobbering; never inject a C++
// call at the old no-call cmp site E4BF3A.
inline constexpr std::uint32_t kClientVoiceShouldDrawRva = 0xE3EDA0;
inline constexpr std::uint8_t kClientVoiceShouldDrawBytes[3] = {
    0x40, 0x53, 0x48};
inline constexpr std::uint8_t kClientVoiceShouldDrawPrologue[6] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};
inline constexpr std::uint32_t kClientVoiceUpdateDispatchRva = 0xE28A3A;
inline constexpr std::uint8_t kClientVoiceUpdateDispatchBytes[5] = {
    0xE9, 0x11, 0x35, 0x02, 0x00};
inline constexpr std::uint32_t kClientVoiceUpdateRva = 0xE4BF50;
inline constexpr std::uint32_t kClientVoiceModeCallRva = 0xE4C03B;
inline constexpr std::uint8_t kClientVoiceModeCallBytes[5] = {
    0xE8, 0xA0, 0xE4, 0xCA, 0xFF};
inline constexpr std::uint32_t kClientVoiceModeFnRva = 0xAFA4E0;
inline constexpr std::uint32_t kClientVoiceSpeakingCallRva = 0xE4C062;
inline constexpr std::uint8_t kClientVoiceSpeakingCallBytes[5] = {
    0xE8, 0xA9, 0x08, 0xD6, 0xFF};
inline constexpr std::uint32_t kClientVoiceSpeakingFnRva = 0xBAC910;
inline constexpr std::uint32_t kClientVoiceStateGetRva = 0xBA75D0;
// Native per-player voice activity/level lookup. Recorded 5E audio updates
// this table even though its playback path bypasses UpdateSpeakerStatus.
inline constexpr std::uint32_t kClientVoiceActivityRva = 0xAE5500;
inline constexpr std::uint8_t kClientVoiceActivityPrologue[8] = {
    0x48, 0x83, 0xEC, 0x28, 0x89, 0x4C, 0x24, 0x30};
// ServerVoice decoder/submit transaction. Its Demo branch plays recorded
// audio and returns before publishing per-player activity for VoiceStatus.
inline constexpr std::uint32_t kClientServerVoiceSubmitRva = 0xAED960;
inline constexpr std::uint8_t kClientServerVoiceSubmitPrologue[5] = {
    0x48, 0x89, 0x4C, 0x24, 0x08};
// Recorded SvcVoiceData handler calls CVoiceStatus::UpdateSpeakerStatus here
// with talking=1. An ABI-identical wrapper records packet-to-speaker activity
// while still calling the native function unchanged.
inline constexpr std::uint32_t kClientVoicePacketSpeakerCallRva = 0x1110C77;
inline constexpr std::uint8_t kClientVoicePacketSpeakerCallBytes[5] = {
    0xE8, 0xE4, 0x7D, 0xAA, 0xFF};
inline constexpr std::uint32_t kClientVoiceUpdateSpeakerStatusRva = 0xBB8A60;

// CCSGO_HudMoney update and Panorama `money__in-buy-zone` class. sub_E29340
// restores its stack and tail-jumps to the updater at E29422. Hook that safe
// dispatch instead of relocating the updater prologue. The native demo path can
// resolve the HLTV/local pawn and keep the cart visible; our post-update
// override uses the followed pawn's real m_bInBuyZone and native predicates.
inline constexpr std::uint32_t kClientHudMoneyUpdateDispatchRva = 0xE29552;
inline constexpr std::uint8_t kClientHudMoneyUpdateDispatchBytes[5] = {
    0xE9, 0x49, 0x8C, 0x01, 0x00};
inline constexpr std::uint32_t kClientHudMoneyUpdateRva = 0xE421A0;
inline constexpr std::uint32_t kClientHudMoneyInBuyZoneClassRva = 0x20AF312;
inline constexpr std::uint32_t kClientGameRulesBuyStateRva = 0x722660;
inline constexpr std::uint32_t kClientBuyTimeElapsedRva = 0x731E00;
inline constexpr std::uintptr_t kPawnInBuyZone = 0x1500;

// CCSGO_HudTeamCounter money reveal: sub_85B6C0(local) || IsHLTVOrReplay
// → always show. Live uses freezetime timer when both false (@ 0xE47BAB).
inline constexpr std::uint32_t kClientSpecMoneyRevealRva = 0x85B6C0;
inline constexpr std::uint8_t kClientSpecMoneyRevealPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};  // 10 bytes
inline constexpr std::size_t kClientSpecMoneyRevealStolen = 10;
inline constexpr std::uint32_t kClientTeamCounterMoneyFnRva = 0xE47B90;
// Sticky money flag setter helper: returns true when dword field == 2 (misread
// as always-on in demo). Live money strip should key off freeze period instead.
inline constexpr std::uint32_t kClientMoneyStickyGateRva = 0x863350;
inline constexpr std::uint8_t kClientMoneyStickyGatePrologue[] = {
    0x48, 0x83, 0xEC, 0x28, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF};  // 9 bytes
inline constexpr std::size_t kClientMoneyStickyGateStolen = 9;
// HudTeamCounter sticky show byte written at 0xE47C2A.
inline constexpr std::uintptr_t kHudTeamCounterMoneySticky = 0x1776D;
// TeamCounter::Update chooses the broadcast/all-player detailed array by
// calling sub_732610. Force only this call false so the native live arrays and
// own-team visibility comparison are used; both sides' avatar slots remain.
inline constexpr std::uint32_t kClientTeamCounterBroadcastCallRva = 0xE4572E;
inline constexpr std::uint8_t kClientTeamCounterBroadcastCallBytes[5] = {
    0xE8, 0xDD, 0xCE, 0x8E, 0xFF};
// TeamCounter builds a compact per-player Panorama payload at E31700, then
// submits it through E42800. Payload: +4 player id, +8 flags (bit10 defuser,
// bit11 C4), +0C health, +10 armor.
inline constexpr std::uint32_t kClientTeamCounterPlayerDataCallRva = 0xE31B1C;
inline constexpr std::uint8_t kClientTeamCounterPlayerDataCallBytes[5] = {
    0xE8, 0x0F, 0x0E, 0x01, 0x00};
inline constexpr std::uint32_t kClientTeamCounterApplyPlayerDataRva = 0xE42930;
inline constexpr std::uint32_t kClientTeamCounterResolvePawnRva = 0xA74A20;

// Panorama flash HUD binder uses GetHudPlayer @ 0xC10D30; call sites in
// 0xCAC000–0xCAD100 must see follow pawn (not HLTV observer).
inline constexpr std::uint32_t kClientGetHudPlayerRva = 0xC11F70;
inline constexpr std::uint32_t kClientFlashHudCallLo = 0xCAD240;
inline constexpr std::uint32_t kClientFlashHudCallHi = 0xCAE340;
// Simpler alive-local getter (xor ecx; call slot→pawn; IsAlive). HudRadar
// UpdatePlayerIcon @ E3424D and TeamCounter paths call THIS, not C10D30.
inline constexpr std::uint32_t kClientGetHudAlivePawnRva = 0xC12520;
inline constexpr std::uint8_t kClientGetHudAlivePawnPrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};  // push rbx; sub rsp,20h
inline constexpr std::size_t kClientGetHudAlivePawnStolen = 6;
// C_BaseEntity::IsAlive (vtable) — same slot C112E0 uses after slot→pawn.
inline constexpr std::uintptr_t kPawnIsAliveVtable = 0x4D8;
// Legacy MVP reference only. Pipeline V2 disables the demo grenade trajectory
// PiP with sv_grenade_trajectory_prac_pipreview=0 and does not patch this gate.
inline constexpr std::uint32_t kClientGrenadePipGateRva = 0x7A6F90;
inline constexpr std::uint8_t kClientGrenadePipGatePrologue[] = {
    0x40, 0x56, 0x48, 0x83, 0xEC, 0x50};  // push rsi; sub rsp,50h
inline constexpr std::size_t kClientGrenadePipGateStolen = 6;
// CBFD10 writes "flashed" via cvttss2si([pawn+0x141c]) with NO *255, while
// "smoked" multiplies by 255 first. Overlay alpha is 0..1 → Panorama got 0/1.
// Real insn is disp32 form (8 bytes), not disp8.
inline constexpr std::uint32_t kClientFlashAmountCvtRva = 0xCC1144;
inline constexpr std::uint8_t kClientFlashAmountCvtBytes[8] = {
    0xF3, 0x0F, 0x2C, 0x93, 0x1C, 0x14, 0x00, 0x00};  // cvttss2si edx,[rbx+0x141c]
inline constexpr std::uint32_t kClientFlashAmountAfterCvtRva = 0xCC114C;
inline constexpr std::uint32_t kClientFlashScale255Rva = 0x1978EFC;  // float 255.0
// World flash overlay load @ 0x8A28FC — capture so HUD binder can match view wash.
inline constexpr std::uint32_t kClientFlashOverlayLoadRva = 0x8A3B3C;
inline constexpr std::uint8_t kClientFlashOverlayLoadBytes[8] = {
    0xF3, 0x0F, 0x10, 0x86, 0x1C, 0x14, 0x00, 0x00};  // movss xmm0,[rsi+0x141c]
inline constexpr std::uint32_t kClientFlashOverlayAfterLoadRva = 0x8A3B44;

// Main render graph has two FlashbangOverlay submissions.  The normal/live
// submission is @ 0x11321BF.  A second submission is gated by
// force_spectator_only_tools after PanoramaAlphaCopy/Setup; that spectator
// composition keeps demo/observer Panorama above the flash wash.  At the gate:
//   call IsSpectatorOnlyToolsAllowed ; test al,al ; je skip_flash
// Replace only `test al,al` with `xor al,al` so demo playback follows the live
// render-order branch while leaving the native scene FlashbangOverlay intact.
inline constexpr std::uint32_t kClientFlashSpectatorCompositeTestRva = 0x1146D14;
inline constexpr std::uint8_t kClientFlashSpectatorCompositeTestBytes[2] = {
    0x84, 0xC0};
inline constexpr std::uint8_t kClientFlashLiveCompositeBytes[2] = {
    0x30, 0xC0};

// cl_teammate_colors_show ConVar static + GetValue helper (same as 0x863350).
// 1 = colors only; 2 = colors + letter initials.
inline constexpr std::uint32_t kClientTeammateColorsCvarRva = 0x2374D60;
// cl_radar_show_all_players_when_spectating — 898740/898630; force 0 (team path).
inline constexpr std::uint32_t kClientRadarShowAllCvarRva = 0x23BC610;
inline constexpr std::uint32_t kClientConVarGetValueRva = 0x1862B30;
// Float GetValue used by flash opacity path @ 0x113E2B5.
inline constexpr std::uint32_t kClientConVarGetFloatRva = 0x165EF0;

// Deathcam gates after victim==local (NOP so follow-deathcam still plays in demo).
inline constexpr std::uint32_t kClientKillSoundCvarJneRva = 0xC830C4;
inline constexpr std::uint8_t kClientKillSoundCvarJneBytes[6] = {
    0x0F, 0x85, 0xDB, 0x08, 0x00, 0x00};
inline constexpr std::uint32_t kClientKillSoundModeJneRva = 0xC830D8;
inline constexpr std::uint8_t kClientKillSoundModeJneBytes[2] = {0x75, 0x46};
inline constexpr std::uint32_t kClientKillSoundModeJeRva = 0xC830E8;
inline constexpr std::uint8_t kClientKillSoundModeJeBytes[2] = {0x74, 0x3D};
inline constexpr std::uint32_t kClientKillSoundFallbackJeRva = 0xC83101;
inline constexpr std::uint8_t kClientKillSoundFallbackJeBytes[2] = {0x74, 0x1D};
}  // namespace offsets
