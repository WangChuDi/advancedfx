#include "stdafx.h"
#include "MirvPovBuyMenu.h"
#include "MirvPovCore.h"
#include "ClientEntitySystem.h"
#include "SchemaSystem.h"
#include "../shared/AfxConsole.h"
#include "../deps/release/Detours/src/detours.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <bcrypt.h>
#include <intrin.h>
#include <algorithm>
#include <cstring>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

namespace {
// This adapter is intentionally tied to the IDA-analyzed build. Never apply
// these RVAs/ABI layouts to an unknown client DLL.
constexpr unsigned char kClientHash[] = {
    0x40,0xbc,0xe8,0x20,0x6f,0x51,0xb9,0x2e,0xe0,0x5d,0x61,0x21,0xc6,0xe4,0x2c,0x71,
    0x7b,0xf3,0xfa,0x0c,0xc0,0xed,0xee,0xb6,0x69,0x87,0x44,0xb1,0xc4,0x79,0x9f,0xeb
};
template<class T> T & Field(void * p, size_t offset) {
    return *reinterpret_cast<T *>(static_cast<unsigned char *>(p) + offset);
}
using PanelFn = intptr_t (__fastcall *)(void *);
using GetterFn = CEntityInstance * (__fastcall *)(int);
using LoadoutFn = void * (__fastcall *)(void *, unsigned int, unsigned int);
using WriteFn = void (__fastcall *)(void *, bool);
using PurchaseFn = char (__fastcall *)(void *, unsigned int, void *);
using SellFn = char (__fastcall *)(void *, int);
using EventFn = void ** (__fastcall *)(void **, void *);
using ModelFn = int (__fastcall *)(void *, void *);
using SelectFn = char (__fastcall *)(void *, unsigned int);
using SetModelFn = intptr_t (__fastcall *)(void *, const char *);
uintptr_t g_Base = 0;
bool g_Hooked = false, g_Active = false, g_Wanted = false, g_Pending = false;
bool g_CancelledPending = false;
bool g_Enabled = false; // User preference survives seeks and demo changes.
ULONGLONG g_PendingSince = 0;
int g_LastTick = -1;
uint64_t g_MenuHandle = 0xFFFFFFFF00000000ULL;
uint32_t g_PawnHandle = 0xFFFFFFFF, g_ControllerHandle = 0xFFFFFFFF;
void * g_Panel = nullptr;
CEntityInstance * g_Pawn = nullptr, * g_Controller = nullptr;
thread_local unsigned int g_Depth = 0;
PanelFn g_Open, g_Close, g_Refresh, g_FullRefresh, g_Hover, g_Think;
GetterFn g_GetPawn, g_GetController;
LoadoutFn g_Loadout, g_LoadoutHover;
WriteFn g_Write;
PurchaseFn g_Purchase;
SellFn g_Sell;
ModelFn g_Model;
SelectFn g_Select;
SetModelFn g_SetModel;
thread_local void * g_ModelPreview = nullptr;
unsigned int g_OpenCount = 0, g_CloseCount = 0, g_LoadoutHits = 0, g_LoadoutMisses = 0;
uint16_t g_HudWashClass = 0xFFFF;
std::vector<uint64_t> g_SuppressedWash;

// Native Panorama weak handles keep restoration safe across layout teardown.
void * ResolveUi(uint64_t handle) {
    if(!g_Base || handle == 0xFFFFFFFF00000000ULL) return nullptr;
    auto engine = *reinterpret_cast<void **>(g_Base + 0x2729660);
    return engine ? reinterpret_cast<void * (__fastcall *)(void *, uint64_t *)>(
        Field<void **>(engine, 0)[34])(engine, &handle) : nullptr;
}
bool HasClass(void * ui, uint16_t symbol) {
    return ui && reinterpret_cast<bool (__fastcall *)(void *, uint16_t)>(
        Field<void **>(ui, 0)[157])(ui, symbol);
}
void SetClass(void * ui, uint16_t symbol, bool enabled) {
    reinterpret_cast<void (__fastcall *)(void *, uint16_t, bool)>(
        Field<void **>(ui, 0)[160])(ui, symbol, enabled);
}
void RestoreWashClasses() {
    for(auto handle : g_SuppressedWash)
        if(auto ui = ResolveUi(handle)) SetClass(ui, g_HudWashClass, true);
    g_SuppressedWash.clear();
}
void ApplyBuyStyles(void * menu) {
    if(!g_Active || !menu || g_HudWashClass == 0xFFFF) return;
    // Spectator HUD color selectors have greater specificity than the native
    // buywheel-cant-buy selector. Exclude only disabled item content from that
    // tint; let buymenu.css provide its original icon/name/price colors.
    const auto cantBuy = *reinterpret_cast<uint16_t *>(g_Base + 0x25BDDF8);
    const auto cantAfford = *reinterpret_cast<uint16_t *>(g_Base + 0x25BDDF4);
    for(int group = 0; group < 5; ++group) {
        int count = Field<int>(menu, 128 + 56 * group);
        auto records = Field<unsigned char *>(menu, 136 + 56 * group);
        if(!records || count < 0 || count > 64) continue;
        for(int i = 0; i < count; ++i) {
            auto record = records + 88 * i;
            auto root = ResolveUi(Field<uint64_t>(record, 0));
            const bool disabled = HasClass(root, cantBuy);
            for(size_t offset : {size_t(8), size_t(16), size_t(24)}) {
                auto handle = Field<uint64_t>(record, offset);
                auto ui = ResolveUi(handle);
                if(!ui) continue;
                auto found = std::find(g_SuppressedWash.begin(), g_SuppressedWash.end(), handle);
                if(disabled || (offset == 24 && HasClass(root, cantAfford))) {
                    if(HasClass(ui, g_HudWashClass)) {
                        if(found == g_SuppressedWash.end()) g_SuppressedWash.push_back(handle);
                        SetClass(ui, g_HudWashClass, false);
                    }
                } else if(found != g_SuppressedWash.end()) {
                    SetClass(ui, g_HudWashClass, true);
                    g_SuppressedWash.erase(found);
                }
            }
        }
    }
}

CEntityInstance * ResolveEntity(uint32_t handle, bool pawn) {
    if(handle == 0xFFFFFFFF) return nullptr;
    auto entity = GetEntityFromIndex(handle & 0x7FFF);
    if(!entity || static_cast<uint32_t>(entity->GetHandle().ToInt()) != handle) return nullptr;
    return (pawn ? entity->IsPlayerPawn() : entity->IsPlayerController()) ? entity : nullptr;
}
struct Scope {
    bool entered;
    Scope() : entered(g_Active) {
        if(entered) {
            // Panorama can tick during disconnect before the next render pass.
            // Entity handles, unlike cached pointers, detect that teardown.
            g_Pawn = ResolveEntity(g_PawnHandle, true);
            g_Controller = ResolveEntity(g_ControllerHandle, false);
            if(!g_Pawn || !g_Controller) g_Wanted = false;
            ++g_Depth;
        }
    }
    ~Scope() { if(entered) --g_Depth; }
};
bool InClient(void * address) {
    const auto rva = reinterpret_cast<uintptr_t>(address) - g_Base;
    return rva < 0x2993000;
}
bool Visible(void * menu) {
    if(!menu) return false;
    void * panel = Field<void *>(menu, 8);
    if(!panel) return false;
    return reinterpret_cast<bool (__fastcall *)(void *)>(Field<void **>(panel, 0)[34])(panel);
}
void * ResolveMenu() {
    if(!g_Base || g_MenuHandle == 0xFFFFFFFF00000000ULL) return nullptr;
    void * engine = *reinterpret_cast<void **>(g_Base + 0x2729660);
    if(!engine) return nullptr;
    auto panel = reinterpret_cast<void * (__fastcall *)(void *, uint64_t *)>(
        Field<void **>(engine, 0)[34])(engine, &g_MenuHandle);
    if(!panel) return nullptr;
    return reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(panel, 0)[8])(panel);
}
void CacheMenu(void * menu) {
    void * engine = *reinterpret_cast<void **>(g_Base + 0x2729660);
    auto panel = reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(menu, 0)[0])(menu);
    uint64_t handle = 0xFFFFFFFF00000000ULL;
    g_MenuHandle = *reinterpret_cast<uint64_t * (__fastcall *)(void *, uint64_t *, void *)>(
        Field<void **>(engine, 0)[33])(engine, &handle, panel);
    g_Panel = menu;
}
bool Dispatch(size_t creator) {
    void * engine = *reinterpret_cast<void **>(g_Base + 0x2729660);
    if(!engine) return false;
    void * event = nullptr;
    reinterpret_cast<EventFn>(g_Base + creator)(&event, nullptr);
    if(!event) return false;
    reinterpret_cast<void (__fastcall *)(void *, void **)>(Field<void **>(engine, 0)[47])(engine, &event);
    return true;
}
CEntityInstance * __fastcall GetPawn(int slot) {
    if(g_Depth && InClient(_ReturnAddress()) && (slot == 0 || slot == -1)) return g_Pawn;
    return g_GetPawn(slot);
}
CEntityInstance * __fastcall GetController(int slot) {
    if(g_Depth && InClient(_ReturnAddress()) && (slot == 0 || slot == -1)) return g_Controller;
    return g_GetController(slot);
}
void * ObservedLoadout(void * inventory, unsigned int team, unsigned int slot) {
    if(!g_Controller || inventory != Field<void *>(g_Controller, 0x820)) return nullptr;
    // ServerAuthoritativeWeaponSlot_t: 56-byte network records. These carry
    // the observed player's chosen weapon definitions, not the viewer's loadout.
    const int count = Field<int>(inventory, 0x88);
    auto records = Field<unsigned char *>(inventory, 0x90);
    auto manager = reinterpret_cast<void * (__fastcall *)()>(g_Base + 0x838C70)();
    if(records && count > 0 && count <= 256) {
        for(int i = 0; i < count; ++i) {
            auto record = records + i * 56;
            if(Field<uint16_t>(record, 48) == team && Field<uint16_t>(record, 50) == slot) {
                auto item = reinterpret_cast<void * (__fastcall *)(void *, unsigned int, int, int)>(
                    g_Base + 0x112DCC0)(manager, Field<uint16_t>(record, 52), 0, 0);
                if(item) { ++g_LoadoutHits; return item; }
            }
        }
    }
    // The server vector is sparse: absent entries use the shared default
    // definition table, exactly as 903D00 does. 83B820 is a direct team/slot
    // array lookup; it does not read the local user's equipped-item interface.
    {
        auto item = reinterpret_cast<LoadoutFn>(g_Base + 0x83B820)(manager, team, slot);
        if(item) { ++g_LoadoutHits; return item; }
    }
    if(slot <= 57) ++g_LoadoutMisses; // Ignore native hover/special-slot sentinels.
    return nullptr;
}
void * __fastcall Loadout(void * inventory, unsigned int team, unsigned int slot) {
    return g_Depth ? ObservedLoadout(inventory, team, slot) : g_Loadout(inventory, team, slot);
}
void * __fastcall LoadoutHover(void * inventory, unsigned int team, unsigned int slot) {
    return g_Depth ? ObservedLoadout(inventory, team, slot) : g_LoadoutHover(inventory, team, slot);
}
void __fastcall Write(void * pawn, bool open) {
    // Never overwrite the networked input bit we are replaying, or send the
    // open_buymenu/close_buymenu commands on behalf of a recorded player.
    if(!g_Active && !g_Depth) g_Write(pawn, open);
}
char __fastcall Purchase(void * panel, unsigned int slot, void * button) {
    return g_Active ? 1 : g_Purchase(panel, slot, button);
}
char __fastcall Sell(void * panel, int slot) {
    return g_Active ? 1 : g_Sell(panel, slot);
}
int __fastcall Model(void * panel, void * item) {
    Scope scope;
    if(!scope.entered) return g_Model(panel, item);
    if(!g_Pawn || !g_Controller) return 0;
    auto previous = g_ModelPreview;
    g_ModelPreview = Field<void *>(panel, 520);
    auto result = g_Model(panel, item);
    g_ModelPreview = previous;
    return result;
}
intptr_t __fastcall SetModel(void * preview, const char * model) {
    if(g_Depth && g_Pawn && preview == g_ModelPreview) {
        // Native weapon selection is reusable, but its agent lookup uses the
        // viewer's equipped inventory. Bind the recorded pawn's model instead.
        model = nullptr;
        reinterpret_cast<void * (__fastcall *)(void *, const char **)>(g_Base + 0x21C060)(g_Pawn, &model);
        if(!model || !*model) model = g_Pawn->GetTeam() == 3
            ? "agents/models/ctm_sas/ctm_sas.vmdl" : "agents/models/tm_phoenix/tm_phoenix.vmdl";
        // E599F0 stores the viewer's agent item ID in this native preview slot.
        // Clear that cosmetic binding before applying the recorded model.
        const int slot = Field<int>(preview, 2284);
        auto records = Field<unsigned char *>(preview, 2296);
        if(records && slot >= 0 && slot < Field<int>(preview, 2288))
            Field<uint64_t>(records + 160 * slot, 40) = 0;
    }
    // This must precede native EquipPlayerWithItem: SetPlayerModel clears the
    // equipment list, so applying it after equip leaves the preview unarmed.
    return g_SetModel(preview, model);
}
char __fastcall Select(void * panel, unsigned int slot) {
    Scope scope;
    if(scope.entered && (!g_Pawn || !g_Controller)) return 0;
    return g_Select(panel, slot);
}
intptr_t __fastcall Open(void * panel) {
    if(g_Pending || g_CancelledPending) {
        g_Pending = g_CancelledPending = false;
        if(!g_Active || !g_Wanted) return 0;
    }
    if(!g_Active) return g_Open(panel);
    Scope scope;
    if(!g_Wanted || !g_Pawn || !g_Controller) return 0;
    // The native open handler toggles an already visible menu.
    if(Visible(panel)) return 0;
    auto result = g_Open(panel);
    if(Visible(panel)) {
        CacheMenu(panel);
        ApplyBuyStyles(panel);
        // Opening does not initialize the preview until a real mouse event.
        // Invalidate its weapon-only cache also when switching equal loadouts.
        Field<uint64_t>(panel, 528) = UINT64_MAX - 1;
        Model(panel, nullptr);
        ++g_OpenCount;
        advancedfx::Message("[mirv_pov_buymenu] opened for %s\n", g_Controller->GetPlayerName());
    }
    return result;
}
intptr_t __fastcall Close(void * panel) {
    Scope scope;
    // Native viewer-side zone/close events must not override the recorded key.
    // Reset clears wanted first and always runs the full native restoration.
    if(g_Active && g_Wanted) return 0;
    RestoreWashClasses();
    auto result = g_Close(panel);
    if(g_Active) {
        ++g_CloseCount;
        g_Panel = nullptr;
        g_MenuHandle = 0xFFFFFFFF00000000ULL;
        advancedfx::Message("[mirv_pov_buymenu] closed\n");
    }
    return result;
}
intptr_t __fastcall Refresh(void * panel) {
    Scope scope;
    auto result = g_Refresh(panel);
    ApplyBuyStyles(panel);
    return result;
}
intptr_t __fastcall FullRefresh(void * panel) {
    Scope scope;
    // A viewer's modifier keys must not turn owned items into donation offers.
    if(g_Active) Field<uint8_t>(panel, 568) = 0;
    auto result = g_FullRefresh(panel);
    ApplyBuyStyles(panel);
    return result;
}
intptr_t __fastcall Hover(void * panel) { Scope scope; return g_Hover(panel); }
intptr_t __fastcall Think(void * panel) {
    Scope scope;
    if(scope.entered && (!g_Pawn || !g_Controller)) {
        Close(panel);
        return 0;
    }
    return g_Think(panel);
}

