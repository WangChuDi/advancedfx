#include "stdafx.h"

#include "MirvPovFeedback.h"
#include "MirvPovCore.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvTime.h"
#include "SchemaSystem.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <Windows.h>
#include <cmath>
#include <limits.h>
#include <stdint.h>

namespace {

using PlayEntitySound_t = void (__fastcall *)(void * entity, void * filterEntity, const char * soundName);
using HashString_t = unsigned int (__fastcall *)(const char * string, unsigned int length, unsigned int lengthXorSeed);
using DamageMessage_t = __int64 (__fastcall *)(void * hudDamageIndicator, void * damageMessage);
using AddDamageDirection_t = void (__fastcall *)(void * hudDamageIndicator, float * sourcePosition, CEntityInstance * victimPawn);
using TriggerSoundControl_t = void (__fastcall *)(void * soundSystem, uint32_t controlHash);
using LocalPlayerFilterCtor_t = void (__fastcall *)(void * filter);
using PlayLocalSound_t = void (__fastcall *)(
    void * outHandle,
    void * filter,
    int entityIndex,
    const char * soundName,
    float volume,
    const void * parameters);

PlayEntitySound_t g_PlayEntitySound = nullptr;
HashString_t g_HashString = nullptr;
DamageMessage_t g_OriginalDamageMessage = nullptr;
AddDamageDirection_t g_AddDamageDirection = nullptr;
using FindHudElement_t = void * (__fastcall *)(const char * name);
FindHudElement_t g_FindHudElement = nullptr;
struct DirectionEvent {
    uint32_t pawnHandle;
    int amount;
    int frame;
    float source[3];
    bool message;
    bool event;
};
DirectionEvent g_DirectionEvents[64] = {};
size_t g_DirectionEventCount = 0;

// Pair individual events/messages, in either arrival order. Do not suppress
// separate hits merely because they share a frame, amount or position.
void RecordDirection(uint32_t handle, int amount, const float * source, bool message)
{
    const int frame = g_MirvTime.framecount_get();
    for(size_t i = 0; i < g_DirectionEventCount; ++i) {
        auto & entry = g_DirectionEvents[i];
        if(entry.pawnHandle == handle && entry.amount == amount && entry.frame == frame
            && (message ? !entry.message : !entry.event)) {
            if(message) entry.message = true;
            else entry.event = true;
            return;
        }
    }
    if(g_DirectionEventCount >= 64) return;
    auto & entry = g_DirectionEvents[g_DirectionEventCount++];
    entry = {handle, amount, frame, {source[0], source[1], source[2]}, message, !message};
}

void FlushDirections()
{
    __try {
        auto pawn = GetCurrentPovPlayerPawn();
        if(MIRV_POV_FEATURE_ACTIVE("feedback") && MIRV_POV_FEATURE_ACTIVE("damage_direction")
            && pawn && pawn->IsPlayerPawn() && pawn != GetRealLocalPlayerPawn()
            && g_FindHudElement && g_AddDamageDirection) {
            // FindHudElement returns the secondary HUD base, not the complete object.
            auto hudBase = static_cast<unsigned char *>(g_FindHudElement("CCSGO_HudDamageIndicator"));
            if(hudBase) {
                for(size_t i = 0; i < g_DirectionEventCount; ++i) {
                    auto & entry = g_DirectionEvents[i];
                    if(entry.event && !entry.message && entry.frame == g_MirvTime.framecount_get()
                        && entry.pawnHandle == pawn->GetHandle().ToInt())
                        g_AddDamageDirection(hudBase - 0x20, entry.source, pawn);
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    g_DirectionEventCount = 0;
}
bool g_DamageMessageHooked = false;
void ** g_SoundSystemSlot = nullptr;
LocalPlayerFilterCtor_t g_LocalPlayerFilterCtor = nullptr;
PlayLocalSound_t g_PlayLocalSound = nullptr;
uint32_t g_LastPovControllerHandle = 0xFFFFFFFFu;
uint32_t g_PreviousPovControllerHandle = 0xFFFFFFFFu;
int g_PreviousPovControllerFrame = INT_MIN;

struct FlashSoundProfile {
    const char * ringSound;
    uint32_t deafenControlHash;
};

// These are the native SoundSystem control hashes used by the current CS2
// AudioParameter path. They trigger the same DSP controls as the live client;
// no sound buses, global volume, or unrelated messages are modified.
constexpr uint32_t kHeGrenadeDeafenControlHash = 0xb60b5483;
const FlashSoundProfile kShortFlashSoundProfile = {
    "Flashbang.Ring.Short", 0x523f9894
};
const FlashSoundProfile kMediumFlashSoundProfile = {
    "Flashbang.Ring.Medium", 0x67bdb4cb
};
const FlashSoundProfile kLongFlashSoundProfile = {
    "Flashbang.Ring.Long", 0xf339fbbb
};

struct FlashDetonation {
    int entityIndex = -1;
    int frame = INT_MIN;
    uint32_t povHandle = 0xFFFFFFFFu;
    float distance = 0.0f;
};
FlashDetonation g_LastFlashDetonation;

SOURCESDK::CS2::GameEventKeySymbol_t MakeKey(const char * name)
{
    size_t length = strlen(name);
    unsigned int hash = g_HashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

bool IsPovAttack(
    SOURCESDK::CS2::IGameEvent * event,
    CEntityInstance *& attackerPawn,
    CEntityInstance *& victimPawn)
{
    attackerPawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey("attacker")));
    victimPawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey("userid")));
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr == povPawn) return false;
    if(nullptr == attackerPawn || nullptr == victimPawn) return false;
    if(attackerPawn != povPawn || attackerPawn == victimPawn) return false;
    return true;
}

bool PawnHasHelmet(CEntityInstance * pawn)
{
    return nullptr != pawn
        && 0 != g_clientDllOffsets.C_CSPlayerPawn.m_bPrevHelmet
        && *reinterpret_cast<bool *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_bPrevHelmet);
}

