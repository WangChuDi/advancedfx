#include "stdafx.h"

#include "MirvPovRadio.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovKillReward.h"
#include "MirvPovCore.h"
#include "MirvPovSoundCircle.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../deps/release/prop/cs2/sdk_src/public/playerslot.h"
#include "../deps/release/Detours/src/detours.h"
#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/binutils.h"

#include <Windows.h>
#include <ctype.h>
#include <deque>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

// IDA's CCSUsrMsg_RadioText formatter (sub_1810D6240) is invoked by
// CGameMessageDelegateHook::Dispatch with RCX = the registered RadioText
// handler/owner object and RDX = the user-message payload.  The generic
// dispatch body only spells out the RCX call in pseudocode; RDX is intentionally
// left live from Dispatch's second argument.  Keep the pointer-width prototype
// here: narrowing RCX to int corrupts the owner before the formatter calls
// HudChat.Message and makes real demo RadioText disappear.
using RadioTextHandler_t = void (__fastcall *)(void * owner, void * message);
// RadioText is delivered through the common CGameMessageDelegateHook
// dispatcher before the typed HudChat formatter is called.  Hooking this
// entry is important for demo playback: the demo path can bypass the typed
// formatter's normal callback registration, while the delegate dispatch is
// still reached for the user message.
using RadioTextDispatch_t = __int64 (__fastcall *)(void * owner, void * message);
// CCSUsrMsg_SendAudio is delivered through the common
// CGameMessageDelegateHook::Dispatch(owner, message) entry.  In the current
// client.dll the SendAudio vtable's Dispatch slot is sub_180B348A0.  The
// generic wrapper carries the typed payload at message+0x30 and its tagged
// radio_sound field at message+0x38.
using SendAudioDispatch_t = __int64 (__fastcall *)(void * owner, void * message);
// Per-message emitter used by the protobuf user-message reader.  Its first
// argument is the temporary generic message object; field +0x10 is the
// SendAudio radio_sound tagged string before it is copied into the wrapper.
using SendAudioEmitter_t = __int64 (__fastcall *)(void * message);
// The protobuf reader's concrete SendAudio parser is a one-argument helper
// reached before the generic delegate dispatch.  IDA identifies the current
// client.dll implementation at image+0xB04D30; its CCSUsrMsg_SendAudio
// CBufferString lives at message+0x48.
using SendAudioParser_t = __int64 (__fastcall *)(void * message);
// CCSUsrMsg_RawAudio_t is dispatched through a normal (owner, message)
// formatter.  It carries voice_filename at message+0x48 and entidx at +0x58.
using RawAudioHandler_t = __int64 (__fastcall *)(void * owner, void * message);
using GetDemoController_t = unsigned char * (__fastcall *)();
using HashString_t = unsigned int (__fastcall *)(
    const char * string,
    unsigned int length,
    unsigned int lengthXorSeed);

struct RecentNativeRadio {
    ULONGLONG timeMs;
    int demoTick;
    int entityIndex;
};

struct RecentSyntheticRadio {
    ULONGLONG timeMs;
    int demoTick;
    int entityIndex;
    bool soundFallback;
};

struct RecentSoundRadio {
    ULONGLONG timeMs;
    int entityIndex;
    int slot;
};

// Native SendAudio/RawAudio is not guaranteed to arrive in the same callback
// as weapon_fire/bomb events.  Keep a short ledger so an event fallback can
// wait for the native voice before it emits a local cue.  The token is kept in
// addition to the numeric slot because RawAudio can arrive without a decoded
// SendAudio slot on some demo builds.
struct RecentNativeAudio {
    ULONGLONG timeMs;
    int demoTick;
    int entityIndex;
    int slot;
    char token[192];
};

struct PendingSyntheticAudio {
    ULONGLONG queuedMs;
    int queuedDemoTick;
    // Controller entry index is retained for native-audio dedupe.  The
    // spatialized sound path must use the pawn entry index below.
    int entityIndex;
    int sourceEntityIndex;
    int controllerHandle;
    int slot;
    char source[32];
    char token[192];
};

// RawAudio is the only demo message that normally carries the fully resolved
// agent voice filename.  Keep the most recent mapping so a later event-only
// fallback can replay the same agent family and command instead of falling
// back to the generic bot voice.  This is deliberately a bounded ledger: demo
// seeks and slot reuse must not retain stale player/agent associations.
struct ObservedAgentVoice {
    ULONGLONG timeMs;
    int demoTick;
    int controllerHandle;
    int entityIndex;
    int slot;
    char family[32];
    char cue[192];
};

struct RecentProjectile {
    ULONGLONG firstSeenMs;
    int firstDemoTick;
    int entityHandle;
    int entityIndex;
    int controllerHandle;
    int grenadeSlot;
    bool emitted;
    bool pendingLogged;
    bool eventMatched;
    char className[96];
};

// A grenade can be observed through more than one path in a demo:
// weapon_fire/grenade_thrown, entity-added, and the periodic entity scan.  A
// separate ledger ties those observations to the thrower and grenade type so
// an entity handle change (slot reuse/full update) cannot create a second
// HudChat notice.
struct RecentGrenadeThrow {
    ULONGLONG timeMs;
    int demoTick;
    int controllerHandle;
    int slot;
    bool matchedProjectile;
};

constexpr size_t kDemoHudChatSuppressOffset = 0x72;
constexpr ULONGLONG kRecentNativeWindowMs = 500;
constexpr ULONGLONG kRecentSoundWindowMs = 1200;
constexpr ULONGLONG kRecentProjectileWindowMs = 5000;
constexpr size_t kMaxRecentRadios = 16;
constexpr size_t kMaxRecentProjectiles = 64;
constexpr ULONGLONG kRecentGrenadeThrowWindowMs = 3000;
constexpr ULONGLONG kRecentGrenadeEventDedupeMs = 250;
constexpr int kRecentGrenadeThrowTickWindow = 64;
constexpr ULONGLONG kSyntheticAudioWaitMs = 180;
constexpr int kSyntheticAudioWaitTicks = 8;
constexpr ULONGLONG kRecentNativeAudioWindowMs = 800;
// RVA values from the client.dll analyzed by IDA Pro (image base
// 0x180000000).  The +0x28 slot in each table is the actual
// CGameMessageDelegateHook::Dispatch implementation for that user message.
// Keep the previous table locations as compatibility fallbacks for older
// client builds; never use a pattern from a neighboring delegate when the
// vtable slot can be read directly.
// IDA Pro (client.dll 2026-08-11): CCSUsrMsg_RadioText is registered by
// sub_1810D2FB0, whose CGameMessageDelegateHook object uses off_181B75608.
// Its +0x28 slot is sub_1810D5040 (the generic Dispatch).  0x1BB7ED0 is a
// different user-message table and never receives RadioText callbacks.
constexpr uintptr_t kRadioTextVtableRva = 0x1B75608;
// IDA Pro (client.dll 2026-08-11): the registered user-message tables are
// adjacent to RadioText.  SendAudio is off_181B756E8 and RawAudio is
// off_181B75640.  The previous source used 0x1AB9220/0x1B756E8, which swapped
// the real message tables and made the native audio delegate hooks miss the
// messages that carry the agent voice filename.
constexpr uintptr_t kSendAudioVtableRva = 0x1B756E8;
constexpr uintptr_t kSendAudioVtableLegacyRva = 0x1AB9220;
constexpr uintptr_t kRawAudioVtableRva = 0x1B75640;
constexpr uintptr_t kRawAudioVtableLegacyRva = 0x1BB7FB0;
// Direct formatter RVAs confirmed in IDA for the exact client.dll build used
// by this worktree.  The generic delegate tables above are still hooked for
// compatibility, but the formatter hooks are the authoritative typed-message
// path during demo playback.
constexpr uintptr_t kRadioTextFormatterRva = 0x10D6240;
// The shared HudChat formatter gate calls sub_180C92590() and tests byte
// +0x72 while a demo is playing. IDA confirms this getter is a tiny
// `lea rax, off_18207CC60; ret` at image+0xC92590; the older signature below
// expected padding bytes that are not present in the current client.dll and
// consequently left g_GetDemoController null.
constexpr uintptr_t kDemoControllerRva = 0xC92590;
constexpr uintptr_t kSendAudioParserRva = 0xB04D30;
constexpr uintptr_t kRawAudioFormatterRva = 0x10D6A50;

RadioTextHandler_t g_OrgRadioTextHandler = nullptr;
RadioTextDispatch_t g_OrgRadioTextDispatch = nullptr;
SendAudioDispatch_t g_OrgSendAudioDispatch = nullptr;
SendAudioEmitter_t g_OrgSendAudioEmitter = nullptr;
SendAudioParser_t g_OrgSendAudioParser = nullptr;
RawAudioHandler_t g_OrgRawAudioHandler = nullptr;
RawAudioHandler_t g_OrgRawAudioFormatter = nullptr;
const void * g_RadioTextVtable = nullptr;
const void * g_SendAudioVtable = nullptr;
const void * g_RawAudioVtable = nullptr;
GetDemoController_t g_GetDemoController = nullptr;
HashString_t g_HashString = nullptr;
bool g_Hooked = false;
bool g_RadioTextHooked = false;
bool g_RadioTextDispatchHooked = false;
bool g_SendAudioHooked = false;
bool g_SendAudioEmitterHooked = false;
bool g_SendAudioParserHooked = false;
bool g_RawAudioHooked = false;
bool g_RawAudioFormatterHooked = false;
volatile LONG g_RadioMode = 4;
// Normal demo playback has a reliable weapon_fire event for grenade throws.
// Entity scanning is an emergency fallback only; keeping it off by default
// prevents a persistent projectile entity from replaying the same notice.
volatile LONG g_ProjectileScanFallback = 0;
volatile LONG g_SyntheticAudioEnabled = 1;
// 0 matches native SendAudio/RawAudio (entidx=-1, global playback).  1 keeps
// the earlier optional player-pawn spatialized fallback for comparison.
volatile LONG g_SyntheticAudioSpatialized = 0;
SRWLOCK g_DemoGuardLock = SRWLOCK_INIT;
SRWLOCK g_StateLock = SRWLOCK_INIT;
thread_local LONG g_RadioDispatchDepth = 0;
int g_LastDemoTick = -1;
std::deque<RecentNativeRadio> g_RecentNativeRadios;
std::deque<RecentSyntheticRadio> g_RecentSyntheticRadios;
std::deque<RecentSoundRadio> g_RecentSoundRadios;
std::deque<RecentProjectile> g_RecentProjectiles;
std::deque<RecentGrenadeThrow> g_RecentGrenadeThrows;
std::deque<RecentNativeAudio> g_RecentNativeAudios;
std::deque<PendingSyntheticAudio> g_PendingSyntheticAudios;
std::deque<ObservedAgentVoice> g_ObservedAgentVoices;
bool g_ProjectileScanPrimed = false;
int g_LastNativeClientIndex = -1;
ULONGLONG g_LastNativeRadioTimeMs = 0;
char g_LastSoundName[192] = "none";
char g_LastSendAudioToken[192] = "none";
int g_LastSendAudioSlot = -1;
ULONGLONG g_LastSendAudioTimeMs = 0;
int g_LastSoundControllerEntityIndex = -1;
int g_LastSoundControllerSlot = -1;
ULONGLONG g_LastSoundControllerTimeMs = 0;
const void * g_LastGameEventPointer = nullptr;
ULONGLONG g_LastGameEventTimeMs = 0;
char g_LastGameEventName[64] = "none";
char g_LastAgentFamily[32] = "none";

