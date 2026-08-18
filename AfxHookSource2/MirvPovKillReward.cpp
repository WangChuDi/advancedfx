#include "stdafx.h"

#include "MirvPovKillReward.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovCore.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../deps/release/Detours/src/detours.h"
#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#include <Windows.h>
#include <deque>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

using HashString_t = unsigned int (__fastcall *)(
    const char * string,
    unsigned int length,
    unsigned int lengthXorSeed);
using PrintHudNotice_t = void (__fastcall *)(
    void * ownerUnused,
    int entityIndex,
    const char * format,
    ...);
using FindHudElement_t = void * (__fastcall *)(const char * name);
using PushHudNotice_t = void (__fastcall *)(
    void * hudVoiceStatus,
    const char * text,
    unsigned int entityIndex,
    const unsigned char flags[2]);
using TextMsgHandler_t = void (__fastcall *)(void * owner, void * message);
using GetDemoController_t = unsigned char * (__fastcall *)();
using RepeatedStringCount_t = int (__fastcall *)(void * container);
using RepeatedStringAt_t = void * (__fastcall *)(void * container, int index);

struct PendingEntry {
    int controllerHandle;
    ULONGLONG timeMs;
    int demoTick;
    int value;
};

struct RecentNativeReward {
    ULONGLONG timeMs;
    int demoTick;
    int amount;
};

constexpr ULONGLONG kMatchWindowMs = 1500;
constexpr ULONGLONG kRecentDeathWindowMs = 3000;
constexpr ULONGLONG kNativeGraceMs = 250;
constexpr int kMatchWindowTicks = 32;
constexpr int kExpiryWindowTicks = 64;
constexpr size_t kMaxPendingEntries = 8;
constexpr size_t kMaxRecentDeaths = 16;
constexpr size_t kMaxRecentNativeRewards = 16;
constexpr size_t kTextMsgParamsOffset = 0x48;
constexpr size_t kTextMsgDestinationOffset = 0x60;
constexpr size_t kDemoHudChatSuppressOffset = 0x72;
// localize.dll's Localize_001 interface. The current CLocalize vtable uses
// +0x78 for Find(const char *), while +0x88 is FindSafe and returns the input
// token when no translation exists. Use Find so missing tokens remain null.
constexpr size_t kLocalizationFindVtableIndex = 0x78 / sizeof(void *);
using CreateInterface_t = void * (__cdecl *)(const char * name, int * returnCode);
using LocalizationFind_t = const char * (__fastcall *)(
    void * localize,
    const char * token);

HMODULE g_LocalizeDll = nullptr;
void * g_Localize = nullptr;
LocalizationFind_t g_LocalizationFind = nullptr;
HashString_t g_HashString = nullptr;
PrintHudNotice_t g_PrintHudNotice = nullptr;
FindHudElement_t g_FindHudElement = nullptr;
PushHudNotice_t g_PushHudNotice = nullptr;
TextMsgHandler_t g_OrgTextMsgHandler = nullptr;
GetDemoController_t g_GetDemoController = nullptr;
RepeatedStringCount_t g_RepeatedStringCount = nullptr;
RepeatedStringAt_t g_RepeatedStringAt = nullptr;
bool g_MoneyHookAvailable = false;
bool g_TextMsgHooked = false;
SRWLOCK g_DemoGuardLock = SRWLOCK_INIT;
SRWLOCK g_RewardNoticeLock = SRWLOCK_INIT;
thread_local LONG g_TextMsgDispatchDepth = 0;
uint8_t * g_HudChatDemoBypassPatch = nullptr;
uint8_t g_HudChatDemoBypassOriginal[6] = {};
bool g_HudChatDemoBypassAvailable = false;
bool g_HudChatDemoBypassApplied = false;
SRWLOCK g_HudChatDemoBypassLock = SRWLOCK_INIT;
int g_CurrentKillReward = -1;
int g_LastControllerHandle = -1;
int g_LastMoneyControllerHandle = -1;
int g_LastResolvedPovHandle = -1;
int g_LastDemoTick = -1;
std::deque<PendingEntry> g_PendingDeaths;
std::deque<PendingEntry> g_PendingRewards;
std::deque<PendingEntry> g_PendingNotices;
std::deque<PendingEntry> g_RecentDeaths;
std::deque<RecentNativeReward> g_RecentNativeRewards;

bool ResolveCallTarget(
    uint8_t * callSite,
    const Afx::BinUtils::MemRange & textRange,
    uint8_t *& target);

using GetHudMoneyPawn_t = CEntityInstance * (__fastcall *)();
using MoneyPanelUpdate_t = void (__fastcall *)(void * This);

GetHudMoneyPawn_t g_MoneyOrgGetHudMoneyPawn = nullptr;
MoneyPanelUpdate_t g_MoneyOrgPanelUpdate = nullptr;
void * g_MoneyPanelReturnAddresses[2] = {};
bool g_MoneyHooked = false;
thread_local bool g_InMoneyPanelUpdate = false;

bool IsMoneyPanelReturnAddress(void * address)
{
    for(size_t i = 0; i < _countof(g_MoneyPanelReturnAddresses); ++i) {
        if(address == g_MoneyPanelReturnAddresses[i]) return true;
    }
    return false;
}

CEntityInstance * __fastcall New_GetHudMoneyPawn()
{
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    CEntityInstance * nativePawn = g_MoneyOrgGetHudMoneyPawn();
    MirvPov_PopHookReturnAddress(previousReturnAddress);

    if(!MirvPov_IsEnabled()
        || (!g_InMoneyPanelUpdate && !IsMoneyPanelReturnAddress(returnAddress)))
        return nativePawn;

    __try {
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn || !povPawn->IsPlayerPawn()) return nativePawn;

        int team = povPawn->GetTeam();
        if(2 == team || 3 == team) return povPawn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    return nativePawn;
}

