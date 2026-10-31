#include "stdafx.h"

#include "MirvPovHud.h"
#include "MirvPovDemoDamage.h"

#include "ClientEntitySystem.h"
#include "MirvPanorama.h"
#include "MirvPovCore.h"
#include "Globals.h"
#include "SchemaSystem.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../shared/ThreadPool.h"
#include <memory>

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <stdint.h>
#include <stddef.h>
#include <intrin.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

static void* g_PovStylePropertyVisibleVtable = nullptr;
static void* g_PovStylePropertyHeightVtable = nullptr;
static void* g_PovPanelStyleVtable = nullptr;

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

// CStylePropertyHeight uses a pixel length (unit 1). Let Panorama apply the
// existing AvatarL__name height transition instead of collapsing visibility.
struct PovStylePropertyHeight {
    void* vtable;
    uint8_t id;
    bool disallowTransition = false;
    unsigned char pad[6] = {};
    float value;
    uint32_t unit = 1;
};
static_assert(sizeof(PovStylePropertyHeight) == 24, "Panorama height property layout");

static int g_GameRulesEntityIndex = -1;

static bool MirvPovHud_GetRoundDowntime(bool& downtime, bool* roundEnded = nullptr) {
    const auto& offsets = g_clientDllOffsets.C_CSGameRules;
    if(offsets.m_bFreezePeriod < 0 || offsets.m_eRoundWinReason < 0
        || g_clientDllOffsets.C_CSGameRulesProxy.m_pGameRules < 0) return false;
    auto entity = GetEntityFromIndex(g_GameRulesEntityIndex);
    if(!entity || !entity->GetClassName() || strcmp(entity->GetClassName(), "cs_gamerules")) {
        g_GameRulesEntityIndex = -1;
        const int highestIndex = GetHighestEntityIndex();
        for(int i = 0; i <= highestIndex; ++i) {
            entity = GetEntityFromIndex(i);
            if(entity && entity->GetClassName() && !strcmp(entity->GetClassName(), "cs_gamerules")) {
                g_GameRulesEntityIndex = i;
                break;
            }
        }
        if(g_GameRulesEntityIndex < 0) return false;
    }
    auto rules = *reinterpret_cast<unsigned char**>(reinterpret_cast<unsigned char*>(entity)
        + g_clientDllOffsets.C_CSGameRulesProxy.m_pGameRules);
    if(!rules) return false;
    downtime = *reinterpret_cast<const bool*>(rules + offsets.m_bFreezePeriod)
        || *reinterpret_cast<const int*>(rules + offsets.m_eRoundWinReason) != 0;
    if(roundEnded) *roundEnded = !*reinterpret_cast<const bool*>(rules + offsets.m_bFreezePeriod)
        && *reinterpret_cast<const int*>(rules + offsets.m_eRoundWinReason) != 0;
    return true;
}

using PovRoundDamageReport_t = intptr_t (__fastcall *)(void*);
using PovRoundDamageController_t = CEntityInstance* (__fastcall *)(int);
using PovFindHud_t = void* (__fastcall *)(const char*);
static PovRoundDamageReport_t g_OrgRoundDamageReport = nullptr;
static PovRoundDamageController_t g_OrgRoundDamageController = nullptr;
static PovFindHud_t g_RoundDamageFindHud = nullptr;
static void* g_RoundDamageControllerReturn = nullptr;
static void* g_RoundDamageVtable = nullptr;
static bool g_RoundDamageHooked = false;
static bool g_RoundDamageOwned = false;
static thread_local uint32_t g_RoundDamageControllerHandle = 0xffffffffu;
static void** g_RoundDamageNetvars = nullptr;
static unsigned int g_RoundDamageReportCalls = 0;
static unsigned int g_RoundDamageControllerOverrides = 0;
static const char* g_RoundDamageInactiveReason = "not-updated";
static const char* g_RoundDamageReadFailure = "not-updated";
static const char* g_RoundDamageDiagnosticStage = "not-updated";

struct PovRoundDamageSnapshot {
    uint32_t controller;
    int update;
    uint64_t fingerprint;
    int records;
};
static PovRoundDamageSnapshot g_RoundDamageSnapshot = {};
static bool g_RoundDamageSnapshotValid = false;
static int g_RoundDamageLastTick = -1;

// Only the verified native publisher consumes this private argument. It is
// not a network CDamageRecord and must never be attached to damage services.
struct alignas(8) PovRoundDamageArgument {
    uint8_t unusedPrefix[0x68];
    float healthRemoved;
    int numHits;
    uint8_t unusedTail[5];
    uint8_t killType;
    uint8_t padding[2];
};
static_assert(sizeof(PovRoundDamageArgument) == 120, "Native damage record size changed");
static_assert(offsetof(PovRoundDamageArgument, healthRemoved) == 0x68, "Native health offset changed");
static_assert(offsetof(PovRoundDamageArgument, numHits) == 0x6c, "Native hits offset changed");
static_assert(offsetof(PovRoundDamageArgument, killType) == 0x75, "Native kill type offset changed");

using PovDamagePublish_t = intptr_t (__fastcall *)(void*, void*, bool, const void*);
using PovDamageClear_t = void (__fastcall *)(void*);
using PovDamageAllocate_t = void* (__fastcall *)(size_t);
using PovDamageSymbol_t = void (__fastcall *)(uint16_t*, const char*);
static PovDamagePublish_t g_RoundDamagePublish = nullptr;
static PovDamageClear_t g_RoundDamageClear = nullptr;
static PovDamageAllocate_t g_RoundDamageAllocate = nullptr;
static PovDamageSymbol_t g_RoundDamageSymbol = nullptr;
static void** g_RoundDamageUIEngine = nullptr;
static const uint16_t* g_RoundDamageRevealSymbol = nullptr;
static void* g_RoundDamageEventVtable = nullptr;

struct PovDemoDamageCache {
    std::atomic<bool> cancel{false};
    std::atomic<int> state{0}; // 0 queued/reading, 1 immutable index, 2 failed
    MirvPovDemoDamage::Index index;
    std::string error;
};
static std::shared_ptr<PovDemoDamageCache> g_DemoDamageCache;
static std::string g_DemoDamagePath;
extern advancedfx::CThreadPool* g_pThreadPool;

static void MirvPovHud_ResetDamageIndex() {
    if(g_DemoDamageCache) g_DemoDamageCache->cancel.store(true);
    g_DemoDamageCache.reset(); g_DemoDamagePath.clear();
}

class PovDemoDamageTask : public advancedfx::CThreadPool::CTask {
    std::shared_ptr<PovDemoDamageCache> m_Cache;
    std::string m_Path;
public:
    PovDemoDamageTask(std::shared_ptr<PovDemoDamageCache> cache, std::string path)
        : m_Cache(std::move(cache)), m_Path(std::move(path)) {}
    void Execute() override {
        try {
            std::ifstream file(m_Path, std::ios::binary);
            if(!file) throw std::runtime_error("demo file could not be opened");
            m_Cache->index.Read(file, m_Cache->cancel);
            m_Cache->state.store(1, std::memory_order_release);
        } catch(const std::exception& error) {
            m_Cache->error = error.what();
            m_Cache->state.store(2, std::memory_order_release);
        }
    }
};

