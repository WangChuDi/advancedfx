#include "stdafx.h"

#include "MirvPovHud.h"

#include "ClientEntitySystem.h"
#include "MirvPanorama.h"
#include "MirvPovCore.h"
#include "Globals.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <stdint.h>
#include <intrin.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

static void* g_PovStylePropertyVisibleVtable = nullptr;

typedef void(__fastcall * PovPanelStyleSetStyleProperty_t)(void* This, void* property, bool transition);
static PovPanelStyleSetStyleProperty_t g_PovPanelStyleSetStyleProperty = nullptr;

typedef uint8_t* (__fastcall * PovResolveStyleProperty_t)(uint8_t* out, const char* stylePropertyName);
static PovResolveStyleProperty_t g_PovResolveStyleProperty = nullptr;

struct PovStylePropertyVisible {
    void* vtable;
    uint8_t id;
    bool disallowTransition = false;
    unsigned char pad[0x6];
    uint16_t value;

    PovStylePropertyVisible(void* vt, uint8_t i, bool v)
        : vtable(vt), id(i), value(v ? 0x0101 : 0x0001) {}
};

static bool MirvPovHud_SetPanelVisible(void* panel, bool value) {
    if(!panel || !g_PovStylePropertyVisibleVtable || !g_PovPanelStyleSetStyleProperty || !g_PovResolveStyleProperty) return false;

    uint8_t id = 0xFF;
    g_PovResolveStyleProperty(&id, "visibility");
    if(0xFF == id) return false;

    PovStylePropertyVisible styleProperty(g_PovStylePropertyVisibleVtable, id, value);
    auto style = (unsigned char*)panel + CS2::PanoramaUIPanel::panelStyle;
    g_PovPanelStyleSetStyleProperty(style, &styleProperty, true);
    return true;
}

static unsigned char* MirvPovHud_GetHudPanel() {
    void ** hudPanel = MirvPanorama_GetHudPanel();
    return hudPanel ? ((unsigned char***)hudPanel)[0][1] : nullptr;
}

static bool MirvPovHud_MakeSymbol(const char* name, short& value) {
    void ** uiEnginePtr = MirvPanorama_GetUIEngine();
    if(!name || !uiEnginePtr || !*uiEnginePtr || !CS2::PanoramaUIEngine::makeSymbol) return false;

    typedef short(__fastcall * MakeSymbol_t)(void*, int, const char*);
    auto uiEngine = *uiEnginePtr;
    auto vtable = *(unsigned char**)uiEngine;
    if(!vtable) return false;

    auto makeSymbol = *(MakeSymbol_t*)(vtable + CS2::PanoramaUIEngine::makeSymbol);
    if(!makeSymbol) return false;

    value = makeSymbol(uiEngine, 0, name);
    return value != (short)-1;
}

static bool MirvPovHud_SetPanelClass(void* panel, const char* className, bool value) {
    if(!panel || !className) return false;

    short classSymbol = -1;
    if(!MirvPovHud_MakeSymbol(className, classSymbol)) return false;

    typedef void (__fastcall * SetPanelClass_t)(void*, short);
    typedef bool (__fastcall * HasPanelClass_t)(void*, short);
    auto vtable = *(void***)panel;
    if(!vtable) return false;

    auto setPanelClass = (SetPanelClass_t)vtable[value ? 144 : 147];
    auto hasPanelClass = (HasPanelClass_t)vtable[157];
    if(!setPanelClass || !hasPanelClass) return false;

    setPanelClass(panel, classSymbol);
    return hasPanelClass(panel, classSymbol) == value;
}

static bool MirvPovHud_SetNativePanelClass(void* panel, const char* className, bool value) {
    if(!panel || !className) return false;

    short classSymbol = -1;
    if(!MirvPovHud_MakeSymbol(className, classSymbol)) return false;

    using SetPanelClassState_t = void (__fastcall *)(void*, uint16_t, bool);
    auto vtable = *(void***)panel;
    if(!vtable || !vtable[160]) return false;

    auto setPanelClass = reinterpret_cast<SetPanelClassState_t>(vtable[160]);
    setPanelClass(panel, static_cast<uint16_t>(classSymbol), value);
    return true;
}

