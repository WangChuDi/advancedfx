#pragma once

#include <cstdint>

namespace live_hud {

bool install_hooks();
void remove_hooks();

// Client hooks use fixed RVAs. Returns false while client.dll is pending or
// when its PE fingerprint differs from offsets/current.h; mismatch is logged
// once and all client-side work must remain disabled.
bool client_build_matches();

// Shared HUD windows built by the radar allowlist scanner (PIPELINE / ISHLTV_LIE).
bool hud_ranges_ready();
// Absolute address in HudRadar-ish window (identity stack filter).
bool addr_in_hud_identity_window(std::uintptr_t addr);
// Absolute return address allowed to receive IsHLTVOrReplay=false.
bool addr_in_ishtlv_lie_allowlist(std::uintptr_t addr);
// Demo seek/gototick in progress (playing && skip_tick != -1). Identity/FoW
// must not touch entities while the demo player rebuilds the world.
bool demo_is_skipping();

}  // namespace live_hud
