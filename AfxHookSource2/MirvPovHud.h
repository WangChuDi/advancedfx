#pragma once

#include <Windows.h>
#include <stdint.h>

using MirvPovMoneyUpdate_t = void (__fastcall *)(void *);

uintptr_t MirvPovHud_FindMoneyUpdate(HMODULE clientDll);
bool MirvPovHud_EnsureMoneyUpdateHook(HMODULE clientDll);
void MirvPovHud_ApplyPatches(HMODULE clientDll);
void MirvPovHud_RemovePatches();
void MirvPovHud_OnPanoramaDllLoaded(HMODULE panoramaDll);
void MirvPovHud_OnLevelInitPreEntity();
void MirvPovHud_ReapplyPanelState();
void MirvPovHud_UpdateRoundDamage();
void MirvPovHud_PrintRoundDamageState();
void MirvPovHud_UpdateSeekDetection(int curTick);
bool MirvPovHud_ShouldSuppressFrame();
void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath);
