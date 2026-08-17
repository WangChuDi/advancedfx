#include "stdafx.h"

#include "MirvPovSoundCircle.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "Globals.h"
#include "MirvPovRadio.h"
#include "SchemaSystem.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include <Windows.h>
#include <atomic>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

using GetLocalPawn_t = CEntityInstance * (__fastcall *)();
using DoStartSoundEvent_t = void (__fastcall *)(void *, void *);
// IDA: sub_1803D91F0 is the native helper used by CS2's direct sound-event
// producers.  It accepts the resolved event id and source entity directly,
// and normalizes pitch when the caller passes -1.  This avoids constructing a
// temporary network-message CBufferString for synthetic radio playback.
using StartSoundEvent_t = void * (__fastcall *)(
    void * soundOpGameSystem,
    void * result,
    void * context,
    uint32_t eventId,
    uint32_t sourceEntityIndex,
    int16_t pitch,
    double volume);
using QueueRadarSound_t = void (__fastcall *)(CEntityInstance *, int, float, bool);
using HashSoundField_t = uint32_t (__fastcall *)(const char *, uint32_t);
using ResolveSoundEventId_t = uint32_t (__fastcall *)(void *, const char *, bool);
using IsSoundEventValid_t = bool (__fastcall *)(void *, uint32_t);
using GetSoundEventName_t = const char * (__fastcall *)(void *, uint32_t);
using GetSoundFieldCount_t = int (__fastcall *)(void *, uint32_t, uint32_t, bool *, bool);
using GetSoundFieldValue_t = bool (__fastcall *)(void *, uint32_t, uint32_t, const void **, int, bool);

GetLocalPawn_t g_OrgGetLocalPawn = nullptr;
DoStartSoundEvent_t g_OrgDoStartSoundEvent = nullptr;
StartSoundEvent_t g_StartSoundEvent = nullptr;
QueueRadarSound_t g_QueueRadarSound = nullptr;
void ** g_SoundEventInterfaceSlot = nullptr;
void * g_SoundGateReturnAddresses[3] = {};
uint32_t g_DistanceCurveKey = 0;
bool g_Hooked = false;
std::atomic_bool g_NativeProducerSeen = false;
SRWLOCK g_SyntheticSoundLock = SRWLOCK_INIT;
void * g_LastSoundOpGameSystem = nullptr;
unsigned char g_LastSoundMessageTemplate[0x68] = {};
bool g_LastSoundMessageTemplateValid = false;

constexpr size_t kSoundEventMessageSize = 0x68;

// The field at message+0x48 is a pointer to a CUtlString-like object.  For
// short event names the object stores the bytes inline and uses capacity <=
// 0x0f; the native handler then reads the object address itself as the string
// pointer.  Keeping this exact shape lets us reuse DoStartSoundEvent without
// invoking any listener-local console command.
struct SyntheticSoundString {
    char inlineData[16];
    int32_t length;
    int32_t reserved;
    uint64_t capacity;
};
static_assert(offsetof(SyntheticSoundString, length) == 0x10, "sound string length offset");
static_assert(offsetof(SyntheticSoundString, capacity) == 0x18, "sound string capacity offset");

void * GetSoundEventInterface();

bool ReadSoundEventInterface(
    void *& outInterface,
    ResolveSoundEventId_t & outResolve,
    IsSoundEventValid_t & outIsValid,
    GetSoundEventName_t & outGetName)
{
    outInterface = nullptr;
    outResolve = nullptr;
    outIsValid = nullptr;
    outGetName = nullptr;
    __try {
        outInterface = GetSoundEventInterface();
        if(nullptr == outInterface) return false;
        void ** vtable = *reinterpret_cast<void ***>(outInterface);
        if(nullptr == vtable) return false;
        outResolve = reinterpret_cast<ResolveSoundEventId_t>(vtable[0]);
        // IDA: qword_1825C9508 + 8 vtable slot 0 resolves a name to an
        // event id, slot 1 validates that id, and slot 2 returns its canonical
        // name.  A non-zero resolver result alone is only a hash/cache key;
        // unknown names also produce a non-zero value on this build.
        outIsValid = reinterpret_cast<IsSoundEventValid_t>(vtable[1]);
        outGetName = reinterpret_cast<GetSoundEventName_t>(vtable[2]);
        return nullptr != outResolve && nullptr != outIsValid && nullptr != outGetName;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        outInterface = nullptr;
        outResolve = nullptr;
        outIsValid = nullptr;
        outGetName = nullptr;
        return false;
    }
}