void __fastcall New_MoneyPanelUpdate(void * This)
{
    int oldAccount = -1;
    int newAccount = -1;
    unsigned int oldControllerHandle = 0xffffffffu;
    unsigned int newControllerHandle = 0xffffffffu;

    __try {
        oldAccount = *reinterpret_cast<int *>(reinterpret_cast<unsigned char *>(This) + 0x20);
        oldControllerHandle = *reinterpret_cast<unsigned int *>(reinterpret_cast<unsigned char *>(This) + 0x24);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    bool previousInUpdate = g_InMoneyPanelUpdate;
    g_InMoneyPanelUpdate = true;
    __try {
        g_MoneyOrgPanelUpdate(This);
    } __finally {
        g_InMoneyPanelUpdate = previousInUpdate;
    }

    __try {
        newAccount = *reinterpret_cast<int *>(reinterpret_cast<unsigned char *>(This) + 0x20);
        newControllerHandle = *reinterpret_cast<unsigned int *>(reinterpret_cast<unsigned char *>(This) + 0x24);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    int controllerHandle = -1;
    __try {
        CEntityInstance * controller = GetCurrentPovPlayerController();
        if(nullptr != controller) {
            auto handle = controller->GetHandle();
            if(handle.IsValid()) controllerHandle = handle.ToInt();
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    MirvPovKillReward_OnMoneyUpdate(
        oldAccount,
        newAccount,
        0xffffffffu != oldControllerHandle ? static_cast<int>(oldControllerHandle) : -1,
        0xffffffffu != newControllerHandle ? static_cast<int>(newControllerHandle) : -1,
        controllerHandle);
}

bool ResolveLocalization()
{
    if(nullptr != g_LocalizationFind) return true;

    HMODULE localizeDll = g_LocalizeDll;
    if(nullptr == localizeDll) localizeDll = GetModuleHandleA("localize.dll");
    if(nullptr == localizeDll) return false;

    __try {
        auto createInterface = reinterpret_cast<CreateInterface_t>(
            GetProcAddress(localizeDll, "CreateInterface"));
        if(nullptr == createInterface) return false;

        void * localize = createInterface("Localize_001", nullptr);
        if(nullptr == localize) return false;

        void ** vtable = *reinterpret_cast<void ***>(localize);
        if(nullptr == vtable || nullptr == vtable[kLocalizationFindVtableIndex])
            return false;

        g_LocalizeDll = localizeDll;
        g_Localize = localize;
        g_LocalizationFind = reinterpret_cast<LocalizationFind_t>(
            vtable[kLocalizationFindVtableIndex]);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_LocalizeDll = nullptr;
        g_Localize = nullptr;
        g_LocalizationFind = nullptr;
        return false;
    }
}

const char * LocalizeToken(const char * token)
{
    if(nullptr == token || '\0' == token[0] || !ResolveLocalization())
        return nullptr;

    __try {
        const char * localized = g_LocalizationFind(g_Localize, token);
        return nullptr != localized && '\0' != localized[0] ? localized : nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SubstituteFirstLocalizationParameter(
    const char * format,
    const char * value,
    char * output,
    size_t outputSize)
{
    if(nullptr == format || nullptr == value || nullptr == output || 0 == outputSize)
        return false;

    const char * marker = strstr(format, "%s1");
    if(nullptr == marker) return false;

    size_t prefixLength = static_cast<size_t>(marker - format);
    size_t valueLength = strlen(value);
    const char * suffix = marker + 3;
    size_t suffixLength = strlen(suffix);
    if(outputSize <= prefixLength + valueLength + suffixLength)
        return false;

    memcpy(output, format, prefixLength);
    memcpy(output + prefixLength, value, valueLength);
    memcpy(output + prefixLength + valueLength, suffix, suffixLength + 1);
    return true;
}

void InitializeMoneyHook(HMODULE clientDll)
{
    if(g_MoneyHooked || nullptr == clientDll) return;

    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    sections.Next(IMAGE_SCN_MEM_EXECUTE);
    if(!sections.Eof()) textRange = sections.GetMemRange();
    if(textRange.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] client.dll text section was not found for money hook.\n");
        return;
    }

    // CCSGOMoneyPanel resolves the active pawn twice before reading the
    // controller's account service. Redirect those pawn lookups to the POV
    // pawn so the native account cache and money animation remain intact.
    const char * moneyPanelUpdatePattern =
        "40 56 57 48 83 EC 48 48 8B 35 ?? ?? ?? ?? 48 8B F9 "
        "48 89 74 24 70 48 85 F6 0F 84 ?? ?? ?? ?? 48 89 6C 24 40 "
        "4C 89 74 24 28 48 89 5C 24 60 E8 ?? ?? ?? ?? 45 33 F6 "
        "BD FF 7F 00 00";
    auto updateSequence = Afx::BinUtils::FindPatternString(textRange, moneyPanelUpdatePattern);
    if(updateSequence.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] CSGOMoneyPanel update function was not found.\n");
        return;
    }
    auto updateRemaining = Afx::BinUtils::MemRange(updateSequence.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(updateRemaining, moneyPanelUpdatePattern).IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] CSGOMoneyPanel update pattern is not unique.\n");
        return;
    }

    uint8_t * update = reinterpret_cast<uint8_t *>(updateSequence.Start);
    uint8_t * callSites[] = { update + 0x2e, update + 0x40 };
    uint8_t * getterTarget = nullptr;
    for(size_t i = 0; i < _countof(callSites); ++i) {
        uint8_t * target = nullptr;
        if(!ResolveCallTarget(callSites[i], textRange, target)
            || (nullptr != getterTarget && getterTarget != target)) {
            memset(g_MoneyPanelReturnAddresses, 0, sizeof(g_MoneyPanelReturnAddresses));
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] HUD-pawn call set is invalid.\n");
            return;
        }
        getterTarget = target;
        g_MoneyPanelReturnAddresses[i] = callSites[i] + 5;
    }

    g_MoneyOrgGetHudMoneyPawn = reinterpret_cast<GetHudMoneyPawn_t>(getterTarget);
    g_MoneyOrgPanelUpdate = reinterpret_cast<MoneyPanelUpdate_t>(update);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_MoneyOrgGetHudMoneyPawn, New_GetHudMoneyPawn);
    DetourAttach(&(PVOID &)g_MoneyOrgPanelUpdate, New_MoneyPanelUpdate);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_MoneyOrgGetHudMoneyPawn = nullptr;
        g_MoneyOrgPanelUpdate = nullptr;
        memset(g_MoneyPanelReturnAddresses, 0, sizeof(g_MoneyPanelReturnAddresses));
        MirvPovKillReward_SetMoneyHookAvailable(false);
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] HUD-pawn / account observer detour failed.\n");
        return;
    }

    g_MoneyHooked = true;
    MirvPovKillReward_SetMoneyHookAvailable(true);
}

void ClearPending(const char * reason);
void ConsumeOneNativeReward(const char * amount);

