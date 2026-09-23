#pragma once

#include <Windows.h>

enum class MirvPovVoiceMode { Team, All, Enemy };

void MirvPov_HookVoiceHud(HMODULE clientDll);
bool MirvPovVoice_IsEnabled();
void MirvPovVoice_SetEnabled(bool enabled);
MirvPovVoiceMode MirvPovVoice_GetMode();
const char * MirvPovVoice_GetModeName();
void MirvPovVoice_SetMode(MirvPovVoiceMode mode);
void MirvPov_ResetVoiceHud();
void MirvPovVoice_ResetDemoState();
void MirvPov_UpdateVoiceTeam();
void MirvPov_UpdateVoiceHud();
void MirvPov_ClearSyntheticSpeaking();
void MirvPovVoice_OnRenderPass();
void MirvPovVoice_AfterRenderPass();