static unsigned char* MirvPovHud_FindPanelByIdRecursive(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel) return nullptr;

    const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
    if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

    const auto children = parentPanel + CS2::PanoramaUIPanel::children;
    const auto childCount = *(int*)children;
    for(int i = 0; i < childCount; ++i) {
        if(auto panel = MirvPovHud_FindPanelByIdRecursive(((unsigned char***)children)[1][i], panelId)) return panel;
    }

    return nullptr;
}

static unsigned char* MirvPovHud_FindPanelById(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel || !panelId) return nullptr;

    const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
    if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

    __try {
        typedef unsigned char* (__fastcall * FindChildTraverse_t)(unsigned char*, const char*);
        auto vtable = *(void***)parentPanel;
        auto findChildTraverse = vtable ? (FindChildTraverse_t)vtable[47] : nullptr;
        if(findChildTraverse) {
            if(auto panel = findChildTraverse(parentPanel, panelId)) return panel;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return MirvPovHud_FindPanelByIdRecursive(parentPanel, panelId);
}

static bool MirvPovHud_PanelContainsId(unsigned char* parentPanel, const char* panelId) {
    return nullptr != MirvPovHud_FindPanelById(parentPanel, panelId);
}

static bool MirvPovHud_SetStrokeSiblingVisibleForAnchor(unsigned char* parentPanel, const char* anchorId) {
    if(!parentPanel) return false;

    const auto children = parentPanel + CS2::PanoramaUIPanel::children;
    const auto childCount = *(int*)children;

    if(2 == childCount) {
        int anchorChild = -1;
        for(int i = 0; i < childCount; ++i) {
            if(MirvPovHud_PanelContainsId(((unsigned char***)children)[1][i], anchorId)) {
                anchorChild = i;
                break;
            }
        }

        if(-1 != anchorChild) {
            const auto strokePanel = ((unsigned char***)children)[1][1 - anchorChild];
            if(MirvPovHud_SetPanelVisible(strokePanel, true)) return true;
        }
    }

    for(int i = 0; i < childCount; ++i) {
        if(MirvPovHud_SetStrokeSiblingVisibleForAnchor(((unsigned char***)children)[1][i], anchorId)) return true;
    }

    return false;
}

static void MirvPovHud_ShowHealthAmmoCenterStrokes() {
    auto hudPanel = MirvPovHud_GetHudPanel();
    if(!hudPanel) return;

    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-HA-main");
    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-WPN-main");
}

static bool MirvPovHud_HideSpecPlayerPanel() {
    auto hudPanel = MirvPovHud_GetHudPanel();
    if(!hudPanel) {
        static bool warned = false;
        if(!warned) {
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] HUD panel was not available while hiding HudSpecplayer.\n");
            warned = true;
        }
        return false;
    }

    auto specPlayerBg = MirvPovHud_FindPanelById(hudPanel, "jsHudSpecplayer__Bg");
    const bool bgHidden = specPlayerBg && MirvPovHud_SetPanelVisible(specPlayerBg, false);

    auto specPlayerAvatar = MirvPovHud_FindPanelById(hudPanel, "HudSpecplayer__Avatar");
    const bool avatarHidden = specPlayerAvatar && MirvPovHud_SetPanelVisible(specPlayerAvatar, false);

    static bool warned = false;
    if(!bgHidden || !avatarHidden) {
        if(!warned) {
            MIRV_POV_DIAGNOSTIC_WARNING(
                "[mirv_pov_hud] HudSpecplayer visibility update incomplete "
                "(bg=%d/%d avatar=%d/%d).\n",
                specPlayerBg ? 1 : 0,
                bgHidden ? 1 : 0,
                specPlayerAvatar ? 1 : 0,
                avatarHidden ? 1 : 0);
            warned = true;
        }
    }

    return bgHidden && avatarHidden;
}

static unsigned char* g_SpectatorHotKeyLabelContainerPanel = nullptr;
static unsigned char* g_LastHudPanel = nullptr;
static bool g_HudPanelStateNeedsRefresh = true;