bool IsExpectedDelegateOwner(void * owner, const void * expectedVtable)
{
    if(nullptr == owner) return false;
    __try {
        const void * actualVtable = *reinterpret_cast<void **>(owner);
        // If a future build does not expose the expected RTTI/vtable RVA,
        // retain the hook as a best-effort fallback rather than suppressing
        // every native message.  For the analyzed build this is an exact
        // owner filter and prevents cloned dispatchers from being confused.
        return nullptr == expectedVtable || actualVtable == expectedVtable;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t ReadOwnerVtable(void * owner)
{
    if(nullptr == owner) return 0;
    __try {
        return reinterpret_cast<uintptr_t>(*reinterpret_cast<void **>(owner));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadDelegateDispatch(const void * vtable, const Afx::BinUtils::MemRange & textRange, size_t & address)
{
    address = 0;
    if(nullptr == vtable) return false;
    __try {
        const void * const * slots = reinterpret_cast<const void * const *>(vtable);
        const size_t candidate = reinterpret_cast<size_t>(slots[5]); // vtable + 0x28
        if(candidate < textRange.Start || textRange.End <= candidate) return false;
        address = candidate;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int GetRadioMode()
{
    return static_cast<int>(InterlockedCompareExchange(&g_RadioMode, 0, 0));
}

bool IsProjectileScanFallbackEnabled()
{
    return 0 != InterlockedCompareExchange(&g_ProjectileScanFallback, 0, 0);
}

bool IsSyntheticAudioEnabled()
{
    return 0 != InterlockedCompareExchange(&g_SyntheticAudioEnabled, 0, 0);
}

bool IsSyntheticAudioSpatialized()
{
    return 0 != InterlockedCompareExchange(&g_SyntheticAudioSpatialized, 0, 0);
}

const char * GetRadioModeDescription(int mode)
{
    switch(mode) {
    case 0: return "off (suppress native and event Radio)";
    case 1: return "native RadioText only";
    case 2: return "game-event synthetic, POV team only";
    case 3: return "game-event synthetic, all T/CT players";
    case 4: return "auto: native, game-event (projectile event-only), then sound fallback";
    case 5: return "sound-event plus game-event fallback, POV team only";
    case 6: return "sound-event plus game-event fallback, all T/CT players";
    default: return "invalid";
    }
}

const char * GetPlayerRadioText(int slot)
{
    switch(slot) {
    case 2: return "Go go go!";
    case 3: return "Fall back!";
    case 4: return "Stick together, team.";
    case 5: return "Hold this position.";
    case 6: return "Follow me.";
    case 8: return "Affirmative.";
    case 9: return "Negative.";
    case 10: return "Cheer!";
    case 11: return "Nice!";
    case 12: return "Thanks!";
    case 14: return "Enemy spotted.";
    case 15: return "Need backup!";
    case 16: return "You take the point.";
    case 17: return "Sector clear.";
    case 18: return "I'm in position.";
    case 19: return "Cover me.";
    case 20: return "Regroup, team.";
    case 21: return "Taking fire, need assistance!";
    case 22: return "Report in, team.";
    case 23: return "Reporting in.";
    case 24: return "Get out of there!";
    case 25: return "Enemy down.";
    default: return nullptr;
    }
}
const unsigned char kSmokeUtf8[] = {
    0xe6, 0x8a, 0x95, 0xe6, 0x8e, 0xb7, 0xe7, 0x83,
    0x9f, 0xe9, 0x9b, 0xbe, 0xe5, 0xbc, 0xb9, 0xef, 0xbc, 0x81, 0
};
const unsigned char kFlashUtf8[] = {
    0xe6, 0x8a, 0x95, 0xe6, 0x8e, 0xb7, 0xe9, 0x97,
    0xaa, 0xe5, 0x85, 0x89, 0xe5, 0xbc, 0xb9, 0xef, 0xbc, 0x81, 0
};
const unsigned char kHeUtf8[] = {
    0xe6, 0x8a, 0x95, 0xe6, 0x8e, 0xb7, 0xe9, 0xab,
    0x98, 0xe7, 0x88, 0x86, 0xe6, 0x89, 0x8b, 0xe9,
    0x9b, 0xb7, 0xef, 0xbc, 0x81, 0
};
const unsigned char kMolotovUtf8[] = {
    0xe6, 0x8a, 0x95, 0xe6, 0x8e, 0xb7, 0xe7, 0x87,
    0x83, 0xe7, 0x83, 0xa7, 0xe7, 0x93, 0xb6, 0xef, 0xbc, 0x81, 0
};
const unsigned char kIncendiaryUtf8[] = {
    0xe6, 0x8a, 0x95, 0xe6, 0x8e, 0xb7, 0xe7, 0x87,
    0x83, 0xe7, 0x83, 0xa7, 0xe5, 0xbc, 0xb9, 0xef, 0xbc, 0x81, 0
};
const unsigned char kDecoyUtf8[] = {
    0xe6, 0x8a, 0x95, 0xe6, 0x8e, 0xb7, 0xe8, 0xaf,
    0xb1, 0xe9, 0xa5, 0xb5, 0xe5, 0xbc, 0xb9, 0xef, 0xbc, 0x81, 0
};
const unsigned char kFullWidthColonUtf8[] = {0xef, 0xbc, 0x9a, 0};
const unsigned char kBombPlantingTextZh[] = {
    0xe6, 0xad, 0xa3, 0xe5, 0x9c, 0xa8, 0xe5, 0xae, 0x89,
    0xe6, 0x94, 0xbe, 0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0
};
const unsigned char kBombPlantedTextZh[] = {
    0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0xe5, 0xb7, 0xb2,
    0xe5, 0xae, 0x89, 0xe6, 0x94, 0xbe, 0
};
const unsigned char kBombDefusingTextZh[] = {
    0xe6, 0xad, 0xa3, 0xe5, 0x9c, 0xa8, 0xe6, 0x8b, 0x86,
    0xe9, 0x99, 0xa4, 0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0
};
const unsigned char kBombAbortDefuseTextZh[] = {
    0xe6, 0x8b, 0x86, 0xe5, 0x8c, 0x85, 0xe5, 0xb7, 0xb2,
    0xe4, 0xb8, 0xad, 0xe6, 0xad, 0xa2, 0
};
const unsigned char kBombDefusedTextZh[] = {
    0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0xe5, 0xb7, 0xb2,
    0xe6, 0x8b, 0x86, 0xe9, 0x99, 0xa4, 0
};
const unsigned char kBombExplodedTextZh[] = {
    0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0xe5, 0xb7, 0xb2,
    0xe7, 0x88, 0x86, 0xe7, 0x82, 0xb8, 0
};
const unsigned char kBombDroppedTextZh[] = {
    0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0xe5, 0xb7, 0xb2,
    0xe6, 0x8e, 0x89, 0xe8, 0x90, 0xbd, 0
};
const unsigned char kBombPickupTextZh[] = {
    0xe6, 0x88, 0x91, 0xe6, 0x8b, 0xbf, 0xe5, 0x88, 0xb0,
    0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0xe4, 0xba, 0x86, 0
};
const unsigned char kBombSiteTextZh[] = {
    0xe7, 0x82, 0xb8, 0xe5, 0xbc, 0xb9, 0xe5, 0x8c, 0xba, 0
};
const char kBombPlantingTextEn[] = "I'm planting the bomb.";
const char kBombPlantedTextEn[] = "Bomb has been planted.";
const char kBombDefusingTextEn[] = "I'm defusing the bomb.";
const char kBombAbortDefuseTextEn[] = "Defuse aborted.";
const char kBombDefusedTextEn[] = "Bomb has been defused.";
const char kBombExplodedTextEn[] = "Bomb has exploded.";
const char kBombDroppedTextEn[] = "Bomb has been dropped.";
const char kBombPickupTextEn[] = "I have the bomb.";
const char kLockAndLoadText[] = "Lock and load.";
const char kOnMyWayText[] = "On my way.";
const char kBombSiteTextEn[] = "Bombsite.";
const char kFireInTheHoleText[] = "Fire in the hole!";

bool UseSimplifiedChineseRadioText();

const char * SelectRadioText(const char * english, const unsigned char * simplifiedChinese)
{
    return UseSimplifiedChineseRadioText()
        ? reinterpret_cast<const char *>(simplifiedChinese)
        : english;
}

const char * GetBombPlantingText() { return SelectRadioText(kBombPlantingTextEn, kBombPlantingTextZh); }
const char * GetBombPlantedText() { return SelectRadioText(kBombPlantedTextEn, kBombPlantedTextZh); }
const char * GetBombDefusingText() { return SelectRadioText(kBombDefusingTextEn, kBombDefusingTextZh); }
const char * GetBombAbortDefuseText() { return SelectRadioText(kBombAbortDefuseTextEn, kBombAbortDefuseTextZh); }
const char * GetBombDefusedText() { return SelectRadioText(kBombDefusedTextEn, kBombDefusedTextZh); }
const char * GetBombExplodedText() { return SelectRadioText(kBombExplodedTextEn, kBombExplodedTextZh); }
const char * GetBombDroppedText() { return SelectRadioText(kBombDroppedTextEn, kBombDroppedTextZh); }
const char * GetBombPickupText() { return SelectRadioText(kBombPickupTextEn, kBombPickupTextZh); }
const char * GetBombSiteText() { return SelectRadioText(kBombSiteTextEn, kBombSiteTextZh); }

// m_szLastPlaceName is stored as the English map token even when the client
// language is Simplified Chinese.  Native RadioText normally localizes this
// token internally; synthetic notices need to do the same before composing
// the `@location` portion of the HudChat line.
struct PlaceTranslation {
    const char * english;
    const char * simplifiedChinese;
};

const char * TranslatePlaceName(const char * placeName)
{
    if(nullptr == placeName || !UseSimplifiedChineseRadioText()) return nullptr;

    static const PlaceTranslation translations[] = {
        {"Arch", "\xE6\x8B\xB1\xE9\x97\xA8"},
        {"Library", "\xE5\x9B\xBE\xE4\xB9\xA6\xE9\xA6\x86"},
        {"BombsiteA", "A\xE5\x8C\x85\xE7\x82\xB9"},
        {"BombsiteB", "B\xE5\x8C\x85\xE7\x82\xB9"},
        {"Bombsite_A", "A\xE5\x8C\x85\xE7\x82\xB9"},
        {"Bombsite_B", "B\xE5\x8C\x85\xE7\x82\xB9"},
        {"Bombsite A", "A\xE5\x8C\x85\xE7\x82\xB9"},
        {"Bombsite B", "B\xE5\x8C\x85\xE7\x82\xB9"},
        {"Banana", "\xE9\xA6\x99\xE8\x95\x89\xE9\x81\x93"},
        {"CTSpawn", "CT\xE5\x87\xBA\xE7\x94\x9F\xE7\x82\xB9"},
        {"TSpawn", "T\xE5\x87\xBA\xE7\x94\x9F\xE7\x82\xB9"},
        {"CT_Spawn", "CT\xE5\x87\xBA\xE7\x94\x9F\xE7\x82\xB9"},
        {"T_Spawn", "T\xE5\x87\xBA\xE7\x94\x9F\xE7\x82\xB9"},
        {"TopofMid", "\xE4\xB8\xAD\xE8\xB7\xAF\xE4\xB8\x8A\xE6\x96\xB9"},
        {"Top of Mid", "\xE4\xB8\xAD\xE8\xB7\xAF\xE4\xB8\x8A\xE6\x96\xB9"},
        {"TopMid", "\xE4\xB8\xAD\xE8\xB7\xAF\xE4\xB8\x8A\xE6\x96\xB9"},
        {"LowerMid", "\xE4\xB8\xAD\xE8\xB7\xAF\xE4\xB8\x8B\xE6\x96\xB9"},
        {"Mid", "\xE4\xB8\xAD\xE8\xB7\xAF"},
        {"Catwalk", "\xE7\x8C\xAB\xE9\x81\x93"},
        {"Short", "\xE7\x9F\xAD\xE9\x81\x93"},
        {"Long", "\xE9\x95\xBF\xE9\x81\x93"},
        {"APlatform", "A\xE5\xB9\xB3\xE5\x8F\xB0"},
        {"Ramp", "\xE5\x9D\xA1\xE9\x81\x93"},
        {"Palace", "\xE7\x9A\x87\xE5\xAE\xAB"},
        {"Ticket", "\xE5\x94\xAE\xE7\xA5\xA8\xE5\xA4\x84"},
        {"Jungle", "\xE4\xB8\x9B\xE6\x9E\x97"},
        {"Connector", "\xE8\xBF\x9E\xE6\x8E\xA5\xE9\x81\x93"},
        {"Stairs", "\xE6\xA5\xBC\xE6\xA2\xAF"},
        {"Underpass", "\xE4\xB8\x8B\xE6\xB0\xB4\xE9\x81\x93"},
        {"Market", "\xE5\xB8\x82\xE5\x9C\xBA"},
        {"Apartments", "\xE5\x85\xAC\xE5\xAF\x93"},
        {"Pit", "\xE5\x9D\x91"},
        {"Car", "\xE6\xB1\xBD\xE8\xBD\xA6"},
        {"Default", "\xE9\xBB\x98\xE8\xAE\xA4"},
        {"Truck", "\xE5\x8D\xA1\xE8\xBD\xA6"},
        {"Ninja", "\xE5\xBF\x8D\xE8\x80\x85\xE4\xBD\x8D"},
        {"Heaven", "\xE5\xA4\xA9\xE5\xA0\x82"},
        {"Hell", "\xE5\x9C\xB0\xE7\x8B\xB1"},
        {"Backsite", "\xE5\x90\x8E\xE7\x82\xB9"},
        {"Frontsite", "\xE5\x89\x8D\xE7\x82\xB9"},
        {"Window", "\xE7\xAA\x97\xE5\x8F\xA3"},
        {"Door", "\xE9\x97\xA8"},
        {"Quad", "\xE5\x9B\x9B\xE7\xAE\xB1"},
        {"Generator", "\xE5\x8F\x91\xE7\x94\xB5\xE6\x9C\xBA"},
        {"Logs", "\xE6\x9C\xA8\xE7\xAE\xB1"},
        {"Ruins", "\xE9\x81\x97\xE8\xBF\xB9"},
        {"Cave", "\xE6\xB4\x9E\xE7\xA9\xB4"},
        {"Dark", "\xE9\xBB\x91\xE5\xB1\x8B"},
        {"Secret", "\xE6\x9A\x97\xE9\x81\x93"}
    };
    for(const PlaceTranslation & translation : translations) {
        if(0 == _stricmp(placeName, translation.english))
            return translation.simplifiedChinese;
    }
    return nullptr;
}

SOURCESDK::CS2::GameEventKeySymbol_t MakeKey(const char * name)
{
    size_t length = strlen(name);
    unsigned int hash = 0;
    if(nullptr != g_HashString) {
        hash = g_HashString(
            name,
            static_cast<unsigned int>(length),
            static_cast<unsigned int>(length) ^ 0x31415926);
    } else {
        MirvPovKillReward_HashGameEventKey(name, hash);
    }
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

bool HasGameEventHash()
{
    return nullptr != g_HashString || MirvPovKillReward_IsAvailable();
}

int GetControllerHandle(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return -1;
    auto handle = controller->GetHandle();
    return handle.IsValid() ? handle.ToInt() : -1;
}

int GetEntityIndex(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return -1;
    auto handle = controller->GetHandle();
    return handle.IsValid() ? handle.GetEntryIndex() : -1;
}

CEntityInstance * GetControllerFromClientSlot(int clientSlot);

bool IsPlayableTeam(int team)
{
    return 2 == team || 3 == team;
}

int ReadEntityTeam(CEntityInstance * entity)
{
    if(nullptr == entity) return 0;
    __try {
        int team = entity->GetTeam();
        return IsPlayableTeam(team) ? team : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// During demo playback the controller's m_iTeamNum can briefly be 0/1 while
// its pawn already has the authoritative T/CT value.  Native RadioText uses
// the pawn-backed value as well, so resolve it before applying POV filtering or
// choosing the [T]/[CT] prefix.
int ResolveControllerTeam(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return 0;

    int team = ReadEntityTeam(controller);
    if(IsPlayableTeam(team)) return team;

    __try {
        auto pawnHandle = controller->GetPlayerPawnHandle();
        if(pawnHandle.IsValid()) {
            CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
            team = ReadEntityTeam(pawn);
            if(IsPlayableTeam(team)) return team;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

// PushHudNotice's entity argument is a client slot (the same 0..63 value that
// CCSUsrMsg_RadioText carries at message+0x6c), not the controller entity
// index.  Passing the latter makes the native 0x03 player-color marker resolve
// to purple/no dot for some players.  Resolve the slot explicitly and only use
// the entity index for internal dedupe ledgers.
int GetControllerClientSlot(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return -1;

    int controllerEntityIndex = GetEntityIndex(controller);
    for(int slot = 0; slot < 64; ++slot) {
        CEntityInstance * candidate = GetControllerFromClientSlot(slot);
        if(nullptr == candidate) continue;
        if(candidate == controller) return slot;
        if(0 <= controllerEntityIndex && GetEntityIndex(candidate) == controllerEntityIndex)
            return slot;
    }

    __try {
        auto handle = controller->GetHandle();
        if(handle.IsValid()) {
            int entryIndex = handle.GetEntryIndex();
            if(1 <= entryIndex && entryIndex <= 64) return entryIndex - 1;
            if(0 <= entryIndex && entryIndex < 64) return entryIndex;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

CEntityInstance * GetControllerFromClientSlot(int clientSlot)
{
    if(clientSlot < 0 || 64 <= clientSlot) return nullptr;
    const int candidateIndices[] = {clientSlot + 1, clientSlot};
    for(int candidateIndex : candidateIndices) {
        CEntityInstance * candidate = GetEntityFromIndex(candidateIndex);
        if(nullptr != candidate && candidate->IsPlayerController()) return candidate;
    }
    return nullptr;
}

CEntityInstance * GetControllerFromPawn(CEntityInstance * pawn);

// Demo game-events are not consistent about the type of the `userid` key:
// depending on whether the record came from FireEvent or FireEventClientSide
// it can be exposed as a CPlayerSlot, a controller handle, or an entity index.
// Normalize the value before resolving the sender so player_radio does not get
// discarded merely because GetPlayerController returned a stale/null pointer.
int GetEventClientSlot(
    SOURCESDK::CS2::IGameEvent * event,
    const SOURCESDK::CS2::GameEventKeySymbol_t & key)
{
    if(nullptr == event) return -1;

    int slot = -1;
    __try {
        slot = event->GetPlayerSlot(key).Get();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        slot = -1;
    }
    if(0 <= slot && slot < 64) return slot;

    int raw = -1;
    __try {
        raw = event->GetInt(key);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        raw = -1;
    }
    if(0 <= raw && raw < 64) return raw;
    if(1 <= raw && raw <= 64) return raw - 1;

    // A serialized controller handle carries the entry index in its low
    // 15 bits.  This fallback is intentionally conservative: only values that
    // map to a live player controller are accepted by the caller.
    if(0 <= raw) {
        int entryIndex = raw & 0x7fff;
        if(0 <= entryIndex && entryIndex < 64) return entryIndex;
        if(1 <= entryIndex && entryIndex <= 64) return entryIndex - 1;
    }
    return -1;
}

CEntityInstance * ResolveEventController(
    SOURCESDK::CS2::IGameEvent * event,
    const SOURCESDK::CS2::GameEventKeySymbol_t & key)
{
    if(nullptr == event) return nullptr;
    CEntityInstance * controller = nullptr;
    CEntityInstance * pawnController = nullptr;
    CEntityInstance * slotController = nullptr;
    __try {
        controller = reinterpret_cast<CEntityInstance *>(event->GetPlayerController(key));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        controller = nullptr;
    }
    // Some demo event records expose only the pawn side of userid. Resolve its
    // controller before falling back to a slot scan.
    __try {
        CEntityInstance * pawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(key));
        if(nullptr != pawn && pawn->IsPlayerPawn())
            pawnController = GetControllerFromPawn(pawn);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        pawnController = nullptr;
    }
    slotController = GetControllerFromClientSlot(GetEventClientSlot(event, key));

    if(nullptr != controller && controller->IsPlayerController()) {
        // Some demo event records carry a stale controller pointer while the
        // slot lookup already points at the live controller/pawn pair. Prefer
        // the slot controller when it has a resolvable T/CT team.
        if(IsPlayableTeam(ResolveControllerTeam(controller))) return controller;
        if(nullptr != pawnController && IsPlayableTeam(ResolveControllerTeam(pawnController)))
            return pawnController;
        if(nullptr != slotController && IsPlayableTeam(ResolveControllerTeam(slotController)))
            return slotController;
        return controller;
    }
    if(nullptr != pawnController && pawnController->IsPlayerController())
        return pawnController;
    return slotController;
}

CEntityInstance * GetControllerFromPawn(CEntityInstance * pawn)
{
    if(nullptr == pawn || !pawn->IsPlayerPawn()) return nullptr;
    auto handle = pawn->GetPlayerControllerHandle();
    if(!handle.IsValid()) return nullptr;
    CEntityInstance * controller = GetEntityFromIndex(handle.GetEntryIndex());
    return nullptr != controller && controller->IsPlayerController() ? controller : nullptr;
}

CEntityInstance * GetPawnFromControllerForAudio(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    __try {
        auto pawnHandle = controller->GetPlayerPawnHandle();
        if(!pawnHandle.IsValid()) return nullptr;
        CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
        return nullptr != pawn && pawn->IsPlayerPawn() ? pawn : nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int GetAudioSourceEntityIndex(CEntityInstance * controller)
{
    CEntityInstance * pawn = GetPawnFromControllerForAudio(controller);
    if(nullptr == pawn) return -1;
    __try {
        auto handle = pawn->GetHandle();
        return handle.IsValid() ? handle.GetEntryIndex() : -1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool CopyControllerLocation(CEntityInstance * controller, char * output, size_t outputSize)
{
    if(nullptr == output || 0 == outputSize) return false;
    output[0] = '\0';
    if(nullptr == controller || !controller->IsPlayerController()
        || g_clientDllOffsets.C_CSPlayerPawn.m_szLastPlaceName < 0) return false;

    __try {
        auto pawnHandle = controller->GetPlayerPawnHandle();
        if(!pawnHandle.IsValid()) return false;
        CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
        if(nullptr == pawn || !pawn->IsPlayerPawn()) return false;

        const char * location = reinterpret_cast<const char *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_szLastPlaceName);
        size_t sourceLength = strnlen_s(location, 18);
        if(0 == sourceLength || 18 <= sourceLength) return false;
        const char * translatedLocation = TranslatePlaceName(location);
        if(nullptr != translatedLocation) location = translatedLocation;
        size_t length = strnlen_s(location, outputSize);
        if(0 == length || outputSize <= length) return false;
        size_t copyLength = length < outputSize - 1 ? length : outputSize - 1;
        memcpy(output, location, copyLength);
        output[copyLength] = '\0';
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

CEntityInstance * GetStablePovController()
{
    CEntityInstance * current = GetCurrentPovPlayerController();
    int trackedHandle = MirvPovKillReward_GetTrackedPovControllerHandle();
    if(0 <= trackedHandle) {
        CEntityInstance * tracked = GetEntityFromIndex(trackedHandle & 0x7fff);
        if(nullptr != tracked && tracked->IsPlayerController()) return tracked;
    }
    return current;
}

bool IsNativeRadioVisibleForCurrentPov(int clientSlot)
{
    if(clientSlot < 0) return true;

    __try {
        CEntityInstance * sender = GetControllerFromClientSlot(clientSlot);
        CEntityInstance * povController = GetStablePovController();
        if(nullptr == sender || !sender->IsPlayerController()
            || nullptr == povController || !povController->IsPlayerController())
            return true;

        int senderTeam = ResolveControllerTeam(sender);
        int povTeam = ResolveControllerTeam(povController);
        if(!IsPlayableTeam(senderTeam) || !IsPlayableTeam(povTeam)) return true;
        return senderTeam == povTeam;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void PruneRecentNativeRadios(ULONGLONG now)
{
    while(!g_RecentNativeRadios.empty()
        && now - g_RecentNativeRadios.front().timeMs > kRecentNativeWindowMs) {
        g_RecentNativeRadios.pop_front();
    }
    while(!g_RecentSyntheticRadios.empty()
        && now - g_RecentSyntheticRadios.front().timeMs > kRecentNativeWindowMs) {
        g_RecentSyntheticRadios.pop_front();
    }
}

bool ConsumeRecentNativeForEvent(int entityIndex, const char * source)
{
    ULONGLONG now = GetTickCount64();
    bool covered = false;
    RecentNativeRadio native = {};

    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeRadios(now);
    for(auto it = g_RecentNativeRadios.begin(); it != g_RecentNativeRadios.end(); ++it) {
        bool entityMatches = 0 <= entityIndex
            && 0 <= it->entityIndex
            && entityIndex == it->entityIndex;
        bool tickMatches = g_LastDemoTick < 0
            || it->demoTick < 0
            || abs(g_LastDemoTick - it->demoTick) <= 2;
        if(!entityMatches || !tickMatches) continue;
        native = *it;
        g_RecentNativeRadios.erase(it);
        covered = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_StateLock);

    return covered;
}

bool ConsumeRecentSyntheticForNative(int entityIndex)
{
    ULONGLONG now = GetTickCount64();
    bool covered = false;
    RecentSyntheticRadio synthetic = {};

    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeRadios(now);
    for(auto it = g_RecentSyntheticRadios.begin(); it != g_RecentSyntheticRadios.end(); ++it) {
        bool entityMatches = 0 <= entityIndex
            && 0 <= it->entityIndex
            && entityIndex == it->entityIndex;
        bool tickMatches = g_LastDemoTick < 0
            || it->demoTick < 0
            || abs(g_LastDemoTick - it->demoTick) <= 2;
        if(!entityMatches || !tickMatches) continue;
        synthetic = *it;
        g_RecentSyntheticRadios.erase(it);
        covered = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_StateLock);

    return covered;
}

void RecordNativeRadio(int entityIndex)
{
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeRadios(now);
    g_RecentNativeRadios.push_back({now, g_LastDemoTick, entityIndex});
    while(kMaxRecentRadios < g_RecentNativeRadios.size())
        g_RecentNativeRadios.pop_front();
    ReleaseSRWLockExclusive(&g_StateLock);

    g_LastNativeClientIndex = entityIndex;
    g_LastNativeRadioTimeMs = now;
}

void RecordSyntheticRadio(int entityIndex, bool soundFallback)
{
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeRadios(now);
    g_RecentSyntheticRadios.push_back({now, g_LastDemoTick, entityIndex, soundFallback});
    while(kMaxRecentRadios < g_RecentSyntheticRadios.size())
        g_RecentSyntheticRadios.pop_front();
    ReleaseSRWLockExclusive(&g_StateLock);
}

bool ConsumeRecentSyntheticForFallback(int entityIndex, const char * source)
{
    ULONGLONG now = GetTickCount64();
    bool covered = false;

    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeRadios(now);
    for(auto it = g_RecentSyntheticRadios.begin(); it != g_RecentSyntheticRadios.end(); ++it) {
        if(it->soundFallback) continue;
        bool entityMatches = 0 <= entityIndex
            && 0 <= it->entityIndex
            && entityIndex == it->entityIndex;
        bool tickMatches = g_LastDemoTick < 0
            || it->demoTick < 0
            || abs(g_LastDemoTick - it->demoTick) <= 2;
        if(!entityMatches || !tickMatches) continue;
        g_RecentSyntheticRadios.erase(it);
        covered = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_StateLock);

    return covered;
}

bool IsDuplicateSoundRadio(int entityIndex, int slot)
{
    ULONGLONG now = GetTickCount64();
    bool duplicate = false;

    AcquireSRWLockExclusive(&g_StateLock);
    while(!g_RecentSoundRadios.empty()
        && now - g_RecentSoundRadios.front().timeMs > kRecentSoundWindowMs) {
        g_RecentSoundRadios.pop_front();
    }
    for(const RecentSoundRadio & recent : g_RecentSoundRadios) {
        if(recent.entityIndex == entityIndex && recent.slot == slot) {
            duplicate = true;
            break;
        }
    }
    if(!duplicate) {
        g_RecentSoundRadios.push_back({now, entityIndex, slot});
        while(kMaxRecentRadios < g_RecentSoundRadios.size())
            g_RecentSoundRadios.pop_front();
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return duplicate;
}

bool IsRecentRadio(int entityIndex, int slot)
{
    if(entityIndex < 0 || slot < 0) return false;
    ULONGLONG now = GetTickCount64();
    bool duplicate = false;

    AcquireSRWLockExclusive(&g_StateLock);
    while(!g_RecentSoundRadios.empty()
        && now - g_RecentSoundRadios.front().timeMs > kRecentSoundWindowMs) {
        g_RecentSoundRadios.pop_front();
    }
    for(const RecentSoundRadio & recent : g_RecentSoundRadios) {
        if(recent.entityIndex == entityIndex && recent.slot == slot) {
            duplicate = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return duplicate;
}

void RecordRecentRadio(int entityIndex, int slot)
{
    if(entityIndex < 0 || slot < 0) return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_StateLock);
    while(!g_RecentSoundRadios.empty()
        && now - g_RecentSoundRadios.front().timeMs > kRecentSoundWindowMs) {
        g_RecentSoundRadios.pop_front();
    }
    g_RecentSoundRadios.push_back({now, entityIndex, slot});
    while(kMaxRecentRadios < g_RecentSoundRadios.size())
        g_RecentSoundRadios.pop_front();
    ReleaseSRWLockExclusive(&g_StateLock);
}

void PruneRecentProjectiles(ULONGLONG now, int demoTick)
{
    for(auto it = g_RecentProjectiles.begin(); it != g_RecentProjectiles.end();) {
        bool expiredByTime = now - it->firstSeenMs > kRecentProjectileWindowMs;
        bool expiredByTick = 0 <= demoTick
            && 0 <= it->firstDemoTick
            && demoTick >= it->firstDemoTick + 128;
        if(expiredByTime || expiredByTick) it = g_RecentProjectiles.erase(it);
        else ++it;
    }
}

RecentProjectile * FindRecentProjectileLocked(int entityHandle);

void PruneRecentGrenadeThrowsLocked(ULONGLONG now, int demoTick)
{
    for(auto it = g_RecentGrenadeThrows.begin(); it != g_RecentGrenadeThrows.end();) {
        bool expiredByTime = now - it->timeMs > kRecentGrenadeThrowWindowMs;
        bool expiredByTick = 0 <= demoTick
            && 0 <= it->demoTick
            && demoTick > it->demoTick + kRecentGrenadeThrowTickWindow;
        if(expiredByTime || expiredByTick) it = g_RecentGrenadeThrows.erase(it);
        else ++it;
    }
}

bool SameGrenadeThrow(int controllerHandle, int slot, int demoTick, ULONGLONG now,
    const RecentGrenadeThrow & recent)
{
    if(controllerHandle < 0 || slot < 0
        || recent.controllerHandle != controllerHandle
        || recent.slot != slot) return false;
    if(now - recent.timeMs > kRecentGrenadeThrowWindowMs) return false;
    if(0 <= demoTick && 0 <= recent.demoTick
        && abs(demoTick - recent.demoTick) > kRecentGrenadeThrowTickWindow) return false;
    return true;
}

bool RecordGrenadeThrowEvent(int controllerHandle, int slot)
{
    if(controllerHandle < 0 || slot < 0) return false;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentGrenadeThrowsLocked(now, g_LastDemoTick);
    // FireEvent and FireEventClientSide can report the same throw twice.  A
    // very short event-only coalescing window removes that duplicate without
    // swallowing two legitimate same-type throws separated by gameplay time.
    for(const RecentGrenadeThrow & recent : g_RecentGrenadeThrows) {
        if(SameGrenadeThrow(controllerHandle, slot, g_LastDemoTick, now, recent)
            && now - recent.timeMs <= kRecentGrenadeEventDedupeMs) {
            ReleaseSRWLockExclusive(&g_StateLock);
            return false;
        }
    }
    g_RecentGrenadeThrows.push_back({now, g_LastDemoTick, controllerHandle, slot, false});
    while(kMaxRecentProjectiles < g_RecentGrenadeThrows.size())
        g_RecentGrenadeThrows.pop_front();
    ReleaseSRWLockExclusive(&g_StateLock);
    return true;
}

bool ConsumeGrenadeThrowEventForProjectile(int controllerHandle, int slot)
{
    if(controllerHandle < 0 || slot < 0) return false;
    ULONGLONG now = GetTickCount64();
    bool matched = false;
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentGrenadeThrowsLocked(now, g_LastDemoTick);
    for(RecentGrenadeThrow & recent : g_RecentGrenadeThrows) {
        if(recent.matchedProjectile) continue;
        if(!SameGrenadeThrow(controllerHandle, slot, g_LastDemoTick, now, recent)) continue;
        recent.matchedProjectile = true;
        matched = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return matched;
}

bool ConsumeProjectileEmissionForGrenadeEvent(int controllerHandle, int slot)
{
    if(controllerHandle < 0 || slot < 0) return false;
    ULONGLONG now = GetTickCount64();
    bool matched = false;
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentProjectiles(now, g_LastDemoTick);
    for(RecentProjectile & recent : g_RecentProjectiles) {
        if(!recent.emitted
            || recent.eventMatched
            || recent.controllerHandle != controllerHandle
            || recent.grenadeSlot != slot) continue;
        if(now - recent.firstSeenMs > kRecentGrenadeThrowWindowMs) continue;
        if(0 <= g_LastDemoTick && 0 <= recent.firstDemoTick
            && abs(g_LastDemoTick - recent.firstDemoTick) > kRecentGrenadeThrowTickWindow) continue;
        recent.eventMatched = true;
        matched = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return matched;
}

bool MarkProjectileThrower(int entityHandle, int controllerHandle, int slot)
{
    if(entityHandle < 0) return false;
    bool updated = false;
    AcquireSRWLockExclusive(&g_StateLock);
    if(RecentProjectile * recent = FindRecentProjectileLocked(entityHandle)) {
        recent->controllerHandle = controllerHandle;
        recent->grenadeSlot = slot;
        updated = true;
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return updated;
}

bool MarkProjectilePendingLogged(int entityHandle)
{
    if(entityHandle < 0) return false;
    bool shouldLog = false;
    AcquireSRWLockExclusive(&g_StateLock);
    if(RecentProjectile * recent = FindRecentProjectileLocked(entityHandle)) {
        shouldLog = !recent->pendingLogged;
        recent->pendingLogged = true;
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return shouldLog;
}

RecentProjectile * FindRecentProjectileLocked(int entityHandle)
{
    for(RecentProjectile & recent : g_RecentProjectiles) {
        if(recent.entityHandle == entityHandle) return &recent;
    }
    return nullptr;
}

bool IsProjectileAlreadyEmitted(int entityHandle, int entityIndex, int grenadeSlot)
{
    ULONGLONG now = GetTickCount64();
    bool emitted = false;
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentProjectiles(now, g_LastDemoTick);
    for(const RecentProjectile & recent : g_RecentProjectiles) {
        if(!recent.emitted) continue;
        if(recent.entityHandle == entityHandle) {
            emitted = true;
            break;
        }
        // Entity slot reuse changes the serial portion of the handle while
        // leaving the entry index intact.  Treat the same entry/type as the
        // same projectile only inside the short recent window.
        if(recent.entityIndex == entityIndex
            && recent.grenadeSlot == grenadeSlot
            && now - recent.firstSeenMs <= kRecentGrenadeThrowWindowMs
            && (g_LastDemoTick < 0 || recent.firstDemoTick < 0
                || abs(g_LastDemoTick - recent.firstDemoTick) <= kRecentGrenadeThrowTickWindow)) {
            emitted = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    return emitted;
}

void RememberProjectile(int entityHandle, int entityIndex, int grenadeSlot, const char * className)
{
    if(entityHandle < 0) return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentProjectiles(now, g_LastDemoTick);
    if(nullptr == FindRecentProjectileLocked(entityHandle)) {
        RecentProjectile recent = {};
        recent.firstSeenMs = now;
        recent.firstDemoTick = g_LastDemoTick;
        recent.entityHandle = entityHandle;
        recent.entityIndex = entityIndex;
        recent.controllerHandle = -1;
        recent.grenadeSlot = grenadeSlot;
        recent.emitted = false;
        recent.pendingLogged = false;
        recent.eventMatched = false;
        strncpy_s(recent.className, sizeof(recent.className), className ? className : "unknown", _TRUNCATE);
        g_RecentProjectiles.push_back(recent);
        while(kMaxRecentProjectiles < g_RecentProjectiles.size())
            g_RecentProjectiles.pop_front();
    }
    ReleaseSRWLockExclusive(&g_StateLock);
}

void MarkProjectileEmitted(int entityHandle)
{
    AcquireSRWLockExclusive(&g_StateLock);
    if(RecentProjectile * recent = FindRecentProjectileLocked(entityHandle))
        recent->emitted = true;
    ReleaseSRWLockExclusive(&g_StateLock);
}

bool ContainsInsensitive(const char * text, const char * needle);

const char * GetGrenadeText(const char * weapon)
{
    if(nullptr == weapon) return nullptr;
    if(0 == strncmp(weapon, "weapon_", 7)) weapon += 7;

    if(ContainsInsensitive(weapon, "smokegrenade") || ContainsInsensitive(weapon, "smoke_grenade"))
        return reinterpret_cast<const char *>(kSmokeUtf8);
    if(ContainsInsensitive(weapon, "flashbang") || ContainsInsensitive(weapon, "flash_grenade"))
        return reinterpret_cast<const char *>(kFlashUtf8);
    if(ContainsInsensitive(weapon, "hegrenade") || ContainsInsensitive(weapon, "high_explosive"))
        return reinterpret_cast<const char *>(kHeUtf8);
    if(ContainsInsensitive(weapon, "molotov"))
        return reinterpret_cast<const char *>(kMolotovUtf8);
    if(ContainsInsensitive(weapon, "incgrenade") || ContainsInsensitive(weapon, "incendiary"))
        return reinterpret_cast<const char *>(kIncendiaryUtf8);
    if(ContainsInsensitive(weapon, "decoy"))
        return reinterpret_cast<const char *>(kDecoyUtf8);
    return nullptr;
}

int GetGrenadeSlot(const char * weapon)
{
    if(nullptr == weapon) return -1;
    if(0 == strncmp(weapon, "weapon_", 7)) weapon += 7;
    if(ContainsInsensitive(weapon, "smokegrenade") || ContainsInsensitive(weapon, "smoke_grenade")) return 101;
    if(ContainsInsensitive(weapon, "flashbang") || ContainsInsensitive(weapon, "flash_grenade")) return 102;
    if(ContainsInsensitive(weapon, "hegrenade") || ContainsInsensitive(weapon, "high_explosive")) return 103;
    if(ContainsInsensitive(weapon, "molotov")) return 104;
    if(ContainsInsensitive(weapon, "incgrenade") || ContainsInsensitive(weapon, "incendiary")) return 105;
    if(ContainsInsensitive(weapon, "decoy")) return 106;
    return -1;
}

// CS2's HudChat converter consumes the same 0x01..0x10 inline controls carried
// by native RadioText.  Keep the sender marker (0x03), location marker (0x04),
// and the event-fallback body color separate.  Native packets already carry
// their own controls; these canonical grenade controls are used only when a
// demo omits that packet and mode4 has to synthesize the line from events.
//
// These values are copied from the installed game's resource/csgo_english.txt
// (pak01_141.vpk from the installed build):
//   SFUI_TitlesTXT_Fire_in_the_hole       "\\x0fHE Grenade!"
//   SFUI_TitlesTXT_Molotov_in_the_hole   "\\x10Molotov!"
//   SFUI_TitlesTXT_Incendiary_in_the_hole "\\x10Incendiary!"
//   SFUI_TitlesTXT_Flashbang_in_the_hole "\\x0bFlashbang!"
//   SFUI_TitlesTXT_Smoke_in_the_hole     "\\x05Smoke!"
//   SFUI_TitlesTXT_Decoy_in_the_hole     "\\x08Decoy!"
// Do not substitute the generic HUD red (0x02) for HE: the native token uses
// item-schema color 6 (0x0f), which renders #eb4b4b.
int GetRadioTextColorControl(int slot)
{
    switch(slot) {
    case 101: // smoke grenade: fixed pale green (#bfff90)
        return 0x05;
    case 102: // flashbang: item-schema color index 2 (#5e98d9)
        return 0x0B;
    case 103: // HE grenade: item-schema color index 6 (#eb4b4b)
        return 0x0F;
    case 104: // molotov: item-schema color index 7 (#e4ae39)
    case 105: // incendiary: item-schema color index 7 (#e4ae39)
        return 0x10;
    case 106: // decoy: fixed HUD gray (#c5cad0)
        return 0x08;
    default:
        return 0x01;
    }
}

const char * GetRadioBodyForSlot(int slot)
{
    switch(slot) {
    case 101: return reinterpret_cast<const char *>(kSmokeUtf8);
    case 102: return reinterpret_cast<const char *>(kFlashUtf8);
    case 103: return reinterpret_cast<const char *>(kHeUtf8);
    case 104: return reinterpret_cast<const char *>(kMolotovUtf8);
    case 105: return reinterpret_cast<const char *>(kIncendiaryUtf8);
    case 106: return reinterpret_cast<const char *>(kDecoyUtf8);
    case 109: return GetBombSiteText();
    case 110: return GetBombPlantedText();
    case 111: return GetBombDefusingText();
    case 112: return GetBombAbortDefuseText();
    case 113: return GetBombDefusedText();
    case 114: return GetBombExplodedText();
    case 117: return kFireInTheHoleText;
    default: return GetPlayerRadioText(slot);
    }
}

bool IsProjectileRadioSlot(int slot)
{
    // 101..106 are the six grenade throw notices; 117 is the native
    // "fire in the hole" voice token.  In mode4 these must come only from
    // weapon_fire/grenade_thrown.  SendAudio/RawAudio/sound-event are kept for
    // manual radio and bomb notices, but are deliberately not a second grenade
    // source in the default mode.
    return (101 <= slot && slot <= 106) || 117 == slot;
}

// Bomb pickup/drop are inventory state notifications, not radio callouts.
// Keep them out of every synthetic and native-audio fallback path so a demo
// does not grow an extra "I have the bomb" / "bomb has been dropped" line.
bool IsSuppressedRadioSlot(int slot)
{
    return 115 == slot || 116 == slot;
}

bool IsGrenadeProjectileClass(const char * className)
{
    if(nullptr == className || '\0' == className[0]) return false;
    // Entity scans also see the player's held weapon entities (for example
    // weapon_molotov).  Those are not thrown projectiles and must never be
    // converted into a radio notice.  Keep an explicit whitelist for the
    // grenade projectile classes identified in the current client.dll; this
    // avoids treating unrelated projectile/model entities as radio sources.
    if(0 == _strnicmp(className, "weapon_", 7)) return false;
    static const char * projectileClasses[] = {
        "smokegrenade_projectile",
        "flashbang_projectile",
        "hegrenade_projectile",
        "molotov_projectile",
        "incgrenade_projectile",
        "decoy_projectile",
        "tagrenade_projectile",
        "C_SmokeGrenadeProjectile",
        "C_FlashbangProjectile",
        "C_HEGrenadeProjectile",
        "C_MolotovProjectile",
        "C_DecoyProjectile"
    };
    for(const char * projectileClass : projectileClasses) {
        if(0 == _stricmp(className, projectileClass)) return true;
    }
    return false;
}

CEntityInstance * ResolveProjectileController(CEntityInstance * projectile)
{
    if(nullptr == projectile) return nullptr;

    CEntityInstance * current = projectile;
    for(int depth = 0; depth < 3 && nullptr != current; ++depth) {
        if(current->IsPlayerController()) return current;
        if(current->IsPlayerPawn()) return GetControllerFromPawn(current);

        CEntityInstance * owner = nullptr;
        __try {
            ptrdiff_t throwerOffset = g_clientDllOffsets.C_BaseGrenade.m_hThrower;
            ptrdiff_t originalThrowerOffset = g_clientDllOffsets.C_BaseGrenade.m_hOriginalThrower;
            ptrdiff_t ownerOffset = g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity;
            if(throwerOffset >= 0) {
                auto handle = SOURCESDK::CS2::CEntityHandle::CEntityHandle(
                    *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(current) + throwerOffset));
                if(handle.IsValid()) owner = GetEntityFromIndex(handle.GetEntryIndex());
            }
            if(nullptr == owner && originalThrowerOffset >= 0) {
                auto handle = SOURCESDK::CS2::CEntityHandle::CEntityHandle(
                    *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(current) + originalThrowerOffset));
                if(handle.IsValid()) owner = GetEntityFromIndex(handle.GetEntryIndex());
            }
            if(nullptr == owner && ownerOffset >= 0) {
                auto handle = SOURCESDK::CS2::CEntityHandle::CEntityHandle(
                    *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(current) + ownerOffset));
                if(handle.IsValid()) owner = GetEntityFromIndex(handle.GetEntryIndex());
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            owner = nullptr;
        }
        if(nullptr == owner) break;
        current = owner;
    }

    // A few projectile builds expose the thrower only through the scene-node
    // parent. Keep this as a final fallback for older schema layouts.
    __try {
        ptrdiff_t sceneOffset = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
        ptrdiff_t parentOffset = g_clientDllOffsets.CGameSceneNode.m_pParent;
        ptrdiff_t sceneOwnerOffset = g_clientDllOffsets.CGameSceneNode.m_pOwner;
        if(sceneOffset >= 0 && parentOffset >= 0 && sceneOwnerOffset >= 0) {
            auto sceneNode = *reinterpret_cast<unsigned char **>(
                reinterpret_cast<unsigned char *>(projectile) + sceneOffset);
            if(nullptr != sceneNode) {
                auto parentNode = *reinterpret_cast<unsigned char **>(sceneNode + parentOffset);
                if(nullptr != parentNode) {
                    auto parentOwner = *reinterpret_cast<CEntityInstance **>(parentNode + sceneOwnerOffset);
                    if(nullptr != parentOwner) {
                        if(parentOwner->IsPlayerController()) return parentOwner;
                        if(parentOwner->IsPlayerPawn()) return GetControllerFromPawn(parentOwner);
                    }
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool ContainsInsensitive(const char * text, const char * needle)
{
    if(nullptr == text || nullptr == needle || '\0' == needle[0]) return false;
    size_t needleLength = strlen(needle);
    for(const char * cursor = text; '\0' != *cursor; ++cursor) {
        if(0 == _strnicmp(cursor, needle, needleLength)) return true;
    }
    return false;
}

bool ContainsNormalizedRadioToken(const char * text, const char * needle)
{
    if(nullptr == text || nullptr == needle || '\0' == needle[0]) return false;
    char normalizedText[256];
    char normalizedNeedle[64];
    size_t textLength = 0;
    for(const unsigned char * cursor = reinterpret_cast<const unsigned char *>(text);
        *cursor != 0 && textLength + 1 < sizeof(normalizedText);
        ++cursor) {
        if(isalnum(*cursor)) normalizedText[textLength++] = static_cast<char>(tolower(*cursor));
    }
    normalizedText[textLength] = '\0';
    size_t needleLength = 0;
    for(const unsigned char * cursor = reinterpret_cast<const unsigned char *>(needle);
        *cursor != 0 && needleLength + 1 < sizeof(normalizedNeedle);
        ++cursor) {
        if(isalnum(*cursor)) normalizedNeedle[needleLength++] = static_cast<char>(tolower(*cursor));
    }
    normalizedNeedle[needleLength] = '\0';
    return '\0' != normalizedNeedle[0] && nullptr != strstr(normalizedText, normalizedNeedle);
}

bool IsSuppressedRadioToken(const char * text)
{
    if(nullptr == text || '\0' == text[0]) return false;
    return ContainsNormalizedRadioToken(text, "bombdropped")
        || ContainsNormalizedRadioToken(text, "bombpickup")
        || ContainsNormalizedRadioToken(text, "spottedloosebomb")
        || ContainsNormalizedRadioToken(text, "loosebomb");
}

bool IsLikelyAgentDefIndex(int value)
{
    // Agent item definitions occupy the 5xxx range.  Reject zero/garbage from
    // a not-yet-replicated controller instead of treating it as a real family.
    return 5000 <= value && value < 7000;
}

int ReadPawnCharacterDefIndex(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()
        || g_clientDllOffsets.CCSPlayerController.m_nPawnCharacterDefIndex < 0) return -1;
    int value = -1;
    __try {
        unsigned char * address = reinterpret_cast<unsigned char *>(controller)
            + g_clientDllOffsets.CCSPlayerController.m_nPawnCharacterDefIndex;
        value = *reinterpret_cast<int *>(address);
        if(!IsLikelyAgentDefIndex(value)) {
            // item_definition_index_t is 16-bit in some schema generations.
            // Read the narrow form only after rejecting the wide value so a
            // neighbouring field cannot be mistaken for an agent id.
            value = static_cast<int>(*reinterpret_cast<uint16_t *>(address));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        value = -1;
    }
    // Some demo builds expose the field as a short item-definition value.  An
    // invalid controller read is not useful, but a valid 5xxx value is safe to
    // retain; this also prevents random entity bytes from selecting an agent.
    return IsLikelyAgentDefIndex(value) ? value : -1;
}

const char * AgentFamilyFromDefIndex(int defIndex)
{
    if(!IsLikelyAgentDefIndex(defIndex)) return nullptr;

    // Current items_game agent definitions.  Keep the ranges explicit because
    // the compact ranges used by older builds mixed 5037 into Phoenix and
    // mapped the CT families to the wrong voice directory.
    if(5036 == defIndex
        || (5038 <= defIndex && defIndex <= 5047)
        || (5053 <= defIndex && defIndex <= 5057)
        || (5200 <= defIndex && defIndex <= 5204)) return "phoenix";
    if(5048 <= defIndex && defIndex <= 5052) return "professional";
    if(5037 == defIndex
        || (5079 <= defIndex && defIndex <= 5082)
        || 5600 == defIndex) return "sas";
    if(5058 <= defIndex && defIndex <= 5067) return "gsg9";
    if(5068 <= defIndex && defIndex <= 5078)
        return "swat";
    if(5083 <= defIndex && defIndex <= 5087) return "swat";
    if(5088 <= defIndex && defIndex <= 5096) return "balkan";
    if(5097 == defIndex) return "fbihrt";
    if(5100 <= defIndex && defIndex <= 5104) return "leet";
    if(5300 <= defIndex && defIndex <= 5304) return "fbihrt";
    return nullptr;
}

const char * FindVoiceFamilyInText(const char * text)
{
    if(nullptr == text || '\0' == text[0]) return nullptr;
    static const char * const families[] = {
        "professional", "gsg9", "sas", "swat", "fbihrt",
        "balkan", "phoenix", "leet", "separatist", "anarchist",
        "pirate", "militia"
    };
    for(const char * family : families) {
        if(ContainsInsensitive(text, family)) return family;
    }
    return nullptr;
}

bool NormalizeObservedVoiceCue(const char * voice, char * output, size_t outputSize)
{
    if(nullptr == voice || nullptr == output || outputSize < 2) return false;
    output[0] = '\0';
    const char * family = FindVoiceFamilyInText(voice);
    if(nullptr == family) return false;

    // Already-normalized sound-event names are preferred as-is.
    if(nullptr != strchr(voice, '.') && nullptr == strchr(voice, '/')
        && nullptr == strchr(voice, '\\')) {
        size_t length = strnlen_s(voice, outputSize);
        if(0 < length && length < outputSize) {
            strncpy_s(output, outputSize, voice, _TRUNCATE);
            return true;
        }
    }

    const char * basename = voice;
    const char * slash = strrchr(voice, '/');
    const char * backslash = strrchr(voice, '\\');
    if(nullptr != slash && slash + 1 > basename) basename = slash + 1;
    if(nullptr != backslash && backslash + 1 > basename) basename = backslash + 1;
    if('\0' == basename[0]) return false;

    char stem[128] = {};
    strncpy_s(stem, sizeof(stem), basename, _TRUNCATE);
    char * extension = strstr(stem, ".wav");
    if(nullptr == extension) extension = strstr(stem, ".vsnd");
    if(nullptr != extension) *extension = '\0';
    if('\0' == stem[0]) return false;

    int written = snprintf(output, outputSize, "%s.%s", family, stem);
    return 0 < written && static_cast<size_t>(written) < outputSize;
}

void RememberObservedAgentVoice(CEntityInstance * controller, int slot, const char * voice)
{
    if(nullptr == controller || slot < 0 || nullptr == voice || '\0' == voice[0]) return;
    char cue[192] = {};
    if(!NormalizeObservedVoiceCue(voice, cue, sizeof(cue))) return;
    const char * family = FindVoiceFamilyInText(cue);
    const int controllerHandle = GetControllerHandle(controller);
    const int entityIndex = GetEntityIndex(controller);
    if(controllerHandle < 0 && entityIndex < 0) return;

    ObservedAgentVoice observed = {};
    observed.timeMs = GetTickCount64();
    observed.demoTick = g_LastDemoTick;
    observed.controllerHandle = controllerHandle;
    observed.entityIndex = entityIndex;
    observed.slot = slot;
    strncpy_s(observed.family, sizeof(observed.family), family ? family : "unknown", _TRUNCATE);
    strncpy_s(observed.cue, sizeof(observed.cue), cue, _TRUNCATE);

    AcquireSRWLockExclusive(&g_StateLock);
    bool replaced = false;
    for(auto & recent : g_ObservedAgentVoices) {
        if(recent.controllerHandle == observed.controllerHandle
            && recent.entityIndex == observed.entityIndex
            && recent.slot == observed.slot) {
            recent = observed;
            replaced = true;
            break;
        }
    }
    if(!replaced) g_ObservedAgentVoices.push_back(observed);
    while(kMaxRecentRadios * 8 < g_ObservedAgentVoices.size())
        g_ObservedAgentVoices.pop_front();
    ReleaseSRWLockExclusive(&g_StateLock);

    strncpy_s(g_LastAgentFamily, sizeof(g_LastAgentFamily), observed.family, _TRUNCATE);
}

bool FindObservedAgentVoice(
    CEntityInstance * controller,
    int slot,
    char * cue,
    size_t cueSize,
    char * family,
    size_t familySize)
{
    if(nullptr == controller || nullptr == cue || cueSize < 2) return false;
    const int controllerHandle = GetControllerHandle(controller);
    const int entityIndex = GetEntityIndex(controller);
    bool found = false;
    AcquireSRWLockShared(&g_StateLock);
    for(auto it = g_ObservedAgentVoices.rbegin(); it != g_ObservedAgentVoices.rend(); ++it) {
        if(slot >= 0 && it->slot != slot) continue;
        if(controllerHandle >= 0 && it->controllerHandle == controllerHandle
            || entityIndex >= 0 && it->entityIndex == entityIndex) {
            strncpy_s(cue, cueSize, it->cue, _TRUNCATE);
            if(nullptr != family && familySize >= 2)
                strncpy_s(family, familySize, it->family, _TRUNCATE);
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_StateLock);
    return found;
}

const char * GetAgentVoicePrefix(CEntityInstance * controller)
{
    const int defIndex = ReadPawnCharacterDefIndex(controller);
    char observedCue[192] = {};
    char observedFamily[32] = {};
    if(FindObservedAgentVoice(
        controller,
        -1,
        observedCue,
        sizeof(observedCue),
        observedFamily,
        sizeof(observedFamily))) {
        if('\0' != observedFamily[0]) {
            strncpy_s(g_LastAgentFamily, sizeof(g_LastAgentFamily), observedFamily, _TRUNCATE);
            return g_LastAgentFamily;
        }
    }

    const char * family = AgentFamilyFromDefIndex(defIndex);
    if(nullptr != family) {
        strncpy_s(g_LastAgentFamily, sizeof(g_LastAgentFamily), family, _TRUNCATE);
        return family;
    }
    return nullptr;
}

bool UseSimplifiedChineseRadioText()
{
    __try {
        if(nullptr == SOURCESDK::CS2::g_pCVar) return false;
        auto handle = SOURCESDK::CS2::g_pCVar->FindConVar("cl_language", false);
        if(!handle.IsValid()) return false;
        auto cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
        if(nullptr == cvar || SOURCESDK::CS2::EConVarType_String != cvar->m_eVarType)
            return false;
        const char * language = cvar->m_Value.m_szValue.Get();
        return ContainsInsensitive(language, "schinese")
            || ContainsInsensitive(language, "tchinese")
            || ContainsInsensitive(language, "chinese")
            || ContainsInsensitive(language, "zh");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char * GetSoundRadioText(const char * soundName, int & slot)
{
    slot = -1;
    if(nullptr == soundName || '\0' == soundName[0]) return nullptr;
    if(IsSuppressedRadioToken(soundName)) return nullptr;

    // Sound event names vary between builds (`Player.Radio.LetsGo`,
    // `RadioBotLetsGo`, `player_voice_radiobot_throwingsmoke`, ...).  Match a
    // separator-free form first so punctuation/case changes do not make the
    // real in-game notice disappear.
    struct NormalizedMapping {
        const char * needle;
        int slot;
    };
    static const NormalizedMapping normalizedMappings[] = {
        {"radiogogogo", 2}, {"radioletsgo", 2}, {"radiobotgogogo", 2}, {"radiobotletsgo", 2},
        {"radiofallback", 3}, {"radiobotfallback", 3},
        {"radiosticktogether", 4}, {"radiobotsticktogether", 4},
        {"radioholdposition", 5}, {"radiobotholdposition", 5}, {"radiobothold", 5},
        {"radiofollowme", 6}, {"radiobotfollowme", 6},
        {"radioaffirmative", 8}, {"radiobotaffirmative", 8},
        {"radionegative", 9}, {"radiobotnegative", 9},
        {"radiocheer", 10}, {"radiobotcheer", 10},
        {"radionice", 11}, {"radiobotnice", 11},
        {"radiothanks", 12}, {"radiobotthanks", 12},
        {"radioenemyspotted", 14}, {"radiobotenemyspotted", 14},
        {"radioneedbackup", 15}, {"radiobotneedbackup", 15},
        {"radiotakepoint", 16}, {"radiobottakepoint", 16},
        {"radiosectorclear", 17}, {"radiobotsectorclear", 17}, {"radiobotclear", 17},
        {"radioinposition", 18}, {"radiobotinposition", 18},
        {"radiocoverme", 19}, {"radiobotcoverme", 19},
        {"radioregroup", 20}, {"radiobotregroup", 20},
        {"radiotakingfire", 21}, {"radiobottakingfire", 21},
        {"radioreportin", 22}, {"radiobotreportin", 22},
        {"radioreportingin", 23}, {"radiobotreportingin", 23},
        {"radiogetout", 24}, {"radiobotgetout", 24},
        {"radioenemydown", 25}, {"radiobotenemydown", 25},
        {"radiofireinthehole", 117}, {"fireinthehole", 117},
        {"throwingsmoke", 101}, {"throwsmoke", 101},
        {"throwingflash", 102}, {"throwflash", 102},
        {"throwinghegrenade", 103}, {"throwgrenade", 103}, {"throwinggrenade", 103},
        {"throwingmolotov", 104}, {"throwmolotov", 104}, {"throwingfire", 104},
        {"throwingincendiary", 105}, {"throwincendiary", 105},
        {"throwingdecoy", 106}, {"throwdecoy", 106},
        {"radiobombsite", 109}, {"radiobombplanted", 110},
        {"radiobegindefuse", 111}, {"radiodefuseabort", 112},
        {"radiobombdefused", 113}, {"radiobombexploded", 114},
        {"radiobombdropped", 115}, {"radiobombpickup", 116}
    };
    for(const NormalizedMapping & mapping : normalizedMappings) {
        if(!ContainsNormalizedRadioToken(soundName, mapping.needle)) continue;
        slot = mapping.slot;
        switch(slot) {
        case 101: return reinterpret_cast<const char *>(kSmokeUtf8);
        case 102: return reinterpret_cast<const char *>(kFlashUtf8);
        case 103: return reinterpret_cast<const char *>(kHeUtf8);
        case 104: return reinterpret_cast<const char *>(kMolotovUtf8);
        case 105: return reinterpret_cast<const char *>(kIncendiaryUtf8);
        case 106: return reinterpret_cast<const char *>(kDecoyUtf8);
        case 109: return GetBombSiteText();
        case 110: return GetBombPlantedText();
        case 111: return GetBombDefusingText();
        case 112: return GetBombAbortDefuseText();
        case 113: return GetBombDefusedText();
        case 114: return GetBombExplodedText();
        case 115: return GetBombDroppedText();
        case 116: return GetBombPickupText();
        case 117: return kFireInTheHoleText;
        default: return GetPlayerRadioText(slot);
        }
    }

    if(ContainsInsensitive(soundName, "throwing_smoke")) {
        slot = 101;
        return reinterpret_cast<const char *>(kSmokeUtf8);
    }
    if(ContainsInsensitive(soundName, "throwing_flash")) {
        slot = 102;
        return reinterpret_cast<const char *>(kFlashUtf8);
    }
    if(ContainsInsensitive(soundName, "throwing_grenade")
        || ContainsInsensitive(soundName, "throwing_hegrenade")) {
        slot = 103;
        return reinterpret_cast<const char *>(kHeUtf8);
    }
    if(ContainsInsensitive(soundName, "throwing_molotov")) {
        slot = 104;
        return reinterpret_cast<const char *>(kMolotovUtf8);
    }
    if(ContainsInsensitive(soundName, "throwing_incendiary")) {
        slot = 105;
        return reinterpret_cast<const char *>(kIncendiaryUtf8);
    }
    if(ContainsInsensitive(soundName, "throwing_fire")) {
        slot = 104;
        return reinterpret_cast<const char *>(kMolotovUtf8);
    }
    if(ContainsInsensitive(soundName, "throwing_decoy")) {
        slot = 106;
        return reinterpret_cast<const char *>(kDecoyUtf8);
    }
    if(ContainsInsensitive(soundName, "locknload")) {
        slot = 107;
        return kLockAndLoadText;
    }
    if(ContainsInsensitive(soundName, "onmyway") || ContainsInsensitive(soundName, "omw")) {
        slot = 108;
        return kOnMyWayText;
    }
    if(ContainsInsensitive(soundName, "bombsite")) {
        slot = 109;
        return GetBombSiteText();
    }
    if(ContainsInsensitive(soundName, "planting") || ContainsInsensitive(soundName, "bombplanted")) {
        slot = 110;
        return GetBombPlantedText();
    }
    if(ContainsInsensitive(soundName, "defusing") || ContainsInsensitive(soundName, "begindefuse")) {
        slot = 111;
        return GetBombDefusingText();
    }
    if(ContainsInsensitive(soundName, "abortdefuse")) {
        slot = 112;
        return GetBombAbortDefuseText();
    }
    if(ContainsInsensitive(soundName, "bombdefused") || ContainsInsensitive(soundName, "defused")) {
        slot = 113;
        return GetBombDefusedText();
    }
    if(ContainsInsensitive(soundName, "bombexploded") || ContainsInsensitive(soundName, "exploded")) {
        slot = 114;
        return GetBombExplodedText();
    }
    if(ContainsInsensitive(soundName, "bombdropped")) {
        slot = 115;
        return GetBombDroppedText();
    }
    if(ContainsInsensitive(soundName, "bombpickup") || ContainsInsensitive(soundName, "pickbomb")) {
        slot = 116;
        return GetBombPickupText();
    }

    // CCSUsrMsg_SendAudio uses the canonical Radio.* token names rather
    // than the sound-event names handled above. Keep both spellings here:
    // the token is case-insensitive and builds have used either dots or
    // underscores between the radio category and command.
    struct CanonicalRadioMapping {
        const char * needle;
        int slot;
    };
    static const CanonicalRadioMapping canonicalMappings[] = {
        {"radio.letsgo", 2},
        {"radio.go_go_go", 2},
        {"radio.gogogo", 2},
        {"radio.fallback", 3},
        {"radio.sticktogether", 4},
        {"radio.holdposition", 5},
        {"radio.hold", 5},
        {"radio.followme", 6},
        {"radio.affirmative", 8},
        {"radio.negative", 9},
        {"radio.cheer", 10},
        {"radio.nice", 11},
        {"radio.thanks", 12},
        {"radio.enemyspotted", 14},
        {"radio.needbackup", 15},
        {"radio.takepoint", 16},
        {"radio.sectorclear", 17},
        {"radio.inposition", 18},
        {"radio.coverme", 19},
        {"radio.regroup", 20},
        {"radio.takingfire", 21},
        {"radio.reportin", 22},
        {"radio.reportingin", 23},
        {"radio.getout", 24},
        {"radio.enemydown", 25},
        {"radio.bombsite", 109},
        {"radio.bombplanted", 110},
        {"radio.begindefuse", 111},
        {"radio.defuseabort", 112},
        {"radio.bombdefused", 113},
        {"radio.bombexploded", 114},
        {"radio.bombdropped", 115},
        {"radio.bombpickup", 116}
    };
    for(const CanonicalRadioMapping & mapping : canonicalMappings) {
        if(!ContainsInsensitive(soundName, mapping.needle)) continue;
        slot = mapping.slot;
        switch(slot) {
        case 109: return GetBombSiteText();
        case 110: return GetBombPlantedText();
        case 111: return GetBombDefusingText();
        case 112: return GetBombAbortDefuseText();
        case 113: return GetBombDefusedText();
        case 114: return GetBombExplodedText();
        case 115: return GetBombDroppedText();
        case 116: return GetBombPickupText();
        default: return GetPlayerRadioText(slot);
        }
    }

    // The game sometimes sends a generic grenade line without preserving the
    // item subtype in the user message. We still reproduce the real radio
    // notice instead of dropping it; the projectile/sound paths can supply a
    // more specific grenade body when available.
    if(ContainsInsensitive(soundName, "radio.fireinthehole")
        || ContainsInsensitive(soundName, "radio.fire_in_the_hole")
        || ContainsInsensitive(soundName, "fireinthehole")) {
        slot = 117;
        return kFireInTheHoleText;
    }

    struct Mapping {
        const char * needle;
        int slot;
    };
    static const Mapping mappings[] = {
        {"radio_letsgo", 2},
        {"radiobotletsgo", 2},
        {"radio_fallback", 3},
        {"radiobotfallback", 3},
        {"radio_sticktogether", 4},
        {"radiobotsticktogether", 4},
        {"radio_hold", 5},
        {"radiobothold", 5},
        {"radio_followme", 6},
        {"radiobotfollowme", 6},
        {"radio_affirmative", 8},
        {"radiobotaffirmative", 8},
        {"radio_negative", 9},
        {"radiobotnegative", 9},
        {"radio_cheer", 10},
        {"radiobotcheer", 10},
        {"radio_nice", 11},
        {"radiobotnice", 11},
        {"radio_thanks", 12},
        {"radiobotthanks", 12},
        {"radio_enemyspotted", 14},
        {"radiobotenemyspotted", 14},
        {"radio_needbackup", 15},
        {"radiobotneedbackup", 15},
        {"radio_takepoint", 16},
        {"radiobottakepoint", 16},
        {"radio_sectorclear", 17},
        {"radiobotclear", 17},
        {"radio_inposition", 18},
        {"radiobotinposition", 18},
        {"radio_coverme", 19},
        {"radiobotcoverme", 19},
        {"radio_regroup", 20},
        {"radiobotregroup", 20},
        {"radio_takingfire", 21},
        {"radiobottakingfire", 21},
        {"radio_report", 22},
        {"radiobotreport", 22},
        {"radio_reportingin", 23},
        {"radiobotreportingin", 23},
        {"radio_getout", 24},
        {"radiobotgetout", 24},
        {"radio_enemydown", 25},
        {"radiobotenemydown", 25}
    };
    for(const Mapping & mapping : mappings) {
        if(!ContainsInsensitive(soundName, mapping.needle)) continue;
        slot = mapping.slot;
        return GetPlayerRadioText(slot);
    }

    // RawAudio voice filenames are usually paths such as
    // characters/counterterrorist/radio/ct_letsgo01.wav.  They do not retain
    // the `radio_` token spelling, so accept the stable command stem as a
    // second mapping pass.  Keep the longer stems before their prefixes.
    static const Mapping voiceMappings[] = {
        {"throw_smoke", 101},
        {"throwing_smoke", 101},
        {"throw_flash", 102},
        {"throwing_flash", 102},
        {"throw_hegrenade", 103},
        {"throw_grenade", 103},
        {"throw_molotov", 104},
        {"throw_fire", 104},
        {"throw_incendiary", 105},
        {"throw_decoy", 106},
        {"fireinthehole", 117},
        {"reportingin", 23},
        {"sticktogether", 4},
        {"enemyspotted", 14},
        {"needbackup", 15},
        {"takingfire", 21},
        {"enemydown", 25},
        {"followme", 6},
        {"affirmative", 8},
        {"negative", 9},
        {"fallback", 3},
        {"letsgo", 2},
        {"holdposition", 5},
        {"hold", 5},
        {"cheer", 10},
        {"nice", 11},
        {"thanks", 12},
        {"takepoint", 16},
        {"sectorclear", 17},
        {"inposition", 18},
        {"coverme", 19},
        {"regroup", 20},
        {"getout", 24},
        {"report", 22},
        {"bombsite", 109},
        {"bomb_site", 109},
        {"bombplanted", 110},
        {"bomb_planted", 110},
        {"begindefuse", 111},
        {"begin_defuse", 111},
        {"bomb_begindefuse", 111},
        {"abortdefuse", 112},
        {"abort_defuse", 112},
        {"bomb_abortdefuse", 112},
        {"bombdefused", 113},
        {"bomb_defused", 113},
        {"bombexploded", 114},
        {"bomb_exploded", 114},
        {"bombdropped", 115},
        {"bomb_dropped", 115},
        {"bombpickup", 116},
        {"bomb_pickup", 116}
    };
    for(const Mapping & mapping : voiceMappings) {
        if(!ContainsInsensitive(soundName, mapping.needle)) continue;
        slot = mapping.slot;
        switch(slot) {
        case 101: return reinterpret_cast<const char *>(kSmokeUtf8);
        case 102: return reinterpret_cast<const char *>(kFlashUtf8);
        case 103: return reinterpret_cast<const char *>(kHeUtf8);
        case 104: return reinterpret_cast<const char *>(kMolotovUtf8);
        case 105: return reinterpret_cast<const char *>(kIncendiaryUtf8);
        case 106: return reinterpret_cast<const char *>(kDecoyUtf8);
        case 109: return GetBombSiteText();
        case 110: return GetBombPlantedText();
        case 111: return GetBombDefusingText();
        case 112: return GetBombAbortDefuseText();
        case 113: return GetBombDefusedText();
        case 114: return GetBombExplodedText();
        case 115: return GetBombDroppedText();
        case 116: return GetBombPickupText();
        case 117: return kFireInTheHoleText;
        default: return GetPlayerRadioText(slot);
        }
    }
    return nullptr;
}

uint32_t VoiceVariantSeed(CEntityInstance * controller, int slot)
{
    const uint32_t handle = static_cast<uint32_t>(GetControllerHandle(controller));
    const uint32_t tick = static_cast<uint32_t>(g_LastDemoTick < 0 ? 0 : g_LastDemoTick);
    const uint32_t sequence = static_cast<uint32_t>(GetTickCount64());
    uint32_t seed = 0x9e3779b9u ^ handle * 0x85ebca6bu ^ static_cast<uint32_t>(slot) * 0xc2b2ae35u;
    seed ^= tick * 0x27d4eb2du ^ sequence * 0x165667b1u;
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    return seed;
}

const char * PickVoiceVariant(const char * const * variants, size_t count, uint32_t seed)
{
    if(nullptr == variants || 0 == count) return nullptr;
    return variants[seed % count];
}

bool UsesCtThrowVoiceNames(const char * prefix)
{
    // CS2 kept the legacy throw-callout families: SAS/SWAT/GSG9/FBIHRT use
    // ct_* resources, while Professional and the T families use t_* resources.
    return nullptr != prefix
        && (0 == strcmp(prefix, "sas")
            || 0 == strcmp(prefix, "swat")
            || 0 == strcmp(prefix, "gsg9")
            || 0 == strcmp(prefix, "fbihrt"));
}

// Build the actual CS2 SoundEvent name used by the agent voice resources.
// If a RawAudio packet was observed earlier in the round, replay its exact
// family/stem first.  Otherwise use the agent item family and a deterministic
// per-event variant.  The native audio hooks still win and cancel this fallback
// when a real voice event is present.
bool BuildSyntheticAudioCue(
    CEntityInstance * controller,
    int slot,
    char * output,
    size_t outputSize)
{
    if(nullptr == controller || nullptr == output || outputSize < 2) return false;
    if(IsSuppressedRadioSlot(slot)) return false;
    output[0] = '\0';

    char observedCue[192] = {};
    char observedFamily[32] = {};
    if(FindObservedAgentVoice(
        controller,
        slot,
        observedCue,
        sizeof(observedCue),
        observedFamily,
        sizeof(observedFamily))) {
        strncpy_s(output, outputSize, observedCue, _TRUNCATE);
        return '\0' != output[0];
    }

    const bool isCt = 3 == ResolveControllerTeam(controller);
    const char * prefix = GetAgentVoicePrefix(controller);
    if(nullptr == prefix) prefix = isCt ? "professional" : "balkan";
    const uint32_t seed = VoiceVariantSeed(controller, slot);
    const char * stem = nullptr;

    static const char * const tSmoke[] = {"t_smoke01", "t_smoke02", "t_smoke03"};
    static const char * const tFlash[] = {"t_flashbang01", "t_flashbang02", "t_flashbang03"};
    static const char * const tHe[] = {"t_grenade01", "t_grenade02"};
    static const char * const tMolotov[] = {"t_molotov01", "t_molotov02"};
    static const char * const ctSmoke[] = {"ct_smoke01", "ct_smoke02"};
    static const char * const ctFlash[] = {"ct_flashbang01", "ct_flashbang02"};
    static const char * const ctHe[] = {"ct_grenade01"};
    static const char * const ctMolotov[] = {"ct_molotov01", "ct_molotov02"};

    if(101 == slot) {
        stem = UsesCtThrowVoiceNames(prefix)
            ? PickVoiceVariant(ctSmoke, sizeof(ctSmoke) / sizeof(ctSmoke[0]), seed)
            : PickVoiceVariant(tSmoke, sizeof(tSmoke) / sizeof(tSmoke[0]), seed);
    } else if(102 == slot) {
        stem = UsesCtThrowVoiceNames(prefix)
            ? PickVoiceVariant(ctFlash, sizeof(ctFlash) / sizeof(ctFlash[0]), seed)
            : PickVoiceVariant(tFlash, sizeof(tFlash) / sizeof(tFlash[0]), seed);
    } else if(103 == slot) {
        stem = UsesCtThrowVoiceNames(prefix)
            ? PickVoiceVariant(ctHe, sizeof(ctHe) / sizeof(ctHe[0]), seed)
            : PickVoiceVariant(tHe, sizeof(tHe) / sizeof(tHe[0]), seed);
    } else if(104 == slot || 105 == slot) {
        stem = UsesCtThrowVoiceNames(prefix)
            ? PickVoiceVariant(ctMolotov, sizeof(ctMolotov) / sizeof(ctMolotov[0]), seed)
            : PickVoiceVariant(tMolotov, sizeof(tMolotov) / sizeof(tMolotov[0]), seed);
    } else if(106 == slot) {
        stem = "t_decoy01";
    } else if(isCt) {
        // Professional's resources use the radiobot* stem names.  The agent
        // family prefix still selects the actual operator voice; the filename
        // does not mean a bot is being used.
        switch(slot) {
        case 2: stem = "radiobotgo01"; break;
        case 3: stem = "radiobotfallback01"; break;
        case 4: stem = "radiobotgo01"; break;
        case 5: stem = "radiobothold02"; break;
        case 6: stem = "radiobotfollowme01"; break;
        case 8: stem = "radiobotreponsepositive01"; break;
        case 9: stem = "radiobotreponsenegative01"; break;
        case 10: stem = "radiobotcheer01"; break;
        case 11: stem = "radiobotniceshot01"; break;
        case 12: stem = "radiobotreponsepositive01"; break;
        case 14: stem = "radiobottarget01"; break;
        case 15: stem = "radiobotunderfire01"; break;
        case 16: stem = "radiobotstart01"; break;
        case 17: stem = "radiobotclear01"; break;
        case 18: stem = "radiobotstart01"; break;
        case 19: stem = "radiobotreponsecoverrequest01"; break;
        case 20: stem = "radiobotregroup01"; break;
        case 21: stem = "radiobotunderfire01"; break;
        case 22: stem = "radiobotreport01"; break;
        case 23: stem = "radiobotreport01"; break;
        case 24: stem = "radiobotunderfire01"; break;
        case 25: stem = "radiobotkill01"; break;
        case 109: stem = "radiobombsite01"; break;
        case 110: stem = "radiobotbombatsafe01"; break;
        case 111: stem = "radiobotbombdefusing01"; break;
        case 112: stem = "radiobotbombdefusing01"; break;
        case 113: stem = "radiobotbombatsafe01"; break;
        case 114: stem = "radiobombsite01"; break;
        default: break;
        }
    } else {
        switch(slot) {
        case 2: stem = "radio_letsgo01"; break;
        case 3: stem = "radio_fallback01"; break;
        case 4: stem = "radio_sticktogether01"; break;
        case 5: stem = "radio_holdposition01"; break;
        case 6: stem = "radio_followme01"; break;
        case 8: stem = "radio_affirmative01"; break;
        case 9: stem = "radio_negative01"; break;
        case 10: stem = "radio_cheer01"; break;
        case 11: stem = "radio_nice01"; break;
        case 12: stem = "radio_thanks01"; break;
        case 14: stem = "radio_enemyspotted02"; break;
        case 15: stem = "radio_needbackup01"; break;
        case 16: stem = "radio_takepoint01"; break;
        case 17: stem = "radio_sectorclear01"; break;
        case 18: stem = "radio_inposition01"; break;
        case 19: stem = "radio_coverme01"; break;
        case 20: stem = "radio_regroup01"; break;
        case 21: stem = "radio_takingfire01"; break;
        case 22: stem = "radio_reportingin01"; break;
        case 23: stem = "radio_reportingin01"; break;
        case 24: stem = "radio_getout01"; break;
        case 25: stem = "radio_enemydown01"; break;
        case 109: stem = "goingtoplantbomb01"; break;
        case 110: stem = "plantingbomb01"; break;
        case 111: stem = "plantingbomb01"; break;
        case 112: stem = "plantingbomb01"; break;
        case 113: stem = "bombtickingdown01"; break;
        case 114: stem = "bombtickingdown01"; break;
        default: break;
        }
    }

    if(nullptr == stem) return false;
    int written = snprintf(output, outputSize, "%s.%s", prefix, stem);
    return 0 < written && static_cast<size_t>(written) < outputSize;
}

bool IsSyntheticAudioSource(const char * source)
{
    if(nullptr == source || '\0' == source[0]) return false;
    // Native audio paths must never enqueue a second local cue.
    return 0 != strcmp(source, "send-audio")
        && 0 != strcmp(source, "raw-audio")
        && 0 != strcmp(source, "sound-event");
}

void PruneRecentNativeAudiosLocked(ULONGLONG now, int demoTick)
{
    for(auto it = g_RecentNativeAudios.begin(); it != g_RecentNativeAudios.end();) {
        bool expiredByTime = now - it->timeMs > kRecentNativeAudioWindowMs;
        bool expiredByTick = 0 <= demoTick
            && 0 <= it->demoTick
            && demoTick > it->demoTick + 48;
        if(expiredByTime || expiredByTick) it = g_RecentNativeAudios.erase(it);
        else ++it;
    }
}

bool NativeAudioMatchesPending(const RecentNativeAudio & native, const PendingSyntheticAudio & pending)
{
    if(native.slot >= 0 && pending.slot >= 0 && native.slot != pending.slot) return false;
    if(native.entityIndex >= 0 && pending.entityIndex >= 0
        && native.entityIndex != pending.entityIndex) return false;
    if(0 <= native.demoTick && 0 <= pending.queuedDemoTick
        && abs(native.demoTick - pending.queuedDemoTick) > kSyntheticAudioWaitTicks + 2) return false;
    if(native.token[0] != '\0' && pending.token[0] != '\0'
        && 0 != strcmp(native.token, pending.token)
        && native.slot < 0 && pending.slot < 0) return false;
    return true;
}

bool HasRecentNativeAudioLocked(int entityIndex, int slot, const char * token, int demoTick)
{
    PendingSyntheticAudio pending = {};
    pending.entityIndex = entityIndex;
    pending.slot = slot;
    pending.queuedDemoTick = demoTick;
    if(token != nullptr) strncpy_s(pending.token, sizeof(pending.token), token, _TRUNCATE);
    for(const RecentNativeAudio & native : g_RecentNativeAudios) {
        if(NativeAudioMatchesPending(native, pending)) return true;
    }
    return false;
}

void RecordNativeAudioObservation(int entityIndex, int slot, const char * token, const char * source)
{
    if(slot < 0 && (nullptr == token || '\0' == token[0])) return;
    const ULONGLONG now = GetTickCount64();
    RecentNativeAudio native = {};
    native.timeMs = now;
    native.demoTick = g_LastDemoTick;
    native.entityIndex = entityIndex;
    native.slot = slot;
    if(token != nullptr) strncpy_s(native.token, sizeof(native.token), token, _TRUNCATE);

    LONG suppressed = 0;
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeAudiosLocked(now, g_LastDemoTick);
    bool duplicate = false;
    for(const RecentNativeAudio & recent : g_RecentNativeAudios) {
        if(recent.slot == native.slot
            && recent.entityIndex == native.entityIndex
            && 0 == strcmp(recent.token, native.token)
            && now - recent.timeMs <= 20) {
            duplicate = true;
            break;
        }
    }
    if(!duplicate) {
        g_RecentNativeAudios.push_back(native);
        while(kMaxRecentRadios < g_RecentNativeAudios.size()) g_RecentNativeAudios.pop_front();
    }

    for(auto it = g_PendingSyntheticAudios.begin(); it != g_PendingSyntheticAudios.end();) {
        if(!NativeAudioMatchesPending(native, *it)) {
            ++it;
            continue;
        }
        it = g_PendingSyntheticAudios.erase(it);
        ++suppressed;
    }
    ReleaseSRWLockExclusive(&g_StateLock);

}

bool HasRecentNativeAudio(int entityIndex, int slot, const char * token)
{
    const ULONGLONG now = GetTickCount64();
    bool found = false;
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeAudiosLocked(now, g_LastDemoTick);
    found = HasRecentNativeAudioLocked(entityIndex, slot, token, g_LastDemoTick);
    ReleaseSRWLockExclusive(&g_StateLock);
    return found;
}

void QueueSyntheticAudio(CEntityInstance * controller, int slot, const char * source)
{
    if(!IsSyntheticAudioEnabled() || nullptr == controller || slot < 0
        || IsSuppressedRadioSlot(slot)) return;
    char cue[192] = {};
    if(!BuildSyntheticAudioCue(controller, slot, cue, sizeof(cue))) {
        return;
    }

    const int entityIndex = GetEntityIndex(controller);
    const int controllerHandle = GetControllerHandle(controller);
    const int sourceEntityIndex = IsSyntheticAudioSpatialized()
        ? GetAudioSourceEntityIndex(controller)
        : -1;
    if(IsSyntheticAudioSpatialized() && sourceEntityIndex < 0) {
        return;
    }
    if(HasRecentNativeAudio(entityIndex, slot, cue)) {
        return;
    }

    PendingSyntheticAudio pending = {};
    pending.queuedMs = GetTickCount64();
    pending.queuedDemoTick = g_LastDemoTick;
    pending.entityIndex = entityIndex;
    pending.sourceEntityIndex = sourceEntityIndex;
    pending.controllerHandle = controllerHandle;
    pending.slot = slot;
    strncpy_s(pending.source, sizeof(pending.source), source ? source : "event", _TRUNCATE);
    strncpy_s(pending.token, sizeof(pending.token), cue, _TRUNCATE);

    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeAudiosLocked(pending.queuedMs, g_LastDemoTick);
    for(const PendingSyntheticAudio & recent : g_PendingSyntheticAudios) {
        if(recent.entityIndex == pending.entityIndex
            && recent.slot == pending.slot
            && pending.queuedMs - recent.queuedMs <= kRecentNativeAudioWindowMs
            && (g_LastDemoTick < 0 || recent.queuedDemoTick < 0
                || abs(g_LastDemoTick - recent.queuedDemoTick) <= kSyntheticAudioWaitTicks)) {
            ReleaseSRWLockExclusive(&g_StateLock);
            return;
        }
    }
    g_PendingSyntheticAudios.push_back(pending);
    while(kMaxRecentRadios < g_PendingSyntheticAudios.size()) g_PendingSyntheticAudios.pop_front();
    ReleaseSRWLockExclusive(&g_StateLock);
}

void EmitSyntheticAudio(const PendingSyntheticAudio & pending)
{
    if(!IsSyntheticAudioEnabled()) return;
    if(IsSyntheticAudioSpatialized() && pending.sourceEntityIndex < 0) {
        return;
    }
    const bool emitted = IsSyntheticAudioSpatialized()
        ? MirvPovSoundCircle_EmitSoundAtEntity(pending.token, pending.sourceEntityIndex)
        : MirvPovSoundCircle_EmitSoundGlobal(pending.token);
    if(!emitted) {
        return;
    }
}

void ProcessPendingSyntheticAudio()
{
    if(!IsSyntheticAudioEnabled()) return;
    const ULONGLONG now = GetTickCount64();
    std::vector<PendingSyntheticAudio> due;
    AcquireSRWLockExclusive(&g_StateLock);
    PruneRecentNativeAudiosLocked(now, g_LastDemoTick);
    for(auto it = g_PendingSyntheticAudios.begin(); it != g_PendingSyntheticAudios.end();) {
        bool dueByTime = now - it->queuedMs >= kSyntheticAudioWaitMs;
        bool dueByTick = 0 <= g_LastDemoTick && 0 <= it->queuedDemoTick
            && g_LastDemoTick >= it->queuedDemoTick + kSyntheticAudioWaitTicks;
        if(!dueByTime && !dueByTick) {
            ++it;
            continue;
        }
        if(HasRecentNativeAudioLocked(it->entityIndex, it->slot, it->token, g_LastDemoTick)) {
            it = g_PendingSyntheticAudios.erase(it);
            continue;
        }
        due.push_back(*it);
        it = g_PendingSyntheticAudios.erase(it);
    }
    ReleaseSRWLockExclusive(&g_StateLock);
    for(const PendingSyntheticAudio & pending : due) EmitSyntheticAudio(pending);
}

bool DispatchRadioNotice(
    CEntityInstance * controller,
    int slot,
    const char * body,
    const char * source,
    bool recordForAutoDedupe,
    bool soundFallbackRecord = false)
{
    if(nullptr == controller || nullptr == body || nullptr == source
        || IsSuppressedRadioSlot(slot)) return false;

    const char * playerName = "player";
    __try {
        const char * value = controller->GetPlayerName();
        if(nullptr != value && '\0' != value[0]) playerName = value;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    int controllerHandle = GetControllerHandle(controller);
    int entityIndex = GetEntityIndex(controller);
    int teamNumber = ResolveControllerTeam(controller);
    const char * teamPrefix = 3 == teamNumber
        ? "[CT]"
        : 2 == teamNumber ? "[T]" : "[?]";
    int clientSlot = GetControllerClientSlot(controller);
    char placeName[64];
    bool hasPlaceName = CopyControllerLocation(controller, placeName, sizeof(placeName));
    const int bodyColor = GetRadioTextColorControl(slot);
    char text[512];
    if(hasPlaceName) {
        snprintf(
            text,
            sizeof(text),
            "%s %c%s%c@%s%c%s%c%s%c",
            teamPrefix,
            0x03,
            playerName,
            0x04,
            placeName,
            0x01,
            reinterpret_cast<const char *>(kFullWidthColonUtf8),
            bodyColor,
            body,
            0x01);
    } else {
        snprintf(
            text,
            sizeof(text),
            "%s %c%s%c%s%c%s%c",
            teamPrefix,
            0x03,
            playerName,
            0x01,
            reinterpret_cast<const char *>(kFullWidthColonUtf8),
            bodyColor,
            body,
            0x01);
    }
    text[sizeof(text) - 1] = '\0';
    // The 0x03 marker is resolved by the native HudChat converter using the
    // client slot, which also restores the small player-color dot.  Keep the
    // Keep the controller entity index for deduplication.
    if(!MirvPovKillReward_PushHudChatText(text, clientSlot, source)) return false;
    if(recordForAutoDedupe) RecordSyntheticRadio(entityIndex, soundFallbackRecord);
    if(recordForAutoDedupe && IsSyntheticAudioSource(source))
        QueueSyntheticAudio(controller, slot, source);
    return true;
}

bool DispatchGrenadeRadioNotice(
    CEntityInstance * controller,
    int slot,
    const char * body,
    const char * source,
    bool recordForAutoDedupe)
{
    if(nullptr == controller || slot < 0 || nullptr == body) return false;
    int entityIndex = GetEntityIndex(controller);
    if(IsRecentRadio(entityIndex, slot)) {
                return false;
    }

    bool dispatched = DispatchRadioNotice(
        controller,
        slot,
        body,
        source,
        recordForAutoDedupe,
        false);
    if(dispatched) RecordRecentRadio(entityIndex, slot);
    return dispatched;
}

void ProcessProjectileEntity(CEntityInstance * entity, int suppliedHandle, const char * source, bool allowEmit)
{
    if(!MirvPov_IsEnabled() || nullptr == entity) return;
    int mode = GetRadioMode();
    if(mode < 2 || 6 < mode) return;

    const char * className = nullptr;
    __try {
        className = entity->GetClassName();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        className = nullptr;
    }
    if(!IsGrenadeProjectileClass(className)) return;
    int slot = GetGrenadeSlot(className);
    const char * body = GetGrenadeText(className);
    if(slot < 0 || nullptr == body) return;

    int entityHandle = suppliedHandle;
    int entityIndex = -1;
    __try {
        auto handle = entity->GetHandle();
        if(handle.IsValid()) {
            entityHandle = handle.ToInt();
            entityIndex = handle.GetEntryIndex();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    if(entityHandle < 0 || entityIndex < 0) return;
    RememberProjectile(entityHandle, entityIndex, slot, className);
    if(IsProjectileAlreadyEmitted(entityHandle, entityIndex, slot)) return;
    if(!allowEmit) {
        // The first entity snapshot after enabling/seeking can contain
        // projectiles that were created earlier in the demo.  Prime the
        // ledger without replaying those historical radio notices.
        MarkProjectileEmitted(entityHandle);
        return;
    }

    CEntityInstance * controller = ResolveProjectileController(entity);
    if(nullptr == controller) {
        return;
    }

    CEntityInstance * povController = GetStablePovController();
    int controllerHandle = GetControllerHandle(controller);
    int controllerEntityIndex = GetEntityIndex(controller);
    int povHandle = GetControllerHandle(povController);
    int controllerTeam = ResolveControllerTeam(controller);
    int povTeam = ResolveControllerTeam(povController);
    MarkProjectileThrower(entityHandle, controllerHandle, slot);
    bool controllerValid = controllerHandle >= 0
        && IsPlayableTeam(controllerTeam);
    bool teamFilterPassed = (3 == mode || 6 == mode)
        ? controllerValid
        : controllerValid
            && ((2 != povTeam && 3 != povTeam) || controllerTeam == povTeam);
    if(!teamFilterPassed) {
        if(controllerValid) MarkProjectileEmitted(entityHandle);
                return;
    }

    if(4 == mode && ConsumeRecentNativeForEvent(controllerEntityIndex, "projectile")) {
        MarkProjectileEmitted(entityHandle);
        return;
    }

    if(ConsumeGrenadeThrowEventForProjectile(controllerHandle, slot)) {
        MarkProjectileEmitted(entityHandle);
                return;
    }

    if(DispatchGrenadeRadioNotice(controller, slot, body, source ? source : "projectile", 4 == mode)) {
        MarkProjectileEmitted(entityHandle);
    }
}

void ScanProjectileEntities(bool primeOnly)
{
    if(!MirvPov_IsEnabled() || nullptr == g_pEntityList || nullptr == *g_pEntityList
        || nullptr == g_GetEntityFromIndex || nullptr == g_GetHighestEntityIndex) return;
    if(!primeOnly && !IsProjectileScanFallbackEnabled()) return;
    int mode = GetRadioMode();
    if(mode < 2 || 6 < mode) return;

    int highestIndex = GetHighestEntityIndex();
    if(highestIndex < 0) return;
    if(highestIndex > 8192) highestIndex = 8192;
    for(int index = 0; index <= highestIndex; ++index) {
        CEntityInstance * entity = GetEntityFromIndex(index);
        if(nullptr != entity) ProcessProjectileEntity(
            entity,
            -1,
            primeOnly ? "projectile-prime" : "projectile-scan",
            !primeOnly);
    }
}

bool FindUniquePattern(
    const Afx::BinUtils::MemRange & textRange,
    const char * pattern,
    size_t & address)
{
    auto sequence = Afx::BinUtils::FindPatternString(textRange, pattern);
    if(sequence.IsEmpty()) return false;

    auto remaining = Afx::BinUtils::MemRange(sequence.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(remaining, pattern).IsEmpty()) return false;

    address = sequence.Start;
    return true;
}

void RestoreDemoHudChatSuppress(unsigned char * demoController, unsigned char value)
{
    if(nullptr == demoController) return;
    __try {
        demoController[kDemoHudChatSuppressOffset] = value;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool CopySendAudioCandidate(const char * source, char * output, size_t outputSize)
{
    if(nullptr == source || nullptr == output || outputSize < 2) return false;
    output[0] = '\0';
    __try {
        size_t length = 0;
        for(; length + 1 < outputSize; ++length) {
            unsigned char value = static_cast<unsigned char>(source[length]);
            if(0 == value) break;
            // User-message radio tokens are ASCII. Reject pointers into a
            // string/object header rather than copying arbitrary memory into
            // the console or HudChat path.
            if(value < 0x20 || value > 0x7e) {
                output[0] = '\0';
                return false;
            }
            output[length] = static_cast<char>(value);
        }
        if(0 == length || length + 1 >= outputSize) {
            output[0] = '\0';
            return false;
        }
        output[length] = '\0';
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

bool CopySendAudioBytes(const char * source, size_t length, char * output, size_t outputSize)
{
    if(nullptr == source || nullptr == output || outputSize < 2
        || 0 == length || outputSize <= length) return false;
    __try {
        for(size_t i = 0; i < length; ++i) {
            unsigned char value = static_cast<unsigned char>(source[i]);
            if(value < 0x20 || value > 0x7e) {
                output[0] = '\0';
                return false;
            }
            output[i] = static_cast<char>(value);
        }
        output[length] = '\0';
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

bool LooksLikeRadioToken(const char * value)
{
    if(nullptr == value || '\0' == value[0]) return false;
    return ContainsInsensitive(value, "radio")
        || ContainsInsensitive(value, "bomb")
        || ContainsInsensitive(value, "grenade")
        || ContainsInsensitive(value, "smoke")
        || ContainsInsensitive(value, "flash")
        || ContainsInsensitive(value, "molotov")
        || ContainsInsensitive(value, "incendiary")
        || ContainsInsensitive(value, "decoy")
        || ContainsInsensitive(value, "characters")
        || ContainsInsensitive(value, "voice")
        || ContainsInsensitive(value, "throwing")
        || ContainsInsensitive(value, "fireinthehole");
}

bool DecodeTaggedRadioField(const void * fieldAddress, char * output, size_t outputSize)
{
    if(nullptr == fieldAddress || nullptr == output || outputSize < 2) return false;
    output[0] = '\0';

    char candidate[192];
    const unsigned char * field = reinterpret_cast<const unsigned char *>(fieldAddress);

    // Current client.dll uses a tagged pointer to a CBufferString-like object:
    // object+0x10 is the byte length, object+0x18 is capacity, and data is
    // inline at object+0x00 when capacity <= 0x0f or points to heap data through
    // object+0x00 otherwise.  This is the exact layout used by the concrete
    // SendAudio parser and by DoStartSoundEvent.
    __try {
        uintptr_t tagged = *reinterpret_cast<const uintptr_t *>(field);
        uintptr_t base = tagged & ~static_cast<uintptr_t>(3);
        if(base >= 0x10000) {
            size_t length = *reinterpret_cast<const uint32_t *>(base + 0x10);
            uint64_t capacity = *reinterpret_cast<const uint64_t *>(base + 0x18);
            if(0 < length && length < sizeof(candidate) && length < outputSize) {
                uintptr_t data = capacity > 0x0f
                    ? *reinterpret_cast<const uintptr_t *>(base)
                    : base;
                if(data >= 0x10000
                    && CopySendAudioBytes(reinterpret_cast<const char *>(data), length, output, outputSize)
                    && LooksLikeRadioToken(output)) return true;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    if(CopySendAudioCandidate(reinterpret_cast<const char *>(field), candidate, sizeof(candidate))
        && LooksLikeRadioToken(candidate)) {
        strncpy_s(output, outputSize, candidate, _TRUNCATE);
        return true;
    }
    if(CopySendAudioCandidate(reinterpret_cast<const char *>(field + 8), candidate, sizeof(candidate))
        && LooksLikeRadioToken(candidate)) {
        strncpy_s(output, outputSize, candidate, _TRUNCATE);
        return true;
    }

    __try {
        uintptr_t tagged = *reinterpret_cast<const uintptr_t *>(field);
        uintptr_t base = tagged & ~static_cast<uintptr_t>(3);
        if(0 == base || base < 0x10000) return false;

        // CBufferString's tagged heap representation points at an object;
        // the actual bytes begin at (tagged & ~3) + 8.
        const char * objectString = reinterpret_cast<const char *>(base + 8);
        if(CopySendAudioCandidate(objectString, candidate, sizeof(candidate))
            && LooksLikeRadioToken(candidate)) {
            strncpy_s(output, outputSize, candidate, _TRUNCATE);
            return true;
        }

        // Keep direct-pointer and pointer-in-object fallbacks for short-string
        // layouts used by older client builds.
        if(CopySendAudioCandidate(reinterpret_cast<const char *>(base), candidate, sizeof(candidate))
            && LooksLikeRadioToken(candidate)) {
            strncpy_s(output, outputSize, candidate, _TRUNCATE);
            return true;
        }
        uintptr_t dataPointer = *reinterpret_cast<const uintptr_t *>(base);
        if(dataPointer >= 0x10000
            && CopySendAudioCandidate(reinterpret_cast<const char *>(dataPointer), candidate, sizeof(candidate))
            && LooksLikeRadioToken(candidate)) {
            strncpy_s(output, outputSize, candidate, _TRUNCATE);
            return true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool IsSuppressedSendAudioMessage(void * message)
{
    char token[192] = {};
    return nullptr != message
        && DecodeTaggedRadioField(
            reinterpret_cast<unsigned char *>(message) + 0x48,
            token,
            sizeof(token))
        && IsSuppressedRadioToken(token);
}

bool IsSuppressedRawAudioMessage(void * message)
{
    char voice[192] = {};
    return nullptr != message
        && DecodeTaggedRadioField(
            reinterpret_cast<unsigned char *>(message) + 0x48,
            voice,
            sizeof(voice))
        && IsSuppressedRadioToken(voice);
}

bool DecodeSendAudioToken(void * message, char * output, size_t outputSize)
{
    if(nullptr == message || nullptr == output || outputSize < 2) return false;
    output[0] = '\0';

    // The SendAudio delegate receives the generic user-message wrapper. IDA
    // shows its CCSUsrMsg_SendAudio payload at wrapper+0x30 and the
    // radio_sound CBufferString at payload+0x08 (wrapper+0x38). Keep the
    // neighbouring offsets as guarded fallbacks for minor demo wrapper
    // changes seen between CS2 builds.
    static const size_t fieldOffsets[] = {0x38, 0x08, 0x48};
    for(size_t fieldOffset : fieldOffsets) {
        if(DecodeTaggedRadioField(
            reinterpret_cast<unsigned char *>(message) + fieldOffset,
            output,
            outputSize)) return true;
    }
    return false;
}

CEntityInstance * ResolveSendAudioController(int slot, bool & usedFallback)
{
    usedFallback = false;
    ULONGLONG now = GetTickCount64();

    // SendAudio itself has no sender field. When the matching sound event was
    // decoded, retain its source entity for a short window and use that for
    // the real player name/location. This covers manual radio, grenades and
    // bomb voice lines on builds that omit RadioText/game events in demos.
    if(0 <= g_LastSoundControllerEntityIndex
        && now - g_LastSoundControllerTimeMs <= kRecentSoundWindowMs
        && (g_LastSoundControllerSlot == slot
            || 100 <= slot
            || 100 <= g_LastSoundControllerSlot)) {
        __try {
            CEntityInstance * controller = GetEntityFromIndex(g_LastSoundControllerEntityIndex);
            if(nullptr != controller && controller->IsPlayerController()) return controller;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // A native RadioText dispatch is an even stronger source hint when it is
    // available immediately before SendAudio.
    if(0 <= g_LastNativeClientIndex
        && now - g_LastNativeRadioTimeMs <= kRecentNativeWindowMs) {
        if(CEntityInstance * controller = GetControllerFromClientSlot(g_LastNativeClientIndex))
            return controller;
    }

    usedFallback = true;
    return GetStablePovController();
}

CEntityInstance * ResolveRawAudioController(int entityIndex)
{
    if(entityIndex < 0) return nullptr;

    // RawAudio entidx is normally a pawn/entity index.  Account for builds
    // that encode a client slot (or an off-by-one entry index) as well.
    const int candidates[] = {entityIndex, entityIndex - 1, entityIndex + 1};
    for(int candidateIndex : candidates) {
        if(candidateIndex < 0) continue;
        __try {
            CEntityInstance * entity = GetEntityFromIndex(candidateIndex);
            if(nullptr == entity) continue;
            if(entity->IsPlayerController()) return entity;
            if(entity->IsPlayerPawn()) {
                if(CEntityInstance * controller = GetControllerFromPawn(entity)) return controller;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return GetControllerFromClientSlot(entityIndex);
}

bool IsSendAudioTeamAllowed(CEntityInstance * controller, int mode)
{
    if(nullptr == controller || !controller->IsPlayerController()) return false;
    int controllerTeam = ResolveControllerTeam(controller);
    CEntityInstance * povController = GetStablePovController();
    int povTeam = ResolveControllerTeam(povController);
    if(!IsPlayableTeam(controllerTeam)) return false;
    if(3 == mode || 6 == mode) return true;
    // During demo playback the camera may be in spectator state and the POV
    // controller has no T/CT team for a few ticks.  Do not drop every real
    // radio notice in that window; native RadioText itself is visible to the
    // spectator, so the fallback follows the same behavior.
    if(!IsPlayableTeam(povTeam)) return true;
    return controllerTeam == povTeam;
}

void HandleSendAudioToken(const char * token)
{
    if(nullptr == token || '\0' == token[0]) return;
    int mode = GetRadioMode();
    if(4 != mode && 5 != mode && 6 != mode) return;

    int slot = -1;
    const char * body = GetSoundRadioText(token, slot);
    if(nullptr == body || slot < 0) {
        return;
    }
    if(IsSuppressedRadioSlot(slot)) {
                return;
    }
    bool usedFallback = false;
    CEntityInstance * controller = ResolveSendAudioController(slot, usedFallback);
    int entityIndex = GetEntityIndex(controller);
    g_LastSendAudioSlot = slot;
    // Record native audio before mode4's projectile filter.  In mode4 the
    // native projectile text path is intentionally ignored, but its audio is
    // still authoritative and must cancel the delayed synthetic cue.
    RecordNativeAudioObservation(entityIndex, slot, token, "SendAudio");
    if(4 == mode && IsProjectileRadioSlot(slot)) {
                return;
    }

    if(nullptr == controller || !IsSendAudioTeamAllowed(controller, mode)) {
                return;
    }

    if(4 == mode && ConsumeRecentNativeForEvent(entityIndex, "send-audio")) return;
    if(IsRecentRadio(entityIndex, slot)) {
                return;
    }
    if((4 == mode || 5 == mode || 6 == mode)
        && ConsumeRecentSyntheticForFallback(entityIndex, "send-audio")) return;

    DispatchRadioNotice(controller, slot, body, "send-audio", true, false);
}

__int64 __fastcall New_SendAudioEmitter(void * message)
{
    char token[sizeof(g_LastSendAudioToken)] = {};
    bool decoded = false;
    __try {
        // sub_180AF3540 passes its generic message object with the protobuf
        // tuple at +0x08; SendAudio.radio_sound is tuple field 1 at +0x10.
        decoded = DecodeTaggedRadioField(
            nullptr != message
                ? reinterpret_cast<unsigned char *>(message) + 0x10
                : nullptr,
            token,
            sizeof(token));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        decoded = false;
    }
    if(decoded) {
        strncpy_s(g_LastSendAudioToken, sizeof(g_LastSendAudioToken), token, _TRUNCATE);
        g_LastSendAudioTimeMs = GetTickCount64();
        HandleSendAudioToken(token);
        }
    return nullptr != g_OrgSendAudioEmitter
        ? g_OrgSendAudioEmitter(message)
        : 0;
}

__int64 __fastcall New_SendAudioParser(void * message)
{
    char token[sizeof(g_LastSendAudioToken)] = {};
    bool decoded = false;
    bool suppressOriginal = false;
    __try {
        // IDA's current client.dll SendAudio parser reads the actual
        // CCSUsrMsg_SendAudio::radio_sound CBufferString from message+0x48.
        // This is the semantic parser path used by demo playback; the
        // generic delegate/emitter hooks below are retained for older builds.
        decoded = DecodeTaggedRadioField(
            nullptr != message
                ? reinterpret_cast<unsigned char *>(message) + 0x48
                : nullptr,
            token,
            sizeof(token));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        decoded = false;
    }

    if(decoded) {
        strncpy_s(g_LastSendAudioToken, sizeof(g_LastSendAudioToken), token, _TRUNCATE);
        g_LastSendAudioTimeMs = GetTickCount64();
        suppressOriginal = IsSuppressedRadioToken(token);
        HandleSendAudioToken(token);
        }

    if(suppressOriginal) {
                return 0;
    }
    return nullptr != g_OrgSendAudioParser
        ? g_OrgSendAudioParser(message)
        : 0;
}

void HandleRawAudioMessage(void * message)
{
    if(nullptr == message) return;
    int mode = GetRadioMode();
    if(4 != mode && 5 != mode && 6 != mode) return;

    // IDA's CCSUsrMsg_RawAudio formatter reads voice_filename directly from
    // message+0x48, pitch from +0x54 and entidx from +0x58.  The voice field is
    // the authoritative semantic source; SendAudio/sound-event state remains
    // only as a compatibility fallback for older demo builds.
    int slot = -1;
    const char * body = nullptr;
    char voice[192] = "<decode-failed>";
    ULONGLONG now = GetTickCount64();

    __try {
        if(DecodeTaggedRadioField(
            reinterpret_cast<unsigned char *>(message) + 0x48,
            voice,
            sizeof(voice))) {
            body = GetSoundRadioText(voice, slot);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        body = nullptr;
        slot = -1;
    }

    // Older builds can deliver RawAudio without a populated voice_filename;
    // retain the previous token/sound association as a guarded fallback.
    if(0 <= g_LastSendAudioSlot
        && now - g_LastSendAudioTimeMs <= 350
        && '\0' != g_LastSendAudioToken[0]
        && 0 != strcmp(g_LastSendAudioToken, "none")) {
        if(nullptr == body) {
            slot = g_LastSendAudioSlot;
            strncpy_s(voice, sizeof(voice), g_LastSendAudioToken, _TRUNCATE);
            body = GetSoundRadioText(voice, slot);
        }
    }
    if(nullptr == body && now - g_LastSoundControllerTimeMs <= kRecentSoundWindowMs) {
        int soundSlot = -1;
        body = GetSoundRadioText(g_LastSoundName, soundSlot);
        if(nullptr != body) {
            slot = soundSlot;
            strncpy_s(voice, sizeof(voice), g_LastSoundName, _TRUNCATE);
        }
    }

    int rawEntityIndex = -1;
    __try {
        // sub_1810D6A50 reads RawAudio.entidx from message+0x58.
        rawEntityIndex = *reinterpret_cast<int *>(reinterpret_cast<unsigned char *>(message) + 0x58);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        rawEntityIndex = -1;
    }

    if(nullptr == body || slot < 0) {
        return;
    }
    if(IsSuppressedRadioSlot(slot) || IsSuppressedRadioToken(voice)) {
                return;
    }
    bool usedFallback = false;
    CEntityInstance * controller = ResolveRawAudioController(rawEntityIndex);
    if(nullptr == controller) controller = ResolveSendAudioController(slot, usedFallback);
    if(nullptr != controller) RememberObservedAgentVoice(controller, slot, voice);
    // RawAudio commonly carries the pawn entry index, while the pending event
    // ledger uses the controller entry index. Normalize the observation when
    // the controller can be resolved so native audio reliably cancels the
    // delayed synthetic cue.
    int entityIndex = nullptr != controller ? GetEntityIndex(controller) : rawEntityIndex;
    RecordNativeAudioObservation(entityIndex, slot, voice, "RawAudio");
    if(4 == mode && IsProjectileRadioSlot(slot)) {
                return;
    }
    if(nullptr == controller || !IsSendAudioTeamAllowed(controller, mode)) {
                return;
    }

    entityIndex = GetEntityIndex(controller);
    if(0 <= entityIndex) {
        g_LastSoundControllerEntityIndex = entityIndex;
        g_LastSoundControllerSlot = slot;
        g_LastSoundControllerTimeMs = GetTickCount64();
    }
    if(4 == mode && ConsumeRecentNativeForEvent(entityIndex, "raw-audio")) return;
    if(IsRecentRadio(entityIndex, slot)) {
            return;
    }
    if((4 == mode || 5 == mode || 6 == mode)
        && ConsumeRecentSyntheticForFallback(entityIndex, "raw-audio")) return;

    DispatchRadioNotice(controller, slot, body, "raw-audio", true, false);
}

__int64 __fastcall New_SendAudioDispatch(void * owner, void * message)
{
    const uintptr_t ownerVtable = ReadOwnerVtable(owner);
    const bool expectedOwner = IsExpectedDelegateOwner(owner, g_SendAudioVtable);
    if(!expectedOwner) {
        // CS2 clones CGameMessageDelegateHook for several user-message
        // registrations.  The same Dispatch body is reached with a cloned
        // owner vtable, so filtering it here silently drops real RadioText.
        // Decode first and let the token mapper decide whether this is a radio
        // message; unrelated user messages are ignored without side effects.
        }
    char token[sizeof(g_LastSendAudioToken)];
    bool decoded = DecodeSendAudioToken(message, token, sizeof(token));
    bool suppressOriginal = false;
    if(decoded) {
        strncpy_s(g_LastSendAudioToken, sizeof(g_LastSendAudioToken), token, _TRUNCATE);
        suppressOriginal = IsSuppressedRadioToken(token);
        HandleSendAudioToken(token);
    } else {
        strncpy_s(g_LastSendAudioToken, sizeof(g_LastSendAudioToken), "<decode-failed>", _TRUNCATE);
        }
    g_LastSendAudioTimeMs = GetTickCount64();
    if(suppressOriginal) {
                return 0;
    }
    return nullptr != g_OrgSendAudioDispatch
        ? g_OrgSendAudioDispatch(owner, message)
        : 0;
}

__int64 __fastcall New_RawAudioHandler(void * owner, void * message)
{
    const uintptr_t ownerVtable = ReadOwnerVtable(owner);
    if(!IsExpectedDelegateOwner(owner, g_RawAudioVtable) && nullptr == message) return 0;
    const bool suppressOriginal = IsSuppressedRawAudioMessage(message);
    HandleRawAudioMessage(message);
    if(suppressOriginal) {
                return 0;
    }
    return nullptr != g_OrgRawAudioHandler
        ? g_OrgRawAudioHandler(owner, message)
        : 0;
}

// Typed RawAudio formatter.  The generic delegate receives a wrapper whose
// layout is not guaranteed to match CCSUsrMsg_RawAudio_t.  IDA shows the
// formatter itself reading voice_filename at +0x48 and entidx at +0x58, so
// process the message here as the authoritative path and keep the generic
// dispatch hook only as a compatibility/deduplication source.
__int64 __fastcall New_RawAudioFormatter(void * owner, void * message)
{
    const bool suppressOriginal = IsSuppressedRawAudioMessage(message);
    HandleRawAudioMessage(message);
    if(suppressOriginal) {
                return 0;
    }
    return nullptr != g_OrgRawAudioFormatter
        ? g_OrgRawAudioFormatter(owner, message)
        : 0;
}

__int64 __fastcall New_RadioTextDispatch(void * owner, void * message)
{
    const uintptr_t ownerVtable = ReadOwnerVtable(owner);

    // The current RadioText table is the one at image+0x1B75608.  The
    // dispatcher is shared by several user-message registrations in some
    // builds, so do not reject cloned owners here; the typed formatter remains
    // responsible for
    // deciding whether the message is actually RadioText.
    if(nullptr != message) {
        }

    unsigned char * demoController = nullptr;
    unsigned char previousSuppress = 0;
    bool restoreSuppress = false;
    const bool povEnabled = MirvPov_IsEnabled();
    // The kill-reward patch and RadioText use related demo guards, but they
    // are not the same formatter on all client builds.  Always manage the
    // RadioText guard here; otherwise a kill-reward initialization can make
    // `IsHudChatDemoBypassApplied()` true while RadioText remains suppressed.
    const bool manageDemoGuard = povEnabled
        && nullptr != g_GetDemoController;
    bool releaseDemoGuardLock = false;

    // Demo playback normally marks HudChat as suppressed.  Clear that guard
    // around the *delegate dispatch*, not only around the typed formatter:
    // this covers the path where the callback is invoked through a temporary
    // message object and the formatter hook is not reached by our detour.
    if(manageDemoGuard) {
        if(0 == g_RadioDispatchDepth) {
            AcquireSRWLockExclusive(&g_DemoGuardLock);
            releaseDemoGuardLock = true;
        }
        ++g_RadioDispatchDepth;
        __try {
            demoController = g_GetDemoController();
            if(nullptr != demoController) {
                previousSuppress = demoController[kDemoHudChatSuppressOffset];
                if(0 != previousSuppress) {
                    demoController[kDemoHudChatSuppressOffset] = 0;
                    restoreSuppress = true;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            demoController = nullptr;
            restoreSuppress = false;
        }
    }

    __int64 result = 0;
    __try {
        result = nullptr != g_OrgRadioTextDispatch
            ? g_OrgRadioTextDispatch(owner, message)
            : 0;
    } __finally {
        if(restoreSuppress) RestoreDemoHudChatSuppress(demoController, previousSuppress);
        if(manageDemoGuard) --g_RadioDispatchDepth;
        if(releaseDemoGuardLock) ReleaseSRWLockExclusive(&g_DemoGuardLock);
    }
    return result;
}

void __fastcall New_RadioTextHandler(void * owner, void * message)
{
    if(nullptr == g_OrgRadioTextHandler) return;

    int clientIndex = -1;
    __try {
        if(nullptr != message) {
            clientIndex = *reinterpret_cast<int *>(
                reinterpret_cast<unsigned char *>(message) + 0x6c);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        clientIndex = -1;
    }
    CEntityInstance * nativeController = GetControllerFromClientSlot(clientIndex);
    int nativeEntityIndex = GetEntityIndex(nativeController);

    bool povEnabled = MirvPov_IsEnabled();
    int mode = GetRadioMode();
    bool callOriginal = !povEnabled || 1 == mode || 4 == mode;
    if(povEnabled && (1 == mode || 4 == mode)
        && !IsNativeRadioVisibleForCurrentPov(clientIndex)) {
        return;
    }
    if(povEnabled && 4 == mode && ConsumeRecentSyntheticForNative(nativeEntityIndex))
        callOriginal = false;
    if(!callOriginal) {
        if(4 != mode)
            return;
    }

    unsigned char * demoController = nullptr;
    unsigned char previousSuppress = 0;
    bool restoreSuppress = false;
    bool manageDemoGuard = povEnabled
        && nullptr != g_GetDemoController;
    bool releaseDemoGuardLock = false;

    if(manageDemoGuard) {
        if(0 == g_RadioDispatchDepth) {
            AcquireSRWLockExclusive(&g_DemoGuardLock);
            releaseDemoGuardLock = true;
        }
        ++g_RadioDispatchDepth;

        __try {
            demoController = g_GetDemoController();
            if(nullptr != demoController) {
                previousSuppress = demoController[kDemoHudChatSuppressOffset];
                if(0 != previousSuppress) {
                    demoController[kDemoHudChatSuppressOffset] = 0;
                    restoreSuppress = true;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            demoController = nullptr;
            restoreSuppress = false;
        }
    }

    __try {
                // Preserve the formatter's owner exactly.  IDA confirms that this is
        // the object passed in RCX by CGameMessageDelegateHook::Dispatch; it is
        // not a client-slot integer.  The payload remains the original RDX.
        g_OrgRadioTextHandler(owner, message);
        g_LastNativeClientIndex = clientIndex;
        if(povEnabled && 4 == mode) RecordNativeRadio(nativeEntityIndex);
    } __finally {
        if(restoreSuppress) RestoreDemoHudChatSuppress(demoController, previousSuppress);
        if(manageDemoGuard) --g_RadioDispatchDepth;
        if(releaseDemoGuardLock) ReleaseSRWLockExclusive(&g_DemoGuardLock);
    }
}

} // namespace

void MirvPovRadio_HandleEntityAdded(CEntityInstance * entity, int handle)
{
    if(!IsProjectileScanFallbackEnabled()) return;
    ProcessProjectileEntity(entity, handle, "entity-add", g_ProjectileScanPrimed);
}

void MirvPovRadio_Initialize(HMODULE clientDll)
{
    if(g_Hooked || nullptr == clientDll) return;

    const uintptr_t clientBase = reinterpret_cast<uintptr_t>(clientDll);
    g_RadioTextVtable = reinterpret_cast<const void *>(clientBase + kRadioTextVtableRva);
    g_SendAudioVtable = reinterpret_cast<const void *>(clientBase + kSendAudioVtableRva);
    g_RawAudioVtable = reinterpret_cast<const void *>(clientBase + kRawAudioVtableRva);

    if(nullptr == g_HashString) {
        g_HashString = reinterpret_cast<HashString_t>(getAddress(
            clientDll,
            "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? 0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20"));
    }

    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    sections.Next(IMAGE_SCN_MEM_EXECUTE);
    if(!sections.Eof()) textRange = sections.GetMemRange();
    if(textRange.IsEmpty()) {
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] client.dll text section was not found.\n");
        return;
    }

    // IDA Pro (client.dll 2026-08-10): this is the actual HudChat formatter
    // for CCSUsrMsg_RadioText_t at 0x1810D6240.  It receives (owner, message)
    // and reads the RadioText protobuf wrapper at message+0x48/+0x60/+0x6c.
    // CGameMessageDelegateHook::Dispatch at 0x1810D5040 is a generic user
    // message dispatcher; its second argument is a different wrapper and
    // must not be hooked with the RadioText message offsets above.
    // The formatter exits when demoController[0x72] is set, so clear that
    // guard only for the duration of this callback and preserve all native
    // localization / player / team formatting.
    const char * radioTextHandlerPattern =
        "48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 "
        "48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 81 A5";
    const char * getDemoControllerPattern =
        "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 85 D2";

    size_t radioTextDispatchAddress = 0;
    size_t radioTextHandlerAddress = 0;
    size_t getDemoControllerAddress = 0;
    const bool radioTextDispatchResolved = ReadDelegateDispatch(
        g_RadioTextVtable,
        textRange,
        radioTextDispatchAddress);
    if(radioTextDispatchResolved) {
        g_OrgRadioTextDispatch = reinterpret_cast<RadioTextDispatch_t>(radioTextDispatchAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgRadioTextDispatch, New_RadioTextDispatch);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_RadioTextDispatchHooked = true;
        } else {
            g_OrgRadioTextDispatch = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RadioText Dispatch detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RadioText Dispatch vtable slot was not resolved.\n");
    }
    bool radioTextPatternFound = FindUniquePattern(
        textRange,
        radioTextHandlerPattern,
        radioTextHandlerAddress);
    bool demoControllerPatternFound = FindUniquePattern(
        textRange,
        getDemoControllerPattern,
        getDemoControllerAddress);

    // The current client.dll's formatter RVA is known from the IDA database.
    // Use it when a compiler update changes the prologue enough for the byte
    // signature above to stop matching; the range check keeps this fallback
    // restricted to an executable address in the loaded module.
    if(!radioTextPatternFound) {
        const size_t knownAddress = clientBase + kRadioTextFormatterRva;
        if(textRange.Start <= knownAddress && knownAddress < textRange.End) {
            radioTextHandlerAddress = knownAddress;
            radioTextPatternFound = true;
                }
    }

    if(radioTextPatternFound) {
        g_OrgRadioTextHandler = reinterpret_cast<RadioTextHandler_t>(radioTextHandlerAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgRadioTextHandler, New_RadioTextHandler);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_RadioTextHooked = true;
        } else {
            g_OrgRadioTextHandler = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RadioText detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RadioText handler pattern was not found uniquely.\n");
    }
    // Prefer the IDA-confirmed RVA. The current getter is only seven bytes
    // (`48 8D 05 ?? ?? ?? ?? C3`) and therefore does not match the legacy
    // padded signature above. Keeping the pattern as a fallback preserves
    // compatibility with older client builds.
    const size_t knownDemoControllerAddress = clientBase + kDemoControllerRva;
    if(textRange.Start <= knownDemoControllerAddress
        && knownDemoControllerAddress < textRange.End) {
        g_GetDemoController = reinterpret_cast<GetDemoController_t>(knownDemoControllerAddress);
        getDemoControllerAddress = knownDemoControllerAddress;
        demoControllerPatternFound = true;
        } else if(demoControllerPatternFound) {
        g_GetDemoController = reinterpret_cast<GetDemoController_t>(getDemoControllerAddress);
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] Demo controller getter was not resolved.\n");
    }

    // IDA Pro (client.dll 2026-08-11): the SendAudio delegate's vtable is
    // 0x181B756E8 and its Dispatch slot (+0x28) is sub_1810D5210.  The
    // function is one of several cloned CGameMessageDelegateHook bodies, so
    // match the unique SendAudio tail (the call to sub_180B12310) and step
    // back 0x8d bytes to the function start.  This is the actual
    // Dispatch(owner, message) path; the nearby CBufferString/protobuf
    // formatter is not a user-message callback and must not be detoured.
    const char * sendAudioTailPattern =
        "48 89 BC 24 E0 00 00 00 E8 D6 D9 FD FF "
        "48 8B 4B 38 48 8D 56 30 F2 0F 10 46 08 48 8B F8";
    size_t sendAudioAddress = 0;
    size_t sendAudioTailAddress = 0;
    bool sendAudioResolved = ReadDelegateDispatch(g_SendAudioVtable, textRange, sendAudioAddress);
    if(!sendAudioResolved) {
        // Keep compatibility with the previous client build whose SendAudio
        // delegate used the neighboring RVA 0x1B05578.
        const void * legacyVtable = reinterpret_cast<const void *>(clientBase + kSendAudioVtableLegacyRva);
        size_t legacyAddress = 0;
        if(ReadDelegateDispatch(legacyVtable, textRange, legacyAddress)) {
            g_SendAudioVtable = legacyVtable;
            sendAudioAddress = legacyAddress;
            sendAudioResolved = true;
        }
    }
    if(!sendAudioResolved
        && FindUniquePattern(textRange, sendAudioTailPattern, sendAudioTailAddress)
        && sendAudioTailAddress >= 0x8d) {
        sendAudioAddress = sendAudioTailAddress - 0x8d;
        sendAudioResolved = true;
    }
    if(sendAudioResolved) {
        g_OrgSendAudioDispatch = reinterpret_cast<SendAudioDispatch_t>(sendAudioAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgSendAudioDispatch, New_SendAudioDispatch);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_SendAudioHooked = true;
        } else {
            g_OrgSendAudioDispatch = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] SendAudio formatter detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] SendAudio Dispatch pattern was not found uniquely.\n");
    }

    // The user-message reader first enters a small per-message emitter and
    // only then calls CGameMessageDelegateHook::Dispatch.  Hooking this stage
    // gives us a stable fallback when a build uses a cloned Dispatch owner or
    // bypasses the delegate vtable during demo playback.
    const char * sendAudioEmitterPattern =
        "40 53 48 81 EC ?? ?? ?? ?? F2 0F 10 41 ?? 48 8D 05 ?? ?? ?? ?? "
        "48 89 44 24 ?? 48 8D 51 ?? 8B 41 ?? 48 8B D9 89 44 24 ?? "
        "0F B6 41 ?? 88 44 24 ?? 8B 41 ?? 89 44 24 ?? 8B 41 ?? "
        "89 44 24 ?? 48 8B 41 ??";
    size_t sendAudioEmitterAddress = 0;
    if(FindUniquePattern(textRange, sendAudioEmitterPattern, sendAudioEmitterAddress)) {
        g_OrgSendAudioEmitter = reinterpret_cast<SendAudioEmitter_t>(sendAudioEmitterAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgSendAudioEmitter, New_SendAudioEmitter);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_SendAudioEmitterHooked = true;
        } else {
            g_OrgSendAudioEmitter = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] SendAudio emitter detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] SendAudio emitter pattern was not found uniquely.\n");
    }

    // IDA Pro (client.dll 2026-08-11): this is the concrete SendAudio
    // protobuf parser at image+0xB04D30. Unlike the generic delegate wrapper,
    // it receives the typed message directly and reads radio_sound from
    // message+0x48. Hook it as the primary semantic path so demo playback
    // still exposes manual radio, grenade and bomb voice messages when the
    // delegate/emitter callbacks are bypassed.
    const char * sendAudioParserPattern =
        "40 53 48 83 EC 60 "
        "48 8B 59 48 48 83 E3 FC "
        "48 83 7B 18 0F 76 ?? 48 8B 1B "
        "80 79 50 00 74 ??";
    size_t sendAudioParserAddress = 0;
    bool sendAudioParserResolved = FindUniquePattern(
        textRange,
        sendAudioParserPattern,
        sendAudioParserAddress);
    if(!sendAudioParserResolved) {
        const size_t knownAddress = clientBase + kSendAudioParserRva;
        if(textRange.Start <= knownAddress && knownAddress < textRange.End) {
            sendAudioParserAddress = knownAddress;
            sendAudioParserResolved = true;
                }
    }
    if(sendAudioParserResolved) {
        g_OrgSendAudioParser = reinterpret_cast<SendAudioParser_t>(sendAudioParserAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgSendAudioParser, New_SendAudioParser);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_SendAudioParserHooked = true;
        } else {
            g_OrgSendAudioParser = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] SendAudio parser detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] SendAudio parser pattern was not found uniquely.\n");
    }

    // RawAudio has the same cloned delegate shape.  IDA identifies the
    // RawAudio table at image+0x1B75640 and its +0x28 slot as
    // sub_1810D55B0.  Fall back to the previous table only on older builds.
    size_t rawAudioAddress = 0;
    bool rawAudioResolved = ReadDelegateDispatch(g_RawAudioVtable, textRange, rawAudioAddress);
    if(!rawAudioResolved) {
        const void * legacyVtable = reinterpret_cast<const void *>(clientBase + kRawAudioVtableLegacyRva);
        size_t legacyAddress = 0;
        if(ReadDelegateDispatch(legacyVtable, textRange, legacyAddress)) {
            g_RawAudioVtable = legacyVtable;
            rawAudioAddress = legacyAddress;
            rawAudioResolved = true;
        }
    }
    if(rawAudioResolved) {
        g_OrgRawAudioHandler = reinterpret_cast<RawAudioHandler_t>(rawAudioAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgRawAudioHandler, New_RawAudioHandler);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_RawAudioHooked = true;
        } else {
            g_OrgRawAudioHandler = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RawAudio formatter detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RawAudio formatter pattern was not found uniquely.\n");
    }

    // Hook the typed RawAudio formatter as well as the generic delegate.  The
    // formatter is where IDA confirms the semantic message offsets
    // (voice_filename +0x48, entidx +0x58); this is the path that remains
    // active when demo playback bypasses the delegate's typed callback.
    const size_t rawAudioFormatterAddress = clientBase + kRawAudioFormatterRva;
    if(textRange.Start <= rawAudioFormatterAddress
        && rawAudioFormatterAddress < textRange.End
        && rawAudioFormatterAddress != rawAudioAddress) {
        g_OrgRawAudioFormatter = reinterpret_cast<RawAudioHandler_t>(rawAudioFormatterAddress);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID &)g_OrgRawAudioFormatter, New_RawAudioFormatter);
        if(NO_ERROR == DetourTransactionCommit()) {
            g_RawAudioFormatterHooked = true;
        } else {
            g_OrgRawAudioFormatter = nullptr;
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RawAudio typed formatter detour failed.\n");
        }
    } else {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radio] RawAudio typed formatter RVA is outside the executable image.\n");
    }

    g_Hooked = g_RadioTextDispatchHooked || g_RadioTextHooked || g_SendAudioHooked || g_SendAudioEmitterHooked
        || g_SendAudioParserHooked || g_RawAudioHooked || g_RawAudioFormatterHooked;
    if(!g_Hooked) {
            return;
    }
}

void MirvPovRadio_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !HasGameEventHash() || !MirvPov_IsEnabled()) return;
    const char * eventName = event->GetName();
    if(nullptr == eventName) return;

    // FireEvent and FireEventClientSide can deliver the same object for one
    // network event. Handle the server-side path as the authoritative source,
    // while suppressing the immediate duplicate client-side callback.
    ULONGLONG now = GetTickCount64();
    if(event == g_LastGameEventPointer
        && now - g_LastGameEventTimeMs <= 20
        && 0 == strcmp(eventName, g_LastGameEventName)) return;
    g_LastGameEventPointer = event;
    g_LastGameEventTimeMs = now;
    strncpy_s(g_LastGameEventName, sizeof(g_LastGameEventName), eventName, _TRUNCATE);

    if(0 == strcmp(eventName, "round_start")) {
        MirvPovRadio_Reset("round_start");
        return;
    }

    bool playerRadio = 0 == strcmp(eventName, "player_radio");
    bool grenadeThrown = 0 == strcmp(eventName, "grenade_thrown");
    bool weaponFire = 0 == strcmp(eventName, "weapon_fire");
    // Dropping or picking up the bomb is an inventory/state update, not a
    // radio callout.  Some demo builds expose these as game events even when
    // no native RadioText/voice packet exists, so stop them before they can
    // reach the synthetic text or spatialized-audio paths.
    bool bombInventoryEvent = 0 == strcmp(eventName, "bomb_dropped")
        || 0 == strcmp(eventName, "bomb_pickup");
    if(bombInventoryEvent) {
                return;
    }
    bool bombEvent = 0 == strcmp(eventName, "bomb_planted")
        || 0 == strcmp(eventName, "bomb_beginplant")
        || 0 == strcmp(eventName, "bomb_begindefuse")
        || 0 == strcmp(eventName, "bomb_abortdefuse")
        || 0 == strcmp(eventName, "bomb_defused")
        || 0 == strcmp(eventName, "bomb_exploded");
    if(!playerRadio && !grenadeThrown && !weaponFire && !bombEvent) return;

    if(grenadeThrown || weaponFire) {
        const char * rawWeapon = event->GetString(MakeKey("weapon"));
    }

    int mode = GetRadioMode();
    if(mode < 2 || 6 < mode) {
            return;
    }

    // Auto mode prefers a native RadioText packet when one arrives. Do not
    // discard the game event here: demos can omit RadioText (and the matching
    // radio voice), in which case the event path is the only recoverable data.
    // The native/event queues below deduplicate whichever path arrives second.
    __try {
        auto useridKey = MakeKey("userid");
        CEntityInstance * controller = ResolveEventController(event, useridKey);
        CEntityInstance * povController = GetStablePovController();
        int eventClientSlot = GetEventClientSlot(event, useridKey);
        int controllerHandle = GetControllerHandle(controller);
        int controllerEntityIndex = GetEntityIndex(controller);
        int povHandle = GetControllerHandle(povController);
        int controllerTeam = ResolveControllerTeam(controller);
        int povTeam = ResolveControllerTeam(povController);

        bool controllerValid = controllerHandle >= 0
            && IsPlayableTeam(controllerTeam);
        // During a demo seek/update the controller can be valid while both its
        // controller and pawn team fields are briefly zero.  We still have a
        // concrete sender (and therefore a usable player name/location), so
        // keep the RadioText fallback instead of dropping the entire notice.
        // Native RadioText remains authoritative whenever it is available; this
        // branch only affects the event/sound fallback modes.
        bool controllerTeamPending = controllerHandle >= 0
            && 0 == controllerTeam
            && (4 == mode || 5 == mode || 6 == mode);
        bool pendingSenderIsPov = controllerTeamPending
            && eventClientSlot >= 0
            && eventClientSlot == GetControllerClientSlot(povController);
        bool teamFilterPassed = (3 == mode || 6 == mode)
            ? (controllerValid || controllerTeamPending)
            : controllerValid
                && ((2 != povTeam && 3 != povTeam) || controllerTeam == povTeam)
                || (controllerTeamPending
                    && (2 != povTeam && 3 != povTeam || pendingSenderIsPov));
        if(!teamFilterPassed) {
                            return;
        }

        if(playerRadio) {
            int slot = event->GetInt(MakeKey("slot"));
            if(0 <= controllerEntityIndex) {
                g_LastSoundControllerEntityIndex = controllerEntityIndex;
                g_LastSoundControllerSlot = slot;
                g_LastSoundControllerTimeMs = GetTickCount64();
            }
            if(4 == mode && ConsumeRecentNativeForEvent(controllerEntityIndex, "player_radio")) return;
            if((4 == mode || 5 == mode || 6 == mode)
                && ConsumeRecentSyntheticForFallback(controllerEntityIndex, "player_radio")) return;
            const char * body = GetPlayerRadioText(slot);
            if(nullptr == body) {
                                        return;
            }
            DispatchRadioNotice(
                controller,
                slot,
                body,
                "player_radio",
                4 == mode);
            return;
        }

        if(grenadeThrown || weaponFire) {
            const char * weapon = event->GetString(MakeKey("weapon"));
            const char * body = GetGrenadeText(weapon);
            int slot = GetGrenadeSlot(weapon);
            const char * source = grenadeThrown ? "grenade_thrown" : "weapon_fire";
            if(0 <= controllerEntityIndex) {
                g_LastSoundControllerEntityIndex = controllerEntityIndex;
                g_LastSoundControllerSlot = slot;
                g_LastSoundControllerTimeMs = GetTickCount64();
            }
            if(4 == mode && ConsumeRecentNativeForEvent(controllerEntityIndex, source)) return;
            if((4 == mode || 5 == mode || 6 == mode)
                && ConsumeRecentSyntheticForFallback(controllerEntityIndex, source)) return;
            if(nullptr == body) {
                if(grenadeThrown)
                    return;
            }
            if(controllerHandle >= 0 && 0 <= slot
                && ConsumeProjectileEmissionForGrenadeEvent(controllerHandle, slot)) {
                                        return;
            }
            if(controllerHandle >= 0 && 0 <= slot
                && !RecordGrenadeThrowEvent(controllerHandle, slot)) {
                                        return;
            }
            DispatchGrenadeRadioNotice(controller, slot, body, source, 4 == mode);
            return;
        }

        const char * body = nullptr;
        int slot = -200;
        if(0 == strcmp(eventName, "bomb_beginplant")) {
            body = GetBombPlantingText(); slot = 109;
        } else if(0 == strcmp(eventName, "bomb_planted")) {
            body = GetBombPlantedText(); slot = 110;
        } else if(0 == strcmp(eventName, "bomb_begindefuse")) {
            body = GetBombDefusingText(); slot = 111;
        } else if(0 == strcmp(eventName, "bomb_abortdefuse")) {
            body = GetBombAbortDefuseText(); slot = 112;
        } else if(0 == strcmp(eventName, "bomb_defused")) {
            body = GetBombDefusedText(); slot = 113;
        } else if(0 == strcmp(eventName, "bomb_exploded")) {
            body = GetBombExplodedText(); slot = 114;
        } else if(0 == strcmp(eventName, "bomb_dropped")) {
            body = GetBombDroppedText(); slot = 115;
        } else if(0 == strcmp(eventName, "bomb_pickup")) {
            body = GetBombPickupText(); slot = 116;
        }
        if(0 <= controllerEntityIndex) {
            g_LastSoundControllerEntityIndex = controllerEntityIndex;
            g_LastSoundControllerSlot = slot;
            g_LastSoundControllerTimeMs = GetTickCount64();
        }
        if(4 == mode && ConsumeRecentNativeForEvent(controllerEntityIndex, eventName)) return;
        if((4 == mode || 5 == mode || 6 == mode)
            && ConsumeRecentSyntheticForFallback(controllerEntityIndex, eventName)) return;
        if(nullptr != body) DispatchRadioNotice(controller, slot, body, eventName, 4 == mode);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
            }
}

void MirvPovRadio_HandleSoundEvent(CEntityInstance * sourcePawn, const char * soundName)
{
    if(!MirvPov_IsEnabled() || nullptr == soundName) return;
    int mode = GetRadioMode();
    if(4 != mode && 5 != mode && 6 != mode) return;
    strncpy_s(g_LastSoundName, sizeof(g_LastSoundName), soundName, _TRUNCATE);

    int slot = -1;
    const char * body = GetSoundRadioText(soundName, slot);
    if(nullptr == body) return;
    if(IsSuppressedRadioSlot(slot) || IsSuppressedRadioToken(soundName)) {
                return;
    }
    if(4 == mode && IsProjectileRadioSlot(slot)) {
                return;
    }

    __try {
        CEntityInstance * controller = GetControllerFromPawn(sourcePawn);
        bool usedFallback = false;
        if(nullptr == controller) controller = ResolveSendAudioController(slot, usedFallback);
        if(nullptr != controller) RememberObservedAgentVoice(controller, slot, soundName);
        CEntityInstance * povController = GetStablePovController();
        int controllerHandle = GetControllerHandle(controller);
        int controllerEntityIndex = GetEntityIndex(controller);
        int povHandle = GetControllerHandle(povController);
        int controllerTeam = ResolveControllerTeam(controller);
        int povTeam = ResolveControllerTeam(povController);

        if(0 <= controllerEntityIndex) {
            g_LastSoundControllerEntityIndex = controllerEntityIndex;
            g_LastSoundControllerSlot = slot;
            g_LastSoundControllerTimeMs = GetTickCount64();
        }

        bool controllerValid = controllerHandle >= 0
            && IsPlayableTeam(controllerTeam);
        bool teamFilterPassed = 6 == mode
            ? controllerValid
            : controllerValid
                && ((2 != povTeam && 3 != povTeam) || controllerTeam == povTeam);
        if(!teamFilterPassed) {
                            return;
        }

        if(4 == mode) {
            if(ConsumeRecentNativeForEvent(controllerEntityIndex, "sound-event")) return;
        }
        if((4 == mode || 5 == mode || 6 == mode)
            && ConsumeRecentSyntheticForFallback(controllerEntityIndex, "sound-event")) return;
        if(IsDuplicateSoundRadio(controllerEntityIndex, slot)) {
            return;
        }

        DispatchRadioNotice(controller, slot, body, "sound-event", 4 == mode, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
            }
}

void MirvPovRadio_OnDemoTick(int demoTick)
{
    if(g_LastDemoTick >= 0 && demoTick + 2 < g_LastDemoTick)
        MirvPovRadio_Reset("demo rewind/seek");
    g_LastDemoTick = demoTick;
    ProcessPendingSyntheticAudio();
    if(!IsProjectileScanFallbackEnabled()) {
        g_ProjectileScanPrimed = true;
        return;
    }
    if(!g_ProjectileScanPrimed) {
        ScanProjectileEntities(true);
        g_ProjectileScanPrimed = true;
            return;
    }
    ScanProjectileEntities(false);
}

void MirvPovRadio_Reset(const char * reason)
{
    AcquireSRWLockExclusive(&g_StateLock);
    g_RecentNativeRadios.clear();
    g_RecentSyntheticRadios.clear();
    g_RecentSoundRadios.clear();
    g_RecentProjectiles.clear();
    g_RecentGrenadeThrows.clear();
    g_RecentNativeAudios.clear();
    g_PendingSyntheticAudios.clear();
    g_ObservedAgentVoices.clear();
    ReleaseSRWLockExclusive(&g_StateLock);

    g_LastDemoTick = -1;
    g_LastSoundControllerEntityIndex = -1;
    g_LastSoundControllerSlot = -1;
    g_LastSoundControllerTimeMs = 0;
    g_LastSendAudioSlot = -1;
    g_LastSendAudioTimeMs = 0;
    g_ProjectileScanPrimed = false;
    strncpy_s(g_LastAgentFamily, sizeof(g_LastAgentFamily), "none", _TRUNCATE);
}

bool MirvPovRadio_IsAvailable()
{
    bool commonNativePath = MirvPovKillReward_IsHudChatDemoBypassAvailable();
    bool delegatePath = g_RadioTextDispatchHooked || g_SendAudioHooked || g_SendAudioEmitterHooked || g_RawAudioHooked || g_RawAudioFormatterHooked
        || (g_RadioTextHooked && nullptr != g_OrgRadioTextHandler && nullptr != g_GetDemoController);
    bool syntheticPath = HasGameEventHash() && MirvPovKillReward_IsAvailable();
    return syntheticPath || commonNativePath || delegatePath;
}

int MirvPovRadio_GetMode()
{
    return GetRadioMode();
}

const char * MirvPovRadio_GetModeDescription(int mode)
{
    return GetRadioModeDescription(mode);
}

#if AFX_MIRV_POV_DIAGNOSTICS

CON_COMMAND(mirv_pov_radio_mode, "Select mirv_pov Radio implementation: 0..6.")
{
    int argc = args->ArgC();
    if(2 == argc) {
        const char * value = args->ArgV(1);
        char * end = nullptr;
        long mode = strtol(value, &end, 10);
        if(nullptr != end && end != value && '\0' == *end && 0 <= mode && mode <= 6) {
            InterlockedExchange(&g_RadioMode, static_cast<LONG>(mode));
            MirvPovRadio_Reset("radio mode changed");
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_mode %ld: %s.\n", mode, GetRadioModeDescription(static_cast<int>(mode)));
            return;
        }
    }

    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "Usage: mirv_pov_radio_mode 0|1|2|3|4|5|6\n"
        "  0 - off: suppress native and event Radio\n"
        "  1 - native RadioText only (highest fidelity)\n"
        "  2 - game-event synthetic, POV team only\n"
        "  3 - game-event synthetic, all T/CT players (may show enemy Radio)\n"
        "  4 - auto: native RadioText, weapon_fire/grenade_thrown for projectiles, then other game-event/sound fallback (default)\n"
        "  5 - SendAudio/RawAudio/sound-event plus game-event fallback, POV team only\n"
        "  6 - SendAudio/RawAudio/sound-event plus game-event fallback, all T/CT players (may show enemy Radio)\n"
        "Current: %d (%s)\n",
        GetRadioMode(),
        GetRadioModeDescription(GetRadioMode()));
}

CON_COMMAND(mirv_pov_radio_projectile_scan, "Toggle emergency grenade projectile entity scanning: 0|1.")
{
    int argc = args->ArgC();
    if(2 == argc) {
        const char * value = args->ArgV(1);
        if(0 == strcmp(value, "1") || 0 == _stricmp(value, "on") || 0 == _stricmp(value, "true")) {
            InterlockedExchange(&g_ProjectileScanFallback, 1);
            MirvPovRadio_Reset("projectile scan fallback enabled");
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_projectile_scan 1: emergency entity fallback enabled.\n");
            return;
        }
        if(0 == strcmp(value, "0") || 0 == _stricmp(value, "off") || 0 == _stricmp(value, "false")) {
            InterlockedExchange(&g_ProjectileScanFallback, 0);
            MirvPovRadio_Reset("projectile scan fallback disabled");
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_projectile_scan 0: event path only (default).\n");
            return;
        }
    }
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "Usage: mirv_pov_radio_projectile_scan 0|1\n"
        "Current: %d (%s)\n",
        IsProjectileScanFallbackEnabled() ? 1 : 0,
        IsProjectileScanFallbackEnabled() ? "emergency entity fallback" : "event path only");
}

CON_COMMAND(mirv_pov_radio_audio, "Toggle delayed synthetic radio audio fallback: 0|1.")
{
    int argc = args->ArgC();
    if(2 == argc) {
        const char * value = args->ArgV(1);
        if(0 == strcmp(value, "1") || 0 == _stricmp(value, "on") || 0 == _stricmp(value, "true")) {
            InterlockedExchange(&g_SyntheticAudioEnabled, 1);
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_audio 1: synthetic audio is enabled only when native SendAudio/RawAudio is absent.\n");
            return;
        }
        if(0 == strcmp(value, "0") || 0 == _stricmp(value, "off") || 0 == _stricmp(value, "false")) {
            InterlockedExchange(&g_SyntheticAudioEnabled, 0);
            AcquireSRWLockExclusive(&g_StateLock);
            g_PendingSyntheticAudios.clear();
            ReleaseSRWLockExclusive(&g_StateLock);
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_audio 0: synthetic audio disabled and pending cues cleared.\n");
            return;
        }
    }
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "Usage: mirv_pov_radio_audio 0|1\n"
        "Current: %d (%s)\n",
        IsSyntheticAudioEnabled() ? 1 : 0,
        IsSyntheticAudioEnabled()
            ? "native SendAudio/RawAudio first, synthetic fallback after wait window"
            : "disabled");
}

CON_COMMAND(mirv_pov_radio_audio_spatial, "Select synthetic radio audio origin: 0=original global, 1=player-pawn spatialized.")
{
    int argc = args->ArgC();
    if(2 == argc) {
        const char * value = args->ArgV(1);
        if(0 == strcmp(value, "1") || 0 == _stricmp(value, "on") || 0 == _stricmp(value, "true")) {
            InterlockedExchange(&g_SyntheticAudioSpatialized, 1);
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_audio_spatial 1: fallback audio uses the thrower's pawn position.\n");
            return;
        }
        if(0 == strcmp(value, "0") || 0 == _stricmp(value, "off") || 0 == _stricmp(value, "false")) {
            InterlockedExchange(&g_SyntheticAudioSpatialized, 0);
            MIRV_POV_DIAGNOSTIC_MESSAGE("mirv_pov_radio_audio_spatial 0: fallback audio matches native global entidx=-1 playback.\n");
            return;
        }
    }
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "Usage: mirv_pov_radio_audio_spatial 0|1\n"
        "Current: %d (%s)\n",
        IsSyntheticAudioSpatialized() ? 1 : 0,
	        IsSyntheticAudioSpatialized() ? "player-pawn spatialized" : "native-style global entidx=-1");
}

#endif
