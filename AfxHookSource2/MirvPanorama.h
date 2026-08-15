#pragma once

#include "DeathMsg.h"
#include <cstddef>

bool MirvPanorama_InitStyleProperties(HMODULE panoramaDll);
void MirvPanorama_SetHudPanel(void** hudPanel);
void MirvPanorama_SetUIEngine(void** uiEngine);
void** MirvPanorama_GetHudPanel();
void** MirvPanorama_GetUIEngine();
void* MirvPanorama_FindChildInLayoutFile(void* parentPanel, const char* panelId);
bool Panorama_SetPanelClass(void* panel, const char* className, bool value);
bool Panorama_HasPanelClass(void* panel, const char* className);
bool Panorama_SetPanelOpacity(void* panel, float value);
bool Panorama_SetPanelVisible(void* panel, bool value);

