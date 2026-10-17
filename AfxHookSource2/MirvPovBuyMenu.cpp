#include "stdafx.h"
#include "MirvPovBuyMenu.h"
#include "MirvPovCore.h"
#include "MirvPanorama.h"
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
    0x65,0x99,0x40,0xc1,0xdb,0x36,0x2e,0xd9,0x81,0xd1,0xdf,0x10,0x63,0x7c,0xec,0x8c,
    0x2a,0x3d,0x99,0x40,0x9b,0x77,0xfc,0x3b,0x73,0xe4,0x35,0xe4,0x64,0x9e,0x36,0xb0
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
using KeyInputFn = intptr_t (__fastcall *)(void *, void *);
using InputFocusFn = intptr_t (__fastcall *)(void *, void *, bool, bool);
using UpdatePreviewFn = void (__fastcall *)(void *, int);
using CopyItemFn = void * (__fastcall *)(void *, void *);
using ItemPredicateFn = bool (__fastcall *)(void *);
using EqualItemsFn = bool (__fastcall *)(void *, void *);
using ClearItemFn = void (__fastcall *)(void *);
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
KeyInputFn g_KeyInput;
InputFocusFn g_InputFocus;
UpdatePreviewFn g_UpdatePreview;
CopyItemFn g_CopyItem;
ItemPredicateFn g_IsGloves;
EqualItemsFn g_EqualItems;
ClearItemFn g_ClearItem;
bool g_GlovesSupported = false;
struct GloveState {
    uint32_t preview = 0xFFFFFFFF, source = 0xFFFFFFFF;
    uint8_t revision = 0;
    bool copied = false;
} g_GloveState;
unsigned int g_GloveCopies = 0, g_GloveClears = 0;
bool g_InputSupported = false;
void * g_OpeningRoot = nullptr;
thread_local void * g_ModelPreview = nullptr;
unsigned int g_OpenCount = 0, g_CloseCount = 0, g_LoadoutHits = 0, g_LoadoutMisses = 0;
uint16_t g_HudWashClass = 0xFFFF;
std::vector<uint64_t> g_SuppressedWash;
bool g_PromoHidden = false;
bool g_LayoutShifted = false;
bool g_PromoSetterCalled = false, g_LayoutSetterCalled = false;

void * FindPanelByClass(void * panel, const char * className, int depth = 0) {
    if(!panel || !className || depth > 64) return nullptr;
    if(Panorama_HasPanelClass(panel, className)) return panel;

    auto bytes = static_cast<unsigned char *>(panel);
    auto children = bytes + CS2::PanoramaUIPanel::children;
    const int count = *reinterpret_cast<int *>(children);
    auto entries = *reinterpret_cast<unsigned char ***>(children + sizeof(void *));
    if(!entries || count < 0 || count > 1024) return nullptr;

    for(int i = 0; i < count; ++i) {
        auto child = entries[i];
        if(child && Panorama_HasPanelClass(child, className)) return child;
    }
    for(int i = 0; i < count; ++i) {
        auto child = entries[i];
        if(!child) continue;
        const auto flags = Field<unsigned char>(child, CS2::PanoramaUIPanel::panelFlags);
        if(0 == (flags & CS2::PanoramaUIPanel::k_EPanelFlag_HasOwnLayoutFile))
            if(auto found = FindPanelByClass(child, className, depth + 1)) return found;
    }
    return nullptr;
}

