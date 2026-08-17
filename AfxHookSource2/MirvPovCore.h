#pragma once

#include <Windows.h>
#include <cstdint>

class CEntityInstance;

namespace SOURCESDK {
namespace CS2 {
class IGameEvent;
}
}

bool MirvPov_IsEnabled();
void MirvPov_Enable(HMODULE clientDll);
void MirvPov_Disable();

CEntityInstance * GetCurrentPovPlayerController();
CEntityInstance * GetCurrentPovPlayerPawn();
CEntityInstance * GetObservedPlayerController();
CEntityInstance * GetObservedPlayerPawn();
bool MirvPov_GetObserverState(uint8_t & observerMode, uint32_t & observerTarget);
CEntityInstance * GetRealLocalPlayerPawn();
CEntityInstance * GetEffectiveSplitScreenPlayer(int slot);

CEntityInstance * GetFakePovRadarController();
int GetFakePovRadarControllerIndex();
void SetFakePovRadarControllerIndex(int index);
void SetFakePovRadarAutoSync(bool enabled);
bool GetFakePovRadarAutoSync();

void * MirvPov_PushHookReturnAddress(void * returnAddress);
void * MirvPov_GetHookReturnAddress();
void MirvPov_PopHookReturnAddress(void * previous);

void MirvPov_UpdateSeekDetection();
void MirvPov_OnFrameStageBefore(int frameStage);
void MirvPov_OnFrameStageAfter(int frameStage);
void MirvPov_OnGameEvent(SOURCESDK::CS2::IGameEvent * event);
void MirvPov_OnPanoramaDllLoaded(HMODULE panoramaDll);
void MirvPov_OnPanoramaLayoutFileLoaded(const char * filePath);
void MirvPov_OnLevelInitPreEntity();