bool IsExecutableAddress(const void * address)
{
    if(nullptr == address) return false;
    MEMORY_BASIC_INFORMATION information = {};
    if(0 == VirtualQuery(address, &information, sizeof(information))) return false;
    if(MEM_COMMIT != information.State || 0 != (information.Protect & PAGE_GUARD)) return false;
    const DWORD protection = information.Protect & 0xff;
    return PAGE_EXECUTE == protection
        || PAGE_EXECUTE_READ == protection
        || PAGE_EXECUTE_READWRITE == protection
        || PAGE_EXECUTE_WRITECOPY == protection;
}

uint32_t FinalizeSoundFieldHash(uint32_t hash)
{
    uint32_t result = hash * 0x5bd1e995;
    result ^= 0x66608f41;
    result = (result ^ (result >> 13)) * 0x5bd1e995;
    return result ^ (result >> 15);
}

CEntityInstance * ResolveSoundSourcePawn(int entityIndex)
{
    if(entityIndex < 0 || 0x7ffe < entityIndex) return nullptr;
    CEntityInstance * entity = GetEntityFromIndex(entityIndex);
    if(nullptr == entity) return nullptr;
    if(entity->IsPlayerPawn()) return entity;

    if(0 == g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode
        || 0 == g_clientDllOffsets.CGameSceneNode.m_pParent
        || 0 == g_clientDllOffsets.CGameSceneNode.m_pOwner) return nullptr;
    unsigned char * sceneNode = *reinterpret_cast<unsigned char **>(
        reinterpret_cast<unsigned char *>(entity)
        + g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode);
    if(nullptr == sceneNode) return nullptr;
    unsigned char * parentNode = *reinterpret_cast<unsigned char **>(
        sceneNode + g_clientDllOffsets.CGameSceneNode.m_pParent);
    if(nullptr == parentNode) return nullptr;
    CEntityInstance * parent = *reinterpret_cast<CEntityInstance **>(
        parentNode + g_clientDllOffsets.CGameSceneNode.m_pOwner);
    return nullptr != parent && parent->IsPlayerPawn() ? parent : nullptr;
}

bool IsCurrentPovPlayer(CEntityInstance * pawn)
{
    if(nullptr == pawn) return false;
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr != povPawn && pawn == povPawn) return true;

    CEntityInstance * povController = GetCurrentPovPlayerController();
    if(nullptr == povController) return false;
    auto pawnController = pawn->GetPlayerControllerHandle();
    auto povControllerHandle = povController->GetHandle();
    return pawnController.IsValid()
        && povControllerHandle.IsValid()
        && pawnController.ToInt() == povControllerHandle.ToInt();
}

void * GetSoundEventInterface()
{
    void * interfaceBase = nullptr != g_SoundEventInterfaceSlot
        ? *g_SoundEventInterfaceSlot
        : nullptr;
    return nullptr != interfaceBase
        ? reinterpret_cast<unsigned char *>(interfaceBase) + 8
        : nullptr;
}

int GetNativeSoundRadius(void * soundEventInterface, uint32_t eventId)
{
    if(nullptr == soundEventInterface) return 0;
    void ** vtable = *reinterpret_cast<void ***>(soundEventInterface);
    auto getCount = reinterpret_cast<GetSoundFieldCount_t>(vtable[0x180 / sizeof(void *)]);
    auto getValue = reinterpret_cast<GetSoundFieldValue_t>(vtable[0x1a0 / sizeof(void *)]);
    if(nullptr == getCount || nullptr == getValue) return 0;

    const void * value = nullptr;
    if(getValue(soundEventInterface, eventId, 0x00442583, &value, 0, false)
        && nullptr != value
        && *reinterpret_cast<const float *>(value) <= 0.5f) return 0;

    float radius = 0.0f;
    value = nullptr;
    if(getValue(soundEventInterface, eventId, 0x7d58c040, &value, 0, false)
        && nullptr != value) {
        radius = *reinterpret_cast<const float *>(value);
    }
    if(radius <= 0.0f) {
        bool isArray = false;
        int count = getCount(
            soundEventInterface,
            eventId,
            g_DistanceCurveKey,
            &isArray,
            false);
        value = nullptr;
        if(isArray && 1 < count
            && getValue(
                soundEventInterface,
                eventId,
                g_DistanceCurveKey,
                &value,
                count - 1,
                false)
            && nullptr != value) {
            radius = *reinterpret_cast<const float *>(value);
        }
    }
    return 1.0f <= radius ? static_cast<int>(radius) : 0;
}

