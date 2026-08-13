#pragma once

#include <Windows.h>

void MirvPovTest_Initialize(HMODULE clientDll);
bool MirvPovTest_SetEnabled(bool enabled);
bool MirvPovTest_IsEnabled();
bool MirvPovTest_IsHooked();
bool MirvPovTest_IsClDemoPredictOverridden();