bool CopyTextMsgParam(void * message, int index, char * output, size_t outputSize)
{
    if(nullptr == message || index < 0 || nullptr == output || 0 == outputSize
        || nullptr == g_RepeatedStringCount || nullptr == g_RepeatedStringAt)
        return false;

    output[0] = '\0';
    __try {
        void * container = reinterpret_cast<unsigned char *>(message) + kTextMsgParamsOffset;
        int count = g_RepeatedStringCount(container);
        if(count <= index || 32 < count) return false;

        unsigned char * item = reinterpret_cast<unsigned char *>(
            g_RepeatedStringAt(container, index));
        if(nullptr == item) return false;

        const char * value = reinterpret_cast<const char *>(item);
        if(*reinterpret_cast<uint64_t *>(item + 0x18) > 0x0f)
            value = *reinterpret_cast<const char **>(item);
        if(nullptr == value) return false;

        size_t length = strnlen_s(value, outputSize);
        if(outputSize <= length) return false;
        memcpy(output, value, length + 1);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

bool IsNativeKillRewardTextMsg(
    void * message,
    int & destination,
    char * token,
    size_t tokenSize,
    char * amount,
    size_t amountSize)
{
    destination = -1;
    if(nullptr != token && 0 < tokenSize) token[0] = '\0';
    if(nullptr != amount && 0 < amountSize) amount[0] = '\0';
    if(nullptr == message) return false;

    __try {
        destination = *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(message) + kTextMsgDestinationOffset);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        destination = -1;
        return false;
    }

    // IDA: destination 3 is the HudChat route used by cash-award TextMsg.
    if(3 != destination || !CopyTextMsgParam(message, 0, token, tokenSize)) return false;

    bool matches = 0 == strcmp(token, "#Player_Cash_Award_Killed_Enemy")
        || 0 == strcmp(token, "#Player_Cash_Award_Killed_Enemy_Generic");
    if(!matches) return false;

    // The original amount string is param[1]. It remains authoritative and is
    // consumed by the game's own localizer.
    CopyTextMsgParam(message, 1, amount, amountSize);
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

void __fastcall New_TextMsgHandler(void * owner, void * message)
{
    if(nullptr == g_OrgTextMsgHandler) return;

    char token[96];
    char amount[64];
    int destination = -1;
    bool isKillReward = MirvPov_IsEnabled()
        && IsNativeKillRewardTextMsg(
            message,
            destination,
            token,
            sizeof(token),
            amount,
            sizeof(amount));
    if(!isKillReward) {
        g_OrgTextMsgHandler(owner, message);
        return;
    }

    unsigned char * demoController = nullptr;
    unsigned char previousSuppress = 0;
    bool restoreSuppress = false;
    bool releaseDemoGuardLock = false;

    if(0 == g_TextMsgDispatchDepth) {
        AcquireSRWLockExclusive(&g_DemoGuardLock);
        releaseDemoGuardLock = true;
    }
    ++g_TextMsgDispatchDepth;

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

    __try {
        g_OrgTextMsgHandler(owner, message);
        ConsumeOneNativeReward(amount);
    } __finally {
        if(restoreSuppress) RestoreDemoHudChatSuppress(demoController, previousSuppress);
        --g_TextMsgDispatchDepth;
        if(releaseDemoGuardLock) ReleaseSRWLockExclusive(&g_DemoGuardLock);
    }
}

bool IsRewardSchemaAvailable()
{
    return 0 <= g_clientDllOffsets.CCSPlayerController.m_pActionTrackingServices
        && 0 <= g_clientDllOffsets.CCSPlayerController_ActionTrackingServices.m_perRoundStats
        && 0 <= g_clientDllOffsets.CSPerRoundStats_t.m_iKillReward
        && g_clientDllOffsets.CSPerRoundStats_t.size
            > (size_t)g_clientDllOffsets.CSPerRoundStats_t.m_iKillReward;
}

SOURCESDK::CS2::GameEventKeySymbol_t MakeKey(const char * name)
{
    size_t length = strlen(name);
    unsigned int hash = g_HashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

int GetControllerHandle(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return -1;
    auto handle = controller->GetHandle();
    return handle.IsValid() ? handle.ToInt() : -1;
}

CEntityInstance * NormalizePlayerController(CEntityInstance * entity)
{
    if(nullptr == entity) return nullptr;
    __try {
        if(entity->IsPlayerController()) return entity;
        if(!entity->IsPlayerPawn()) return nullptr;
        auto controllerHandle = entity->GetPlayerControllerHandle();
        if(!controllerHandle.IsValid()) return nullptr;
        CEntityInstance * controller = GetEntityFromIndex(controllerHandle.GetEntryIndex());
        return nullptr != controller && controller->IsPlayerController()
            ? controller
            : nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

int GetPlayableTeam(CEntityInstance * entity)
{
    if(nullptr == entity) return 0;
    __try {
        int team = entity->GetTeam();
        if(2 == team || 3 == team) return team;

        CEntityInstance * controller = NormalizePlayerController(entity);
        if(nullptr != controller && controller != entity) {
            team = controller->GetTeam();
            if(2 == team || 3 == team) return team;
        }

        if(nullptr != controller) {
            auto pawnHandle = controller->GetPlayerPawnHandle();
            if(pawnHandle.IsValid()) {
                CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
                if(nullptr != pawn) {
                    team = pawn->GetTeam();
                    if(2 == team || 3 == team) return team;
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

bool HandlesReferToSameEntity(int first, int second)
{
    // Event objects and HUD caches can briefly expose different serial bits for
    // the same controller around interpolation / seek boundaries. The 15-bit
    // entity index remains stable for the current client entity list.
    return 0 <= first && 0 <= second && (first & 0x7fff) == (second & 0x7fff);
}

bool ReadNumericCvar(const char * name, float & value)
{
    if(nullptr == SOURCESDK::CS2::g_pCVar || nullptr == name) return false;
    auto handle = SOURCESDK::CS2::g_pCVar->FindConVar(name, false);
    if(!handle.IsValid()) return false;
    SOURCESDK::CS2::Cvar_s * cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
    if(nullptr == cvar) return false;

    switch(cvar->m_eVarType) {
    case SOURCESDK::CS2::EConVarType_Int16:
        value = static_cast<float>(cvar->m_Value.m_i16Value);
        return true;
    case SOURCESDK::CS2::EConVarType_UInt16:
        value = static_cast<float>(cvar->m_Value.m_u16Value);
        return true;
    case SOURCESDK::CS2::EConVarType_Int32:
        value = static_cast<float>(cvar->m_Value.m_i32Value);
        return true;
    case SOURCESDK::CS2::EConVarType_UInt32:
        value = static_cast<float>(cvar->m_Value.m_u32Value);
        return true;
    case SOURCESDK::CS2::EConVarType_Float32:
        value = cvar->m_Value.m_flValue;
        return true;
    case SOURCESDK::CS2::EConVarType_Float64:
        value = static_cast<float>(cvar->m_Value.m_dbValue);
        return true;
    default:
        return false;
    }
}

bool GetWeaponBaseKillReward(const char * weapon, int & reward)
{
    reward = 0;
    if(nullptr == weapon || '\0' == weapon[0]) return false;
    if(0 == strncmp(weapon, "weapon_", 7)) weapon += 7;

    if(0 == strncmp(weapon, "knife", 5)
        || 0 == strcmp(weapon, "bayonet")) {
        reward = 1500;
        return true;
    }

    if(0 == strcmp(weapon, "awp")
        || 0 == strcmp(weapon, "cz75a")) {
        reward = 100;
        return true;
    }

    if(0 == strcmp(weapon, "taser")) {
        reward = 0;
        return true;
    }

    if(0 == strcmp(weapon, "nova")
        || 0 == strcmp(weapon, "mag7")
        || 0 == strcmp(weapon, "sawedoff")) {
        reward = 900;
        return true;
    }

    if(0 == strcmp(weapon, "xm1014")) {
        reward = 600;
        return true;
    }

    if(0 == strcmp(weapon, "mp9")
        || 0 == strcmp(weapon, "mac10")
        || 0 == strcmp(weapon, "mp7")
        || 0 == strcmp(weapon, "mp5sd")
        || 0 == strcmp(weapon, "ump45")
        || 0 == strcmp(weapon, "bizon")) {
        reward = 600;
        return true;
    }

    const char * defaultAwardWeapons[] = {
        "ak47", "aug", "famas", "galilar", "m4a1", "m4a1_silencer", "sg556",
        "ssg08", "scar20", "g3sg1", "p90", "m249", "negev",
        "deagle", "elite", "fiveseven", "glock", "hkp2000", "usp_silencer",
        "p250", "tec9", "revolver", "hegrenade", "inferno", "molotov",
        "incgrenade", "decoy"
    };
    for(const char * candidate : defaultAwardWeapons) {
        if(0 == strcmp(weapon, candidate)) {
            reward = 300;
            return true;
        }
    }

    return false;
}

bool GetWeaponKillReward(const char * weapon, int & baseReward, float & factor, int & reward)
{
    baseReward = 0;
    factor = 1.0f;
    reward = 0;
    if(!GetWeaponBaseKillReward(weapon, baseReward)) {
        float defaultReward = 300.0f;
        ReadNumericCvar("cash_player_killed_enemy_default", defaultReward);
        if(defaultReward < 0.0f) defaultReward = 0.0f;
        baseReward = static_cast<int>(defaultReward + 0.5f);
    }

    ReadNumericCvar("cash_player_killed_enemy_factor", factor);
    if(factor < 0.0f) factor = 0.0f;
    reward = static_cast<int>(baseReward * factor + 0.5f);
    return true;
}

bool IsTickCompatible(int first, int second)
{
    if(first < 0 || second < 0) return true;
    int delta = first - second;
    if(delta < 0) delta = -delta;
    return delta <= kMatchWindowTicks;
}

void PruneQueue(
    std::deque<PendingEntry> & queue,
    ULONGLONG now,
    int currentDemoTick,
    const char * name)
{
    for(auto it = queue.begin(); it != queue.end();) {
        bool expiredByTime = now - it->timeMs > kMatchWindowMs;
        bool expiredByTick = 0 <= currentDemoTick
            && 0 <= it->demoTick
            && currentDemoTick >= it->demoTick
            && kExpiryWindowTicks < currentDemoTick - it->demoTick;
        if(!expiredByTime && !expiredByTick) {
            ++it;
            continue;
        }

                it = queue.erase(it);
    }
}

void PruneRecentDeaths(ULONGLONG now)
{
    while(!g_RecentDeaths.empty()
        && now - g_RecentDeaths.front().timeMs > kRecentDeathWindowMs) {
        g_RecentDeaths.pop_front();
    }
}

void PruneRecentNativeRewards(ULONGLONG now, int currentDemoTick)
{
    for(auto it = g_RecentNativeRewards.begin(); it != g_RecentNativeRewards.end();) {
        bool expiredByTime = now - it->timeMs > kMatchWindowMs;
        bool expiredByTick = 0 <= currentDemoTick
            && 0 <= it->demoTick
            && currentDemoTick >= it->demoTick
            && kExpiryWindowTicks < currentDemoTick - it->demoTick;
        if(!expiredByTime && !expiredByTick) {
            ++it;
            continue;
        }
        it = g_RecentNativeRewards.erase(it);
    }
}

void PruneQueues(ULONGLONG now, int currentDemoTick)
{
    PruneQueue(g_PendingDeaths, now, currentDemoTick, "death");
    PruneQueue(g_PendingRewards, now, currentDemoTick, "reward");
    PruneRecentDeaths(now);
}

void LimitQueue(std::deque<PendingEntry> & queue, size_t maximum, const char * name)
{
    while(queue.size() > maximum) {
                queue.pop_front();
    }
}

bool IsDuplicateDeath(int controllerHandle, int victimHandle, int demoTick, ULONGLONG now)
{
    PruneRecentDeaths(now);
    for(const PendingEntry & entry : g_RecentDeaths) {
        if(entry.controllerHandle == controllerHandle
            && entry.value == victimHandle
            && entry.demoTick == demoTick) return true;
    }

    g_RecentDeaths.push_back({controllerHandle, now, demoTick, victimHandle});
    LimitQueue(g_RecentDeaths, kMaxRecentDeaths, "recent-death");
    return false;
}

bool PushHudChatText(const char * text, int entityIndex, const char * source)
{
    if(nullptr == text || '\0' == text[0]
        || nullptr == g_FindHudElement || nullptr == g_PushHudNotice) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_hudchat] push skipped: text=%p find=%p push=%p source=%s\n",
            text,
            reinterpret_cast<void *>(g_FindHudElement),
            reinterpret_cast<void *>(g_PushHudNotice),
            nullptr != source ? source : "<null>");
        return false;
    }

    bool dispatched = false;
    __try {
        void * element = g_FindHudElement("CCSGO_HudVoiceStatus");
        void * hudVoiceStatus = nullptr != element
            ? reinterpret_cast<unsigned char *>(element) - 0x20
            : nullptr;
        if(nullptr != hudVoiceStatus) {
            // IDA: {0,1} runs the native chat-markup converter and turns
            // embedded 0x01..0x10 controls into Panorama <font color=...>
            // spans. {1,1} only copied the bytes and rendered one white line.
            const unsigned char flags[2] = {0, 1};
            g_PushHudNotice(
                hudVoiceStatus,
                text,
                0 <= entityIndex ? static_cast<unsigned int>(entityIndex) : 0xffffffffu,
                flags);
            dispatched = true;
        } else {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_hudchat] CCSGO_HudVoiceStatus was not found, source=%s\n",
                nullptr != source ? source : "<null>");
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_hudchat] PushNotice raised an exception, source=%s\n",
            nullptr != source ? source : "<null>");
    }

    if(!dispatched) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_hudchat] push failed, source=%s\n",
            nullptr != source ? source : "<null>");
        return false;
    }

    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "[mirv_pov_hudchat] push ok: entity=%d source=%s\n",
        entityIndex,
        nullptr != source ? source : "<null>");

    return true;
}

