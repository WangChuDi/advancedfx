#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace live_hud::native_pipeline {

bool install(HMODULE client);
void restore();
// Called from the seek-edge watcher. The actual HUD command is deferred to a
// stable followed-player radar transaction on the game thread.
void note_seek_end() noexcept;

}  // namespace live_hud::native_pipeline
