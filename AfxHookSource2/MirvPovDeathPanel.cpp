#include "stdafx.h"

#include "MirvPovDeathPanel.h"

#include "Globals.h"
#include "addresses.h"
#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "MirvPanorama.h"
#include "MirvTime.h"
#include "WrpConsole.h"
#include "../shared/binutils.h"

#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <cstring>

MirvPovDeathPanelState g_MirvPovDeathPanelState;
thread_local CEntityInstance * g_MirvPovDeathPanelLocalPawnOverride = nullptr;
MirvPovHashString_t g_MirvPovHashString = nullptr;

SOURCESDK::CS2::CKV3MemberName MirvPovDeathPanel_MakeGameEventKey(const char * name)
{
    if(nullptr == name) return SOURCESDK::CS2::CKV3MemberName(0, -1, "");

    const size_t length = std::strlen(name);
    if(nullptr == g_MirvPovHashString) {
        return SOURCESDK::CS2::CKV3MemberName(0, -1, name);
    }

    const unsigned int hash = g_MirvPovHashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}
int MirvPovDeathPanel_TryGetHeadshot(SOURCESDK::CS2::IGameEvent * gameEvent)
{
    if(nullptr == gameEvent || nullptr == g_MirvPovHashString) return -1;
    __try {
        return gameEvent->GetInt(MirvPovDeathPanel_MakeGameEventKey("headshot"));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool IsExecutableAddress(const void * address)
{
    if(nullptr == address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if(0 == VirtualQuery(address, &info, sizeof(info))) return false;
    const DWORD protection = info.Protect & 0xFF;
    return MEM_COMMIT == info.State
        && (PAGE_EXECUTE == protection || PAGE_EXECUTE_READ == protection
            || PAGE_EXECUTE_READWRITE == protection || PAGE_EXECUTE_WRITECOPY == protection);
}

void MirvPovDeathPanel_ResolveAddresses(HMODULE clientDll)
{
    if(nullptr == clientDll) return;

    if(auto address = getAddress(clientDll,
        "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? "
        "0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20")) {
        g_MirvPovHashString = reinterpret_cast<MirvPovHashString_t>(address);
    }

    if(auto address = getAddress(clientDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 02 48 8B F9 48 8B CA")) {
        g_MirvPovDeathPanelState.originalHandlePlayerDeath = reinterpret_cast<MirvPovDeathPanelHandlePlayerDeathListener_t>(address);
    }

    auto constructorAddress = getAddress(clientDll,
        "48 89 5C 24 18 55 57 41 56 48 8B EC 48 83 EC 40 "
        "48 8B F9 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? "
        "48 8D 4F 20 E8 ?? ?? ?? ?? 45 33 F6 48 8D 05 ?? ?? ?? ?? 48 89 07");
    g_MirvPovDeathPanelState.originalConstructor = IsExecutableAddress(reinterpret_cast<void *>(constructorAddress))
        ? reinterpret_cast<MirvPovDeathPanelConstructor_t>(constructorAddress) : nullptr;

    auto destructorAddress = getAddress(clientDll,
        "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 "
        "E8 ?? ?? ?? ?? F6 C3 01 74 ?? BA F0 00 00 00 "
        "48 8B CF E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C7 "
        "48 83 C4 20 5F C3");
    g_MirvPovDeathPanelState.originalDestructor = IsExecutableAddress(reinterpret_cast<void *>(destructorAddress))
        ? reinterpret_cast<MirvPovDeathPanelDestructor_t>(destructorAddress) : nullptr;

    g_MirvPovDeathPanelState.hide = reinterpret_cast<MirvPovDeathPanelHide_t>(getAddress(clientDll,
        "48 89 5C 24 ?? 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B D9 8B 15 ?? ?? ?? ?? C6 05"));
    g_MirvPovDeathPanelState.setMainVisible = reinterpret_cast<MirvPovDeathPanelSetVisible_t>(getAddress(clientDll,
        "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B D9 44 8B 05 ?? ?? ?? ?? 0F B6 F2"));
    g_MirvPovDeathPanelState.setSecondaryVisible = reinterpret_cast<MirvPovDeathPanelSetVisible_t>(getAddress(clientDll,
        "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B F9 44 8B 05"));
    g_MirvPovDeathPanelState.show = reinterpret_cast<MirvPovDeathPanelShow_t>(getAddress(clientDll,
        "40 57 41 56 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B F9 C6 05 ?? ?? ?? ?? ?? 33 C9 F3 0F 10 40 30"));
    g_MirvPovDeathPanelState.originalGetLocalPawn = reinterpret_cast<MirvPovDeathPanelGetLocalPawn_t>(getAddress(clientDll,
        "48 83 EC 28 83 F9 FF 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 4C 8D 05 ?? ?? ?? ?? 33 D2 4D 8B 04 C0 4D 85 C0"));

    auto replayGate = reinterpret_cast<unsigned char *>(getAddress(clientDll,
        "48 8D 0D ?? ?? ?? ?? 44 89 87 08 01 00 00 45 33 F6 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? BA FF FF FF FF 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 0B 48 8B 05 ?? ?? ?? ?? 48 8B 40 08 44 38 30"));
    if(nullptr != replayGate) {
        g_MirvPovDeathPanelState.replayObject = replayGate + 42 + *reinterpret_cast<int32_t *>(replayGate + 38);
        g_MirvPovDeathPanelState.resolveReplayValue = reinterpret_cast<MirvPovDeathPanelResolveReplayValue_t>(replayGate + 47 + *reinterpret_cast<int32_t *>(replayGate + 43));
        g_MirvPovDeathPanelState.replayFallbackObject = reinterpret_cast<void **>(replayGate + 59 + *reinterpret_cast<int32_t *>(replayGate + 55));
    }
}

size_t MirvPovDeathPanel_ResolveEntityTokenAddress(HMODULE clientDll)
{
    if(nullptr == clientDll) return 0;
    return getAddress(clientDll,
        "40 53 48 83 EC ?? 8B 51 ?? 48 8B D9 83 FA FF 0F 84 ?? ?? ?? ?? "
        "4C 8B 0D ?? ?? ?? ??");
}

// The observer target is the lifecycle boundary. This is only a corruption or
// lost-observer failsafe; normal playback ends the panel on a target change.
static constexpr int kPovDeathPanelReapplyFrameWindow = 1024;

// The native hide routine clears both Panorama classes and the game's global
// DeathPanel-active flag. In POV playback it can run after player_death has
// populated the panel, so keep it from invalidating the current banner until
// the observer target changes and the normal cleanup path disarms the state.
__int64 __fastcall MirvPovDeathPanel_HideWhileAlive(u_char * deathPanel)
{
	const bool suppress = MirvPov_IsEnabled()
		&& MirvPov_IsDeathFeedbackEnabled()
		&& g_MirvPovDeathPanelState.reapplyArmed
		&& nullptr != deathPanel
		&& deathPanel == g_MirvPovDeathPanelState.reapplyPanel;
	if (suppress) {
		const int currentFrame = g_MirvTime.framecount_get();
		if (false
			&& (g_MirvPovDeathPanelState.lastSuppressedHideFrame < 0
				|| currentFrame - g_MirvPovDeathPanelState.lastSuppressedHideFrame >= 64)) {
			MIRV_POV_DIAGNOSTIC_MESSAGE(
				"[mirv_pov_feedback] DeathPanel native hide suppressed panel=%p "
				"frame=%d curtime=%.3f\n",
				deathPanel,
				currentFrame,
				g_MirvTime.curtime_get());
		}
		g_MirvPovDeathPanelState.lastSuppressedHideFrame = currentFrame;
		return 1;
	}

	return nullptr != g_MirvPovDeathPanelState.hide
		? g_MirvPovDeathPanelState.hide(deathPanel)
		: 0;
}

static u_char * DeathPanel_FindChildById(void * parentPanel, const char * panelId)
{
	if(nullptr == parentPanel || nullptr == panelId) return nullptr;
	return reinterpret_cast<u_char *>(MirvPanorama_FindChildInLayoutFile(
		reinterpret_cast<u_char *>(parentPanel),
		panelId));
}

bool DeathPanel_ForceVisibility(
	u_char * deathPanel,
	unsigned int & actionMask,
	unsigned long & exceptionCode)
{
	exceptionCode = 0;
	if(nullptr == deathPanel) return false;

	bool invoked = false;
	bool rootHiddenBefore = false;
	bool mainHiddenBefore = false;
	bool mainNativeVisibleBefore = false;
	bool secondaryHiddenBefore = false;
	__try {
		void * rootPanel = *reinterpret_cast<void **>(deathPanel + 0x08);
		void * mainPanel = DeathPanel_FindChildById(rootPanel, "DeathPanel");
		void * secondaryPanel = DeathPanel_FindChildById(rootPanel, "DeathPanelSS");

		rootHiddenBefore = Panorama_HasPanelClass(rootPanel, "DeathPanelRoot--Hidden");
		mainHiddenBefore = Panorama_HasPanelClass(mainPanel, "DeathPanel--Hidden");
		secondaryHiddenBefore = Panorama_HasPanelClass(secondaryPanel, "DeathPanelSS--Hidden");
		mainNativeVisibleBefore = *reinterpret_cast<unsigned char *>(deathPanel + 0x1A1) != 0;

			if(nullptr != rootPanel
				&& rootHiddenBefore
				&& Panorama_SetPanelClass(rootPanel, "DeathPanelRoot--Hidden", false)) {
			actionMask |= DeathPanelAction_RemoveHiddenClass;
					invoked = true;
		}

			// The root class only controls the outer layer. The reference image is
			// the ordinary #DeathPanel child, which has its own Hidden/FadeIn state.
			if(nullptr != mainPanel) {
				if(mainHiddenBefore && Panorama_SetPanelClass(mainPanel, "DeathPanel--Hidden", false)) {
					actionMask |= DeathPanelAction_RemoveHiddenClass;
					invoked = true;
		}
			}

		// #DeathPanelSS is the screenshot/replay variant, not the banner in the
		// reference image. Keep that alternate panel collapsed.
			if(nullptr != secondaryPanel
				&& !secondaryHiddenBefore
				&& Panorama_SetPanelClass(secondaryPanel, "DeathPanelSS--Hidden", true)) {
			invoked = true;
		}

			// sub_180E08700 is the game's authoritative main-panel transition. It
			// adds DeathPanel--FadeIn and updates +0x1A1. Calling it every frame
			// restarts the CSS transition and leaves the banner nearly transparent,
			// so only repair a state that is actually hidden or inactive.
			if(nullptr != g_MirvPovDeathPanelState.setMainVisible
				&& (mainHiddenBefore || !mainNativeVisibleBefore)) {
					g_MirvPovDeathPanelState.setMainVisible(deathPanel, true);
				actionMask |= DeathPanelAction_MainVisible;
				invoked = true;
			}

			if(nullptr != g_MirvPovDeathPanelState.setSecondaryVisible && !secondaryHiddenBefore) {
				// Native Show calls sub_180E08820(panel, 0): keep the
				// screenshot container hidden and clear its flash class.
				g_MirvPovDeathPanelState.setSecondaryVisible(deathPanel, false);
			actionMask |= DeathPanelAction_SecondaryVisible;
			invoked = true;
		}

		if(invoked) actionMask |= DeathPanelAction_NativeVisibilityFallback;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				exceptionCode = GetExceptionCode();
				return false;
			}
			return invoked;
}

static void DeathPanel_DisarmPovReapply()
{
	g_MirvPovDeathPanelState.reapplyPanel = nullptr;
	g_MirvPovDeathPanelState.reapplyPawn = nullptr;
	g_MirvPovDeathPanelState.reapplyPawnHandle = 0xFFFFFFFFu;
	g_MirvPovDeathPanelState.reapplyFrame = -1;
	g_MirvPovDeathPanelState.lastRefreshFrame = -1;
	g_MirvPovDeathPanelState.lastSuppressedHideFrame = -1;
	g_MirvPovDeathPanelState.reapplyArmed = false;
}

static bool DeathPanel_TryGetObserverTarget(uint32_t & targetHandle)
{
	targetHandle = 0xFFFFFFFFu;
	__try {
		uint8_t observerMode = 0;
		return MirvPov_GetObserverState(observerMode, targetHandle);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		targetHandle = 0xFFFFFFFFu;
		return false;
	}
}

bool MirvPovDeathPanel_Reapply(const char * source)
{
	if(!g_MirvPovDeathPanelState.reapplyArmed
		|| !MirvPov_IsEnabled()
		|| !MirvPov_IsDeathFeedbackEnabled()
		|| nullptr == g_MirvPovDeathPanelState.reapplyPanel
		|| nullptr == g_MirvPovDeathPanelState.show) {
		if(false) {
			MIRV_POV_DIAGNOSTIC_MESSAGE(
				"[mirv_pov_feedback] DeathPanel post-dispatch reapply skipped source=%s "
				"armed=%d enabled=%d panel=%p show=%p\n",
				source ? source : "[unknown]",
				g_MirvPovDeathPanelState.reapplyArmed ? 1 : 0,
				MirvPov_IsEnabled() ? 1 : 0,
				g_MirvPovDeathPanelState.reapplyPanel,
				reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(g_MirvPovDeathPanelState.show)));
		}
		return false;
	}

	const int currentFrame = g_MirvTime.framecount_get();
	const int age = g_MirvPovDeathPanelState.reapplyFrame >= 0 && currentFrame >= g_MirvPovDeathPanelState.reapplyFrame
		? currentFrame - g_MirvPovDeathPanelState.reapplyFrame
		: 0;
	if(kPovDeathPanelReapplyFrameWindow < age) {
		if(false) {
			MIRV_POV_DIAGNOSTIC_MESSAGE(
				"[mirv_pov_feedback] DeathPanel lifetime failsafe expired frame=%d age=%d; clearing.\n",
				currentFrame,
				age);
		}
		MirvPovDeathPanel_Clear();
		return false;
	}
	u_char * panel = g_MirvPovDeathPanelState.reapplyPanel;
	CEntityInstance * pawn = g_MirvPovDeathPanelState.reapplyPawn;
	CEntityInstance * previousOverride = g_MirvPovDeathPanelLocalPawnOverride;
	if(nullptr != pawn) g_MirvPovDeathPanelLocalPawnOverride = pawn;

		bool needsNativeShow = true;
		bool rootHidden = false;
		bool mainHidden = false;
		bool nativeVisible = false;
		__try {
			void * rootPanel = *reinterpret_cast<void **>(panel + 0x08);
			void * mainPanel = DeathPanel_FindChildById(rootPanel, "DeathPanel");
			rootHidden = Panorama_HasPanelClass(rootPanel, "DeathPanelRoot--Hidden");
			mainHidden = Panorama_HasPanelClass(mainPanel, "DeathPanel--Hidden");
			nativeVisible = *reinterpret_cast<unsigned char *>(panel + 0x1A1) != 0;
			needsNativeShow = rootHidden || mainHidden || !nativeVisible;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			needsNativeShow = true;
		}

		bool showSucceeded = false;
		unsigned int visibilityActions = 0;
		unsigned long visibilityException = 0;
		if(needsNativeShow) {
				__try {
					g_MirvPovDeathPanelState.show(panel);
				showSucceeded = true;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				showSucceeded = false;
			}
		}
		g_MirvPovDeathPanelLocalPawnOverride = previousOverride;

	// The native Show routine is authoritative for content and transition state;
	// complete only its native visibility helpers if an observer update left one
	// of the Panorama hidden classes behind.
		if(showSucceeded) {
			DeathPanel_ForceVisibility(panel, visibilityActions, visibilityException);
		}

			return showSucceeded || 0 != visibilityActions;
	}

void MirvPovDeathPanel_Update()
	{
		if(!g_MirvPovDeathPanelState.reapplyArmed || !MirvPov_IsEnabled()
			|| !MirvPov_IsDeathFeedbackEnabled()
			|| nullptr == g_MirvPovDeathPanelState.reapplyPanel) return;

		const int currentFrame = g_MirvTime.framecount_get();
		const int age = g_MirvPovDeathPanelState.reapplyFrame >= 0 && currentFrame >= g_MirvPovDeathPanelState.reapplyFrame
			? currentFrame - g_MirvPovDeathPanelState.reapplyFrame
			: 0;
		if(kPovDeathPanelReapplyFrameWindow < age) {
			if(false) {
				MIRV_POV_DIAGNOSTIC_MESSAGE(
					"[mirv_pov_feedback] DeathPanel lifetime failsafe expired during frame update "
					"frame=%d age=%d; clearing.\n",
					currentFrame,
					age);
			}
			MirvPovDeathPanel_Clear();
			return;
		}

			uint32_t currentTargetHandle = 0xFFFFFFFFu;
			if(DeathPanel_TryGetObserverTarget(currentTargetHandle)
				&& currentTargetHandle != 0xFFFFFFFFu
				&& g_MirvPovDeathPanelState.reapplyPawnHandle != 0xFFFFFFFFu
				&& currentTargetHandle != g_MirvPovDeathPanelState.reapplyPawnHandle) {
					if(false) {
						MIRV_POV_DIAGNOSTIC_MESSAGE(
							"[mirv_pov_feedback] DeathPanel lifetime ended on observer target change "
							"frame=%d age=%d expectedPawn=%p expectedHandle=0x%08x "
							"currentTarget=0x%08x curtime=%.3f; clearing.\n",
							currentFrame,
							age,
							g_MirvPovDeathPanelState.reapplyPawn,
							static_cast<unsigned int>(g_MirvPovDeathPanelState.reapplyPawnHandle),
							static_cast<unsigned int>(currentTargetHandle),
							g_MirvTime.curtime_get());
					}
					MirvPovDeathPanel_Clear();
					return;
			}

			unsigned int actionMask = 0;
			unsigned long exceptionCode = 0;
			DeathPanel_ForceVisibility(
				g_MirvPovDeathPanelState.reapplyPanel,
					actionMask,
				exceptionCode);
		g_MirvPovDeathPanelState.lastRefreshFrame = currentFrame;
	}

void MirvPovDeathPanel_Clear()
{
	DeathPanel_DisarmPovReapply();
	u_char * panel = g_MirvPovDeathPanelState.lastPanel;
	if(nullptr != panel && nullptr != g_MirvPovDeathPanelState.hide) {
		__try {
			g_MirvPovDeathPanelState.hide(panel);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	g_MirvPovDeathPanelState.lastPanel = nullptr;
}