bool PrintReward(int amount, const char * source)
{
    if(amount <= 0) {
        return false;
    }

    char amountText[32];
    snprintf(amountText, sizeof(amountText), "%d", amount);

    char message[256];
    const char * localized = LocalizeToken(
        "#Player_Cash_Award_Killed_Enemy_Generic");
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "[mirv_pov_reward] template=%p amount=%d source=%s\n",
        localized,
        amount,
        nullptr != source ? source : "<null>");
    if(!SubstituteFirstLocalizationParameter(
        localized,
        amountText,
        message,
        sizeof(message))) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_reward] localization substitution failed, amount=%d source=%s\n",
            amount,
            nullptr != source ? source : "<null>");
        return false;
    }
    message[sizeof(message) - 1] = '\0';

    if(!PushHudChatText(message, -1, source)) return false;

    return true;
}

void ScheduleRewardNotice(int controllerHandle, int reward, int demoTick, const char * source)
{
    ULONGLONG now = GetTickCount64();
    bool nativeCovered = false;
    size_t pendingCount = 0;

    AcquireSRWLockExclusive(&g_RewardNoticeLock);
    PruneRecentNativeRewards(now, demoTick);
    for(auto it = g_RecentNativeRewards.begin(); it != g_RecentNativeRewards.end(); ++it) {
        if((0 < it->amount && it->amount != reward)
            || !IsTickCompatible(it->demoTick, demoTick)) continue;
        g_RecentNativeRewards.erase(it);
        nativeCovered = true;
        break;
    }
    if(!nativeCovered) {
        g_PendingNotices.push_back({controllerHandle, now, demoTick, reward});
        while(g_PendingNotices.size() > kMaxPendingEntries)
            g_PendingNotices.pop_front();
        pendingCount = g_PendingNotices.size();
    }
    ReleaseSRWLockExclusive(&g_RewardNoticeLock);

    if(nativeCovered) {
        return;
    }

}

