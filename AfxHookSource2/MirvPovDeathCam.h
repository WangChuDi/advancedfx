#pragma once

#include <Windows.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

void MirvPovDeathCam_Initialize(HMODULE clientDll);
void MirvPovDeathCam_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPovDeathCam_UpdateDemoTick(int demoTick);
void MirvPovDeathCam_UpdateLifecycle();
void MirvPovDeathCam_Reset();
void MirvPovDeathCam_ResetPresentation();

bool MirvPovDeathCam_IsHooked();
class CEntityInstance;
CEntityInstance * MirvPovDeathCam_GetEffectPawn(void * returnAddress);