// Native Panorama weak handles keep restoration safe across layout teardown.
void * ResolveUi(uint64_t handle) {
    if(!g_Base || handle == 0xFFFFFFFF00000000ULL) return nullptr;
    auto engine = *reinterpret_cast<void **>(g_Base + 0x2733F40);
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
void ApplyPromoVisibility(void * menu) {
    if(!g_Active || !menu) return;
    const bool hide = MIRV_POV_FEATURE_ACTIVE("buymenu_promo_hide");
    auto root = Field<void *>(menu, 8);
    auto promo = root ? MirvPanorama_FindChildInLayoutFile(root, "CategoryContainerPromo") : nullptr;
    g_PromoSetterCalled = promo && Panorama_SetPanelVisible(promo, !hide);
    if(g_PromoSetterCalled) g_PromoHidden = hide;
}
void RestorePromoVisibility(void * menu) {
    if(!g_PromoHidden) return;
    auto root = menu ? Field<void *>(menu, 8) : nullptr;
    auto promo = root ? MirvPanorama_FindChildInLayoutFile(root, "CategoryContainerPromo") : nullptr;
    if(promo) Panorama_SetPanelVisible(promo, true);
    g_PromoHidden = false;
}
void ApplyLayoutOffset(void * menu) {
    if(!g_Active || !menu) return;
    const bool shift = MIRV_POV_FEATURE_ACTIVE("buymenu_layout");
    auto root = Field<void *>(menu, 8);
    auto left = root ? FindPanelByClass(root, "buymenu-left") : nullptr;
    // Existing workaround, not a verified native anchor. Retain it unchanged
    // while the diagnostic snapshots establish what happens during mouseover.
    g_LayoutSetterCalled = left && Panorama_SetPanelX(left, shift ? -116.0f : 0.0f);
    if(g_LayoutSetterCalled) g_LayoutShifted = shift;
}
void RestoreLayoutOffset(void * menu) {
    if(!g_LayoutShifted) return;
    auto root = menu ? Field<void *>(menu, 8) : nullptr;
    auto left = root ? FindPanelByClass(root, "buymenu-left") : nullptr;
    if(left) Panorama_SetPanelX(left, 0.0f);
    g_LayoutShifted = false;
}
void ApplyBuyStyles(void * menu) {
    if(!g_Active || !menu) return;
    ApplyPromoVisibility(menu);
    ApplyLayoutOffset(menu);
    if(g_HudWashClass == 0xFFFF) return;
    // Spectator HUD color selectors have greater specificity than the native
    // buywheel-cant-buy selector. Exclude only disabled item content from that
    // tint; let buymenu.css provide its original icon/name/price colors.
    const auto cantBuy = *reinterpret_cast<uint16_t *>(g_Base + 0x25C8838);
    const auto cantAfford = *reinterpret_cast<uint16_t *>(g_Base + 0x25C8834);
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
    return rva < 0x299D000;
}
bool Visible(void * menu) {
    if(!menu) return false;
    void * panel = Field<void *>(menu, 8);
    if(!panel) return false;
    return reinterpret_cast<bool (__fastcall *)(void *)>(Field<void **>(panel, 0)[34])(panel);
}
void * ResolveMenu() {
    if(!g_Base || g_MenuHandle == 0xFFFFFFFF00000000ULL) return nullptr;
    void * engine = *reinterpret_cast<void **>(g_Base + 0x2733F40);
    if(!engine) return nullptr;
    auto panel = reinterpret_cast<void * (__fastcall *)(void *, uint64_t *)>(
        Field<void **>(engine, 0)[34])(engine, &g_MenuHandle);
    if(!panel) return nullptr;
    return reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(panel, 0)[8])(panel);
}
void CacheMenu(void * menu) {
    void * engine = *reinterpret_cast<void **>(g_Base + 0x2733F40);
    auto panel = reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(menu, 0)[0])(menu);
    uint64_t handle = 0xFFFFFFFF00000000ULL;
    g_MenuHandle = *reinterpret_cast<uint64_t * (__fastcall *)(void *, uint64_t *, void *)>(
        Field<void **>(engine, 0)[33])(engine, &handle, panel);
    g_Panel = menu;
}

// Diagnostic snapshots contain values only: never dereference saved panel
// pointers when printing after the menu has closed or its layout was rebuilt.
bool MatchesBuild(HMODULE module, const unsigned char * expected);
struct PanelSnapshot {
    void * address = nullptr;
    void * parent = nullptr;
    bool supported = false, visible = false, hiddenClass = false;
    float layout[11] = {};
};
struct BuySnapshot {
    const char * phase = "";
    ULONGLONG time = 0;
    int tick = -1, hovered = -1, promoEnabled = -1, promoSlot = -1;
    void * item = nullptr;
    int definition = -1, itemValid = -1;
    bool hideRequested = false, shiftRequested = false;
    bool promoSetter = false, layoutSetter = false;
    PanelSnapshot panels[7];
};
constexpr size_t kSnapshotCount = 12;
BuySnapshot g_Snapshots[kSnapshotCount];
size_t g_SnapshotNext = 0, g_SnapshotSize = 0;
unsigned int g_SettleFrames = 0;
int g_SampledHover = INT_MIN;

bool DiagnosticPanoramaSupported() {
    static HMODULE checked = nullptr;
    static bool supported = false;
    auto module = GetModuleHandleW(L"panorama.dll");
    if(module && module != checked) {
        constexpr unsigned char expected[] = {
            0xb5,0x02,0x8e,0x90,0x77,0xf0,0x17,0x6c,0xd3,0x38,0xe6,0xcd,0xac,0x92,0xb8,0xd5,
            0x02,0x45,0x19,0xe6,0x83,0x3b,0xd0,0xe9,0x79,0xa2,0xf1,0x06,0x2c,0xce,0xf4,0x12
        };
        supported = MatchesBuild(module, expected);
        checked = module;
    }
    return module && supported;
}

// Input ABI is checked independently of the client build. Only the replayed
// menu owns these changes; console/demo UI focus must always pass through.
struct BuyInputState {
    uint64_t root = 0xFFFFFFFF00000000ULL;
    bool hitTest = false, hitTestChildren = false;
    bool acceptsFocus = false, focusContext = false;
} g_InputState;