void FlushRewardNotices(ULONGLONG now)
{
    for(;;) {
        PendingEntry entry = {};
        bool hasEntry = false;
        AcquireSRWLockExclusive(&g_RewardNoticeLock);
        if(!g_PendingNotices.empty()
            && now - g_PendingNotices.front().timeMs >= kNativeGraceMs) {
            entry = g_PendingNotices.front();
            g_PendingNotices.pop_front();
            hasEntry = true;
        }
        ReleaseSRWLockExclusive(&g_RewardNoticeLock);
        if(!hasEntry) break;

        int reward = entry.value;
        int controllerHandle = entry.controllerHandle;
        int demoTick = entry.demoTick;
        if(PrintReward(reward, "account-after-death")) {
                }
    }
}

void ConsumeOneNativeReward(const char * amount)
{
    int parsedAmount = nullptr != amount ? atoi(amount) : 0;
    ULONGLONG now = GetTickCount64();
    int demoTick = g_LastDemoTick;
    bool coveredNotice = false;
    PendingEntry covered = {};

    AcquireSRWLockExclusive(&g_RewardNoticeLock);
    PruneRecentNativeRewards(now, demoTick);
    auto notice = g_PendingNotices.end();
    if(0 < parsedAmount) {
        for(auto it = g_PendingNotices.begin(); it != g_PendingNotices.end(); ++it) {
            if(it->value == parsedAmount && IsTickCompatible(it->demoTick, demoTick)) {
                notice = it;
                break;
            }
        }
    }
    if(notice == g_PendingNotices.end()) {
        for(auto it = g_PendingNotices.begin(); it != g_PendingNotices.end(); ++it) {
            if(IsTickCompatible(it->demoTick, demoTick)) {
                notice = it;
                break;
            }
        }
    }
    if(notice != g_PendingNotices.end()) {
        covered = *notice;
        g_PendingNotices.erase(notice);
        coveredNotice = true;
    } else {
        g_RecentNativeRewards.push_back({now, demoTick, parsedAmount});
        while(g_RecentNativeRewards.size() > kMaxRecentNativeRewards)
            g_RecentNativeRewards.pop_front();
    }
    ReleaseSRWLockExclusive(&g_RewardNoticeLock);

    if(coveredNotice) {
    }
}

bool MatchDeathWithPendingReward(int controllerHandle, int demoTick)
{
    for(auto it = g_PendingRewards.begin(); it != g_PendingRewards.end(); ++it) {
        if(it->controllerHandle != controllerHandle
            || !IsTickCompatible(it->demoTick, demoTick)) continue;

        int reward = it->value;
        int rewardTick = it->demoTick;
        g_PendingRewards.erase(it);
            ScheduleRewardNotice(controllerHandle, reward, demoTick, "death-after-credit");
        return true;
    }
    return false;
}