static void MirvPovHud_SetSpectatorHotKeyLabelsVisible(bool visible) {
    __try {
        if(!g_SpectatorHotKeyLabelContainerPanel) {
            auto hudPanel = MirvPovHud_GetHudPanel();
            if(!hudPanel) return;

            g_SpectatorHotKeyLabelContainerPanel = MirvPovHud_FindPanelById(
                hudPanel,
                "HotKeyLabelContainer");
        }

        // Reuse the native DemoUI state that hudlegend.css already handles:
        // .DemoControllerFull .HudSpecplayer__key-hints-text { visibility: collapse; }
        if(g_SpectatorHotKeyLabelContainerPanel) {
            if(!MirvPovHud_SetPanelClass(
                g_SpectatorHotKeyLabelContainerPanel,
                "DemoControllerFull",
                !visible)) {
                g_SpectatorHotKeyLabelContainerPanel = nullptr;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_SpectatorHotKeyLabelContainerPanel = nullptr;
    }
}

using MirvPovHud_BuyStatePredicate_t = bool (__fastcall *)(void *);

static bool MirvPovHud_IsExecutableAddress(const void * address) {
    if(nullptr == address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if(0 == VirtualQuery(address, &info, sizeof(info))) return false;
    const DWORD protection = info.Protect & 0xFF;
    return MEM_COMMIT == info.State
        && (PAGE_EXECUTE == protection || PAGE_EXECUTE_READ == protection
            || PAGE_EXECUTE_READWRITE == protection || PAGE_EXECUTE_WRITECOPY == protection);
}

static bool MirvPovHud_ShouldShowBuyZoneIcon(CEntityInstance * povPawn) {
    if(nullptr == povPawn) return false;

    bool buyZoneAvailable = false;
    const bool inBuyZone = povPawn->GetInBuyZone(buyZoneAvailable);
    if(!buyZoneAvailable) return false;

    // These are stable client.dll predicates for the native HudMoney update:
    // the replicated pawn flag alone is not enough after the buy timer ends.
    constexpr uintptr_t kGameRulesGlobalRva = 0x23A8BD8;
    constexpr uintptr_t kBuyStatePredicateRva = 0x722660;
    constexpr uintptr_t kBuyTimeElapsedPredicateRva = 0x731E00;
    HMODULE clientDll = GetModuleHandleW(L"client.dll");
    if(nullptr == clientDll) return false;

    auto base = reinterpret_cast<uint8_t *>(clientDll);
    auto buyState = reinterpret_cast<MirvPovHud_BuyStatePredicate_t>(
        base + kBuyStatePredicateRva);
    auto buyTimeElapsed = reinterpret_cast<MirvPovHud_BuyStatePredicate_t>(
        base + kBuyTimeElapsedPredicateRva);
    if(!MirvPovHud_IsExecutableAddress(buyState)
        || !MirvPovHud_IsExecutableAddress(buyTimeElapsed)) return false;

    __try {
        void * gameRules = *reinterpret_cast<void **>(base + kGameRulesGlobalRva);
        if(nullptr == gameRules) return false;
        return inBuyZone && buyState(gameRules) && !buyTimeElapsed(gameRules);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void MirvPovHud_RefreshBuyZoneIcon() {
    if(!MirvPov_IsEnabled()) return;

    auto hudPanel = MirvPovHud_GetHudPanel();
    if(!hudPanel) return;

    auto moneyPanel = MirvPovHud_FindPanelById(hudPanel, "HudMoney");
    if(nullptr == moneyPanel) {
        moneyPanel = MirvPovHud_FindPanelById(hudPanel, "HudMoneyPanel");
    }
    if(nullptr == moneyPanel) return;

    MirvPovHud_SetNativePanelClass(
        moneyPanel,
        "money__in-buy-zone",
        MirvPovHud_ShouldShowBuyZoneIcon(GetCurrentPovPlayerPawn()));
}

static void MirvPovHud_ResetPanelState() {
    g_LastHudPanel = nullptr;
    g_HudPanelStateNeedsRefresh = true;
    g_SpectatorHotKeyLabelContainerPanel = nullptr;
}

static void MirvPovHud_RefreshPanelState() {
    __try {
        auto hudPanel = MirvPovHud_GetHudPanel();
        if(hudPanel != g_LastHudPanel) {
            g_LastHudPanel = hudPanel;
            g_HudPanelStateNeedsRefresh = true;
            g_SpectatorHotKeyLabelContainerPanel = nullptr;
        }

        if(!g_HudPanelStateNeedsRefresh || !hudPanel) return;

        const bool specPlayerHidden = MirvPovHud_HideSpecPlayerPanel();
        MirvPovHud_ShowHealthAmmoCenterStrokes();
        MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);
        g_HudPanelStateNeedsRefresh = !specPlayerHidden;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_LastHudPanel = nullptr;
        g_HudPanelStateNeedsRefresh = true;
        g_SpectatorHotKeyLabelContainerPanel = nullptr;
    }
}

void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath) {
    if(0 == strcmp("panorama\\layout\\hud\\hudhealthammocenter.xml", filePath)) {
        g_HudPanelStateNeedsRefresh = true;
        MirvPovHud_RefreshPanelState();
    } else if(0 == strcmp("panorama\\layout\\hud\\hudlegend.xml", filePath)) {
        MirvPovHud_ResetPanelState();
        if(MirvPov_IsEnabled()) MirvPovHud_RefreshPanelState();
    }
}

void MirvPovHud_OnPanoramaDllLoaded(HMODULE panoramaDll) {
    g_PovStylePropertyVisibleVtable = (void**)Afx::BinUtils::FindClassVtable(
        panoramaDll,
        ".?AVCStylePropertyVisible@panorama@@",
        0,
        0);
    if(!g_PovStylePropertyVisibleVtable) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] Panorama visibility property was not found.\n");
    }

    auto resolveAddress = getAddress(
        panoramaDll,
        "40 55 56 57 41 54 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 44 8B 05 ?? ?? ?? ?? 48 8B F9 65 48 8B 04 25 58 00 00 00 45 33 E4 C6 01 FF 48 8B F2");
    g_PovResolveStyleProperty = (PovResolveStyleProperty_t)resolveAddress;
    if(!g_PovResolveStyleProperty) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] Panorama style-property resolver was not found.\n");
    }

    auto setterAddress = getAddress(
        panoramaDll,
        "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 89 45 ?? EB");
    g_PovPanelStyleSetStyleProperty = setterAddress
        ? (PovPanelStyleSetStyleProperty_t)(setterAddress + 5 + *(int32_t*)(setterAddress + 1))
        : nullptr;
    if(!g_PovPanelStyleSetStyleProperty) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] Panorama style setter was not found.\n");
    }

    MirvPovHud_ResetPanelState();
}

