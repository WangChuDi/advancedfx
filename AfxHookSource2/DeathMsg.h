#pragma once

#include <windows.h>
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include "MirvPanorama.h"

void HookDeathMsg(HMODULE clientDll);
void HookPanorama(HMODULE panoramaDll);
SOURCESDK::CS2::CKV3MemberName DeathMsg_MakeGameEventKey(const char * name);

struct currentGameCamera {
	double origin[3];
	double angles[3];
	float time;
};

extern currentGameCamera g_CurrentGameCamera;

namespace CS2 {
// https://github.com/danielkrupinski/Osiris
};