void MatchRewardWithPendingDeaths(int controllerHandle, int reward, int demoTick)
{
    size_t matchedDeaths = 0;
    for(auto it = g_PendingDeaths.begin(); it != g_PendingDeaths.end();) {
        if(it->controllerHandle != controllerHandle
            || !IsTickCompatible(it->demoTick, demoTick)) {
            ++it;
            continue;
        }

        ++matchedDeaths;
        it = g_PendingDeaths.erase(it);
    }

    if(0 == matchedDeaths) {
        g_PendingRewards.push_back({controllerHandle, GetTickCount64(), demoTick, reward});
        LimitQueue(g_PendingRewards, kMaxPendingEntries, "reward");
        return;
    }

    // A single network update can contain several deaths. The cumulative
    // m_iKillReward delta is authoritative, but it cannot prove the split for
    // custom weapon-reward cvars. Emit the exact aggregate once instead of
    // inventing equal per-kill values.
    ScheduleRewardNotice(
        controllerHandle,
        reward,
        demoTick,
        1 < matchedDeaths ? "batched-credit" : "credit-after-death");
}

void ClearPending(const char *)
{
    AcquireSRWLockExclusive(&g_RewardNoticeLock);
    g_PendingNotices.clear();
    g_RecentNativeRewards.clear();
    ReleaseSRWLockExclusive(&g_RewardNoticeLock);
    g_PendingDeaths.clear();
    g_PendingRewards.clear();
    g_RecentDeaths.clear();
}

void ObserveCurrentKillReward(int demoTick)
{
    // The MoneyPanel cache is the authoritative POV/account source when its
    // hook is available. During demo playback the observer target can advance
    // before/after player_death, so using it here to rebase state can erase a
    // valid death before the corresponding account credit is observed.
    if(g_MoneyHookAvailable) return;

    CEntityInstance * controller = GetCurrentPovPlayerController();
    int controllerHandle = GetControllerHandle(controller);
    int killReward = -1;
    bool readable = false;

    __try {
        readable = nullptr != controller && controller->GetCurrentRoundKillReward(killReward);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        readable = false;
    }

    if(!readable || controllerHandle < 0 || killReward < 0) {
        return;
    }

    if(g_LastControllerHandle != controllerHandle) {
        ClearPending(0 <= g_LastControllerHandle ? "POV player changed" : "POV baseline");
        g_LastControllerHandle = controllerHandle;
        g_CurrentKillReward = killReward;
        return;
    }

    if(g_CurrentKillReward < 0) {
        g_CurrentKillReward = killReward;
        return;
    }

    int delta = killReward - g_CurrentKillReward;
    g_CurrentKillReward = killReward;

    if(delta < 0) {
        ClearPending("kill reward counter moved backwards");
        return;
    }
    if(0 == delta) return;

    // Per-round m_iKillReward is cumulative; per-death events provide the
    // display amount and native TextMsg remains authoritative when available.
}

bool ResolveCallTarget(
    uint8_t * callSite,
    const Afx::BinUtils::MemRange & textRange,
    uint8_t *& target)
{
    if(0xe8 != *callSite) return false;
    int32_t relative = 0;
    memcpy(&relative, callSite + 1, sizeof(relative));
    target = callSite + 5 + relative;
    return textRange.Start <= reinterpret_cast<size_t>(target)
        && reinterpret_cast<size_t>(target) < textRange.End;
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

bool UpdateHudChatDemoBypass(bool enabled)
{
    AcquireSRWLockExclusive(&g_HudChatDemoBypassLock);

    bool result = false;
    __try {
        if(!g_HudChatDemoBypassAvailable || nullptr == g_HudChatDemoBypassPatch) {
            __leave;
        }
        if(g_HudChatDemoBypassApplied == enabled) {
            result = true;
            __leave;
        }

        const uint8_t bypassBytes[sizeof(g_HudChatDemoBypassOriginal)] = {
            0x90, 0x90, 0x90, 0x90, 0x90, 0x90
        };
        const uint8_t * expectedCurrent = enabled
            ? g_HudChatDemoBypassOriginal
            : bypassBytes;
        const uint8_t * replacement = enabled
            ? bypassBytes
            : g_HudChatDemoBypassOriginal;

        if(0 != memcmp(
            g_HudChatDemoBypassPatch,
            expectedCurrent,
            sizeof(g_HudChatDemoBypassOriginal))) {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_killreward] Common HudChat guard bytes changed unexpectedly; refusing to patch.\n");
            __leave;
        }

        DWORD oldProtect = 0;
        if(!VirtualProtect(
            g_HudChatDemoBypassPatch,
            sizeof(g_HudChatDemoBypassOriginal),
            PAGE_EXECUTE_READWRITE,
            &oldProtect)) {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_killreward] VirtualProtect failed for common HudChat guard (error %lu).\n",
                GetLastError());
            __leave;
        }

        memcpy(
            g_HudChatDemoBypassPatch,
            replacement,
            sizeof(g_HudChatDemoBypassOriginal));
        bool written = 0 == memcmp(
            g_HudChatDemoBypassPatch,
            replacement,
            sizeof(g_HudChatDemoBypassOriginal))
            && 0 != FlushInstructionCache(
                GetCurrentProcess(),
                g_HudChatDemoBypassPatch,
                sizeof(g_HudChatDemoBypassOriginal));

        if(!written) {
            memcpy(
                g_HudChatDemoBypassPatch,
                expectedCurrent,
                sizeof(g_HudChatDemoBypassOriginal));
            FlushInstructionCache(
                GetCurrentProcess(),
                g_HudChatDemoBypassPatch,
                sizeof(g_HudChatDemoBypassOriginal));
        }

        DWORD unused = 0;
        if(!VirtualProtect(
            g_HudChatDemoBypassPatch,
            sizeof(g_HudChatDemoBypassOriginal),
            oldProtect,
            &unused)) {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_killreward] Failed to restore protection for common HudChat guard (error %lu).\n",
                GetLastError());
        }

        if(!written) {
            __leave;
        }

        g_HudChatDemoBypassApplied = enabled;
        result = true;
    } __finally {
        ReleaseSRWLockExclusive(&g_HudChatDemoBypassLock);
    }
    return result;
}

} // namespace

bool MirvPovKillReward_PushHudChatText(
    const char * text,
    int entityIndex,
    const char * source)
{
    return PushHudChatText(text, entityIndex, source);
}

int MirvPovKillReward_GetTrackedPovControllerHandle()
{
    return 0 <= g_LastMoneyControllerHandle
        ? g_LastMoneyControllerHandle
        : g_LastResolvedPovHandle;
}

bool MirvPovKillReward_HashGameEventKey(const char * name, unsigned int & outHash)
{
    outHash = 0;
    if(nullptr == name || nullptr == g_HashString) return false;
    size_t length = strlen(name);
    outHash = g_HashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return true;
}

