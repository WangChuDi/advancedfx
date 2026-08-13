#pragma once

#include <Windows.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

void MirvPovDeathCam_Initialize(HMODULE clientDll);
void MirvPovDeathCam_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPovDeathCam_UpdateDemoTick(int demoTick);
void MirvPovDeathCam_Reset();

bool MirvPovDeathCam_IsHooked();
