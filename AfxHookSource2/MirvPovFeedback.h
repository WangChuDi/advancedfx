#pragma once

#include <Windows.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

void MirvPovFeedback_Initialize(HMODULE clientDll);
void MirvPovFeedback_UpdatePovSelection();
void MirvPovFeedback_ResetPovSelection();
void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);
bool MirvPovFeedback_IsLocalPlayerVictim(SOURCESDK::CS2::IGameEvent * event);
bool MirvPovFeedback_IsRealLocalPlayerVictim(SOURCESDK::CS2::IGameEvent * event);
// Returns true when the event's userid resolves to the currently selected POV pawn.
bool MirvPovFeedback_IsCurrentPovVictim(
    SOURCESDK::CS2::IGameEvent * event,
    bool * headshot = nullptr);