bool MirvPovKillReward_ApplyHudChatDemoBypass(bool enabled)
{
    return UpdateHudChatDemoBypass(enabled);
}

bool MirvPovKillReward_IsHudChatDemoBypassAvailable()
{
    return g_HudChatDemoBypassAvailable;
}

bool MirvPovKillReward_IsHudChatDemoBypassApplied()
{
    return g_HudChatDemoBypassApplied;
}

const char * MirvPovKillReward_LocalizeToken(const char * token)
{
    return LocalizeToken(token);
}

void MirvPovKillReward_Initialize(HMODULE clientDll)
{
    if(nullptr == clientDll) return;
    if(nullptr == g_HashString) {
        g_HashString = reinterpret_cast<HashString_t>(getAddress(
            clientDll,
            "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? 0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20"));
    }

    if(nullptr == g_PrintHudNotice) {
        Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
        Afx::BinUtils::ImageSectionsReader sections(clientDll);
        sections.Next(IMAGE_SCN_MEM_EXECUTE);
        if(!sections.Eof()) textRange = sections.GetMemRange();

        const char * sayTextHandlerPattern =
            "48 83 EC 58 8B 42 54 83 F8 FF 74 ?? FF C8 EB ?? "
            "B8 FF FF FF FF 4C 8B 4A 48 49 83 E1 FC 80 7A 50 00";
        auto handlerSequence = Afx::BinUtils::FindPatternString(textRange, sayTextHandlerPattern);
        if(!handlerSequence.IsEmpty()) {
            auto remaining = Afx::BinUtils::MemRange(handlerSequence.Start + 1, textRange.End);
            if(Afx::BinUtils::FindPatternString(remaining, sayTextHandlerPattern).IsEmpty()) {
                uint8_t * target = nullptr;
                if(ResolveCallTarget(
                    reinterpret_cast<uint8_t *>(handlerSequence.Start + 0x47),
                    textRange,
                    target)) {
                    g_PrintHudNotice = reinterpret_cast<PrintHudNotice_t>(target);

                    // IDA: the native formatter calls FindHudElement at +0xc3
                    // and CCSGO_HudVoiceStatus::PushNotice at +0xea. We resolve
                    // those callees but skip the formatter itself because both
                    // native wrappers return early during demo playback.
                    uint8_t * findHudTarget = nullptr;
                    uint8_t * pushNoticeTarget = nullptr;
                    if(ResolveCallTarget(target + 0xc3, textRange, findHudTarget)
                        && ResolveCallTarget(target + 0xea, textRange, pushNoticeTarget)) {
                        g_FindHudElement = reinterpret_cast<FindHudElement_t>(findHudTarget);
                        g_PushHudNotice = reinterpret_cast<PushHudNotice_t>(pushNoticeTarget);
                    }

                    // IDA Pro MCP, client.dll 2026-08-09 (SHA-256 EA2B721E...):
                    //   target resolved above is sub_18110BCC0 (SayText formatter)
                    //   the immediately preceding sibling sub_18110BBB0 is used
                    //   by both TextMsg kill rewards and RadioText.
                    //
                    // sub_18110BBB0:
                    //   call IsPlayingDemo
                    //   jz   format_and_push
                    //   call GetDemoController
                    //   cmp  byte ptr [rax+72h], 0
                    //   jnz  return
                    //
                    // Patching only sub_18110BBB0's final jnz preserves the
                    // game's native localization, player/team/location text and
                    // exact amount without affecting unrelated demo behavior.
                    size_t guardAddress = 0;
                    Afx::BinUtils::MemRange formatterRange(
                        reinterpret_cast<size_t>(target - 0x200),
                        reinterpret_cast<size_t>(target));
                    if(FindUniquePattern(
                        formatterRange,
                        "80 78 72 00 0F 85 ?? ?? ?? ??",
                        guardAddress)) {
                        uint8_t * patch = reinterpret_cast<uint8_t *>(guardAddress + 4);
                        int32_t relative = 0;
                        memcpy(&relative, patch + 2, sizeof(relative));
                        uint8_t * jumpTarget = patch + 6 + relative;
                        if(0x0f == patch[0]
                            && 0x85 == patch[1]
                            && target - 0x200 < jumpTarget
                            && jumpTarget < target) {
                            g_HudChatDemoBypassPatch = patch;
                            memcpy(
                                g_HudChatDemoBypassOriginal,
                                patch,
                                sizeof(g_HudChatDemoBypassOriginal));
                            g_HudChatDemoBypassAvailable = true;
                        }
                    }
                }
            }
        }
    }

    if(!g_TextMsgHooked) {
        Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
        Afx::BinUtils::ImageSectionsReader sections(clientDll);
        sections.Next(IMAGE_SCN_MEM_EXECUTE);
        if(!sections.Eof()) textRange = sections.GetMemRange();

        // IDA Pro MCP, client.dll 2026-08-09 (SHA-256 EA2B721E...):
        //   CUserMessageTextMsg handler  sub_18110F560
        //   demo controller getter      sub_180CC8860
        // The handler's repeated-param count / item calls are at +0x94/+0xad.
        const char * textMsgHandlerPattern =
            "48 89 4C 24 ?? 55 53 56 57 41 54 41 55 41 56 41 57 "
            "48 8D AC 24 ?? ?? ?? ?? B8";
        const char * getDemoControllerPattern =
            "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 85 D2";

        size_t textMsgHandlerAddress = 0;
        size_t getDemoControllerAddress = 0;
        uint8_t * countTarget = nullptr;
        uint8_t * atTarget = nullptr;
        bool resolved = !textRange.IsEmpty()
            && FindUniquePattern(textRange, textMsgHandlerPattern, textMsgHandlerAddress)
            && FindUniquePattern(textRange, getDemoControllerPattern, getDemoControllerAddress)
            && ResolveCallTarget(
                reinterpret_cast<uint8_t *>(textMsgHandlerAddress + 0x94),
                textRange,
                countTarget)
            && ResolveCallTarget(
                reinterpret_cast<uint8_t *>(textMsgHandlerAddress + 0xad),
                textRange,
                atTarget)
            && countTarget != atTarget;

        if(resolved) {
            g_OrgTextMsgHandler = reinterpret_cast<TextMsgHandler_t>(textMsgHandlerAddress);
            g_GetDemoController = reinterpret_cast<GetDemoController_t>(getDemoControllerAddress);
            g_RepeatedStringCount = reinterpret_cast<RepeatedStringCount_t>(countTarget);
            g_RepeatedStringAt = reinterpret_cast<RepeatedStringAt_t>(atTarget);

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID &)g_OrgTextMsgHandler, New_TextMsgHandler);
            if(NO_ERROR == DetourTransactionCommit()) {
                g_TextMsgHooked = true;
            } else {
                g_OrgTextMsgHandler = nullptr;
                g_GetDemoController = nullptr;
                g_RepeatedStringCount = nullptr;
                g_RepeatedStringAt = nullptr;
            }
        }
    }

    if(nullptr == g_HashString) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] Game-event hash function was not found.\n");
    }
    if(nullptr == g_PrintHudNotice || nullptr == g_FindHudElement || nullptr == g_PushHudNotice) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] Native HudChat PushNotice path was not found.\n");
    }
    if(!g_TextMsgHooked) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] Native kill-reward TextMsg hook was not installed.\n");
    }
    if(!g_HudChatDemoBypassAvailable) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_killreward] Common HudChat demo guard patch was not found.\n");
    } else if(MirvPov_IsEnabled()) {
        UpdateHudChatDemoBypass(true);
    }

    InitializeMoneyHook(clientDll);
}