bool PawnHasArmor(CEntityInstance * pawn)
{
    return nullptr != pawn
        && 0 != g_clientDllOffsets.C_CSPlayerPawn.m_ArmorValue
        && 0 < *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_ArmorValue);
}

bool IsRemotePovPawn(CEntityInstance * pawn)
{
    if(nullptr == pawn || pawn != GetCurrentPovPlayerPawn()) return false;
    CEntityInstance * realLocalPawn = GetRealLocalPlayerPawn();
    return nullptr == realLocalPawn || pawn != realLocalPawn;
}

bool ApplyDeafenControl(uint32_t controlHash)
{
    if(0 == controlHash || nullptr == g_SoundSystemSlot) return false;

    __try {
        void * soundSystem = *g_SoundSystemSlot;
        if(nullptr == soundSystem) return false;
        void ** vtable = *reinterpret_cast<void ***>(soundSystem);
        if(nullptr == vtable) return false;
        auto triggerControl = reinterpret_cast<TriggerSoundControl_t>(
            vtable[0x1b8 / sizeof(void *)]);
        if(nullptr == triggerControl) return false;
        triggerControl(soundSystem, controlHash);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PlayLocalFlashRing(const char * soundName)
{
    if(nullptr == soundName || '\0' == soundName[0]
        || nullptr == g_LocalPlayerFilterCtor
        || nullptr == g_PlayLocalSound) return false;

    alignas(16) unsigned char outHandle[0x20] = {};
    alignas(16) unsigned char localPlayerFilter[0x20] = {};
    __try {
        g_LocalPlayerFilterCtor(localPlayerFilter);
        g_PlayLocalSound(
            outHandle,
            localPlayerFilter,
            -1,
            soundName,
            1.0f,
            nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const FlashSoundProfile * GetFlashSoundProfile(float distance)
{
    // Native server.dll selects DSP/ring by blast-to-eye distance, independently
    // of the view-angle-dependent blind duration (100 / 500 / 1000 units).
    if(!std::isfinite(distance) || distance < 0.0f) return nullptr;
    if(distance < 100.0f) return &kLongFlashSoundProfile;
    if(distance < 500.0f) return &kMediumFlashSoundProfile;
    if(distance < 1000.0f) return &kShortFlashSoundProfile;
    return nullptr;
}

void Play(CEntityInstance * victimPawn, const char * soundName)
{
    if(nullptr == g_PlayEntitySound || nullptr == victimPawn || nullptr == soundName) return;
    g_PlayEntitySound(victimPawn, nullptr, soundName);
}

__int64 __fastcall New_DamageMessage(void * hudDamageIndicator, void * damageMessage)
{
    __int64 result = nullptr != g_OriginalDamageMessage
        ? g_OriginalDamageMessage(hudDamageIndicator, damageMessage)
        : 0;
    if(!MIRV_POV_FEATURE_ACTIVE("feedback")
        || !MIRV_POV_FEATURE_ACTIVE("damage_direction")
        || nullptr == hudDamageIndicator
        || nullptr == damageMessage
        || nullptr == g_AddDamageDirection) return result;

    __try {
        unsigned char * message = reinterpret_cast<unsigned char *>(damageMessage);
        int amount = *reinterpret_cast<int *>(message + 0x50);
        int victimEntityIndex = *reinterpret_cast<int *>(message + 0x54);
        if(amount <= 0) return result;

        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn || !povPawn->IsPlayerPawn()
            || !IsRemotePovPawn(povPawn)) return result;
        auto povHandle = povPawn->GetHandle();
        if(!povHandle.IsValid() || povHandle.GetEntryIndex() != victimEntityIndex) return result;

        unsigned char * sourceMessage = *reinterpret_cast<unsigned char **>(message + 0x48);
        if(nullptr == sourceMessage) return result;
        float sourcePosition[3] = {
            *reinterpret_cast<float *>(sourceMessage + 0x18),
            *reinterpret_cast<float *>(sourceMessage + 0x1C),
            *reinterpret_cast<float *>(sourceMessage + 0x20)
        };
        // Replay the actual server-provided inflictor position through the
        // native HUD helper. It owns direction, intensity and accumulation;
        // the native HUD owns presentation and decay. A player_hurt event's
        // attacker position is not equivalent (e.g. a grenade's thrower).
        if(!std::isfinite(sourcePosition[0])
            || !std::isfinite(sourcePosition[1])
            || !std::isfinite(sourcePosition[2])) return result;
        g_AddDamageDirection(hudDamageIndicator, sourcePosition, povPawn);
        RecordDirection(povHandle.ToInt(), amount, sourcePosition, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

bool TryGetEventPawn(
    SOURCESDK::CS2::IGameEvent * event,
    const char * keyName,
    CEntityInstance *& pawn)
{
    pawn = nullptr;
    if(nullptr == event || nullptr == keyName || nullptr == g_HashString) return false;
    pawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey(keyName)));
    if(nullptr != pawn && pawn->IsPlayerPawn()) return true;

    CEntityInstance * controller = reinterpret_cast<CEntityInstance *>(
        event->GetPlayerController(MakeKey(keyName)));
    if(nullptr == controller || !controller->IsPlayerController()) return false;
    auto pawnHandle = controller->GetPlayerPawnHandle();
    if(!pawnHandle.IsValid()) return false;
    pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
    return nullptr != pawn && pawn->IsPlayerPawn();
}

void QueueHurtDirection(SOURCESDK::CS2::IGameEvent * event, CEntityInstance * victim)
{
    if(!MIRV_POV_FEATURE_ACTIVE("damage_direction") || !IsRemotePovPawn(victim)) return;
    const int amount = event->GetInt(MakeKey("dmg_health"));
    if(amount <= 0) return;
    const char * weapon = event->GetString(MakeKey("weapon"));
    // Grenade/fire/world damage needs the inflictor position from Damage.
    if(!weapon || !*weapon || strstr(weapon, "grenade") || strstr(weapon, "inferno")
        || strstr(weapon, "molotov") || strstr(weapon, "incendiary")
        || !strcmp(weapon, "world") || !strcmp(weapon, "worldspawn")
        || !strcmp(weapon, "planted_c4")) return;
    CEntityInstance * attacker = nullptr;
    if(!TryGetEventPawn(event, "attacker", attacker) || attacker == victim) return;
    float source[3] = {};
    attacker->GetOrigin(source[0], source[1], source[2]);
    if(!std::isfinite(source[0]) || !std::isfinite(source[1]) || !std::isfinite(source[2])) return;
    RecordDirection(victim->GetHandle().ToInt(), amount, source, false);
}

void HandleHurt(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    auto healthKey = MakeKey("health");
    if(event->HasKey(healthKey) && event->GetInt(healthKey) <= 0) return;

    bool headshot = 1 == event->GetInt(MakeKey("hitgroup"));
    bool armored = headshot
        ? PawnHasHelmet(victimPawn)
        : PawnHasArmor(victimPawn) || 0 < event->GetInt(MakeKey("dmg_armor"));
    Play(victimPawn,
        headshot
            ? (armored
                ? "Player.DamageHeadShotArmor.AttackerFeedback"
                : "Player.DamageHeadShot.AttackerFeedback")
            : (armored
                ? "Player.DamageBodyArmor.AttackerFeedback"
                : "Player.DamageBody.AttackerFeedback"));
}

void HandleFlashDetonate(SOURCESDK::CS2::IGameEvent * event)
{
    g_LastFlashDetonation = FlashDetonation();
    if(!MirvPovDebug_IsFeatureEnabled("deafen")) return;
    CEntityInstance * pawn = GetCurrentPovPlayerPawn();
    if(!IsRemotePovPawn(pawn)) return;
    const auto entityKey = MakeKey("entityid");
    const auto xKey = MakeKey("x");
    const auto yKey = MakeKey("y");
    const auto zKey = MakeKey("z");
    if(!event->HasKey(entityKey) || !event->HasKey(xKey)
        || !event->HasKey(yKey) || !event->HasKey(zKey)) return;

    float eye[3] = {};
    pawn->GetRenderEyeOrigin(eye);
    const float dx = event->GetFloat(xKey) - eye[0];
    const float dy = event->GetFloat(yKey) - eye[1];
    // RadiusFlash raises the detonation origin by one unit before computing
    // visibility and eye distance. flashbang_detonate contains the original Z.
    const float dz = event->GetFloat(zKey) + 1.0f - eye[2];
    g_LastFlashDetonation.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    g_LastFlashDetonation.entityIndex = event->GetInt(entityKey);
    g_LastFlashDetonation.frame = g_MirvTime.framecount_get();
    g_LastFlashDetonation.povHandle = pawn->GetHandle().ToInt();
}

void HandlePlayerBlind(CEntityInstance * victimPawn, SOURCESDK::CS2::IGameEvent * event)
{
    if(!MirvPovDebug_IsFeatureEnabled("deafen")) return;
    if(!IsRemotePovPawn(victimPawn) || nullptr == event) return;
    const auto entityKey = MakeKey("entityid");
    if(!event->HasKey(entityKey)
        || event->GetInt(entityKey) != g_LastFlashDetonation.entityIndex
        || victimPawn->GetHandle().ToInt() != g_LastFlashDetonation.povHandle
        || g_LastFlashDetonation.frame != g_MirvTime.framecount_get()) return;
    const float distance = g_LastFlashDetonation.distance;
    g_LastFlashDetonation = FlashDetonation();
    const FlashSoundProfile * profile = GetFlashSoundProfile(distance);
    if(nullptr == profile) return;

    // Additive fallback only: never suppress the native ring or AudioParameter
    // message. If either native helper is unavailable, the untouched original
    // CS2 path remains authoritative.
    const bool deafenApplied = ApplyDeafenControl(profile->deafenControlHash);
    const bool ringPlayed = PlayLocalFlashRing(profile->ringSound);
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "[mirv_pov_feedback] remote POV flash distance=%.3f deafen=%d ring=%d sound=%s\n",
        distance,
        deafenApplied ? 1 : 0,
        ringPlayed ? 1 : 0,
        profile->ringSound);
}

void HandleHeGrenadeHurt(CEntityInstance * victimPawn, SOURCESDK::CS2::IGameEvent * event)
{
    if(!MirvPovDebug_IsFeatureEnabled("deafen")) return;
    if(nullptr == event) return;

    auto weaponKey = MakeKey("weapon");
    if(!event->HasKey(weaponKey)) return;
    const char * weapon = event->GetString(weaponKey);
    if(nullptr == weapon
        || (0 != _stricmp(weapon, "hegrenade")
            && 0 != _strnicmp(weapon, "hegrenade_", 10))) return;

    MIRV_POV_DIAGNOSTIC_MESSAGE("[mirv_pov_feedback] HE candidate damage=%d remote=%d ruleOffset=%d\n",
        event->GetInt(MakeKey("dmg_health")), IsRemotePovPawn(victimPawn),
        g_clientDllOffsets.C_CSGameRules.m_eRoundWinReason);
    if(!IsRemotePovPawn(victimPawn)) return;

    auto damageKey = MakeKey("dmg_health");
    // Native blast feedback starts at 30 damage, and couples the HE DSP control
    // with the long flash-ring sound. Do not scale this by remaining health or
    // add armor damage: player_hurt reports damage before the health-zero clamp.
    if(!event->HasKey(damageKey) || event->GetInt(damageKey) < 30) return;
    // The native server handler is disabled after the bomb target explodes.
    // Its m_bTargetBombed is server-only; use the replicated TargetBombed round
    // result (1), including when enabling POV or seeking after the explosion.
    if(g_clientDllOffsets.C_CSGameRules.m_eRoundWinReason < 0) return;
    bool rulesAvailable = false;
    const int highestIndex = GetHighestEntityIndex();
    for(int i = 0; i <= highestIndex; ++i) {
        CEntityInstance * entity = GetEntityFromIndex(i);
        if(!entity) continue;
        const char * className = entity->GetClassName();
        if(!className || 0 != strcmp(className, "cs_gamerules")) continue;
        auto rules = *reinterpret_cast<unsigned char **>(
            reinterpret_cast<unsigned char *>(entity)
            + g_clientDllOffsets.C_CSGameRulesProxy.m_pGameRules);
        if(!rules || 1 == *reinterpret_cast<const int *>(
            rules + g_clientDllOffsets.C_CSGameRules.m_eRoundWinReason)) return;
        rulesAvailable = true;
        break;
    }
    if(!rulesAvailable) return;
    const bool deafenApplied = ApplyDeafenControl(kHeGrenadeDeafenControlHash);
    const bool ringPlayed = PlayLocalFlashRing(kLongFlashSoundProfile.ringSound);
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "[mirv_pov_feedback] remote POV HE deafen=%d ring=%d damage=%d\n",
        deafenApplied ? 1 : 0,
        ringPlayed ? 1 : 0,
        event->GetInt(damageKey));
}

void HandleDeath(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    bool headshot = event->GetBool(MakeKey("headshot"));
    bool armored = headshot ? PawnHasHelmet(victimPawn) : PawnHasArmor(victimPawn);
    Play(victimPawn,
        headshot
            ? (armored
                ? "Player.DeathHeadShotArmor.AttackerFeedback"
                : "Player.DeathHeadShot.AttackerFeedback")
            : (armored
                ? "Player.DeathBodyArmor.AttackerFeedback"
                : "Player.DeathBody.AttackerFeedback"));
}

} // namespace

void MirvPovFeedback_ResetDeafen()
{
    g_LastFlashDetonation = FlashDetonation();
}

void MirvPovFeedback_Initialize(HMODULE clientDll)
{
    if(nullptr == clientDll) return;
    if(nullptr == g_PlayEntitySound) {
        g_PlayEntitySound = reinterpret_cast<PlayEntitySound_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 49 8B E8"));
    }
    if(nullptr == g_HashString) {
        g_HashString = reinterpret_cast<HashString_t>(getAddress(
            clientDll,
            "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? 0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20"));
    }

    if(nullptr == g_SoundSystemSlot) {
        size_t audioParameterHandler = getAddress(
            clientDll,
            "8B 41 ?? 48 8B D1 39 05");
        uint8_t * soundSystemLoad = 0 != audioParameterHandler
            ? reinterpret_cast<uint8_t *>(audioParameterHandler) + 0x52
            : nullptr;
        const uint8_t expectedLoad[] = {0x48, 0x8b, 0x0d};
        const uint8_t expectedControlTail[] = {
            0x8b, 0x52, 0x4c,
            0x48, 0x8b, 0x01,
            0x48, 0xff, 0xa0, 0xb8, 0x01, 0x00, 0x00,
            0xc3
        };
        if(nullptr != soundSystemLoad
            && 0 == memcmp(soundSystemLoad, expectedLoad, sizeof(expectedLoad))
            && 0 == memcmp(
                soundSystemLoad + 7,
                expectedControlTail,
                sizeof(expectedControlTail))) {
            int32_t relative = 0;
            memcpy(&relative, soundSystemLoad + 3, sizeof(relative));
            g_SoundSystemSlot = reinterpret_cast<void **>(
                soundSystemLoad + 7 + relative);
        } else {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_feedback] Native deafen control path was not found.\n");
        }
    }

    if(nullptr == g_LocalPlayerFilterCtor || nullptr == g_PlayLocalSound) {
        size_t sendAudioHandler = getAddress(
            clientDll,
            "40 53 48 83 EC 60 48 8B 59 48 48 83 E3 FC 48 83 7B 18 0F 76");
        uint8_t * filterCtorCall = 0 != sendAudioHandler
            ? reinterpret_cast<uint8_t *>(sendAudioHandler) + 0x35
            : nullptr;
        uint8_t * playLocalSoundCall = 0 != sendAudioHandler
            ? reinterpret_cast<uint8_t *>(sendAudioHandler) + 0x64
            : nullptr;
        uint8_t * filterCtor = nullptr;
        uint8_t * playLocalSound = nullptr;
        if(nullptr != filterCtorCall && 0xe8 == filterCtorCall[0]) {
            int32_t relative = 0;
            memcpy(&relative, filterCtorCall + 1, sizeof(relative));
            filterCtor = filterCtorCall + 5 + relative;
        }
        if(nullptr != playLocalSoundCall && 0xe8 == playLocalSoundCall[0]) {
            int32_t relative = 0;
            memcpy(&relative, playLocalSoundCall + 1, sizeof(relative));
            playLocalSound = playLocalSoundCall + 5 + relative;
        }
        const uint8_t expectedFilterCtorPrefix[] = {0x48, 0x89, 0x5c, 0x24};
        const uint8_t expectedPlayLocalSoundPrefix[] = {0x40, 0x53, 0x48, 0x83, 0xec};
        if(nullptr != filterCtor
            && nullptr != playLocalSound
            && 0 == memcmp(filterCtor, expectedFilterCtorPrefix, sizeof(expectedFilterCtorPrefix))
            && 0 == memcmp(
                playLocalSound,
                expectedPlayLocalSoundPrefix,
                sizeof(expectedPlayLocalSoundPrefix))) {
            g_LocalPlayerFilterCtor = reinterpret_cast<LocalPlayerFilterCtor_t>(filterCtor);
            g_PlayLocalSound = reinterpret_cast<PlayLocalSound_t>(playLocalSound);
        } else {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_feedback] Native local flash-ring path was not found.\n");
        }
    }

    if(nullptr == g_OriginalDamageMessage && !g_DamageMessageHooked) {
        g_OriginalDamageMessage = reinterpret_cast<DamageMessage_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B FA 48 8B E9"));
    }
    if(nullptr == g_AddDamageDirection) {
        g_AddDamageDirection = reinterpret_cast<AddDamageDirection_t>(getAddress(
            clientDll,
            "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC 90 00 00 00 0F 29 7C 24 70 48 8B FA 0F 57 FF"));
    }

    if(nullptr == g_FindHudElement) {
        g_FindHudElement = reinterpret_cast<FindHudElement_t>(getAddress(clientDll,
            "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 48 85 C0 74 ?? 48 89 5C 24 38 48 8D 88 58 02 00 00 48 85 DB 74 ?? 4C 8B C1 48 8D 54 24 38"));
    }

    const bool attachDamageMessage =
        !g_DamageMessageHooked
        && nullptr != g_OriginalDamageMessage
        && nullptr != g_AddDamageDirection;
    if(attachDamageMessage) {
        LONG beginResult = DetourTransactionBegin();
        LONG updateResult = NO_ERROR;
        LONG messageAttachResult = NO_ERROR;
        LONG transactionResult = -1;
        if(NO_ERROR == beginResult) updateResult = DetourUpdateThread(GetCurrentThread());
        if(NO_ERROR == beginResult && NO_ERROR == updateResult) {
            messageAttachResult = DetourAttach(
                &(PVOID &)g_OriginalDamageMessage,
                New_DamageMessage);
            transactionResult = NO_ERROR == messageAttachResult
                ? DetourTransactionCommit()
                : DetourTransactionAbort();
        } else if(NO_ERROR == beginResult) {
            transactionResult = DetourTransactionAbort();
        }
        const bool installed = NO_ERROR == beginResult
            && NO_ERROR == updateResult
            && NO_ERROR == messageAttachResult
            && NO_ERROR == transactionResult;
        if(installed) {
            g_DamageMessageHooked = true;
        }
    }
}

