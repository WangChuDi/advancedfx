#pragma once

#include <Windows.h>

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

namespace MirvPov {

void OnClientLoaded(HMODULE client);
void OnClientInit();
void OnClientShutdown();
void OnFrameStage(SOURCESDK::CS2::ClientFrameStage_t stage);

bool Requested();
bool Installed();
void RemoveHooks();

} // namespace MirvPov