static void MirvPovHud_UpdateDamageIndex() {
    // This runs before the SEH-only UI code. A task holds only file data and its
    // own shared cache; replacing a demo cancels it without waiting on rendering.
    const char* path = g_pEngineToClient && g_pEngineToClient->GetDemoFile()
        ? g_pEngineToClient->GetDemoFilePath() : nullptr;
    if(!path || !*path) {
        MirvPovHud_ResetDamageIndex(); return;
    }
    if(g_DemoDamagePath == path && g_DemoDamageCache) return;
    if(g_DemoDamagePath != path) {
        MirvPovHud_ResetDamageIndex(); g_DemoDamagePath = path;
        g_RoundDamageSnapshotValid = false;
    }
    if(!MirvPov_IsEnabled() || !MIRV_POV_FEATURE_ACTIVE("hud") || !MIRV_POV_FEATURE_ACTIVE("round_damage")
        || !g_RoundDamagePublish || !g_pThreadPool || !g_pThreadPool->GetThreadCount()) return;
    std::string filename = path;
    // The engine can return a path relative to csgo. Resolve it against the
    // current client module, never the launcher's working directory.
    if(filename.size() < 2 || (filename[1] != ':' && filename[0] != '/' && filename[0] != '\\')) {
        char module[MAX_PATH] = {};
        if(!GetModuleFileNameA(GetModuleHandleA("client.dll"), module, MAX_PATH)) return;
        std::string base = module;
        for(int i = 0; i < 3; ++i) {
            const size_t slash = base.find_last_of("/\\");
            if(slash == std::string::npos) return;
            base.resize(slash);
        }
        filename = base + "\\" + filename;
    }
    auto cache = std::make_shared<PovDemoDamageCache>();
    auto task = std::make_unique<PovDemoDamageTask>(cache, filename);
    g_pThreadPool->QueueTask(task.get()); task.release();
    g_DemoDamageCache = std::move(cache);
    advancedfx::Message("[mirv_pov_round_damage] demo-index-queued file=%s\n", filename.c_str());
}

static const MirvPovDemoDamage::Round* MirvPovHud_GetDemoDamageRound() {
    if(!g_DemoDamageCache || g_DemoDamageCache->state.load(std::memory_order_acquire) != 1
        || !g_pEngineToClient || !g_pEngineToClient->GetDemoFile()) return nullptr;
    return g_DemoDamageCache->index.At(g_pEngineToClient->GetDemoFile()->GetDemoTick());
}

static void MirvPovHud_UpdateDamageIndexSafe() {
    try { MirvPovHud_UpdateDamageIndex(); } catch(const std::exception&) {}
}

static bool MirvPovHud_RevealDemoDamage(void* client, void* avatar, int order, bool friendly) {
    auto engine = g_RoundDamageUIEngine ? *g_RoundDamageUIEngine : nullptr;
    if(!engine || !g_RoundDamageAllocate || !g_RoundDamageSymbol
        || !g_RoundDamageRevealSymbol || !g_RoundDamageEventVtable) return false;
    auto panel = *reinterpret_cast<void**>(static_cast<uint8_t*>(avatar) + 8);
    uint16_t symbol = 0;
    using SetInt_t = void (__fastcall *)(void*, uint16_t, int);
    auto setInt = reinterpret_cast<SetInt_t>((*reinterpret_cast<void***>(panel))[292]);
    g_RoundDamageSymbol(&symbol, "order_index"); setInt(panel, symbol, order);
    g_RoundDamageSymbol(&symbol, "friendlyfire"); setInt(panel, symbol, friendly ? 1 : 0);
    using GetPanel_t = void* (__fastcall *)(void*);
    using GetHandle_t = uint64_t* (__fastcall *)(void*, uint64_t*, void*);
    using Dispatch_t = void (__fastcall *)(void*, void**);
    auto engineVtable = *reinterpret_cast<void***>(engine);
    auto getHandle = reinterpret_cast<GetHandle_t>(engineVtable[33]);
    const auto avatarPanel = reinterpret_cast<GetPanel_t>((*reinterpret_cast<void***>(avatar))[0])(avatar);
    const auto targetPanel = reinterpret_cast<GetPanel_t>((*reinterpret_cast<void***>(client))[0])(client);
    if(!avatarPanel || !targetPanel) return false;
    uint64_t avatarHandle = 0xffffffff00000000ULL, targetHandle = avatarHandle;
    const uint64_t parameter = *getHandle(engine, &avatarHandle, avatarPanel);
    const uint64_t target = *getHandle(engine, &targetHandle, targetPanel);
    auto event = static_cast<uint8_t*>(g_RoundDamageAllocate(32));
    if(!event) return false;
    // Match the native CUIEvent<1,CPanelPtr<CPanel2D>> construction. Dispatch
    // takes ownership and runs the game's ShowDamageReport and reveal animation.
    memset(event, 0, 32);
    memcpy(event, &g_RoundDamageEventVtable, 8);
    memcpy(event + 8, g_RoundDamageRevealSymbol, 2);
    memcpy(event + 12, &target, 8);
    memcpy(event + 24, &parameter, 8);
    void* ownedEvent = event;
    reinterpret_cast<Dispatch_t>(engineVtable[47])(engine, &ownedEvent);
    return true;
}

static int MirvPovHud_PublishDemoDamage(void* client, uint32_t handle) {
    const auto round = MirvPovHud_GetDemoDamageRound();
    if(!round || !g_RoundDamagePublish || !g_RoundDamageClear) return -1;
    const int slot = SOURCESDK::CS2::CEntityHandle(handle).GetEntryIndex() - 1;
    auto target = GetEntityFromIndex(slot + 1);
    if(slot < 0 || slot >= 64 || !target || !target->IsPlayerController()
        || target->GetHandle().ToInt() != handle) return -1;
    g_RoundDamageClear(client);
    const int tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
    int published = 0;
    for(int i = 0; i < 20; ++i) {
        auto row = static_cast<uint8_t*>(client) + 0xf0 + i * 0xe0;
        const int otherSlot = *reinterpret_cast<int*>(row);
        auto avatar = *reinterpret_cast<void**>(row + 0x10);
        auto other = otherSlot >= 0 && otherSlot < 64 ? GetEntityFromIndex(otherSlot + 1) : nullptr;
        if(!avatar || !other || !other->IsPlayerController() || otherSlot == slot) continue;
        bool hasDamage = false;
        for(int direction = 0; direction < 2; ++direction) {
            const auto damage = direction ? round->At(slot, otherSlot, tick) : round->At(otherSlot, slot, tick);
            if(damage.health <= 0) continue;
            PovRoundDamageArgument record = {};
            record.healthRemoved = float(damage.health);
            record.numHits = damage.hits;
            record.killType = damage.kill;
            g_RoundDamagePublish(client, avatar, direction != 0, &record);
            hasDamage = true;
        }
        if(hasDamage && MirvPovHud_RevealDemoDamage(client, avatar, i, other->GetTeam() == target->GetTeam())) ++published;
    }
    return published;
}

