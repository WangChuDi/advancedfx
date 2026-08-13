#pragma once

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"

#define ADVANCEDFX_STARTMOVIE_WAV_KEY "advancedfx-802bb089-972b-4841-bdf3-5108175ab59d"

bool AfxStreams_IsRcording();
const wchar_t * AfxStreams_GetTakeDir();

void AfxStreams_ShutDown();

void RenderSystemDX11_EngineThread_Prepare();
void RenderSystemDX11_EngineThread_BeforeRender();

bool RenderSystemDX11_EngineThread_HasNextRenderPass();

void RenderSystemDX11_EngineThread_BeginNextRenderPass();
void RenderSystemDX11_EngineThread_EndNextRenderPass();

void RenderSystemDX11_EngineThread_BeginMainRenderPass();
void RenderSystemDX11_EngineThread_EndMainRenderPass();

void Hook_RenderSystemDX11(void * hModule);

void Hook_SceneSystem(void * hModule);

void RenderSystemDX11_SupplyProjectionMatrix(const SOURCESDK::VMatrix & projectionMatrix);

void RenderSystemDX11_DeathFade_Initialize(void * clientDll);
void RenderSystemDX11_DeathFade_Hurt();
void RenderSystemDX11_DeathFade_Death();
void RenderSystemDX11_DeathFade_ObserveHurtEvent();
void RenderSystemDX11_DeathFade_ObserveDeathEvent();
void RenderSystemDX11_DeathFade_Reset();
void RenderSystemDX11_DeathFade_ClearForObserverChange();
void RenderSystemDX11_DeathFade_UpdateObserverState();
void RenderSystemDX11_DeathFade_ResetObserverState();
void RenderSystemDX11_DeathFade_ProcessPending();