static int g_ScoreboardSeekSuppressFrames = 0;
static int g_LastDemoTick = -1;

void MirvPovHud_OnLevelInitPreEntity() {
    MirvPovHud_ResetPanelState();
    g_ScoreboardSeekSuppressFrames = 0;
    g_LastDemoTick = -1;
}

void MirvPovHud_ReapplyPanelState() {
    MirvPovHud_RefreshPanelState();
    if(MirvPov_IsEnabled()) {
        // Reapply the existing Demo-card background/avatar state after native
        // observer updates.
        MirvPovHud_HideSpecPlayerPanel();
        MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);
        MirvPovHud_RefreshBuyZoneIcon();
    }
}

void MirvPovHud_UpdateSeekDetection(int curTick) {
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);

    if(g_LastDemoTick >= 0) {
        int delta = curTick - g_LastDemoTick;
        if(delta < 0) delta = -delta;
        if(delta > 2) {
            g_ScoreboardSeekSuppressFrames = 16;
        }
    }
    g_LastDemoTick = curTick;
    if(g_ScoreboardSeekSuppressFrames > 0) {
        g_ScoreboardSeekSuppressFrames--;
    }
}

bool MirvPovHud_ShouldSuppressFrame() {
    return g_ScoreboardSeekSuppressFrames > 0;
}

typedef bool (__fastcall * FlashViewPredicate_t)();
static FlashViewPredicate_t g_OrgFlashViewPredicate = nullptr;
static bool g_bFlashViewPredicateHooked = false;
static void * g_FlashViewPredicateReturnAddresses[2] = {};
static bool g_FlashHooksActive = false;

