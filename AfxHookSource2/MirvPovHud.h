#pragma once

#include <Windows.h>

void MirvPovHud_ApplyPatches(HMODULE clientDll);
void MirvPovHud_RemovePatches();
void MirvPovHud_UpdateSeekDetection(int curTick);
void MirvPovHud_InvalidatePanelCache();
bool MirvPovHud_ShouldSuppressFrame();
void MirvPovHud_OnPanoramaDllLoaded(HMODULE panoramaDll);
void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath);