bool InputPassthrough() {
    return g_InputSupported && g_Active && g_Wanted
        && MIRV_POV_FEATURE_ACTIVE("buymenu_input_passthrough");
}
bool InputPanelSupported(void * ui) {
    if(!ui || !g_InputSupported) return false;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"panorama.dll"));
    auto table = Field<uintptr_t *>(ui, 0);
    return table[31] == base + 0x112B60 && table[32] == base + 0x112B30
        && table[165] == base + 0x1125A0 && table[166] == base + 0x104440
        && table[178] == base + 0x112510 && table[179] == base + 0x1117C0
        && table[203] == base + 0x1126B0 && table[204] == base + 0x112570
        && table[206] == base + 0x112630 && table[207] == base + 0x112560;
}
bool InputFlag(void * ui, size_t slot) {
    return reinterpret_cast<bool (__fastcall *)(void *)>(Field<void **>(ui, 0)[slot])(ui);
}
void SetInputFlag(void * ui, size_t slot, bool value) {
    reinterpret_cast<void (__fastcall *)(void *, bool)>(Field<void **>(ui, 0)[slot])(ui, value);
}
void * InputRoot() {
    if(g_OpeningRoot) return g_OpeningRoot;
    return ResolveUi(g_MenuHandle);
}
bool WithinInputRoot(void * ui, void * root) {
    if(!root) return false;
    for(int depth = 0; ui && depth < 128; ++depth) {
        if(ui == root) return true;
        if(!InputPanelSupported(ui)) return false;
        ui = reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(ui, 0)[31])(ui);
    }
    return false;
}
void * MenuInput(void * root) {
    if(!InputPanelSupported(root)) return nullptr;
    auto window = reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(root, 0)[32])(root);
    if(!window) return nullptr;
    auto input = reinterpret_cast<void * (__fastcall *)(void *)>(Field<void **>(window, 0)[4])(window);
    if(!input) return nullptr;
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"panorama.dll"));
    auto table = Field<uintptr_t *>(input, 0);
    return table[19] == base + 0x13A4B0 && table[22] == base + 0x139B90 ? input : nullptr;
}
void ReleaseMenuFocus(void * root) {
    if(auto input = MenuInput(root)) {
        // +32 is the actual focused panel's weak handle, +24 its context.
        // Clear only focus still owned by this menu, never another overlay.
        auto focused = ResolveUi(Field<uint64_t>(input, 32));
        auto context = ResolveUi(Field<uint64_t>(input, 24));
        if(WithinInputRoot(focused, root) || WithinInputRoot(context, root))
            g_InputFocus(input, nullptr, false, false);
    }
}
void RestoreInputState() {
    if(auto root = ResolveUi(g_InputState.root)) {
        if(InputPanelSupported(root)) {
            SetInputFlag(root, 203, g_InputState.hitTest);
            SetInputFlag(root, 206, g_InputState.hitTestChildren);
            SetInputFlag(root, 166, g_InputState.acceptsFocus);
            SetInputFlag(root, 179, g_InputState.focusContext);
        }
    }
    g_InputState = BuyInputState{};
}
void ApplyInputState(void * menu) {
    if(!InputPassthrough()) { RestoreInputState(); return; }
    auto root = menu ? Field<void *>(menu, 8) : nullptr;
    if(!InputPanelSupported(root)) return;
    // Native Open calls FullRefresh before CacheMenu. Do not change flags until
    // we can save and restore this exact root through its weak handle.
    if(g_MenuHandle == 0xFFFFFFFF00000000ULL || ResolveUi(g_MenuHandle) != root) return;
    if(g_InputState.root != g_MenuHandle) RestoreInputState();
    if(g_InputState.root == 0xFFFFFFFF00000000ULL) {
        g_InputState.root = g_MenuHandle;
        g_InputState.hitTest = InputFlag(root, 204);
        g_InputState.hitTestChildren = InputFlag(root, 207);
        g_InputState.acceptsFocus = InputFlag(root, 165);
        g_InputState.focusContext = InputFlag(root, 178);
    }
    if(InputFlag(root, 204)) SetInputFlag(root, 203, false);
    if(InputFlag(root, 207)) SetInputFlag(root, 206, false);
    if(InputFlag(root, 165)) SetInputFlag(root, 166, false);
    if(InputFlag(root, 178)) SetInputFlag(root, 179, false);
    ReleaseMenuFocus(root);
}
intptr_t __fastcall InputFocus(void * input, void * ui, bool scroll, bool context) {
    if(InputPassthrough() && WithinInputRoot(ui, InputRoot())) return 0;
    return g_InputFocus(input, ui, scroll, context);
}
intptr_t __fastcall KeyInput(void * handler, void * event) {
    auto root = InputPassthrough() ? InputRoot() : nullptr;
    if(root && Field<void *>(static_cast<unsigned char *>(handler) - 32, 8) == root) return 0;
    return g_KeyInput(handler, event);
}
struct OpeningInputScope {
    void * previous;
    OpeningInputScope(void * panel) : previous(g_OpeningRoot) { g_OpeningRoot = Field<void *>(panel, 8); }
    ~OpeningInputScope() { g_OpeningRoot = previous; }
};