void MirvPovFeedback_UpdatePovSelection()
{
    FlushDirections();
    if(!MIRV_POV_FEATURE_ACTIVE("feedback")) return;

    CEntityInstance * controller = GetCurrentPovPlayerController();
    if(nullptr == controller) controller = GetObservedPlayerController();
    if(nullptr == controller || !controller->IsPlayerController()) return;

    auto handle = controller->GetHandle();
    if(!handle.IsValid()) return;
    const uint32_t rawHandle = handle.ToInt();
    if(rawHandle == g_LastPovControllerHandle) return;

    g_PreviousPovControllerHandle = g_LastPovControllerHandle;
    g_PreviousPovControllerFrame = g_MirvTime.framecount_get();
    g_LastPovControllerHandle = rawHandle;
}

void MirvPovFeedback_ResetDirections()
{
    g_DirectionEventCount = 0;
}

void MirvPovFeedback_ResetPovSelection()
{
    g_DirectionEventCount = 0;
    g_LastFlashDetonation = FlashDetonation();
    g_LastPovControllerHandle = 0xFFFFFFFFu;
    g_PreviousPovControllerHandle = 0xFFFFFFFFu;
    g_PreviousPovControllerFrame = INT_MIN;

}

bool MirvPovFeedback_IsCurrentPovVictim(
    SOURCESDK::CS2::IGameEvent * event,
    bool * headshot)
{
    if(nullptr != headshot) *headshot = false;
    if(nullptr == event || nullptr == g_HashString) return false;

    __try {
        CEntityInstance * victimPawn = reinterpret_cast<CEntityInstance *>(
            event->GetPlayerPawn(MakeKey("userid")));
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == victimPawn || nullptr == povPawn || victimPawn != povPawn) return false;

        if(nullptr != headshot) *headshot = event->GetBool(MakeKey("headshot"));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(nullptr != headshot) *headshot = false;
        return false;
    }
}

