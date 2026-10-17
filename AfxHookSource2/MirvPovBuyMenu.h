#pragma once
#include <Windows.h>

// Installs pass-through hooks only for the supported client build.
void MirvPovBuyMenu_Initialize(HMODULE clientDll);
void MirvPovBuyMenu_Update();
// Closes the simulated menu and releases runtime state; preserves the setting.
void MirvPovBuyMenu_Reset();
// Disabled by default. Also requires mirv_pov and offline demo playback.
void MirvPovBuyMenu_SetEnabled(bool enabled);
bool MirvPovBuyMenu_IsEnabled();
// Applies the independent input control even when the demo tick is paused.
void MirvPovBuyMenu_ReapplyInput();
// Restores native gloves or reapplies the observed gloves while paused, too.
void MirvPovBuyMenu_ReapplyGloves();
void MirvPovBuyMenu_PrintStatus();
