#pragma once

#include <Windows.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }
class CEntityInstance;

void MirvPovRadio_Initialize(HMODULE clientDll);
void MirvPovRadio_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPovRadio_HandleSoundEvent(CEntityInstance * sourcePawn, const char * soundName);
void MirvPovRadio_HandleEntityAdded(CEntityInstance * entity, int handle);
void MirvPovRadio_OnDemoTick(int demoTick);
void MirvPovRadio_Reset(const char * reason);
bool MirvPovRadio_IsAvailable();
int MirvPovRadio_GetMode();
const char * MirvPovRadio_GetModeDescription(int mode);
