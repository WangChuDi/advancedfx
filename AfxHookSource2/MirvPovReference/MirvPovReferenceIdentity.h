#pragma once

namespace live_hud {

// LIVE_HUD_PIPELINE=1: remap slot→local-pawn getter so live HUD/camera see the
// current demo spectate target pawn (not the HLTV observer pawn).
bool want_pipeline();
bool install_local_identity_remap();
void restore_local_identity_remap();
void try_local_identity_remap_once();
void try_early_icon_style_once();
void try_early_radar_style_once();
void log_identity_diag_if_due();
// After demo seek ends: invalidate icon styles + dirty radar.
void restyle_radar_after_seek();
// Watcher: restyle radar on freeze start/end (numbers stick through freeze).
void poll_freeze_radar_restyle();
// Watcher-thread: deferred HudRadar icon wipe (unsafe inside GetLocal hook).
void poll_radar_fow_work();
// Force cl_teammate_colors_show=1 (safe memory write; ok from watcher).
void force_teammate_colors_no_letters();
}  // namespace live_hud