void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MIRV_POV_FEATURE_ACTIVE("feedback") || nullptr == g_HashString) return;
    const char * name = event->GetName();
    if(nullptr == name) return;

    __try {
        if(0 == strcmp(name, "flashbang_detonate") || 0 == strcmp(name, "player_blind")) {
            MIRV_POV_DIAGNOSTIC_MESSAGE("[mirv_pov_feedback] flash event=%s entity=%d userid=%d frame=%d cached=%d/%d\n",
                name, event->GetInt(MakeKey("entityid")), event->GetInt(MakeKey("userid")),
                g_MirvTime.framecount_get(), g_LastFlashDetonation.entityIndex, g_LastFlashDetonation.frame);
        }
        if(0 == strcmp(name, "flashbang_detonate")) {
            HandleFlashDetonate(event);
        } else if(0 == strcmp(name, "round_start")) {
            g_DirectionEventCount = 0;
            g_LastFlashDetonation = FlashDetonation();
        } else if(0 == strcmp(name, "player_blind")) {
            CEntityInstance * victimPawn = nullptr;
            if(TryGetEventPawn(event, "userid", victimPawn)) {
                HandlePlayerBlind(victimPawn, event);
            }
        } else if(0 == strcmp(name, "player_hurt")) {
            CEntityInstance * victimPawn = nullptr;
            if(TryGetEventPawn(event, "userid", victimPawn)) {
                HandleHeGrenadeHurt(victimPawn, event);
                QueueHurtDirection(event, victimPawn);
            }
            HandleHurt(event);
        } else if(0 == strcmp(name, "player_death")) {
            HandleDeath(event);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool MirvPovFeedback_IsLocalPlayerVictim(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MIRV_POV_FEATURE_ACTIVE("feedback") || nullptr == g_HashString) {
        return false;
    }

    const char * eventName = nullptr;
    CEntityInstance * victimController = nullptr;
    CEntityInstance * povController = nullptr;
    CEntityInstance * observedController = nullptr;
    CEntityInstance * realController = nullptr;
    CEntityInstance * selectedController = nullptr;
    CEntityInstance * localController = nullptr;
    int userId = -1;
    int victimHandle = -1;
    int povHandle = -1;
    int realHandle = -1;
    bool result = false;

    __try {
        MirvPovFeedback_UpdatePovSelection();
        eventName = event->GetName();
        auto useridKey = MakeKey("userid");
        userId = event->GetInt(useridKey);
        // userid is a controller/user id field. GetPlayerPawn("userid") is not
        // reliable here; the SDK documents it as being intended for _pawn keys.
        victimController = reinterpret_cast<CEntityInstance *>(event->GetPlayerController(useridKey));
        if(nullptr == victimController && 0 <= userId) {
            CEntityInstance * fallback = GetEntityFromIndex(userId + 1);
            if(nullptr != fallback && fallback->IsPlayerController()) victimController = fallback;
        }
        povController = GetCurrentPovPlayerController();
        observedController = GetObservedPlayerController();
        realController = GetRealSplitScreenPlayer(0);
        selectedController = nullptr != povController
            ? povController
            : (nullptr != observedController ? observedController : realController);
        localController = selectedController;

        if(nullptr != victimController) victimHandle = victimController->GetHandle().ToInt();
        if(nullptr != povController) povHandle = povController->GetHandle().ToInt();
        if(nullptr != observedController && nullptr == povController) povHandle = observedController->GetHandle().ToInt();
        if(nullptr != realController) realHandle = realController->GetHandle().ToInt();

        const bool selectedMatch = nullptr != victimController
            && nullptr != selectedController
            && victimHandle == selectedController->GetHandle().ToInt();
        const bool cachedCurrentMatch = nullptr != victimController
            && static_cast<uint32_t>(victimHandle) == g_LastPovControllerHandle;
        const int frameDelta = g_MirvTime.framecount_get() - g_PreviousPovControllerFrame;
        const bool cachedPreviousDeathMatch = nullptr != victimController
            && nullptr != eventName
            && 0 == strcmp(eventName, "player_death")
            && 0 <= frameDelta
            && frameDelta <= 2
            && static_cast<uint32_t>(victimHandle) == g_PreviousPovControllerHandle;
        result = selectedMatch || cachedCurrentMatch || cachedPreviousDeathMatch;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

bool MirvPovFeedback_IsRealLocalPlayerVictim(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || nullptr == g_HashString) return false;

    bool result = false;
    int userId = -1;
    CEntityInstance * victimController = nullptr;
    CEntityInstance * realController = nullptr;

    __try {
        auto useridKey = MakeKey("userid");
        userId = event->GetInt(useridKey);
        victimController = reinterpret_cast<CEntityInstance *>(event->GetPlayerController(useridKey));
        if(nullptr == victimController && 0 <= userId) {
            CEntityInstance * fallback = GetEntityFromIndex(userId + 1);
            if(nullptr != fallback && fallback->IsPlayerController()) victimController = fallback;
        }
        realController = GetRealSplitScreenPlayer(0);
        result = nullptr != victimController
            && nullptr != realController
            && victimController->GetHandle().ToInt() == realController->GetHandle().ToInt();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    return result;
}