bool MatchesBuild(HMODULE module) {
    wchar_t path[MAX_PATH];
    if(!GetModuleFileNameW(module, path, MAX_PATH)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if(ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    unsigned char buffer[65536], digest[32];
    DWORD size = 0;
    while(ok) {
        if(!ReadFile(file, buffer, sizeof(buffer), &size, nullptr)) { ok = false; break; }
        if(!size) break;
        ok = BCryptHashData(hash, buffer, size, 0) >= 0;
    }
    if(ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0
        && 0 == memcmp(digest, kClientHash, sizeof(digest));
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}
}

void MirvPovBuyMenu_Initialize(HMODULE module) {
    if(g_Hooked) return;
    if(!module || !MatchesBuild(module) || g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen != 0x15EA) {
        advancedfx::Warning("[mirv_pov_buymenu] unsupported client build/schema; simulation unavailable.\n");
        return;
    }
    g_Base = reinterpret_cast<uintptr_t>(module);
    reinterpret_cast<void (__fastcall *)(uint16_t *, const char *)>(g_Base + 0x177E630)(
        &g_HudWashClass, "hud-colorize-wash");
    g_Open = reinterpret_cast<PanelFn>(g_Base + 0xDB4AA0);
    g_Close = reinterpret_cast<PanelFn>(g_Base + 0xD9DC50);
    g_Refresh = reinterpret_cast<PanelFn>(g_Base + 0xDB8580);
    g_FullRefresh = reinterpret_cast<PanelFn>(g_Base + 0xDB8620);
    g_Hover = reinterpret_cast<PanelFn>(g_Base + 0xDBFB20);
    g_Think = reinterpret_cast<PanelFn>(g_Base + 0xDA6300);
    g_GetPawn = reinterpret_cast<GetterFn>(g_Base + 0x9698F0);
    g_GetController = reinterpret_cast<GetterFn>(g_Base + 0x9698B0);
    g_Loadout = reinterpret_cast<LoadoutFn>(g_Base + 0x903150);
    g_LoadoutHover = reinterpret_cast<LoadoutFn>(g_Base + 0x9030B0);
    g_Write = reinterpret_cast<WriteFn>(g_Base + 0xC93350);
    g_Purchase = reinterpret_cast<PurchaseFn>(g_Base + 0xDA71E0);
    g_Sell = reinterpret_cast<SellFn>(g_Base + 0xDA7520);
    g_Model = reinterpret_cast<ModelFn>(g_Base + 0xDBE3D0);
    g_Select = reinterpret_cast<SelectFn>(g_Base + 0xDB4150);
    g_SetModel = reinterpret_cast<SetModelFn>(g_Base + 0xE59BD0);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_Open, Open);
    DetourAttach(&(PVOID &)g_Close, Close);
    DetourAttach(&(PVOID &)g_Refresh, Refresh);
    DetourAttach(&(PVOID &)g_FullRefresh, FullRefresh);
    DetourAttach(&(PVOID &)g_Hover, Hover);
    DetourAttach(&(PVOID &)g_Think, Think);
    DetourAttach(&(PVOID &)g_GetPawn, GetPawn);
    DetourAttach(&(PVOID &)g_GetController, GetController);
    DetourAttach(&(PVOID &)g_Loadout, Loadout);
    DetourAttach(&(PVOID &)g_LoadoutHover, LoadoutHover);
    DetourAttach(&(PVOID &)g_Write, Write);
    DetourAttach(&(PVOID &)g_Purchase, Purchase);
    DetourAttach(&(PVOID &)g_Sell, Sell);
    DetourAttach(&(PVOID &)g_Model, Model);
    DetourAttach(&(PVOID &)g_Select, Select);
    DetourAttach(&(PVOID &)g_SetModel, SetModel);
    g_Hooked = NO_ERROR == DetourTransactionCommit();
    if(g_Hooked) advancedfx::Message("[mirv_pov_buymenu] native simulation hooks installed.\n");
    else advancedfx::Warning("[mirv_pov_buymenu] detour transaction failed.\n");
}

void MirvPovBuyMenu_Reset() {
    g_Wanted = false;
    // Close synchronously before releasing the native panel/target. This also
    // restores the TeamCounter parent and mouse focus before a level unload.
    if(auto panel = ResolveMenu()) Close(panel);
    RestoreWashClasses();
    g_CancelledPending = g_CancelledPending || g_Pending;
    g_Active = g_Pending = false;
    g_Panel = nullptr;
    g_MenuHandle = 0xFFFFFFFF00000000ULL;
    g_Pawn = g_Controller = nullptr;
    g_PawnHandle = g_ControllerHandle = 0xFFFFFFFF;
    g_LastTick = -1;
}

void MirvPovBuyMenu_SetEnabled(bool enabled) {
    g_Enabled = enabled;
    if(!enabled) MirvPovBuyMenu_Reset();
}

bool MirvPovBuyMenu_IsEnabled() {
    return g_Enabled;
}

void MirvPovBuyMenu_Update() {
    if(!g_Hooked) return;
    if(!g_Enabled || !MIRV_POV_FEATURE_ACTIVE("buymenu") || !g_pEngineToClient || !g_pEngineToClient->GetDemoFile()) {
        MirvPovBuyMenu_Reset();
        return;
    }
    // Resolve outside the native scope: the real controller getter must retain
    // its type and identity for the core's observer-target lookup.
    auto pawn = GetCurrentPovPlayerPawn();
    auto controller = GetCurrentPovPlayerController();
    const int tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
    if(pawn != g_Pawn || controller != g_Controller
        || (g_LastTick >= 0 && (tick < g_LastTick || tick - g_LastTick > 256))) MirvPovBuyMenu_Reset();
    const bool tickChanged = tick != g_LastTick;
    g_LastTick = tick;
    g_Pawn = pawn;
    g_Controller = controller;
    g_PawnHandle = pawn ? static_cast<uint32_t>(pawn->GetHandle().ToInt()) : 0xFFFFFFFF;
    g_ControllerHandle = controller ? static_cast<uint32_t>(controller->GetHandle().ToInt()) : 0xFFFFFFFF;
    const bool wanted = pawn && controller && pawn->GetHealth() > 0
        && Field<uint8_t>(pawn, g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen) != 0;
    if(!wanted) { MirvPovBuyMenu_Reset(); return; }
    g_Wanted = g_Active = true;
    g_Panel = ResolveMenu();
    if(g_Pending && GetTickCount64() - g_PendingSince > 1000) g_Pending = false;
    if(g_Panel && tickChanged) {
        Scope scope;
        g_Refresh(g_Panel);
        ApplyBuyStyles(g_Panel);
    } else if(!g_Panel && !g_Pending) {
        g_Pending = true;
        g_PendingSince = GetTickCount64();
        if(!Dispatch(0xDAF500)) g_Pending = false;
    }
}

void MirvPovBuyMenu_PrintStatus() {
    g_Panel = ResolveMenu();
    advancedfx::Message("[mirv_pov_buymenu] enabled=%d\n", g_Enabled);
    advancedfx::Message("[mirv_pov_buymenu] hooks=%d active=%d wanted=%d pending=%d visible=%d opens=%u closes=%u loadout_hits=%u loadout_misses=%u\n",
        g_Hooked, g_Active, g_Wanted, g_Pending, g_Panel && Visible(g_Panel),
        g_OpenCount, g_CloseCount, g_LoadoutHits, g_LoadoutMisses);
    if(!g_Hooked || !g_pEngineToClient || !g_pEngineToClient->GetDemoFile()) return;
    // Read-only scan helps locate recorded open/close transitions in a demo.
    for(int i = 1; i <= GetHighestEntityIndex(); ++i) {
        auto controller = GetEntityFromIndex(i);
        if(!controller || !controller->IsPlayerController()) continue;
        auto handle = controller->GetPlayerPawnHandle();
        auto pawn = handle.IsValid() ? GetEntityFromIndex(handle.GetEntryIndex()) : nullptr;
        if(!pawn || pawn->GetHandle() != handle || !pawn->IsPlayerPawn()) continue;
        auto inventory = Field<void *>(controller, 0x820);
        advancedfx::Message("[mirv_pov_buymenu] player=%d name=%s open=%d team=%d loadout_count=%d\n",
            i, controller->GetPlayerName(), Field<uint8_t>(pawn, 0x15EA) != 0, pawn->GetTeam(),
            inventory ? Field<int>(inventory, 0x88) : -1);
    }
    if(g_Panel && g_Pawn && g_Controller) {
        Scope scope;
        if(!g_Pawn || !g_Controller) return;
        auto preview = Field<void *>(g_Panel, 520);
        if(preview) {
            auto records = Field<unsigned char *>(preview, 2296);
            const int count = Field<int>(preview, 2288);
            advancedfx::Message("[mirv_pov_buymenu] preview=%p visible=%d item=%llu slots=%d\n",
                preview, Visible(preview), Field<uint64_t>(g_Panel, 528), count);
            if(records && count > 0) advancedfx::Message("[mirv_pov_buymenu] preview_model=%s entity=%u dirty=%d\n",
                Field<const char *>(records, 24) ? Field<const char *>(records, 24) : "", Field<uint32_t>(records, 0), Field<uint8_t>(preview, 2240));
        } else advancedfx::Message("[mirv_pov_buymenu] preview=null\n");
        for(int group = 0; group < 5; ++group) {
            const int count = Field<int>(g_Panel, 128 + 56 * group);
            auto records = Field<unsigned char *>(g_Panel, 136 + 56 * group);
            if(!records || count < 0 || count > 64) continue;
            for(int i = 0; i < count; ++i) {
                auto record = records + 88 * i;
                auto item = Field<void *>(record, 72);
                if(!item) {
                    advancedfx::Message("[mirv_pov_buymenu] slot=%d empty\n", Field<int>(record, 64));
                    continue;
                }
                const int result = reinterpret_cast<int (__fastcall *)(void *, void *, int, int *)>(
                    g_Base + 0x8BFCA0)(Field<void *>(g_Pawn, 0x12F8), item, 1, nullptr);
                auto owned = reinterpret_cast<void * (__fastcall *)(void *, void *)>(g_Base + 0x8FE030)(
                    Field<void *>(g_Pawn, 0x12F0), item);
                advancedfx::Message("[mirv_pov_buymenu] slot=%d definition=%u price=%d acquire=%d owned_weapon=%d\n",
                    Field<int>(record, 64), Field<uint16_t>(item, 442), Field<int>(record, 80), result, owned != nullptr);
            }
        }
    }
}