CEntityInstance * __fastcall New_GetLocalPawn()
{
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    CEntityInstance * nativePawn = g_OrgGetLocalPawn();
    MirvPov_PopHookReturnAddress(previousReturnAddress);
    if(!MirvPov_IsEnabled()) return nativePawn;

    int gate = returnAddress == g_SoundGateReturnAddresses[0]
        ? 0
        : (returnAddress == g_SoundGateReturnAddresses[1]
            ? 1
            : (returnAddress == g_SoundGateReturnAddresses[2] ? 2 : -1));
    if(gate < 0) return nativePawn;

    if(0 == gate) g_NativeProducerSeen = true;
    __try {
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn) return nativePawn;
        return povPawn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nativePawn;
    }
}

void __fastcall New_DoStartSoundEvent(void * soundOpGameSystem, void * netMessage)
{
    uint32_t eventHash = 0;
    CEntityInstance * sourcePawn = nullptr;
    bool isPov = false;
    __try {
        if(nullptr != netMessage) {
            unsigned char * message = reinterpret_cast<unsigned char *>(netMessage);
            eventHash = *reinterpret_cast<uint32_t *>(message + 0x54);
            int sourceEntityIndex = *reinterpret_cast<int *>(message + 0x60);
            sourcePawn = ResolveSoundSourcePawn(sourceEntityIndex);
            isPov = IsCurrentPovPlayer(sourcePawn);

            // Retain a native message template and the sound-op system so a
            // later event fallback can re-enter the real CS2 sound-event path
            // with a different event id/source entity.  The +0x48 string
            // pointer is overwritten by EmitSoundAtEntity and is never kept
            // from this temporary network message.
            AcquireSRWLockExclusive(&g_SyntheticSoundLock);
            memcpy(g_LastSoundMessageTemplate, message, kSoundEventMessageSize);
            g_LastSoundOpGameSystem = soundOpGameSystem;
            g_LastSoundMessageTemplateValid = true;
            ReleaseSRWLockExclusive(&g_SyntheticSoundLock);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    g_OrgDoStartSoundEvent(soundOpGameSystem, netMessage);

    if(!MirvPov_IsEnabled() || 0 == eventHash) return;

    __try {
        void * soundEventInterface = GetSoundEventInterface();
        if(nullptr == soundEventInterface) return;
        void ** vtable = *reinterpret_cast<void ***>(soundEventInterface);
        auto getName = reinterpret_cast<GetSoundEventName_t>(vtable[2]);
        auto resolveEventId = reinterpret_cast<ResolveSoundEventId_t>(vtable[0]);
        if(nullptr == getName || nullptr == resolveEventId) return;

        const char * name = getName(soundEventInterface, eventHash);
        if(nullptr == name) return;

        // Radio/throw/bomb sound events can be emitted with an entity index
        // that is not a player pawn (or -1 in demos).  Let the radio layer
        // consume the name and use its POV/controller fallback; radar sound
        // synthesis below still requires a resolved pawn.
        MirvPovRadio_HandleSoundEvent(sourcePawn, name);

        if(!isPov || nullptr == sourcePawn || g_NativeProducerSeen.load() || nullptr == strstr(name, ".Step")) return;

        uint32_t eventId = resolveEventId(soundEventInterface, name, true);
        if(0 == eventId) return;
        int radius = GetNativeSoundRadius(soundEventInterface, eventId);
        if(radius < 1) return;

        g_QueueRadarSound(sourcePawn, radius, 0.5f, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

void MirvPovSoundCircle_Initialize(HMODULE clientDll)
{
    if(g_Hooked || nullptr == clientDll) return;

    size_t playerSoundProducer = getAddress(
        clientDll,
        "8B D8 E8 ?? ?? ?? ?? 48 3B C6 0F 85 ?? ?? ?? ?? 0F 28 DE 44 88 74 24 20 44 8B C3 48 8D 0D ?? ?? ?? ?? 48 8B D6 E8 ?? ?? ?? ??");
    size_t queueRadarSound = getAddress(
        clientDll,
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 0F 29 74 24 30 41 0F B6 F9 0F 28 F2 8B F2 48 8B D9 E8 ?? ?? ?? ?? 48 3B C3 75 ??");
    size_t positionUpdaterBody = getAddress(
        clientDll,
        "48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? 00 00 48 89 6C 24 ?? 48 8D 54 24 ?? 48 89 74 24 ?? 48 8B C8");
    if(0 == playerSoundProducer || 0 == queueRadarSound || 0 == positionUpdaterBody) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native radar sound path was not found.\n");
        return;
    }
    uint8_t * callSites[3] = {
        reinterpret_cast<uint8_t *>(playerSoundProducer) + 2,
        reinterpret_cast<uint8_t *>(queueRadarSound) + 0x20,
        reinterpret_cast<uint8_t *>(positionUpdaterBody) + 3
    };
    if(0xe8 != callSites[0][0] || 0xe8 != callSites[1][0] || 0xe8 != callSites[2][0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native local-Pawn calls changed.\n");
        return;
    }

    uint8_t * localPawnTargets[3];
    for(int i = 0; i < 3; ++i) {
        int32_t relative = *reinterpret_cast<int32_t *>(callSites[i] + 1);
        localPawnTargets[i] = callSites[i] + 5 + relative;
    }
    if(localPawnTargets[0] != localPawnTargets[1]
        || localPawnTargets[0] != localPawnTargets[2]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native local-Pawn target validation failed.\n");
        return;
    }
    if(!IsExecutableAddress(localPawnTargets[0])) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native local-Pawn target is not executable.\n");
        return;
    }
    size_t doStartSoundEvent = getAddress(
        clientDll,
        "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 81 EC 90 00 00 00 8B 42 40");
    if(0 == doStartSoundEvent) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native SOS start-sound handler was not found.\n");
        return;
    }

    // IDA Pro: sub_1803D91F0 (CSoundOpGameSystem direct StartSoundEvent).
    // Keep this pattern separate from DoStartSoundEvent: the two functions
    // share the same sound core but have different argument layouts.
    size_t startSoundEventAnchor = getAddress(
        clientDll,
        "48 8B FA 48 8B F1 BA FF FF FF FF 48 8D 0D");
    // The stable anchor is 0x18 bytes into sub_1803D91F0 on the analyzed
    // client.dll.  The shorter pattern avoids depending on the RIP-relative
    // convar address embedded immediately after the anchor.
    size_t startSoundEvent = 0 < startSoundEventAnchor
        ? startSoundEventAnchor - 0x18
        : 0;
    uint8_t * createSoundEventCall = reinterpret_cast<uint8_t *>(doStartSoundEvent) + 0xec;
    if(0xe8 != createSoundEventCall[0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native SOS create-sound call validation failed.\n");
        return;
    }
    int32_t createSoundEventRelative = *reinterpret_cast<int32_t *>(createSoundEventCall + 1);
    uint8_t * createSoundEvent = createSoundEventCall + 5 + createSoundEventRelative;
    uint8_t * soundEventGlobalLoad = createSoundEvent + 0xb8;
    const uint8_t expectedGlobalLoadTail[] = {0x41, 0x8b, 0xd4, 0x48, 0x83, 0xc1, 0x08};
    if(0x48 != soundEventGlobalLoad[0]
        || 0x8b != soundEventGlobalLoad[1]
        || 0x0d != soundEventGlobalLoad[2]
        || 0 != memcmp(
            soundEventGlobalLoad + 7,
            expectedGlobalLoadTail,
            sizeof(expectedGlobalLoadTail))) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native sound-event interface validation failed.\n");
        return;
    }
    int32_t soundEventGlobalRelative = *reinterpret_cast<int32_t *>(soundEventGlobalLoad + 3);
    void ** soundEventInterfaceSlot = reinterpret_cast<void **>(
        soundEventGlobalLoad + 7 + soundEventGlobalRelative);

    size_t hashSoundField = getAddress(
        clientDll,
        "48 89 5C 24 08 44 0F B6 09 44 8B DA 4C 8B C1 41 8D 41 BF 3C 19 77 04 41 80 C1 20");
    if(0 == hashSoundField) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native sound-field hash helper was not found.\n");
        return;
    }
    auto hashField = reinterpret_cast<HashSoundField_t>(hashSoundField);
    static const char distanceCurvePath[] = "public.distance_volume_mapping_curve";
    uint32_t curveHash = hashField(distanceCurvePath + 16, 0x26835b00);
    curveHash = hashField(distanceCurvePath + 24, curveHash);
    uint32_t distanceCurveKey = FinalizeSoundFieldHash(curveHash);
    if(0xd7da5bc8 != distanceCurveKey) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native distance-curve key validation failed.\n");
        return;
    }

    g_OrgGetLocalPawn = reinterpret_cast<GetLocalPawn_t>(localPawnTargets[0]);
    g_OrgDoStartSoundEvent = reinterpret_cast<DoStartSoundEvent_t>(doStartSoundEvent);
    g_StartSoundEvent = reinterpret_cast<StartSoundEvent_t>(startSoundEvent);
    g_QueueRadarSound = reinterpret_cast<QueueRadarSound_t>(queueRadarSound);
    g_SoundEventInterfaceSlot = soundEventInterfaceSlot;
    g_DistanceCurveKey = distanceCurveKey;
    for(int i = 0; i < 3; ++i) g_SoundGateReturnAddresses[i] = callSites[i] + 5;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgGetLocalPawn, New_GetLocalPawn);
    DetourAttach(&(PVOID &)g_OrgDoStartSoundEvent, New_DoStartSoundEvent);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgGetLocalPawn = nullptr;
        g_OrgDoStartSoundEvent = nullptr;
        g_StartSoundEvent = nullptr;
        g_QueueRadarSound = nullptr;
        g_SoundEventInterfaceSlot = nullptr;
        g_DistanceCurveKey = 0;
        g_SoundGateReturnAddresses[0] = nullptr;
        g_SoundGateReturnAddresses[1] = nullptr;
        g_SoundGateReturnAddresses[2] = nullptr;
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_sound_circle] Native sound-circle detour failed.\n");
        return;
    }

    g_Hooked = true;
}

