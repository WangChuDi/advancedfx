#include "stdafx.h"
#include "MirvPovMvpMusic.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovCore.h"
#include "MirvPovDeathPanel.h"
#include "WrpConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <cstring>
#include <atomic>

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

namespace {

constexpr uint32_t kNoPawn = 0xFFFFFFFFu;
thread_local uint32_t g_EventPawn = kNoPawn;
void * g_PlayerGetterReturn = nullptr;
void * g_ListenerReturn = nullptr;
void * g_MutePredicateReturn = nullptr;
uintptr_t g_ClientBase = 0;
uintptr_t g_ClientTextEnd = 0;
thread_local MirvPovMvpMusicTrace * g_Trace = nullptr;
std::atomic_uint64_t g_DispatchCount {0};
std::atomic_uint64_t g_MvpCount {0};

using Eligibility_t = char (__fastcall *)(SOURCESDK::CS2::IGameEvent *);
using PlayMusic_t = void (__fastcall *)(void *, int, uint16_t);
using SubmitSound_t = void * (__fastcall *)(void *, void *, const char *, int, bool);
Eligibility_t g_OrgEligibility = nullptr;
PlayMusic_t g_OrgPlayMusic = nullptr;
SubmitSound_t g_OrgSubmitSound = nullptr;
void ** g_InventorySlot = nullptr;
void ** g_SoundSystemSlot = nullptr;
const unsigned char * g_DemoSuppress = nullptr;
bool g_PlaybackObserversReady = false;
std::atomic_bool g_SoundObserverReady {false};
SRWLOCK g_SoundHookLock = SRWLOCK_INIT;
SRWLOCK g_LastTraceLock = SRWLOCK_INIT;
MirvPovMvpMusicTrace g_LastTrace;
thread_local bool g_InNativeMusic = false;

void TraceGate(const char * gate)
{
    if(g_Trace) g_Trace->gate = gate;
}

// client 9b4f46db: C_CSPlayer getter C7C460, MVP eligibility EC4AC0.
// Keep the IsPlayerPawn slot and the complete caller context in the signatures.
constexpr char kPlayerGetter[] =
    "40 53 48 83 EC 20 33 C9 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? "
    "48 8B 00 48 8B CB FF 90 F0 04 00 00 84 C0 74 ?? 48 8B C3 "
    "48 83 C4 20 5B C3 33 C0";
constexpr char kMutePredicateCall[] =
    "E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 00 48 8B CB "
    "FF 90 F0 04 00 00 84 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? "
    "84 C0 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 5C 24 40";
constexpr char kListenerCalls[] =
    "E8 ?? ?? ?? ?? 48 8B D8 48 89 85 08 05 00 00 E8 ?? ?? ?? ?? "
    "48 8D 4C 24 78 48 8B F0 E8 ?? ?? ?? ?? 49 8B 14 24 49 8B CC";
constexpr char kMvpPlaybackCalls[] =
    "49 8B CC 66 41 89 9D 48 01 00 00 E8 ?? ?? ?? ?? 84 C0 74 22 "
    "0F 57 DB 48 8D 4C 24 78 44 0F B7 C3 BA 0B 00 00 00 E8 ?? ?? ?? ??";
constexpr char kMusicEntry[] =
    "40 55 53 57 48 8D AC 24 F0 FE FF FF 48 81 EC 10 02 00 00 "
    "48 63 DA 41 0F B7 F8 8D 43 F8 A9 FC FF FF FF";
constexpr char kMusicDemoGate[] =
    "E8 ?? ?? ?? ?? 80 B8 82 00 00 00 00 0F 85 ?? ?? ?? ?? 48 89 B4 24 30 02 00 00";
constexpr char kMusicSubmit[] =
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 4C 8B 50 68 8B 45 34 0F BA E0 1E";

size_t FindUnique(const Afx::BinUtils::MemRange & text, const char * pattern)
{
    auto match = Afx::BinUtils::FindPatternString(text, pattern);
    if(match.IsEmpty() || !Afx::BinUtils::FindPatternString(
        Afx::BinUtils::MemRange(match.Start + 1, text.End), pattern).IsEmpty()) return 0;
    return match.Start;
}

size_t CallTarget(size_t call)
{
    return call + 5 + *reinterpret_cast<const int32_t *>(call + 1);
}

void * __fastcall ObserveSubmitSound(void * system, void * result, const char * name, int entity, bool flag)
{
    // Only copy the request made synchronously by the native MVP helper.
    // Reaching this interface is not proof that the audio device played it.
    if(g_Trace && g_InNativeMusic) {
        ++g_Trace->soundCalls;
        __try {
            if(name) strncpy_s(g_Trace->soundName, name, _TRUNCATE);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_OrgSubmitSound(system, result, name, entity, flag);
}

void EnsureSoundObserver()
{
    if(g_SoundObserverReady.load()) return;
    AcquireSRWLockExclusive(&g_SoundHookLock);
    __try {
        if(g_SoundObserverReady.load() || !g_SoundSystemSlot || !*g_SoundSystemSlot) __leave;
        g_OrgSubmitSound = reinterpret_cast<SubmitSound_t>((*reinterpret_cast<void ***>(*g_SoundSystemSlot))[13]);
        if(!g_OrgSubmitSound) __leave;
        LONG error = DetourTransactionBegin();
        if(NO_ERROR != error) __leave;
        error = DetourUpdateThread(GetCurrentThread());
        if(NO_ERROR == error) error = DetourAttach(&(PVOID &)g_OrgSubmitSound, ObserveSubmitSound);
        if(NO_ERROR == error) error = DetourTransactionCommit();
        else DetourTransactionAbort();
        if(NO_ERROR == error) g_SoundObserverReady.store(true);
        else g_OrgSubmitSound = nullptr;
    } __finally {
        ReleaseSRWLockExclusive(&g_SoundHookLock);
    }
}

char __fastcall ObserveEligibility(SOURCESDK::CS2::IGameEvent * event)
{
    char result = g_OrgEligibility(event);
    if(g_Trace) {
        ++g_Trace->eligibilityCalls;
        g_Trace->eligible = result ? 1 : 0;
    }
    return result;
}

void __fastcall ObservePlayMusic(void * filter, int type, uint16_t kit)
{
    const bool previous = g_InNativeMusic;
    g_InNativeMusic = g_Trace && type == 11;
    __try {
        if(g_InNativeMusic) {
            ++g_Trace->musicCalls;
            g_Trace->nativeKit = kit;
            g_Trace->demoSuppress = g_DemoSuppress ? *g_DemoSuppress : -1;
            EnsureSoundObserver();
        }
        g_OrgPlayMusic(filter, type, kit);
    } __finally {
        g_InNativeMusic = previous;
    }
}

void ResolvePlaybackObservers(const Afx::BinUtils::MemRange & text, size_t listener, size_t muteCall)
{
    if(g_PlaybackObserversReady) return;
    const auto calls = FindUnique(text, kMvpPlaybackCalls);
    const auto music = FindUnique(text, kMusicEntry);
    const auto gate = FindUnique(text, kMusicDemoGate);
    const auto submit = FindUnique(text, kMusicSubmit);
    if(!calls || !music || !gate || !submit || CallTarget(calls + 37) != music
        || gate != music + 0x60 || submit != music + 0x309) return;
    const auto eligibility = CallTarget(calls + 11);
    if(eligibility + 0x68 != muteCall) return;
    const auto inventory = CallTarget(listener);
    const auto demo = CallTarget(gate);
    if(eligibility < text.Start || eligibility + 16 >= text.End
        || inventory < text.Start || inventory + 8 >= text.End
        || demo < text.Start || demo + 8 >= text.End
        || memcmp(reinterpret_cast<void *>(inventory), "\x48\x8B\x05", 3)
        || *reinterpret_cast<const unsigned char *>(inventory + 7) != 0xC3
        || memcmp(reinterpret_cast<void *>(demo), "\x48\x8D\x05", 3)
        || *reinterpret_cast<const unsigned char *>(demo + 7) != 0xC3) return;
    g_InventorySlot = reinterpret_cast<void **>(inventory + 7 + *reinterpret_cast<int32_t *>(inventory + 3));
    g_DemoSuppress = reinterpret_cast<const unsigned char *>(demo + 7 + *reinterpret_cast<int32_t *>(demo + 3) + 0x82);
    g_SoundSystemSlot = reinterpret_cast<void **>(submit + 7 + *reinterpret_cast<int32_t *>(submit + 3));
    g_OrgEligibility = reinterpret_cast<Eligibility_t>(eligibility);
    g_OrgPlayMusic = reinterpret_cast<PlayMusic_t>(music);
    LONG error = DetourTransactionBegin();
    if(NO_ERROR != error) return;
    error = DetourUpdateThread(GetCurrentThread());
    if(NO_ERROR == error) error = DetourAttach(&(PVOID &)g_OrgEligibility, ObserveEligibility);
    if(NO_ERROR == error) error = DetourAttach(&(PVOID &)g_OrgPlayMusic, ObservePlayMusic);
    if(NO_ERROR == error) error = DetourTransactionCommit();
    else DetourTransactionAbort();
    g_PlaybackObserversReady = NO_ERROR == error;
}

} // namespace

void MirvPovMvpMusic_ResolveAddresses(HMODULE clientDll, const void * localPawnGetter)
{
    g_PlayerGetterReturn = g_ListenerReturn = g_MutePredicateReturn = nullptr;
    g_ClientBase = reinterpret_cast<uintptr_t>(clientDll);
    g_ClientTextEnd = 0;
    if(!clientDll || !localPawnGetter) return;
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    sections.Next(IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
    if(sections.Eof()) return;
    const auto text = sections.GetMemRange();
    g_ClientTextEnd = text.End;
    const auto getter = FindUnique(text, kPlayerGetter);
    const auto muteCall = FindUnique(text, kMutePredicateCall);
    const auto listener = FindUnique(text, kListenerCalls);
    if(!getter || !muteCall || !listener
        || CallTarget(listener + 15) != getter
        || CallTarget(getter + 8) != reinterpret_cast<size_t>(localPawnGetter)
        || CallTarget(muteCall) != reinterpret_cast<size_t>(localPawnGetter)) {
        advancedfx::Warning("[mirv_pov_mvp_music] native context validation failed; MVP music fix unavailable.\n");
        return;
    }
    g_PlayerGetterReturn = reinterpret_cast<void *>(getter + 13);
    // TeamHealth/SoundCircle also detour the enclosing no-argument getter.
    // Their return-address guard preserves this outer call site all the way
    // into the shared slot getter, instead of its inner getter+13 call site.
    g_ListenerReturn = reinterpret_cast<void *>(listener + 20);
    g_MutePredicateReturn = reinterpret_cast<void *>(muteCall + 5);
    ResolvePlaybackObservers(text, listener, muteCall);
    advancedfx::Message("[mirv_pov_mvp_music] diagnostic=20260927-4 native_context_ready=1 playback_observers=%d\n", g_PlaybackObserversReady ? 1 : 0);
}

uint32_t MirvPovMvpMusic_PushEvent(SOURCESDK::CS2::IGameEvent * event)
{
    const uint32_t previous = g_EventPawn;
    // Nested non-MVP dispatch must suspend the outer context too.
    g_EventPawn = kNoPawn;
    TraceGate("no-event");
    if(!event) return previous;
    TraceGate("unresolved-context");
    if(!g_PlayerGetterReturn || !g_ListenerReturn || !g_MutePredicateReturn) return previous;
    TraceGate("feature-disabled");
    if(!MIRV_POV_FEATURE_ACTIVE("mvp_music")) return previous;
    __try {
        TraceGate("no-demo");
        if(!g_pEngineToClient || !g_pEngineToClient->GetDemoFile()) return previous;
        const char * name = event->GetName();
        TraceGate("not-mvp");
        if(!name || 0 != strcmp(name, "round_mvp")) return previous;
        auto pawn = GetCurrentPovPlayerPawn();
        TraceGate("no-pov-pawn");
        if(!pawn) return previous;
        TraceGate("not-player-pawn");
        if(!pawn->IsPlayerPawn()) return previous;
        TraceGate("non-playing-team");
        if(2 != pawn->GetTeam() && 3 != pawn->GetTeam()) return previous;
        g_EventPawn = pawn->GetHandle().ToInt();
        if(g_Trace) g_Trace->pawn = g_EventPawn;
        TraceGate("armed");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        g_EventPawn = kNoPawn;
        TraceGate("exception");
    }
    return previous;
}

void MirvPovMvpMusic_PopEvent(uint32_t previous)
{
    g_EventPawn = previous;
}

CEntityInstance * MirvPovMvpMusic_GetLocalPawn(void * caller)
{
    const bool playerCaller = caller == g_PlayerGetterReturn || caller == g_ListenerReturn;
    if(g_Trace) {
        if(!g_Trace->getterCalls && reinterpret_cast<uintptr_t>(caller) >= g_ClientBase
            && reinterpret_cast<uintptr_t>(caller) < g_ClientTextEnd)
            g_Trace->firstCallerRva = reinterpret_cast<uintptr_t>(caller) - g_ClientBase;
        ++g_Trace->getterCalls;
        if(playerCaller) ++g_Trace->playerQueries;
        if(caller == g_MutePredicateReturn) ++g_Trace->muteQueries;
    }
    if(kNoPawn == g_EventPawn || !MIRV_POV_FEATURE_ACTIVE("mvp_music")
        || (!playerCaller && caller != g_MutePredicateReturn)) return nullptr;
    __try {
        auto pawn = GetCurrentPovPlayerPawn();
        if(pawn && pawn->IsPlayerPawn() && pawn->GetHandle().ToInt() == g_EventPawn) {
            if(g_Trace) {
                if(playerCaller) ++g_Trace->playerOverrides;
                if(caller == g_MutePredicateReturn) ++g_Trace->muteOverrides;
            }
            return pawn;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void MirvPovMvpMusic_BeginTrace(SOURCESDK::CS2::IGameEvent * event, MirvPovMvpMusicTrace & trace)
{
    trace.previous = g_Trace;
    g_Trace = nullptr; // Suspend diagnostics during unrelated nested dispatch.
    if(!MirvPov_IsEnabled()) return;
    ++g_DispatchCount;
    __try {
        const char * name = event ? event->GetName() : nullptr;
        if(!name || strcmp(name, "round_mvp")) return;
        trace.mvp = true;
        ++g_MvpCount;
        g_Trace = &trace;
        trace.inventory = g_InventorySlot ? (*g_InventorySlot ? 1 : 0) : -1;
        trace.demoSuppress = g_DemoSuppress ? *g_DemoSuppress : -1;
        if(g_pEngineToClient) {
            auto demo = g_pEngineToClient->GetDemoFile();
            if(demo) trace.tick = demo->GetDemoTick();
        }
        if(g_MirvPovHashString) {
            auto kitKey = MirvPovDeathPanel_MakeGameEventKey("musickitid");
            auto muteKey = MirvPovDeathPanel_MakeGameEventKey("nomusic");
            auto userKey = MirvPovDeathPanel_MakeGameEventKey("userid");
            trace.controller = event->GetPlayerController(userKey) ? 1 : 0;
            if(!event->IsEmpty(kitKey)) trace.kit = event->GetInt(kitKey);
            trace.noMusic = event->IsEmpty(muteKey) ? 0 : event->GetInt(muteKey);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // A failed diagnostic read must not alter native event handling.
    }
}

static float ReadMusicCvar(const char * name)
{
    __try {
        if(!SOURCESDK::CS2::g_pCVar) return -1.0f;
        auto handle = SOURCESDK::CS2::g_pCVar->FindConVar(name, false);
        if(!handle.IsValid()) return -1.0f;
        auto value = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
        if(!value) return -1.0f;
        if(value->m_eVarType == SOURCESDK::CS2::EConVarType_Float32) return value->m_Value.m_flValue;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1.0f;
}

static void PrintMusicVolumes()
{
    const float musicVolume = ReadMusicCvar("snd_musicvolume");
    advancedfx::Message("[mirv_pov_mvp_music] volume=%.3f music_volume=%.3f gain=%.3f mvp_volume=%.3f\n",
        ReadMusicCvar("volume"), musicVolume, ReadMusicCvar("snd_gain"), ReadMusicCvar("snd_mvp_volume"));
    advancedfx::Message("[mirv_pov_mvp_music] casual=%.3f armsrace=%.3f deathmatch=%.3f\n",
        ReadMusicCvar("snd_mvp_volume_casual"), ReadMusicCvar("snd_mvp_volume_armsrace"),
        ReadMusicCvar("snd_mvp_volume_deathmatch"));
    if(musicVolume == 0.0f)
        advancedfx::Message("[mirv_pov_mvp_music] snd_musicvolume is 0: native MVP music is muted even when snd_mvp_volume is nonzero.\n");
}

void MirvPovMvpMusic_EndTrace(MirvPovMvpMusicTrace & trace)
{
    g_Trace = trace.previous;
    if(!trace.mvp) return;
    AcquireSRWLockExclusive(&g_LastTraceLock);
    g_LastTrace = trace;
    g_LastTrace.previous = nullptr;
    ReleaseSRWLockExclusive(&g_LastTraceLock);
    advancedfx::Message(
        "[mirv_pov_mvp_music] tick=%d kit=%d nomusic=%d gate=%s pawn=%08X "
        "getter_calls=%u first_caller_rva=0x%llX player_override=%u\n",
        trace.tick, trace.kit, trace.noMusic, trace.gate, trace.pawn,
        trace.getterCalls, static_cast<unsigned long long>(trace.firstCallerRva),
        trace.playerOverrides);
    advancedfx::Message("[mirv_pov_mvp_music] inventory=%d controller=%d demo_suppress=%d eligibility_calls=%u eligible=%d music_calls=%u native_kit=%d\n",
        trace.inventory, trace.controller, trace.demoSuppress, trace.eligibilityCalls, trace.eligible, trace.musicCalls, trace.nativeKit);
    advancedfx::Message("[mirv_pov_mvp_music] sound_observer=%d sound_calls=%u sound=%s\n",
        g_SoundObserverReady.load() ? 1 : 0, trace.soundCalls, trace.soundName[0] ? trace.soundName : "none");
    PrintMusicVolumes();
}

void MirvPovMvpMusic_PrintStatus()
{
    advancedfx::Message("[mirv_pov_mvp_music] diagnostic=20260927-4 ready=%d active=%d "
        "dispatches=%llu mvp_events=%llu\n",
        g_PlayerGetterReturn && g_ListenerReturn && g_MutePredicateReturn ? 1 : 0,
        MIRV_POV_FEATURE_ACTIVE("mvp_music") ? 1 : 0,
        static_cast<unsigned long long>(g_DispatchCount.load()),
        static_cast<unsigned long long>(g_MvpCount.load()));
    MirvPovMvpMusicTrace last;
    AcquireSRWLockShared(&g_LastTraceLock);
    last = g_LastTrace;
    ReleaseSRWLockShared(&g_LastTraceLock);
    advancedfx::Message("[mirv_pov_mvp_music] playback_observers=%d last_tick=%d kit=%d gate=%s player_override=%u\n",
        g_PlaybackObserversReady ? 1 : 0, last.tick, last.kit, last.gate, last.playerOverrides);
    advancedfx::Message("[mirv_pov_mvp_music] inventory=%d controller=%d demo_suppress=%d eligibility_calls=%u eligible=%d music_calls=%u native_kit=%d\n",
        last.inventory, last.controller, last.demoSuppress, last.eligibilityCalls, last.eligible, last.musicCalls, last.nativeKit);
    advancedfx::Message("[mirv_pov_mvp_music] sound_observer=%d sound_calls=%u sound=%s\n",
        g_SoundObserverReady.load() ? 1 : 0, last.soundCalls, last.soundName[0] ? last.soundName : "none");
    PrintMusicVolumes();
}
