#include "stdafx.h"

#include "MirvPov.h"
#include "MirvPovReference/MirvPovReferenceCompat.h"
#include "MirvPovReference/MirvPovReferenceHooks.h"
#include "MirvPovReference/MirvPovReferencePov.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"

#include <atomic>

extern HMODULE g_H_ClientDll;

namespace MirvPov {
namespace {

std::atomic<bool> g_requested{false};
std::atomic<bool> g_reference_hooks_started{false};
bool g_client_initialized = false;

void try_enable() {
    if (!g_requested.load(std::memory_order_acquire) ||
        !g_client_initialized ||
        g_reference_hooks_started.load(std::memory_order_acquire)) {
        return;
    }

    live_hud::set_hlae_pipeline_requested(true);
    if (live_hud::install_hooks()) {
        g_reference_hooks_started.store(true, std::memory_order_release);
        advancedfx::Message("[mirv_pov] reference POV watcher started; "
                            "native pipeline waits for demo playing\n");
    } else {
        advancedfx::Warning(
            "[mirv_pov] reference POV hook bootstrap failed; no hooks kept\n");
    }
}

void print_status() {
    const auto identity = live_hud::pov::snapshot();
    advancedfx::Message(
        "[mirv_pov] requested=%d reference_hooks=%d client_initialized=%d "
        "client=%p identity_pawn=%p controller=%p slot=%d team=%d generation=%llu\n",
        g_requested.load(std::memory_order_acquire) ? 1 : 0,
        g_reference_hooks_started.load(std::memory_order_acquire) ? 1 : 0,
        g_client_initialized ? 1 : 0, g_H_ClientDll, identity.pawn,
        identity.controller, identity.slot, identity.team,
        static_cast<unsigned long long>(identity.generation));
}

} // namespace

CON_COMMAND(mirv_pov, "Use the reference POV pipeline for native HUD transactions.")
{
    if (args->ArgC() < 2 || _stricmp(args->ArgV(1), "status") == 0) {
        print_status();
        return;
    }

    if (_stricmp(args->ArgV(1), "1") == 0 ||
        _stricmp(args->ArgV(1), "on") == 0) {
        g_requested.store(true, std::memory_order_release);
        live_hud::set_hlae_pipeline_requested(true);
        try_enable();
        print_status();
        return;
    }

    if (_stricmp(args->ArgV(1), "0") == 0 ||
        _stricmp(args->ArgV(1), "off") == 0) {
        g_requested.store(false, std::memory_order_release);
        live_hud::set_hlae_pipeline_requested(false);
        live_hud::remove_hooks();
        g_reference_hooks_started.store(false, std::memory_order_release);
        print_status();
        return;
    }

    advancedfx::Message("[mirv_pov] usage: mirv_pov [1|0|status]\n");
}

void OnClientLoaded(HMODULE client) {
    (void)client;
    if (g_requested.load(std::memory_order_acquire)) {
        try_enable();
    }
}

void OnClientInit() {
    g_client_initialized = true;
    try_enable();
}

void OnClientShutdown() {
    live_hud::remove_hooks();
    g_reference_hooks_started.store(false, std::memory_order_release);
    g_client_initialized = false;
    live_hud::pov::invalidate();
}

void OnFrameStage(SOURCESDK::CS2::ClientFrameStage_t stage) {
    (void)stage;
    // The reference watcher owns install retry, demo seek edges, and identity
    // publication. No per-frame install loop is allowed here.
}

bool Requested() {
    return g_requested.load(std::memory_order_acquire);
}

bool Installed() {
    return g_reference_hooks_started.load(std::memory_order_acquire);
}

void RemoveHooks() {
    live_hud::remove_hooks();
    g_reference_hooks_started.store(false, std::memory_order_release);
    live_hud::pov::invalidate();
}

} // namespace MirvPov