static void MirvPovHud_TraceRoundDamage(const char* stage,
    const PovRoundDamageSnapshot* snapshot = nullptr, int panels = -1, bool force = false) {
    static const char* lastStage = "not-updated";
    static uint32_t lastController = 0xffffffffu;
    static int lastRecords = -1, lastUpdate = -1, lastPanels = -1, lastMode = -1;
    int mode = -1, tick = -1;
    __try {
        if(g_RoundDamageNetvars && *g_RoundDamageNetvars)
            mode = *reinterpret_cast<const bool*>(static_cast<unsigned char*>(*g_RoundDamageNetvars) + 0x58) ? 1 : 0;
        if(g_pEngineToClient && g_pEngineToClient->GetDemoFile())
            tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    const uint32_t controller = snapshot ? snapshot->controller : lastController;
    const int records = snapshot ? snapshot->records : lastRecords;
    const int update = snapshot ? snapshot->update : lastUpdate;
    g_RoundDamageDiagnosticStage = stage;
    if(!force && !strcmp(stage, lastStage) && controller == lastController
        && records == lastRecords && update == lastUpdate && panels == lastPanels && mode == lastMode) return;
    lastStage = stage; lastController = controller; lastRecords = records;
    lastUpdate = update; lastPanels = panels; lastMode = mode;
    advancedfx::Message("[mirv_pov_round_damage] stage=%s tick=%d controller=%08X records=%d update=%d panels=%d netvars=%d reports=%u overrides=%u\n",
        stage, tick, controller, records, update, panels, mode,
        g_RoundDamageReportCalls, g_RoundDamageControllerOverrides);
}

void MirvPovHud_PrintRoundDamageState() {
    MirvPovHud_TraceRoundDamage(g_RoundDamageDiagnosticStage, nullptr, -1, true);
}

static bool MirvPovHud_RoundDamageActive() {
    bool downtime = false, ended = false;
    if(!g_RoundDamageHooked) g_RoundDamageInactiveReason = "hook-unavailable";
    else if(!g_PovPanelStyleVtable || !g_PovResolveStyleProperty) g_RoundDamageInactiveReason = "style-unavailable";
    else if(!MirvPov_IsEnabled()) g_RoundDamageInactiveReason = "pov-disabled";
    else if(!MIRV_POV_FEATURE_ACTIVE("hud")) g_RoundDamageInactiveReason = "hud-disabled";
    else if(!MIRV_POV_FEATURE_ACTIVE("round_damage")) g_RoundDamageInactiveReason = "feature-disabled";
    else if(!g_pEngineToClient || !g_pEngineToClient->GetDemoFile()) g_RoundDamageInactiveReason = "no-demo";
    else if(!MirvPovHud_GetRoundDowntime(downtime, &ended)) g_RoundDamageInactiveReason = "rules-unavailable";
    else if(!ended) g_RoundDamageInactiveReason = "round-active-or-freeze";
    else { g_RoundDamageInactiveReason = "round-ended"; return true; }
    return false;
}

static void* MirvPovHud_GetRoundDamageClient() {
    if(!g_RoundDamageFindHud || !g_RoundDamageVtable) return nullptr;
    auto hud = static_cast<unsigned char*>(g_RoundDamageFindHud("CCSGO_HudTeamCounter"));
    auto client = hud ? hud - 0x20 : nullptr;
    return client && *reinterpret_cast<void**>(client) == g_RoundDamageVtable ? client : nullptr;
}

static uint64_t MirvPovHud_DamageHash(uint64_t hash, const void* data, size_t size) {
    auto bytes = static_cast<const uint8_t*>(data);
    for(size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

static bool MirvPovHud_RestoreDamageVisibility(unsigned char* panel) {
    if(!panel || !g_PovPanelStyleVtable || !g_PovResolveStyleProperty) return false;
    auto style = panel + CS2::PanoramaUIPanel::panelStyle;
    if(*reinterpret_cast<void**>(style) != g_PovPanelStyleVtable) return false;
    uint8_t id = 0xff;
    g_PovResolveStyleProperty(&id, "visibility");
    auto vtable = *reinterpret_cast<void***>(style);
    if(id == 0xff || !vtable[151]) return false;
    // Restore the CSS cascade, including the native bottom-aligned collapse.
    using ClearProperty_t = void (__fastcall *)(void*, uint8_t);
    reinterpret_cast<ClearProperty_t>(vtable[151])(style, id);
    return true;
}

static bool MirvPovHud_ReadDamageSnapshot(void* client, CEntityInstance* controller,
    PovRoundDamageSnapshot& result) {
    if(!client) { g_RoundDamageReadFailure = "hud-client-unavailable"; return false; }
    if(!controller || !controller->IsPlayerController()) {
        g_RoundDamageReadFailure = "pov-target-unavailable"; return false;
    }
    if(controller->GetTeam() != 2 && controller->GetTeam() != 3) {
        g_RoundDamageReadFailure = "pov-team-unavailable"; return false;
    }
    // These layouts are also checked against the native reporter at install.
    const auto& offsets = g_clientDllOffsets;
    if(offsets.CCSPlayerController.m_pDamageServices != 0x830
        || offsets.CCSPlayerController_DamageServices.m_nSendUpdate != 0x40
        || offsets.CCSPlayerController_DamageServices.m_DamageList != 0x48) {
        g_RoundDamageReadFailure = "schema-mismatch"; return false;
    }
    auto services = *reinterpret_cast<unsigned char**>(reinterpret_cast<unsigned char*>(controller) + 0x830);
    if(!services || !controller->GetHandle().IsValid()) {
        g_RoundDamageReadFailure = "damage-services-unavailable"; return false;
    }
    const int count = *reinterpret_cast<const int*>(services + 0x48);
    auto records = *reinterpret_cast<const unsigned char**>(services + 0x50);
    if(count < 0 || count > 256 || (count && !records)) {
        g_RoundDamageReadFailure = "invalid-damage-list"; return false;
    }
    result.controller = controller->GetHandle().ToInt();
    result.records = count;
    result.update = *reinterpret_cast<const int*>(services + 0x40);
    result.fingerprint = MirvPovHud_DamageHash(14695981039346656037ULL, records, count * 120);
    // A layout rebuild/row reorder must repaint even if network data is unchanged.
    for(int i = 0; i < 20; ++i) {
        auto row = static_cast<unsigned char*>(client) + 0xf0 + i * 0xe0;
        result.fingerprint = MirvPovHud_DamageHash(result.fingerprint, row, 4);
        result.fingerprint = MirvPovHud_DamageHash(result.fingerprint, row + 0x10, 8);
    }
    if(!count) {
        const int state = g_DemoDamageCache ? g_DemoDamageCache->state.load(std::memory_order_acquire) : -1;
        result.fingerprint = MirvPovHud_DamageHash(result.fingerprint, &state, sizeof(state));
        if(const auto round = MirvPovHud_GetDemoDamageRound()) {
            result.fingerprint = MirvPovHud_DamageHash(result.fingerprint, &round->start, sizeof(round->start));
            result.fingerprint = MirvPovHud_DamageHash(result.fingerprint, &round->end, sizeof(round->end));
            const int tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
            size_t events = 0;
            for(const auto& event : round->events) { if(event.tick > tick) break; ++events; }
            result.fingerprint = MirvPovHud_DamageHash(result.fingerprint, &events, sizeof(events));
        }
    }
    return true;
}

static void MirvPovHud_ClearDamagePanels(unsigned char* panel, bool finish, int depth = 0) {
    if(!panel || depth > 64) return;
    if(Panorama_HasPanelClass(panel, "afx-pov-round-damage")) {
        MirvPovHud_SetPanelClass(panel, "show-prdr", false);
        // Native $.Schedule has no hard deadline or cancellation token. A
        // visibility barrier keeps late reveals hidden even during UI stalls.
        auto report = MirvPovHud_FindPanelById(panel, "PostRoundDamageReport");
        if(report && (finish ? MirvPovHud_RestoreDamageVisibility(report)
            : Panorama_SetPanelVisible(report, false)) && finish)
            MirvPovHud_SetPanelClass(panel, "afx-pov-round-damage", false);
    }
    const auto children = panel + CS2::PanoramaUIPanel::children;
    const int count = *reinterpret_cast<const int*>(children);
    auto entries = *reinterpret_cast<unsigned char***>(children + 8);
    if(count < 0 || count > 1024 || !entries) return;
    for(int i = 0; i < count; ++i) MirvPovHud_ClearDamagePanels(entries[i], finish, depth + 1);
}

static void MirvPovHud_ClearRoundDamage(bool release = false) {
    g_RoundDamageSnapshotValid = false;
    if(!g_RoundDamageOwned) return;
    MirvPovHud_ClearDamagePanels(MirvPovHud_GetHudPanel(), release);
    if(release) g_RoundDamageOwned = false;
}

static CEntityInstance* __fastcall New_RoundDamageController(int slot) {
    void* previous = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void* caller = MirvPov_GetHookReturnAddress();
    CEntityInstance* result = nullptr;
    __try {
        if(slot == 0 && caller == g_RoundDamageControllerReturn
            && g_RoundDamageControllerHandle != 0xffffffffu && MirvPovHud_RoundDamageActive()) {
            // The report already resolved the POV before entering this scope.
            // Auto-sync resolves through this hooked getter, so repeating that
            // lookup here would recurse with the preserved native call site.
            const SOURCESDK::CS2::CEntityHandle handle(g_RoundDamageControllerHandle);
            auto controller = GetEntityFromIndex(handle.GetEntryIndex());
            if(controller && controller->IsPlayerController()
                && controller->GetHandle().ToInt() == g_RoundDamageControllerHandle)
                result = controller;
            if(result) ++g_RoundDamageControllerOverrides;
        }
        if(!result) result = g_OrgRoundDamageController(slot);
    } __finally {
        MirvPov_PopHookReturnAddress(previous);
    }
    return result;
}

static intptr_t __fastcall New_RoundDamageReport(void* client) {
    PovRoundDamageSnapshot snapshot = {};
    bool active = false;
    __try {
        active = MirvPovHud_RoundDamageActive() && client == MirvPovHud_GetRoundDamageClient()
            && MirvPovHud_ReadDamageSnapshot(client, GetCurrentPovPlayerController(), snapshot);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if(!active) return g_OrgRoundDamageReport(client);
    bool prepared = false;
    int avatars = 0, panels = 0;
    __try {
        MirvPovHud_ClearDamagePanels(MirvPovHud_GetHudPanel(), false);
        for(int i = 0; i < 20; ++i) {
            auto row = static_cast<unsigned char*>(client) + 0xf0 + i * 0xe0;
            auto avatar = *reinterpret_cast<unsigned char**>(row + 0x10);
            if(avatar && *reinterpret_cast<const int*>(row) != -1) {
                ++avatars;
                auto panel = *reinterpret_cast<unsigned char**>(avatar + 8);
                auto report = MirvPovHud_FindPanelById(panel, "PostRoundDamageReport");
                // GOTV's bottom-aligned CSS collapses this native panel. While
                // owned by POV, override visibility; release restores the CSS.
                if(report && Panorama_SetPanelVisible(report, true)) {
                    ++panels;
                    MirvPovHud_SetPanelClass(panel, "afx-pov-round-damage", true);
                    g_RoundDamageOwned = true;
                }
            }
        }
        // Do not cache an attempt made before the native child panels exist.
        prepared = panels > 0 && panels == avatars;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_RoundDamageSnapshotValid = false;
    }
    if(!prepared) {
        g_RoundDamageSnapshotValid = false;
        MirvPovHud_TraceRoundDamage("panels-not-ready", &snapshot, panels);
        return g_OrgRoundDamageReport(client);
    }
    const uint32_t previous = g_RoundDamageControllerHandle;
    intptr_t result = 0;
    __try {
        g_RoundDamageControllerHandle = snapshot.controller;
        ++g_RoundDamageReportCalls;
        result = g_OrgRoundDamageReport(client);
        int demoRows = -1;
        if(!snapshot.records) {
            __try { demoRows = MirvPovHud_PublishDemoDamage(client, snapshot.controller); }
            __except(EXCEPTION_EXECUTE_HANDLER) {
                MirvPovHud_TraceRoundDamage("demo-publish-exception", &snapshot, panels);
                __try { MirvPovHud_ClearRoundDamage(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
                g_RoundDamageSnapshotValid = false;
                return result;
            }
        }
        g_RoundDamageSnapshot = snapshot;
        g_RoundDamageSnapshotValid = true;
        const char* stage = snapshot.records ? "native-report-called" : demoRows > 0 ? "demo-report-called"
            : demoRows == 0 ? "demo-no-damage" : !g_DemoDamageCache ? "demo-index-unavailable"
            : g_DemoDamageCache->state.load(std::memory_order_acquire) == 0 ? "demo-index-loading"
            : g_DemoDamageCache->state.load(std::memory_order_acquire) == 2 ? "demo-index-failed" : "demo-round-unavailable";
        MirvPovHud_TraceRoundDamage(stage, &snapshot, panels);
        if(demoRows >= 0) advancedfx::Message("[mirv_pov_round_damage] demo-report rows=%d round-start=%d round-end=%d\n",
            demoRows, MirvPovHud_GetDemoDamageRound()->start, MirvPovHud_GetDemoDamageRound()->end);
        else if(g_DemoDamageCache && g_DemoDamageCache->state.load(std::memory_order_acquire) == 2)
            advancedfx::Message("[mirv_pov_round_damage] demo-index-error=%s\n", g_DemoDamageCache->error.c_str());
    } __finally {
        g_RoundDamageControllerHandle = previous;
    }
    return result;
}

void MirvPovHud_UpdateRoundDamage() {
    __try {
        MirvPovHud_UpdateDamageIndexSafe();
        if(!MirvPovHud_RoundDamageActive()) {
            MirvPovHud_TraceRoundDamage(g_RoundDamageInactiveReason);
            MirvPovHud_ClearRoundDamage(!g_pEngineToClient || !g_pEngineToClient->GetDemoFile());
            g_RoundDamageLastTick = -1;
            return;
        }
        auto client = MirvPovHud_GetRoundDamageClient();
        PovRoundDamageSnapshot snapshot = {};
        if(!MirvPovHud_ReadDamageSnapshot(client, GetCurrentPovPlayerController(), snapshot)) {
            MirvPovHud_TraceRoundDamage(g_RoundDamageReadFailure);
            MirvPovHud_ClearRoundDamage();
            return;
        }
        const int tick = g_pEngineToClient->GetDemoFile()->GetDemoTick();
        if(tick < g_RoundDamageLastTick) g_RoundDamageSnapshotValid = false;
        g_RoundDamageLastTick = tick;
        if(!g_RoundDamageSnapshotValid || snapshot.controller != g_RoundDamageSnapshot.controller
            || snapshot.update != g_RoundDamageSnapshot.update
            || snapshot.fingerprint != g_RoundDamageSnapshot.fingerprint)
            New_RoundDamageReport(client);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_RoundDamageSnapshotValid = false;
        MirvPovHud_TraceRoundDamage("exception");
    }
}

static void MirvPovHud_InstallRoundDamageHook(HMODULE clientDll) {
    if(g_RoundDamageHooked) return;
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(sections.Eof()) return;
    auto text = sections.GetMemRange();
    const char* pattern = "41 56 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 4C 8B F1 80 78 58 00 0F 84 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 33 C9 4C 89 6C 24 48 E8";
    auto match = Afx::BinUtils::FindPatternString(text, pattern);
    if(match.IsEmpty() || !Afx::BinUtils::FindPatternString(
        Afx::BinUtils::MemRange(match.Start + 1, text.End), pattern).IsEmpty()) return;
    auto report = reinterpret_cast<unsigned char*>(match.Start);
    const uint8_t services[] = {0x4d,0x8b,0xa5,0x30,0x08,0,0};
    const uint8_t records[] = {0x48,0x6b,0xe8,0x78,0x49,0x03,0x6c,0x24,0x50};
    const uint8_t rows[] = {0x49,0x8d,0xbe,0xf0,0,0,0,0x8b,0x0f};
    const uint8_t stride[] = {0x48,0x81,0xc7,0xe0,0,0,0,0x48,0x83,0xfb,0x14};
    // Validate the exact record/row contract before touching either layout.
    if(match.Start + 0x2c0 > text.End || memcmp(report + 0x7c, services, sizeof(services))
        || memcmp(report + 0x94, "\x41\x39\x44\x24\x48", 5)
        || memcmp(report + 0xa3, records, sizeof(records))
        || memcmp(report + 0xf0, rows, sizeof(rows))
        || memcmp(report + 0x106, stride, sizeof(stride))) {
        advancedfx::Warning("[mirv_pov_round_damage] Native report layout validation failed.\n");
        return;
    }
    auto getterCall = report + 0x2f;
    auto getter = getterCall + 5 + *reinterpret_cast<const int32_t*>(getterCall + 1);
    auto lookup = getAddress(clientDll, "40 53 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 8B D9 48 85 C0 74 ?? 48 89 5C 24 38 48 8D 88 58 02 00 00 48 85 DB 74 ?? 4C 8B C1 48 8D 54 24 38");
    auto vtable = reinterpret_cast<void*>(Afx::BinUtils::FindClassVtable(clientDll, ".?AVCCSGO_HudTeamCounter@@", 0, 0));
    if(reinterpret_cast<size_t>(getter) < text.Start || reinterpret_cast<size_t>(getter) >= text.End
        || !lookup || !vtable) return;
    // Derive unhooked playback helpers and event data from this verified native
    // reporter. A changed instruction contract disables only the demo fallback.
    if(report[0x77] == 0xe8 && report[0x138] == 0xe8 && report[0x1e5] == 0xe8
        && !memcmp(report + 0x19a, "\x48\x8b\x3d", 3)
        && !memcmp(report + 0x1f2, "\x0f\xb7\x0d", 3)
        && !memcmp(report + 0x256, "\x48\x8d\x05", 3)) {
        auto relativeCall = [report](size_t offset) {
            return report + offset + 5 + *reinterpret_cast<const int32_t*>(report + offset + 1);
        };
        auto clear = relativeCall(0x77), publish = relativeCall(0x138), allocate = relativeCall(0x1e5);
        auto inText = [&text](const unsigned char* address, size_t length) {
            return reinterpret_cast<size_t>(address) >= text.Start
                && reinterpret_cast<size_t>(address) + length <= text.End;
        };
        const uint8_t publishStart[] = {0x48,0x89,0x5c,0x24,8,0x48,0x89,0x6c,0x24,0x20,0x56,0x57,0x41,0x56,0x48,0x83,0xec,0x30};
        if(inText(clear, 14) && inText(publish, 0x122) && inText(allocate, 1)
            && !memcmp(clear, "\x48\x8b\xc4\x55\x48\x8d\x68\xa1\x48\x81\xec\xf0\0\0", 14)
            && !memcmp(publish, publishStart, sizeof(publishStart))
            && !memcmp(publish + 0x4f, "\xf3\x0f\x10\x76\x68", 5)
            && !memcmp(publish + 0x83, "\x8b\x5e\x6c", 3)
            && !memcmp(publish + 0xba, "\x0f\xb6\x46\x75", 4)
            && report[0x17d] == 0xe8) {
            auto symbol = relativeCall(0x17d);
            auto uiEngine = report + 0x1a1 + *reinterpret_cast<int32_t*>(report + 0x19d);
            auto revealSymbol = report + 0x1f9 + *reinterpret_cast<int32_t*>(report + 0x1f5);
            auto eventVtable = report + 0x25d + *reinterpret_cast<int32_t*>(report + 0x259);
            auto module = reinterpret_cast<uint8_t*>(clientDll);
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + reinterpret_cast<IMAGE_DOS_HEADER*>(module)->e_lfanew);
            const size_t moduleStart = reinterpret_cast<size_t>(module), moduleEnd = moduleStart + nt->OptionalHeader.SizeOfImage;
            auto inImage = [moduleStart, moduleEnd](const uint8_t* address, size_t length) {
                return reinterpret_cast<size_t>(address) >= moduleStart
                    && reinterpret_cast<size_t>(address) + length <= moduleEnd;
            };
            if(inText(symbol, 1) && inImage(uiEngine, 8) && inImage(revealSymbol, 2) && inImage(eventVtable, 8)) {
                g_RoundDamageClear = reinterpret_cast<PovDamageClear_t>(clear);
                g_RoundDamagePublish = reinterpret_cast<PovDamagePublish_t>(publish);
                g_RoundDamageAllocate = reinterpret_cast<PovDamageAllocate_t>(allocate);
                g_RoundDamageSymbol = reinterpret_cast<PovDamageSymbol_t>(symbol);
                g_RoundDamageUIEngine = reinterpret_cast<void**>(uiEngine);
                g_RoundDamageRevealSymbol = reinterpret_cast<uint16_t*>(revealSymbol);
                g_RoundDamageEventVtable = eventVtable;
            }
        }
    }
    g_OrgRoundDamageReport = reinterpret_cast<PovRoundDamageReport_t>(report);
    g_OrgRoundDamageController = reinterpret_cast<PovRoundDamageController_t>(getter);
    g_RoundDamageControllerReturn = getterCall + 5;
    if(NO_ERROR != DetourTransactionBegin()) return;
    if(NO_ERROR != DetourUpdateThread(GetCurrentThread())
        || NO_ERROR != DetourAttach(&(PVOID&)g_OrgRoundDamageReport, New_RoundDamageReport)
        || NO_ERROR != DetourAttach(&(PVOID&)g_OrgRoundDamageController, New_RoundDamageController)) {
        DetourTransactionAbort();
        return;
    }
    g_RoundDamageHooked = NO_ERROR == DetourTransactionCommit();
    if(g_RoundDamageHooked) {
        g_RoundDamageFindHud = reinterpret_cast<PovFindHud_t>(lookup);
        g_RoundDamageVtable = vtable;
        g_RoundDamageNetvars = reinterpret_cast<void**>(report + 0x0d
            + *reinterpret_cast<const int32_t*>(report + 9));
        MirvPovHud_TraceRoundDamage("hooks-installed");
        advancedfx::Message("[mirv_pov_round_damage] demo-fallback=%d\n", g_RoundDamagePublish ? 1 : 0);
    }
}

static void MirvPovHud_SetPlayerNameHeights(
    unsigned char* panel,
    short classSymbol,
    uint8_t heightId,
    bool enabled,
    bool downtime,
    bool competitive = false) {
    if(!panel) return;
    competitive = competitive || Panorama_HasPanelClass(panel, "competitive")
        || Panorama_HasPanelClass(panel, "scrimcomp2v2");

    auto vtable = *(void***)panel;
    if(vtable) {
        using HasPanelClass_t = bool (__fastcall *)(void*, short);
        auto hasPanelClass = reinterpret_cast<HasPanelClass_t>(vtable[157]);
        if(hasPanelClass && hasPanelClass(panel, classSymbol)) {
            auto style = panel + CS2::PanoramaUIPanel::panelStyle;
            if(*reinterpret_cast<void**>(style) == g_PovPanelStyleVtable) {
                if(enabled && competitive) {
                    PovStylePropertyHeight property{g_PovStylePropertyHeightVtable, heightId, false, {}, downtime ? 14.0f : 0.0f};
                    g_PovPanelStyleSetStyleProperty(style, &property, true);
                    MirvPovHud_SetNativePanelClass(panel, "afx-pov-playernames", true);
                } else if(Panorama_HasPanelClass(panel, "afx-pov-playernames")) {
                    // Remove only our height override and recompute the native
                    // CSS cascade. Never reset other inline panel properties.
                    using ClearProperty_t = void (__fastcall *)(void*, uint8_t);
                    reinterpret_cast<ClearProperty_t>((*reinterpret_cast<void***>(style))[151])(style, heightId);
                    MirvPovHud_SetNativePanelClass(panel, "afx-pov-playernames", false);
                }
            }
#if AFX_MIRV_POV_DIAGNOSTICS
            static int lastState = -1;
            const int state = enabled && competitive ? (downtime ? 14 : 0) : -2;
            if(lastState != state) {
                advancedfx::Message("[mirv_pov_hud] player names height=%d (native transition)\n", state);
                lastState = state;
            }
#endif
        }
    }

    const auto children = panel + CS2::PanoramaUIPanel::children;
    const auto childCount = *(int*)children;
    for(int i = 0; i < childCount; ++i) {
        MirvPovHud_SetPlayerNameHeights(
            ((unsigned char***)children)[1][i],
            classSymbol, heightId, enabled, downtime, competitive);
    }
}

static void MirvPovHud_RefreshTeamCounterPlayerNames(bool restore = false) {
    if(!g_PovStylePropertyHeightVtable || !g_PovPanelStyleVtable
        || !g_PovPanelStyleSetStyleProperty || !g_PovResolveStyleProperty) return;
    __try {
        auto hudPanel = MirvPovHud_GetHudPanel();
        if(!hudPanel) return;

        short nameClass = -1;
        if(!MirvPovHud_MakeSymbol("AvatarL__name", nameClass)) return;
        uint8_t heightId = 0xFF;
        g_PovResolveStyleProperty(&heightId, "height");
        if(heightId == 0xFF) return;
        bool downtime = false;
        const bool enabled = !restore && MIRV_POV_FEATURE_ACTIVE("hud")
            && MirvPovHud_GetRoundDowntime(downtime);
        MirvPovHud_SetPlayerNameHeights(hudPanel, nameClass, heightId, enabled, downtime);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static bool MirvPovHud_SetStrokeSiblingVisibleForAnchor(unsigned char* parentPanel, const char* anchorId, bool visible) {
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
            if(MirvPovHud_SetPanelVisible(strokePanel, visible)) return true;
        }
    }

    for(int i = 0; i < childCount; ++i) {
        if(MirvPovHud_SetStrokeSiblingVisibleForAnchor(((unsigned char***)children)[1][i], anchorId, visible)) return true;
    }

    return false;
}

static void MirvPovHud_SetHealthAmmoCenterStrokesVisible(bool visible) {
    auto hudPanel = MirvPovHud_GetHudPanel();
    if(!hudPanel) return;

    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-HA-main", visible);
    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-WPN-main", visible);
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
using MirvPovHud_MoneyUpdate_t = void (__fastcall *)(void *);
static MirvPovHud_MoneyUpdate_t g_OrgMoneyUpdate = nullptr;
static void * g_MoneyPanelClientVtable = nullptr;
static bool g_MoneyUpdateHooked = false;

static bool MirvPovHud_IsExecutableAddress(const void * address) {
    if(nullptr == address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if(0 == VirtualQuery(address, &info, sizeof(info))) return false;
    const DWORD protection = info.Protect & 0xFF;
    return MEM_COMMIT == info.State
        && (PAGE_EXECUTE == protection || PAGE_EXECUTE_READ == protection
            || PAGE_EXECUTE_READWRITE == protection || PAGE_EXECUTE_WRITECOPY == protection);
}

static uintptr_t MirvPovHud_GetBuyZoneCallSite() {
    // Resolve all three native HudMoney predicates from the same call site.
    // The zone predicate also handles mp_buy_anywhere; a network flag alone
    // does not describe native availability or the elapsed buy timer.
    HMODULE clientDll = GetModuleHandleW(L"client.dll");
    if(nullptr == clientDll) return 0;
    static const auto site = getAddress(clientDll,
        "48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B CE E8 ?? ?? ?? ?? 84 C0 75 ?? 48 85 DB 74 ?? 33 D2 48 8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? B3 01 EB ?? 32 DB");
    return site;
}

static bool MirvPovHud_ShouldShowBuyZoneIcon(CEntityInstance * povPawn) {
    if(!MIRV_POV_FEATURE_ACTIVE("buyzone") || nullptr == povPawn) return false;
    const auto site = MirvPovHud_GetBuyZoneCallSite();
    if(!site) return false;
    auto buyState = reinterpret_cast<MirvPovHud_BuyStatePredicate_t>(site + 12 + *reinterpret_cast<int32_t *>(site + 8));
    auto buyTimeElapsed = reinterpret_cast<MirvPovHud_BuyStatePredicate_t>(site + 24 + *reinterpret_cast<int32_t *>(site + 20));
    auto inBuyZone = reinterpret_cast<bool (__fastcall *)(void *, bool)>(site + 43 + *reinterpret_cast<int32_t *>(site + 39));
    if(!MirvPovHud_IsExecutableAddress(buyState)
        || !MirvPovHud_IsExecutableAddress(buyTimeElapsed)
        || !MirvPovHud_IsExecutableAddress(inBuyZone)) return false;

    __try {
        void * gameRules = *reinterpret_cast<void **>(site + 7 + *reinterpret_cast<int32_t *>(site + 3));
        if(nullptr == gameRules) return false;
        // Platform demos can enable buy-anywhere. The native predicate then
        // succeeds outside the physical zone; the POV icon must still reflect
        // this player's replicated zone membership.
        bool zoneAvailable = false;
        bool physicallyInZone = povPawn->GetInBuyZone(zoneAvailable);
        return zoneAvailable && physicallyInZone
            && buyState(gameRules) && !buyTimeElapsed(gameRules)
            && inBuyZone(povPawn, false);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static unsigned char * MirvPovHud_GetMoneyPanel() {
    auto hudPanel = MirvPovHud_GetHudPanel();
    if(!hudPanel) return nullptr;

    auto moneyPanel = MirvPovHud_FindPanelById(hudPanel, "HudMoney");
    if(nullptr == moneyPanel) {
        moneyPanel = MirvPovHud_FindPanelById(hudPanel, "HudMoneyPanel");
    }
    return moneyPanel;
}

static void * MirvPovHud_GetMoneyPanelClient(unsigned char * panel) {
    if(!panel || !g_MoneyPanelClientVtable) return nullptr;
    auto vtable = *reinterpret_cast<void ***>(panel);
    if(!vtable || !vtable[8]) return nullptr;
    auto client = reinterpret_cast<void * (__fastcall *)(void *)>(vtable[8])(panel);
    // Resolve afresh instead of retaining a client/panel pointer across seeks
    // or layout teardown. Only touch the verified CCSGOMoneyPanel cache.
    if(!client || *reinterpret_cast<void **>(client) != g_MoneyPanelClientVtable
        || *reinterpret_cast<void **>(static_cast<unsigned char *>(client) + 8) != panel) return nullptr;
    return client;
}

static void MirvPovHud_SetBuyZoneIcon(void * client, unsigned char * panel, bool visible) {
    if(!client || !panel) return;
    if(MirvPovHud_SetNativePanelClass(panel, "money__in-buy-zone", visible)) {
        // Native HudMoney only writes the class when this cached byte changes.
        // Keep it consistent with our class so later native updates can clear
        // the icon, including after POV is disabled.
        *reinterpret_cast<uint8_t *>(static_cast<unsigned char *>(client) + 0x28) = visible ? 1 : 0;
    }
}

static void MirvPovHud_RefreshBuyZoneIcon(bool clear = false) {
    if(!clear && !MIRV_POV_FEATURE_ACTIVE("hud")) return;
    __try {
        auto panel = MirvPovHud_GetMoneyPanel();
        auto client = MirvPovHud_GetMoneyPanelClient(panel);
        MirvPovHud_SetBuyZoneIcon(client, panel,
            !clear && MirvPovHud_ShouldShowBuyZoneIcon(GetCurrentPovPlayerPawn()));
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall New_MoneyUpdate(void * client) {
    g_OrgMoneyUpdate(client);
    if(!MIRV_POV_FEATURE_ACTIVE("hud")) return;
    __try {
        auto panel = *reinterpret_cast<unsigned char **>(static_cast<unsigned char *>(client) + 8);
        if(panel && panel == MirvPovHud_GetMoneyPanel()) {
            // A native Panorama update can run after our frame-stage refresh.
            // Correct its final class AND cache, not just the earlier frame.
            MirvPovHud_SetBuyZoneIcon(client, panel,
                MirvPovHud_ShouldShowBuyZoneIcon(GetCurrentPovPlayerPawn()));
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void MirvPovHud_InstallMoneyUpdateHook(HMODULE clientDll) {
    if(g_MoneyUpdateHooked) return;
    const auto site = MirvPovHud_GetBuyZoneCallSite();
    const auto entry = getAddress(clientDll,
        "40 56 57 48 83 EC 48 48 8B 35 ?? ?? ?? ?? 48 8B F9 48 89 74 24 70 48 85 F6 0F 84 ?? ?? ?? ?? 48 89 6C 24 40 4C 89 74 24");
    DWORD64 imageBase = 0;
    auto function = site ? RtlLookupFunctionEntry(site, &imageBase, nullptr) : nullptr;
    bool entryMatches = false;
    if(function && imageBase == reinterpret_cast<uintptr_t>(clientDll)
        && site + 85 <= imageBase + function->EndAddress) {
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(clientDll);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(imageBase + dos->e_lfanew);
        const auto imageSize = nt->OptionalHeader.SizeOfImage;
        // MSVC can split one function into several chained unwind records.
        // Follow those records back to the prologue instead of treating a
        // mid-function fragment as a different HudMoney implementation.
        for(int depth = 0; depth < 8; ++depth) {
            if(imageBase + function->BeginAddress == entry) {
                entryMatches = true;
                break;
            }
            const auto unwindRva = function->UnwindData;
            if((unwindRva & 1) || imageSize < 4 || unwindRva > imageSize - 4) break;
            auto unwind = reinterpret_cast<const uint8_t *>(imageBase + unwindRva);
            if(0 == ((unwind[0] >> 3) & UNW_FLAG_CHAININFO)) break;
            const auto chainOffset = (4u + 2u * unwind[2] + 3u) & ~3u;
            if(imageSize - unwindRva < chainOffset + sizeof(RUNTIME_FUNCTION)) break;
            function = reinterpret_cast<PRUNTIME_FUNCTION>(imageBase + unwindRva + chainOffset);
        }
    }
    // Bind the entry to the predicate chain and the +0x28 cache/class writer.
    static const uint8_t cacheCompare[] = {0x38, 0x5F, 0x28, 0x74, 0x1B, 0x48, 0x8B, 0x4F, 0x08};
    static const uint8_t cacheStore[] = {0x88, 0x5F, 0x28};
    auto panelVtable = reinterpret_cast<void *>(Afx::BinUtils::FindClassVtable(
        clientDll, ".?AVCCSGOMoneyPanel@@", 0, 0));
    if(!entry || !entryMatches
        || memcmp(reinterpret_cast<void *>(site + 53), cacheCompare, sizeof(cacheCompare))
        || memcmp(reinterpret_cast<void *>(site + 82), cacheStore, sizeof(cacheStore)) || !panelVtable) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] HudMoney update/cache validation failed.\n");
        return;
    }
    g_OrgMoneyUpdate = reinterpret_cast<MirvPovHud_MoneyUpdate_t>(entry);
    if(NO_ERROR != DetourTransactionBegin()) return;
    if(NO_ERROR != DetourUpdateThread(GetCurrentThread())
        || NO_ERROR != DetourAttach(&(PVOID &)g_OrgMoneyUpdate, New_MoneyUpdate)) {
        DetourTransactionAbort();
        return;
    }
    g_MoneyUpdateHooked = NO_ERROR == DetourTransactionCommit();
    if(g_MoneyUpdateHooked) g_MoneyPanelClientVtable = panelVtable;
}

static void MirvPovHud_ResetPanelState() {
    g_LastHudPanel = nullptr;
    g_HudPanelStateNeedsRefresh = true;
    g_SpectatorHotKeyLabelContainerPanel = nullptr;
}

static void MirvPovHud_RefreshPanelState() {
    if(!MIRV_POV_FEATURE_ACTIVE("hud")) return;
    __try {
        auto hudPanel = MirvPovHud_GetHudPanel();
        if(hudPanel != g_LastHudPanel) {
            g_LastHudPanel = hudPanel;
            g_HudPanelStateNeedsRefresh = true;
            g_SpectatorHotKeyLabelContainerPanel = nullptr;
        }

        if(!g_HudPanelStateNeedsRefresh || !hudPanel) return;

        const bool specPlayerHidden = MirvPovHud_HideSpecPlayerPanel();
        MirvPovHud_SetHealthAmmoCenterStrokesVisible(true);
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
        if(MIRV_POV_FEATURE_ACTIVE("hud")) MirvPovHud_RefreshPanelState();
    }
}

void MirvPovHud_OnPanoramaDllLoaded(HMODULE panoramaDll) {
    g_PovStylePropertyHeightVtable = (void*)Afx::BinUtils::FindClassVtable(
        panoramaDll, ".?AVCStylePropertyHeight@panorama@@", 0, 0);
    g_PovPanelStyleVtable = (void*)Afx::BinUtils::FindClassVtable(
        panoramaDll, ".?AVCPanelStyle@panorama@@", 0, 0);
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
    __try { MirvPovHud_ClearRoundDamage(true); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    MirvPovHud_ResetDamageIndex();
    g_RoundDamageLastTick = -1;
    MirvPovHud_RefreshBuyZoneIcon(true);
    g_GameRulesEntityIndex = -1;
    MirvPovHud_ResetPanelState();
    g_ScoreboardSeekSuppressFrames = 0;
    g_LastDemoTick = -1;
}

void MirvPovHud_ReapplyPanelState() {
    MirvPovHud_RefreshPanelState();
    if(MIRV_POV_FEATURE_ACTIVE("hud")) {
        // Reapply the existing Demo-card background/avatar state after native
        // observer updates.
        MirvPovHud_HideSpecPlayerPanel();
        MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);
        MirvPovHud_RefreshTeamCounterPlayerNames();
        MirvPovHud_RefreshBuyZoneIcon();
    }
}

void MirvPovHud_UpdateSeekDetection(int curTick) {
    if(!MIRV_POV_FEATURE_ACTIVE("hud")) return;
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

using HudObserverMode_t = int (__fastcall *)();
static HudObserverMode_t g_OrgHudObserverMode = nullptr;
static void * g_HealthAmmoObserverModeReturns[2] = {};
static bool g_HudObserverModeHooked = false;

static int __fastcall New_HudObserverMode() {
    void * previous = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * caller = MirvPov_GetHookReturnAddress();
    int mode = g_OrgHudObserverMode();
    MirvPov_PopHookReturnAddress(previous);
    if(MIRV_POV_FEATURE_ACTIVE("hud")
        && (caller == g_HealthAmmoObserverModeReturns[0]
            || caller == g_HealthAmmoObserverModeReturns[1])) {
#if AFX_MIRV_POV_DIAGNOSTICS
        static bool reported = false;
        if(!reported && (mode == 2 || mode == 3)) {
            advancedfx::Message("[mirv_pov_hud] HealthAmmoCenter observer mode %d -> 0\n", mode);
            reported = true;
        }
#endif
        return 0;
    }
    return mode;
}

static void MirvPovHud_InstallObserverModeHook(HMODULE clientDll) {
    if(g_HudObserverModeHooked) return;

    // HealthAmmoCenter selects ui_hud_kill_streaks_spectator_* for modes 2/3.
    // Only this presentation decision needs live POV semantics; keep the
    // native kill queue, effect playback and all other observer queries intact.
    auto match = reinterpret_cast<uint8_t *>(getAddress(clientDll,
        "E8 ?? ?? ?? ?? 83 F8 02 74 0A E8 ?? ?? ?? ?? 83 F8 03 75 09 48 85 DB 74 04 B2 01"));
    auto target = MirvPovHud_GetRelativeCallTarget(match);
    if(!target || target != MirvPovHud_GetRelativeCallTarget(match + 10)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] HealthAmmoCenter observer-mode calls were not found.\n");
        return;
    }
    g_OrgHudObserverMode = reinterpret_cast<HudObserverMode_t>(target);
    g_HealthAmmoObserverModeReturns[0] = match + 5;
    g_HealthAmmoObserverModeReturns[1] = match + 15;
    if(NO_ERROR != DetourTransactionBegin()) return;
    if(NO_ERROR != DetourUpdateThread(GetCurrentThread())
        || NO_ERROR != DetourAttach(&(PVOID &)g_OrgHudObserverMode, New_HudObserverMode)) {
        DetourTransactionAbort();
        return;
    }
    g_HudObserverModeHooked = NO_ERROR == DetourTransactionCommit();
    if(!g_HudObserverModeHooked)
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] HealthAmmoCenter observer-mode detour failed.\n");
}

static bool __fastcall New_FlashViewPredicate() {
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    bool result = g_OrgFlashViewPredicate();
    MirvPov_PopHookReturnAddress(previousReturnAddress);

    if(!g_FlashHooksActive || !MIRV_POV_FEATURE_ACTIVE("hud")) return result;
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
        "84 C0 74 4C 8B 85 38 02 00 00 49 8D 8F ?? ?? ?? ?? 0F 10 85 20 02 00 00");
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

void MirvPovHud_ApplyPatches(HMODULE clientDll) {
    if(nullptr == clientDll) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_hud] client.dll is not loaded.\n");
        return;
    }

    MirvPovHud_InstallFlashPredicateHook(clientDll);
    MirvPovHud_InstallObserverModeHook(clientDll);
    MirvPovHud_InstallMoneyUpdateHook(clientDll);
    MirvPovHud_InstallRoundDamageHook(clientDll);
    g_FlashHooksActive = true;

    MirvPovHud_ResetPanelState();
    MirvPovHud_ReapplyPanelState();

    return;
}

void MirvPovHud_RemovePatches() {
    __try { MirvPovHud_ClearRoundDamage(); } __except(EXCEPTION_EXECUTE_HANDLER) {}
    g_RoundDamageLastTick = -1;
    g_FlashHooksActive = false;
    MirvPovHud_RefreshBuyZoneIcon(true);
    // TeamHealth hooks this same predicate. Keep the detour chain installed;
    // inactive wrappers pass through instead of detaching beneath another hook.
    __try {
        auto hudPanel = MirvPovHud_GetHudPanel();
        if(hudPanel) {
            MirvPovHud_SetPanelVisible(MirvPovHud_FindPanelById(hudPanel, "jsHudSpecplayer__Bg"), true);
            MirvPovHud_SetPanelVisible(MirvPovHud_FindPanelById(hudPanel, "HudSpecplayer__Avatar"), true);
            MirvPovHud_SetHealthAmmoCenterStrokesVisible(false);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(true);
    MirvPovHud_RefreshTeamCounterPlayerNames(true);
    MirvPovHud_ResetPanelState();
    g_ScoreboardSeekSuppressFrames = 0;
    g_LastDemoTick = -1;
}