void MirvPovKillReward_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || nullptr == g_HashString) return;
    const char * name = event->GetName();
    if(nullptr == name) return;

    if(0 == strcmp(name, "round_start")) {
        MirvPovKillReward_Reset("round_start");
        return;
    }
    if(0 != strcmp(name, "player_death") || !MirvPov_IsEnabled()) return;

    __try {
        auto attackerKey = MakeKey("attacker");
        auto victimKey = MakeKey("userid");
        auto weaponKey = MakeKey("weapon");
        CEntityInstance * attacker = NormalizePlayerController(
            reinterpret_cast<CEntityInstance *>(event->GetPlayerController(attackerKey)));
        CEntityInstance * victim = NormalizePlayerController(
            reinterpret_cast<CEntityInstance *>(event->GetPlayerController(victimKey)));
        CEntityInstance * povController = GetCurrentPovPlayerController();
        if(nullptr == povController) povController = GetObservedPlayerController();
        const char * weapon = event->GetString(weaponKey);

        int attackerHandle = GetControllerHandle(attacker);
        int victimHandle = GetControllerHandle(victim);
        int povHandle = GetControllerHandle(povController);
        if(0 <= povHandle) g_LastResolvedPovHandle = povHandle;
        int attackerTeam = GetPlayableTeam(attacker);
        int victimTeam = GetPlayableTeam(victim);

        bool currentPovMatches = HandlesReferToSameEntity(attackerHandle, povHandle);
        bool moneyPovMatches = HandlesReferToSameEntity(
            attackerHandle,
            g_LastMoneyControllerHandle);
        bool resolvedHistoryMatches = HandlesReferToSameEntity(
            attackerHandle,
            g_LastResolvedPovHandle);

        if(attackerHandle < 0
            || (!currentPovMatches && !moneyPovMatches && !resolvedHistoryMatches)
            || victimHandle < 0
            || attackerHandle == victimHandle
            || (2 != attackerTeam && 3 != attackerTeam)
            || (2 != victimTeam && 3 != victimTeam)
            || attackerTeam == victimTeam) {
            return;
        }

        ULONGLONG now = GetTickCount64();
        PruneQueues(now, g_LastDemoTick);
        if(IsDuplicateDeath(attackerHandle, victimHandle, g_LastDemoTick, now)) {
            return;
        }

        int baseReward = 0;
        float rewardFactor = 1.0f;
        int derivedReward = 0;
        bool derivedRewardAvailable = GetWeaponKillReward(
            weapon,
            baseReward,
            rewardFactor,
            derivedReward);

        if(!derivedRewardAvailable || derivedReward <= 0) {
            return;
        }

        // Each qualified death owns one independent fallback notice. The native
        // TextMsg remains authoritative and consumes this notice during the
        // short grace period. Account changes are capped at $16000 and can
        // contain round/objective credits.
        ScheduleRewardNotice(
            attackerHandle,
            derivedReward,
            g_LastDemoTick,
            "player_death-table-factor");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

void MirvPovKillReward_OnMoneyUpdate(
    int oldAccount,
    int newAccount,
    int oldPanelControllerHandle,
    int newPanelControllerHandle,
    int resolvedPovControllerHandle)
{
    bool ownershipMatchesPov = 0 <= newPanelControllerHandle
        && 0 <= resolvedPovControllerHandle
        && newPanelControllerHandle == resolvedPovControllerHandle;

    // MoneyPanel is asynchronous around POV switches. Never clear a valid kill
    // notice because its cached handle changed; only remember the cache when it
    // agrees with the currently resolved POV controller.
    if(ownershipMatchesPov) g_LastMoneyControllerHandle = newPanelControllerHandle;
    if(0 <= resolvedPovControllerHandle)
        g_LastResolvedPovHandle = resolvedPovControllerHandle;
    int delta = newAccount - oldAccount;

    if(!MirvPov_IsEnabled()
        || oldAccount < 0
        || delta <= 0) return;

    // Account deltas can include round/objective income; never use them as the
    // displayed kill award.
}

void MirvPovKillReward_OnDemoTick(int demoTick)
{
    if(g_LastDemoTick >= 0 && demoTick + 2 < g_LastDemoTick) {
        MirvPovKillReward_Reset("demo rewind/seek");
    }
    g_LastDemoTick = demoTick;

    if(!MirvPov_IsEnabled()) return;

    ULONGLONG now = GetTickCount64();
    PruneQueues(now, demoTick);
    FlushRewardNotices(now);
    ObserveCurrentKillReward(demoTick);
}

void MirvPovKillReward_Reset(const char * reason)
{
    ClearPending(reason);
    g_CurrentKillReward = -1;
    g_LastControllerHandle = -1;
    g_LastMoneyControllerHandle = -1;
    g_LastResolvedPovHandle = -1;
}

void MirvPovKillReward_SetMoneyHookAvailable(bool available)
{
    g_MoneyHookAvailable = available;
}

bool MirvPovKillReward_IsAvailable()
{
    return nullptr != g_HashString
        && nullptr != g_FindHudElement
        && nullptr != g_PushHudNotice;
}