bool MirvPovSoundCircle_IsHooked()
{
    return g_Hooked;
}

bool MirvPovSoundCircle_IsDirectEmitterReady()
{
    return g_Hooked && nullptr != g_StartSoundEvent;
}

bool MirvPovSoundCircle_EmitSoundAtEntity(const char * soundName, int sourceEntityIndex)
{
    if(!g_Hooked || nullptr == g_OrgDoStartSoundEvent
        || nullptr == soundName || '\0' == soundName[0]
        || sourceEntityIndex < -1) return false;
    // Re-resolve the pawn at emission time.  A demo seek or entity slot reuse
    // must never make a delayed fallback play from an unrelated entity.
    if(sourceEntityIndex >= 0 && nullptr == ResolveSoundSourcePawn(sourceEntityIndex)) return false;

    void * soundEventInterface = nullptr;
    ResolveSoundEventId_t resolveEventId = nullptr;
    IsSoundEventValid_t isSoundEventValid = nullptr;
    GetSoundEventName_t getName = nullptr;
    if(!ReadSoundEventInterface(soundEventInterface, resolveEventId, isSoundEventValid, getName)) return false;

    uint32_t eventId = 0;
    __try {
        eventId = resolveEventId(soundEventInterface, soundName, true);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        eventId = 0;
    }
    if(0 == eventId) return false;
    __try {
        if(!isSoundEventValid(soundEventInterface, eventId)) return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const char * resolvedName = nullptr;
    __try {
        resolvedName = getName(soundEventInterface, eventId);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        resolvedName = nullptr;
    }
    if(nullptr == resolvedName || '\0' == resolvedName[0]) return false;

    void * soundOpGameSystem = nullptr;
    alignas(16) unsigned char message[kSoundEventMessageSize] = {};
    bool templateValid = false;
    AcquireSRWLockShared(&g_SyntheticSoundLock);
    soundOpGameSystem = g_LastSoundOpGameSystem;
    templateValid = g_LastSoundMessageTemplateValid;
    if(templateValid) memcpy(message, g_LastSoundMessageTemplate, sizeof(message));
    ReleaseSRWLockShared(&g_SyntheticSoundLock);
    if(nullptr == soundOpGameSystem) return false;

    // Prefer the same direct helper used by native CS2 sound producers.  It
    // performs the complete sound-op enqueue and uses the supplied pawn entry
    // for positional attenuation.  Passing pitch=-1 is important: the helper
    // then asks the sound system for its current/default pitch.  A synthetic
    // network message with pitch=0 can be accepted by the parser but produce
    // no audible output.
    if(nullptr != g_StartSoundEvent) {
        alignas(16) unsigned char result[0x40] = {};
        __try {
            g_StartSoundEvent(
                soundOpGameSystem,
                result,
                nullptr,
                eventId,
                static_cast<uint32_t>(sourceEntityIndex < 0 ? -1 : sourceEntityIndex),
                static_cast<int16_t>(-1),
                0.0);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Fall through to the message-compatible path for older builds or
            // a client update where the direct helper signature changed.
        }
    }

    SyntheticSoundString soundString = {};
    // CBufferString stores up to 15 bytes inline.  Agent-qualified CS2 voice
    // events (for example professional.radiobotgo01) are longer, so use the
    // heap representation instead of silently truncating the event name.
    char longSoundName[256] = {};
    size_t length = strnlen_s(soundName, sizeof(longSoundName) - 1);
    if(0 == length || sizeof(longSoundName) - 1 <= length) return false;
    if(sizeof(soundString.inlineData) - 1 < length) {
        memcpy(longSoundName, soundName, length);
        longSoundName[length] = '\0';
        *reinterpret_cast<char **>(soundString.inlineData) = longSoundName;
        soundString.capacity = static_cast<uint64_t>(length + 1);
    } else {
        memcpy(soundString.inlineData, soundName, length);
        soundString.inlineData[length] = '\0';
        soundString.capacity = sizeof(soundString.inlineData) - 1;
    }
    soundString.length = static_cast<int32_t>(length);
    soundString.reserved = 0;

    __try {
        // The native handler only needs these fields for a regular sound
        // event. Clear flags/volume so stale network metadata cannot alter the
        // fallback's attenuation, then provide the player pawn as the source.
        *reinterpret_cast<uint32_t *>(message + 0x40) = 0;
        *reinterpret_cast<void **>(message + 0x48) = &soundString;
        *reinterpret_cast<int *>(message + 0x50) = 0;
        *reinterpret_cast<uint32_t *>(message + 0x54) = eventId;
        // -1 asks the native handler to use the sound system's default pitch.
        *reinterpret_cast<int16_t *>(message + 0x58) = -1;
        *reinterpret_cast<float *>(message + 0x5c) = 0.0f;
        *reinterpret_cast<int *>(message + 0x60) = sourceEntityIndex;

        // g_OrgDoStartSoundEvent is the Detours trampoline, so this call
        // bypasses New_DoStartSoundEvent and cannot recursively enter the
        // radio observer.  The string object remains alive for the duration
        // of the native call.
        g_OrgDoStartSoundEvent(soundOpGameSystem, message);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    (void)templateValid;
    (void)resolvedName;
    return true;
}

bool MirvPovSoundCircle_EmitSoundGlobal(const char * soundName)
{
    // The native SendAudio/RawAudio path uses entidx=-1.  Keeping this as a
    // separate entry point makes the original-global and optional spatialized
    // fallback behaviors explicit at the radio layer.
    return MirvPovSoundCircle_EmitSoundAtEntity(soundName, -1);
}
