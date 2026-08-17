#pragma once

#include <Windows.h>

void MirvPovHud_ApplyPatches(HMODULE clientDll);
void MirvPovHud_RemovePatches();
void MirvPovHud_OnPanoramaDllLoaded(HMODULE panoramaDll);
void MirvPovHud_OnLevelInitPreEntity();
void MirvPovHud_ReapplyPanelState();
void MirvPovHud_UpdateSeekDetection(int curTick);
bool MirvPovHud_ShouldSuppressFrame();
void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath);