PanelSnapshot SnapshotPanel(void * ui) {
    PanelSnapshot result;
    result.address = ui;
    if(!ui || !DiagnosticPanoramaSupported()) return result;
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"panorama.dll"));
    auto table = Field<uintptr_t *>(ui, 0);
    // Also reject an unexpected dynamic panel type before reading private data.
    if(table[100] != base + 0x111AD0 || table[103] != base + 0x111A90
        || table[115] != base + 0x112860 || table[117] != base + 0x111970) return result;
    result.supported = true;
    result.parent = Field<void *>(ui, 24);
    result.visible = reinterpret_cast<bool (__fastcall *)(void *)>(table[34])(ui);
    result.hiddenClass = Panorama_HasPanelClass(ui, "Hidden");
    // Keep native offset labels: these are not claimed to be screen pixels,
    // final transformed bounds, opacity, or a proof of rendered visibility.
    const size_t offsets[] = {0x1B0,0x1B4,0x1B8,0x1BC,0x1C0,0x1C4,0x1D0,0x1D4,0x1F0,0x1F4,0x1F8};
    for(size_t i = 0; i < 11; ++i) result.layout[i] = Field<float>(ui, offsets[i]);
    return result;
}
void CaptureBuySnapshot(void * menu, const char * phase) {
    if(!g_Hooked || !menu) return;
    BuySnapshot s;
    s.phase = phase;
    s.time = GetTickCount64();
    s.tick = g_LastTick;
    s.hovered = Field<int>(menu, 664);
    auto promoted = *reinterpret_cast<void **>(g_Base + 0x252DE80);
    if(promoted) s.promoEnabled = Field<uint8_t>(promoted, 88);
    s.promoSlot = Field<int>(menu, 640);
    s.item = Field<void *>(menu, 648);
    if(s.item) {
        s.definition = Field<uint16_t>(s.item, 442);
        s.itemValid = Field<uint8_t>(s.item, 488);
    }
    s.hideRequested = MIRV_POV_FEATURE_ACTIVE("buymenu_promo_hide");
    s.shiftRequested = MIRV_POV_FEATURE_ACTIVE("buymenu_layout");
    s.promoSetter = g_PromoSetterCalled;
    s.layoutSetter = g_LayoutSetterCalled;
    auto root = Field<void *>(menu, 8);
    s.panels[0] = SnapshotPanel(root);
    if(root && DiagnosticPanoramaSupported()) {
        s.panels[1] = SnapshotPanel(FindPanelByClass(root, "buymenu-left"));
        s.panels[2] = SnapshotPanel(FindPanelByClass(root, "buymenu-right"));
        s.panels[3] = SnapshotPanel(MirvPanorama_FindChildInLayoutFile(root, "CategoryContainerPromo"));
        s.panels[4] = SnapshotPanel(ResolveUi(Field<uint64_t>(menu, 576)));
        auto desc = Field<void *>(menu, 440);
        auto preview = Field<void *>(menu, 520);
        s.panels[5] = SnapshotPanel(desc ? Field<void *>(desc, 8) : nullptr);
        s.panels[6] = SnapshotPanel(preview ? Field<void *>(preview, 8) : nullptr);
    }
    g_Snapshots[g_SnapshotNext] = s;
    g_SnapshotNext = (g_SnapshotNext + 1) % kSnapshotCount;
    if(g_SnapshotSize < kSnapshotCount) ++g_SnapshotSize;
}
void PrintBuySnapshots() {
    advancedfx::Message("[buymenu_diag] version=20260927-2 panorama_supported=%d samples=%zu setter_flags=last_apply_not_readback\n",
        DiagnosticPanoramaSupported(), g_SnapshotSize);
    const char * names[] = {"root", "left", "right", "promo_container", "promo_item", "ItemDesc", "preview"};
    for(size_t i = 0; i < g_SnapshotSize; ++i) {
        const auto & s = g_Snapshots[(g_SnapshotNext + kSnapshotCount - g_SnapshotSize + i) % kSnapshotCount];
        advancedfx::Message("[buymenu_diag] sample=%zu phase=%s ms=%llu tick=%d hovered=%d promoted_enabled=%d promo_slot=%d item=%p definition=%d valid=%d hide_requested=%d shift_requested=%d promo_setter=%d layout_setter=%d\n",
            i, s.phase, s.time, s.tick, s.hovered, s.promoEnabled, s.promoSlot, s.item, s.definition, s.itemValid,
            s.hideRequested, s.shiftRequested, s.promoSetter, s.layoutSetter);
        for(size_t j = 0; j < 7; ++j) {
            const auto & p = s.panels[j];
            advancedfx::Message("[buymenu_diag] %s=%p parent=%p readback=%d visible=%d Hidden=%d native_1b0=(%.2f,%.2f) native_1b8=(%.2f,%.2f) native_1c0=(%.2f,%.2f) native_1d0=(%.2f,%.2f) native_1f0=(%.2f,%.2f,%.2f)\n",
                names[j], p.address, p.parent, p.supported, p.visible, p.hiddenClass,
                p.layout[0], p.layout[1], p.layout[2], p.layout[3], p.layout[4], p.layout[5],
                p.layout[6], p.layout[7], p.layout[8], p.layout[9], p.layout[10]);
        }
    }
}
bool Dispatch(size_t creator) {
    void * engine = *reinterpret_cast<void **>(g_Base + 0x2733F40);
    if(!engine) return false;
    void * event = nullptr;
    reinterpret_cast<EventFn>(g_Base + creator)(&event, nullptr);
    if(!event) return false;
    reinterpret_cast<void (__fastcall *)(void *, void **)>(Field<void **>(engine, 0)[47])(engine, &event);
    return true;
}
CEntityInstance * __fastcall GetPawn(int slot) {
    if(g_Depth && InClient(_ReturnAddress()) && (slot == 0 || slot == -1)) return g_Pawn;
    // Preserve the native call site for other scoped consumers of this getter.
    void * previous = MirvPov_PushHookReturnAddress(_ReturnAddress());
    CEntityInstance * pawn = nullptr;
    __try {
        pawn = g_GetPawn(slot);
    } __finally {
        MirvPov_PopHookReturnAddress(previous);
    }
    return pawn;
}
CEntityInstance * __fastcall GetController(int slot) {
    if(g_Depth && InClient(_ReturnAddress()) && (slot == 0 || slot == -1)) return g_Controller;
    void * previous = MirvPov_PushHookReturnAddress(_ReturnAddress());
    CEntityInstance * controller = nullptr;
    __try {
        controller = g_GetController(slot);
    } __finally {
        MirvPov_PopHookReturnAddress(previous);
    }
    return controller;
}
void * ObservedLoadout(void * inventory, unsigned int team, unsigned int slot) {
    if(!g_Controller || inventory != Field<void *>(g_Controller, 0x820)) return nullptr;
    // ServerAuthoritativeWeaponSlot_t: 56-byte network records. These carry
    // the observed player's chosen weapon definitions, not the viewer's loadout.
    const int count = Field<int>(inventory, 0x88);
    auto records = Field<unsigned char *>(inventory, 0x90);
    auto manager = reinterpret_cast<void * (__fastcall *)()>(g_Base + 0x8396F0)();
    if(records && count > 0 && count <= 256) {
        for(int i = 0; i < count; ++i) {
            auto record = records + i * 56;
            if(Field<uint16_t>(record, 48) == team && Field<uint16_t>(record, 50) == slot) {
                auto item = reinterpret_cast<void * (__fastcall *)(void *, unsigned int, int, int)>(
                    g_Base + 0x11360B0)(manager, Field<uint16_t>(record, 52), 0, 0);
                if(item) { ++g_LoadoutHits; return item; }
            }
        }
    }
    // The server vector is sparse: absent entries use the shared default
    // definition table, exactly as the native loadout traversal does. The
    // direct team/slot lookup does not read the local equipped-item interface.
    {
        auto item = reinterpret_cast<LoadoutFn>(g_Base + 0x83C2A0)(manager, team, slot);
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

unsigned char * PreviewRecord(void * preview, int slot) {
    if(!preview || slot < 0 || slot >= Field<int>(preview, 2288)) return nullptr;
    auto records = Field<unsigned char *>(preview, 2296);
    return records ? records + 160 * slot : nullptr;
}
bool OwnsGlovePreview(void * preview) {
    if(!preview || !g_Hooked || !g_GlovesSupported || !g_Enabled || !g_Active || !g_Wanted
        || !MIRV_POV_FEATURE_ACTIVE("buymenu") || !g_pEngineToClient
        || !g_pEngineToClient->GetDemoFile()) return false;
    auto menu = ResolveMenu();
    return menu && Field<void *>(menu, 520) == preview;
}
void SyncPreviewGloves(void * preview, int slot, bool nativeUpdated) {
    if(!OwnsGlovePreview(preview) || !MIRV_POV_FEATURE_ACTIVE("buymenu_gloves")) return;
    auto record = PreviewRecord(preview, slot);
    // Native spawn completion clears +144. Never write into a pending entity,
    // the recorded pawn, or an entity that reused the preview's old index.
    if(!record || Field<uint8_t>(record, 144)) return;
    const auto handle = Field<uint32_t>(record, 0);
    if(handle == 0xFFFFFFFF) return;
    auto target = GetEntityFromIndex(handle & 0x7FFF);
    auto source = ResolveEntity(g_PawnHandle, true);
    if(!target || !source || target == source
        || static_cast<uint32_t>(target->GetHandle().ToInt()) != handle
        || 0 != strcmp(target->GetClassName(), "csgo_previewplayer")) return;

    const auto & offsets = g_clientDllOffsets.C_CSPlayerPawn;
    auto from = reinterpret_cast<unsigned char *>(source) + offsets.m_EconGloves;
    auto to = reinterpret_cast<unsigned char *>(target) + offsets.m_EconGloves;
    const auto definition = Field<uint16_t>(from, 442);
    const bool custom = Field<uint8_t>(from, 488) && definition && definition != 0xFFFF && g_IsGloves(from);
    const auto revision = Field<uint8_t>(source, offsets.m_nEconGlovesChanged);
    if(custom) {
        if(nativeUpdated || !g_GloveState.copied || g_GloveState.preview != handle
            || g_GloveState.source != g_PawnHandle || g_GloveState.revision != revision
            || !g_EqualItems(from, to)) {
            // Use the game's C_EconItemView assignment, including dynamic
            // paint/seed/wear attributes. A byte copy would alias owned buffers.
            g_CopyItem(to, from);
            Field<uint8_t>(target, offsets.m_bNeedToReApplyGloves) = 1;
            ++g_GloveCopies;
        }
    } else if(Field<uint8_t>(to, 488) || Field<uint16_t>(to, 442)) {
        // Missing/default recorded gloves must not inherit the viewer's skin.
        g_ClearItem(to);
        Field<uint8_t>(target, offsets.m_bNeedToReApplyGloves) = 1;
        ++g_GloveClears;
    }
    g_GloveState.preview = handle;
    g_GloveState.source = g_PawnHandle;
    g_GloveState.revision = revision;
    g_GloveState.copied = custom;
}
void __fastcall UpdatePreview(void * preview, int slot) {
    // This update is deferred beyond the buy-menu callback's g_Depth scope.
    // Match the live menu's weak handle instead of extending that global scope.
    g_UpdatePreview(preview, slot);
    SyncPreviewGloves(preview, slot, true);
}
void RestorePreviewGloves(void * menu) {
    if(!g_GlovesSupported || !menu || g_GloveState.preview == 0xFFFFFFFF) return;
    auto preview = Field<void *>(menu, 520);
    auto record = preview ? PreviewRecord(preview, Field<int>(preview, 2284)) : nullptr;
    if(record && Field<uint32_t>(record, 0) == g_GloveState.preview)
        g_UpdatePreview(preview, Field<int>(preview, 2284));
    g_GloveState = GloveState{};
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
        reinterpret_cast<void * (__fastcall *)(void *, const char **)>(g_Base + 0x21C8C0)(g_Pawn, &model);
        if(!model || !*model) model = g_Pawn->GetTeam() == 3
            ? "agents/models/ctm_sas/ctm_sas.vmdl" : "agents/models/tm_phoenix/tm_phoenix.vmdl";
        // Native preview setup stores the viewer's agent item ID in this slot.
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
    if(scope.entered) CaptureBuySnapshot(panel, "select-before");
    auto result = g_Select(panel, slot);
    if(scope.entered) {
        CaptureBuySnapshot(panel, "select-after");
        g_SettleFrames = 2;
    }
    return result;
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
    OpeningInputScope inputScope(panel);
    auto result = g_Open(panel);
    if(Visible(panel)) {
        CacheMenu(panel);
        g_SnapshotNext = g_SnapshotSize = 0;
        g_PromoSetterCalled = g_LayoutSetterCalled = false;
        CaptureBuySnapshot(panel, "open-before-style");
        ApplyBuyStyles(panel);
        ApplyInputState(panel);
        // Opening does not initialize the preview until a real mouse event.
        // Invalidate its weapon-only cache also when switching equal loadouts.
        Field<uint64_t>(panel, 528) = UINT64_MAX - 1;
        Model(panel, nullptr);
        CaptureBuySnapshot(panel, "open-after-style");
        g_SettleFrames = 2;
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
    if(g_Active) RestorePreviewGloves(panel);
    RestoreInputState();
    RestoreWashClasses();
    RestorePromoVisibility(panel);
    RestoreLayoutOffset(panel);
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
    ApplyInputState(panel);
    return result;
}
intptr_t __fastcall FullRefresh(void * panel) {
    Scope scope;
    // A viewer's modifier keys must not turn owned items into donation offers.
    if(g_Active) Field<uint8_t>(panel, 568) = 0;
    auto result = g_FullRefresh(panel);
    ApplyBuyStyles(panel);
    ApplyInputState(panel);
    return result;
}
intptr_t __fastcall Hover(void * panel) {
    Scope scope;
    auto result = g_Hover(panel);
    ApplyBuyStyles(panel);
    ApplyInputState(panel);
    return result;
}
intptr_t __fastcall Think(void * panel) {
    Scope scope;
    if(scope.entered && (!g_Pawn || !g_Controller)) {
        Close(panel);
        return 0;
    }
    auto result = g_Think(panel);
    ApplyBuyStyles(panel);
    ApplyInputState(panel);
    return result;
}

bool MatchesBuild(HMODULE module, const unsigned char * expected) {
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
        && 0 == memcmp(digest, expected, sizeof(digest));
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}
}

void MirvPovBuyMenu_Initialize(HMODULE module) {
    if(g_Hooked) return;
    if(!module || !MatchesBuild(module, kClientHash) || g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen != 0x15EA) {
        advancedfx::Warning("[mirv_pov_buymenu] unsupported client build/schema; simulation unavailable.\n");
        return;
    }
    g_Base = reinterpret_cast<uintptr_t>(module);
    reinterpret_cast<void (__fastcall *)(uint16_t *, const char *)>(g_Base + 0x17858C0)(
        &g_HudWashClass, "hud-colorize-wash");
    g_Open = reinterpret_cast<PanelFn>(g_Base + 0xDB7E90);
    g_Close = reinterpret_cast<PanelFn>(g_Base + 0xDA1100);
    g_Refresh = reinterpret_cast<PanelFn>(g_Base + 0xDBB1E0);
    g_FullRefresh = reinterpret_cast<PanelFn>(g_Base + 0xDBB280);
    g_Hover = reinterpret_cast<PanelFn>(g_Base + 0xDC2530);
    g_Think = reinterpret_cast<PanelFn>(g_Base + 0xDA9450);
    g_GetPawn = reinterpret_cast<GetterFn>(g_Base + 0x96A8F0);
    g_GetController = reinterpret_cast<GetterFn>(g_Base + 0x96A8B0);
    g_Loadout = reinterpret_cast<LoadoutFn>(g_Base + 0x904140);
    g_LoadoutHover = reinterpret_cast<LoadoutFn>(g_Base + 0x9040A0);
    g_Write = reinterpret_cast<WriteFn>(g_Base + 0xC949F0);
    g_Purchase = reinterpret_cast<PurchaseFn>(g_Base + 0xDAA360);
    g_Sell = reinterpret_cast<SellFn>(g_Base + 0xDAA6A0);
    g_Model = reinterpret_cast<ModelFn>(g_Base + 0xDC0D40);
    g_Select = reinterpret_cast<SelectFn>(g_Base + 0xDB7730);
    g_SetModel = reinterpret_cast<SetModelFn>(g_Base + 0xE5EDF0);
    // Optional independently gated effect; unknown glove schema leaves the
    // existing buy-menu simulation available with native preview cosmetics.
    const auto & gloveOffsets = g_clientDllOffsets.C_CSPlayerPawn;
    g_GlovesSupported = gloveOffsets.m_EconGloves == 0x1770
        && gloveOffsets.m_bNeedToReApplyGloves == 0x176D
        && gloveOffsets.m_nEconGlovesChanged == 0x1D20;
    if(g_GlovesSupported) {
        g_UpdatePreview = reinterpret_cast<UpdatePreviewFn>(g_Base + 0xE68560);
        g_CopyItem = reinterpret_cast<CopyItemFn>(g_Base + 0x1184AD0);
        g_EqualItems = reinterpret_cast<EqualItemsFn>(g_Base + 0x1185620);
        g_ClearItem = reinterpret_cast<ClearItemFn>(g_Base + 0x119B0B0);
        g_IsGloves = reinterpret_cast<ItemPredicateFn>(g_Base + 0x119B150);
    }
    g_InputSupported = DiagnosticPanoramaSupported();
    g_KeyInput = reinterpret_cast<KeyInputFn>(g_Base + 0xDADA00);
    if(g_InputSupported) g_InputFocus = reinterpret_cast<InputFocusFn>(
        reinterpret_cast<uintptr_t>(GetModuleHandleW(L"panorama.dll")) + 0x13A4B0);
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
    if(g_GlovesSupported) DetourAttach(&(PVOID &)g_UpdatePreview, UpdatePreview);
    if(g_InputSupported) {
        DetourAttach(&(PVOID &)g_KeyInput, KeyInput);
        DetourAttach(&(PVOID &)g_InputFocus, InputFocus);
    }
    g_Hooked = NO_ERROR == DetourTransactionCommit();
    if(g_Hooked) advancedfx::Message("[mirv_pov_buymenu] native simulation hooks installed.\n");
    else advancedfx::Warning("[mirv_pov_buymenu] detour transaction failed.\n");
    if(!g_InputSupported) advancedfx::Warning("[mirv_pov_buymenu] unsupported panorama build; input passthrough unavailable.\n");
    if(!g_GlovesSupported) advancedfx::Warning("[mirv_pov_buymenu] unsupported glove schema; observed preview gloves unavailable.\n");
}

void MirvPovBuyMenu_Reset() {
    g_Wanted = false;
    // Close synchronously before releasing the native panel/target. This also
    // restores the TeamCounter parent and mouse focus before a level unload.
    if(auto panel = ResolveMenu()) Close(panel);
    RestoreInputState();
    RestoreWashClasses();
    g_PromoHidden = false;
    g_LayoutShifted = false;
    g_CancelledPending = g_CancelledPending || g_Pending;
    g_Active = g_Pending = false;
    g_Panel = nullptr;
    g_MenuHandle = 0xFFFFFFFF00000000ULL;
    g_Pawn = g_Controller = nullptr;
    g_PawnHandle = g_ControllerHandle = 0xFFFFFFFF;
    g_LastTick = -1;
    g_SampledHover = INT_MIN;
    g_SettleFrames = 0;
    g_GloveState = GloveState{};
}

void MirvPovBuyMenu_SetEnabled(bool enabled) {
    g_Enabled = enabled;
    if(!enabled) MirvPovBuyMenu_Reset();
}

bool MirvPovBuyMenu_IsEnabled() {
    return g_Enabled;
}

void MirvPovBuyMenu_ReapplyInput() {
    if(auto panel = ResolveMenu()) ApplyInputState(panel);
    else RestoreInputState();
}

void MirvPovBuyMenu_ReapplyGloves() {
    g_GloveState = GloveState{};
    auto menu = ResolveMenu();
    auto preview = menu ? Field<void *>(menu, 520) : nullptr;
    if(!OwnsGlovePreview(preview)) return;
    const int slot = Field<int>(preview, 2284);
    if(!PreviewRecord(preview, slot)) return;
    // Off runs the original equipment update to restore native cosmetics;
    // on runs that same update and then copies the observed player's gloves.
    UpdatePreview(preview, slot);
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
    if(g_Panel) {
        const int hover = Field<int>(g_Panel, 664);
        if(hover != g_SampledHover) {
            CaptureBuySnapshot(g_Panel, "hover-state-change");
            g_SampledHover = hover;
            g_SettleFrames = 2;
        } else if(g_SettleFrames) {
            CaptureBuySnapshot(g_Panel, "next-frame");
            --g_SettleFrames;
        }
        ApplyPromoVisibility(g_Panel);
        ApplyLayoutOffset(g_Panel);
        ApplyInputState(g_Panel); // Also runs while the demo tick is paused.
    }
    if(g_Pending && GetTickCount64() - g_PendingSince > 1000) g_Pending = false;
    if(g_Panel && tickChanged) {
        Scope scope;
        g_Refresh(g_Panel);
        ApplyBuyStyles(g_Panel);
    } else if(!g_Panel && !g_Pending) {
        g_Pending = true;
        g_PendingSince = GetTickCount64();
        if(!Dispatch(0xDB2AC0)) g_Pending = false;
    }
    if(auto menu = ResolveMenu()) {
        auto preview = Field<void *>(menu, 520);
        if(preview) SyncPreviewGloves(preview, Field<int>(preview, 2284), false);
    }
}

void MirvPovBuyMenu_PrintStatus() {
    g_Panel = ResolveMenu();
    advancedfx::Message("[mirv_pov_buymenu] enabled=%d\n", g_Enabled);
    advancedfx::Message("[mirv_pov_buymenu] gloves_supported=%d gloves_enabled=%d glove_copies=%u glove_clears=%u\n",
        g_GlovesSupported, MIRV_POV_FEATURE_ACTIVE("buymenu_gloves"), g_GloveCopies, g_GloveClears);
    advancedfx::Message("[mirv_pov_buymenu] input_supported=%d input_passthrough=%d input_state_saved=%d\n",
        g_InputSupported, InputPassthrough(), g_InputState.root != 0xFFFFFFFF00000000ULL);
    advancedfx::Message("[mirv_pov_buymenu] hooks=%d active=%d wanted=%d pending=%d visible=%d opens=%u closes=%u loadout_hits=%u loadout_misses=%u\n",
        g_Hooked, g_Active, g_Wanted, g_Pending, g_Panel && Visible(g_Panel),
        g_OpenCount, g_CloseCount, g_LoadoutHits, g_LoadoutMisses);
    if(g_Panel) CaptureBuySnapshot(g_Panel, "status");
    PrintBuySnapshots();
    if(!g_Hooked || !g_pEngineToClient || !g_pEngineToClient->GetDemoFile()) return;
    auto currentPawn = GetCurrentPovPlayerPawn();
    auto currentController = GetCurrentPovPlayerController();
    advancedfx::Message(
        "[mirv_pov_buymenu] current_target pawn=%p controller=%p name=%s health=%d team=%d open=%d pawn_handle=%u controller_handle=%u\n",
        currentPawn,
        currentController,
        currentController ? currentController->GetPlayerName() : "<none>",
        currentPawn ? currentPawn->GetHealth() : -1,
        currentPawn ? currentPawn->GetTeam() : -1,
        currentPawn ? Field<uint8_t>(currentPawn, g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen) != 0 : 0,
        currentPawn ? static_cast<uint32_t>(currentPawn->GetHandle().ToInt()) : 0xFFFFFFFF,
        currentController ? static_cast<uint32_t>(currentController->GetHandle().ToInt()) : 0xFFFFFFFF);
    if(g_GlovesSupported && currentPawn) {
        auto gloves = reinterpret_cast<unsigned char *>(currentPawn) + g_clientDllOffsets.C_CSPlayerPawn.m_EconGloves;
        advancedfx::Message("[mirv_pov_buymenu] glove_source_definition=%u initialized=%d revision=%u preview_handle=%u\n",
            Field<uint16_t>(gloves, 442), Field<uint8_t>(gloves, 488) != 0,
            Field<uint8_t>(currentPawn, g_clientDllOffsets.C_CSPlayerPawn.m_nEconGlovesChanged), g_GloveState.preview);
    }
    // Read-only scan helps locate recorded open/close transitions in a demo.
    for(int i = 1; i <= GetHighestEntityIndex(); ++i) {
        auto controller = GetEntityFromIndex(i);
        if(!controller || !controller->IsPlayerController()) continue;
        auto handle = controller->GetPlayerPawnHandle();
        auto pawn = handle.IsValid() ? GetEntityFromIndex(handle.GetEntryIndex()) : nullptr;
        if(!pawn || pawn->GetHandle() != handle || !pawn->IsPlayerPawn()) continue;
        auto inventory = Field<void *>(controller, 0x820);
        advancedfx::Message("[mirv_pov_buymenu] player=%d name=%s open=%d team=%d loadout_count=%d\n",
            i, controller->GetPlayerName(), Field<uint8_t>(pawn, g_clientDllOffsets.C_CSPlayerPawn.m_bIsBuyMenuOpen) != 0, pawn->GetTeam(),
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
                    g_Base + 0x8C0CE0)(Field<void *>(g_Pawn, 0x12F8), item, 1, nullptr);
                auto owned = reinterpret_cast<void * (__fastcall *)(void *, void *)>(g_Base + 0x8FF0D0)(
                    Field<void *>(g_Pawn, 0x12F0), item);
                advancedfx::Message("[mirv_pov_buymenu] slot=%d definition=%u price=%d acquire=%d owned_weapon=%d\n",
                    Field<int>(record, 64), Field<uint16_t>(item, 442), Field<int>(record, 80), result, owned != nullptr);
            }
        }
    }
}
