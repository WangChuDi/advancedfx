#pragma once

#include <Windows.h>

void MirvPovSoundCircle_Initialize(HMODULE clientDll);
bool MirvPovSoundCircle_IsHooked();
bool MirvPovSoundCircle_IsDirectEmitterReady();

// Emit a native CS2 sound-event with an explicit source entity.  The caller
// must pass the player pawn/entity entry index so the engine can apply normal
// positional attenuation and stereo spatialization.  This never falls back
// to a listener-local console "play" command.
bool MirvPovSoundCircle_EmitSoundAtEntity(const char * soundName, int sourceEntityIndex);

// Emit the same native CS2 sound-event path with entidx=-1, matching the
// original SendAudio/RawAudio user-message behavior (global, non-spatialized).
bool MirvPovSoundCircle_EmitSoundGlobal(const char * soundName);