static uint8_t * MirvPovHud_GetRelativeCallTarget(uint8_t * callSite) {
    if(nullptr == callSite || 0xE8 != callSite[0]) return nullptr;
    int32_t relative = *reinterpret_cast<int32_t *>(callSite + 1);
    return callSite + 5 + relative;
}

static bool __fastcall New_FlashViewPredicate() {
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    bool result = g_OrgFlashViewPredicate();
    MirvPov_PopHookReturnAddress(previousReturnAddress);

    if(!g_FlashHooksActive || !MirvPov_IsEnabled()) return result;
    if(returnAddress == g_FlashViewPredicateReturnAddresses[0]
        || returnAddress == g_FlashViewPredicateReturnAddresses[1]) return false;
    return result;
}

static bool MirvPovHud_ResolveFlashContexts(HMODULE clientDll) {
    const size_t compactPathMatch = getAddress(
        clientDll,
        "48 8B F2 48 8B E9 E8 ?? ?? ?? ?? 84 C0 0F 85");
    const size_t perViewPathMatch = getAddress(
        clientDll,
        "84 C0 74 4C 8B 85 B0 02 00 00 49 8D 8D 48 03 00 00");
    if(0 == compactPathMatch || 0 == perViewPathMatch) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_flash] Flash render contexts were not found.\n");
        return false;
    }

    uint8_t * compactPredicateCall = reinterpret_cast<uint8_t *>(compactPathMatch) + 6;
    uint8_t * perViewPredicateCall = reinterpret_cast<uint8_t *>(perViewPathMatch) - 5;
    uint8_t * compactPredicateTarget = MirvPovHud_GetRelativeCallTarget(compactPredicateCall);
    uint8_t * perViewPredicateTarget = MirvPovHud_GetRelativeCallTarget(perViewPredicateCall);
    if(nullptr == compactPredicateTarget || compactPredicateTarget != perViewPredicateTarget) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_flash] Flash view predicate validation failed.\n");
        return false;
    }

    g_OrgFlashViewPredicate = reinterpret_cast<FlashViewPredicate_t>(compactPredicateTarget);
    g_FlashViewPredicateReturnAddresses[0] = compactPredicateCall + 5;
    g_FlashViewPredicateReturnAddresses[1] = perViewPredicateCall + 5;
    return true;
}

static bool MirvPovHud_InstallFlashPredicateHook(HMODULE clientDll) {
    if(g_bFlashViewPredicateHooked) return true;
    if(!MirvPovHud_ResolveFlashContexts(clientDll)) return false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgFlashViewPredicate, New_FlashViewPredicate);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgFlashViewPredicate = nullptr;
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_flash] Flash view predicate Detour failed.\n");
        return false;
    }

    g_bFlashViewPredicateHooked = true;
    return true;
}

static void MirvPovHud_RemoveFlashPredicateHook() {
    if(!g_bFlashViewPredicateHooked || nullptr == g_OrgFlashViewPredicate) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID &)g_OrgFlashViewPredicate, New_FlashViewPredicate);
    if(NO_ERROR != DetourTransactionCommit()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_flash] Flash view predicate Detour removal failed.\n");
        return;
    }

    g_bFlashViewPredicateHooked = false;
    g_OrgFlashViewPredicate = nullptr;
}

void MirvPovHud_ApplyPatches(HMODULE clientDll) {
    if(nullptr == clientDll) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] client.dll is not loaded.\n");
        return;
    }

    MirvPovHud_InstallFlashPredicateHook(clientDll);
    g_FlashHooksActive = true;

    MirvPovHud_ResetPanelState();
    MirvPovHud_ReapplyPanelState();

    return;
}

void MirvPovHud_RemovePatches() {
    g_FlashHooksActive = false;
    MirvPovHud_RemoveFlashPredicateHook();
    g_FlashViewPredicateReturnAddresses[0] = nullptr;
    g_FlashViewPredicateReturnAddresses[1] = nullptr;
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(true);
    MirvPovHud_ResetPanelState();
    g_ScoreboardSeekSuppressFrames = 0;
    g_LastDemoTick = -1;
}
