#pragma once

#include <Windows.h>

namespace SOURCESDK { namespace CS2 { class IGameEvent; } }

void MirvPovKillReward_Initialize(HMODULE clientDll);
void MirvPovKillReward_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPovKillReward_OnMoneyUpdate(
    int oldAccount,
    int newAccount,
    int oldPanelControllerHandle,
    int newPanelControllerHandle,
    int resolvedPovControllerHandle);
void MirvPovKillReward_OnDemoTick(int demoTick);
void MirvPovKillReward_Reset(const char * reason);
void MirvPovKillReward_SetMoneyHookAvailable(bool available);
bool MirvPovKillReward_ApplyHudChatDemoBypass(bool enabled);
bool MirvPovKillReward_IsHudChatDemoBypassAvailable();
bool MirvPovKillReward_IsHudChatDemoBypassApplied();
bool MirvPovKillReward_IsAvailable();
bool MirvPovKillReward_PushHudChatText(
    const char * text,
    int entityIndex,
    const char * source);
int MirvPovKillReward_GetTrackedPovControllerHandle();
bool MirvPovKillReward_HashGameEventKey(const char * name, unsigned int & outHash);
