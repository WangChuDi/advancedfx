#include "stdafx.h"

#include "MirvPovContext.h"

#include <atomic>

namespace MirvPovContext {
namespace {

thread_local std::uint32_t g_domains = 0;

std::atomic<void*> g_pawn{nullptr};
std::atomic<void*> g_controller{nullptr};
std::atomic<std::int32_t> g_slot{-1};
std::atomic<std::int32_t> g_team{0};
std::atomic<std::uint64_t> g_generation{0};

std::atomic<void*> g_pinned_pawn{nullptr};
std::atomic<void*> g_pinned_controller{nullptr};
std::atomic<std::int32_t> g_pinned_slot{-1};
std::atomic<std::int32_t> g_pinned_team{0};
std::atomic<std::uint64_t> g_pinned_generation{0};
std::atomic<bool> g_pin_active{false};
std::atomic<std::uint64_t> g_pin_sequence{0};
std::atomic_flag g_writer = ATOMIC_FLAG_INIT;

constexpr std::uint32_t bits(Domain domain) noexcept {
    return static_cast<std::uint32_t>(domain);
}

void lock_writer() noexcept {
    while (g_writer.test_and_set(std::memory_order_acquire)) {
    }
}

void unlock_writer() noexcept {
    g_writer.clear(std::memory_order_release);
}

} // namespace

Scope::Scope(Domain domain) noexcept
    : previous_domains_(g_domains) {
    g_domains |= bits(domain);
}

Scope::~Scope() noexcept {
    g_domains = previous_domains_;
}

bool active() noexcept {
    return g_domains != 0;
}

bool active(Domain domains_value) noexcept {
    return (g_domains & bits(domains_value)) != 0;
}

Domain domains() noexcept {
    return static_cast<Domain>(g_domains);
}

void publish(const Snapshot& value) noexcept {
    lock_writer();
    // Odd generations are unstable. Readers accept only a stable even pair.
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    g_pawn.store(value.pawn, std::memory_order_relaxed);
    g_controller.store(value.controller, std::memory_order_relaxed);
    g_slot.store(value.slot, std::memory_order_relaxed);
    g_team.store(value.team, std::memory_order_relaxed);
    g_generation.fetch_add(1, std::memory_order_release);
    unlock_writer();
}

Snapshot snapshot() noexcept {
    Snapshot out{};
    for (;;) {
        const auto pin_before = g_pin_sequence.load(std::memory_order_acquire);
        if ((pin_before & 1U) != 0) {
            continue;
        }

        if (g_pin_active.load(std::memory_order_relaxed)) {
            out.pawn = g_pinned_pawn.load(std::memory_order_relaxed);
            out.controller = g_pinned_controller.load(std::memory_order_relaxed);
            out.slot = g_pinned_slot.load(std::memory_order_relaxed);
            out.team = g_pinned_team.load(std::memory_order_relaxed);
            out.generation =
                g_pinned_generation.load(std::memory_order_relaxed);
            const auto pin_after = g_pin_sequence.load(std::memory_order_acquire);
            if (pin_before == pin_after) {
                return out;
            }
            continue;
        }

        const auto before = g_generation.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        out.pawn = g_pawn.load(std::memory_order_relaxed);
        out.controller = g_controller.load(std::memory_order_relaxed);
        out.slot = g_slot.load(std::memory_order_relaxed);
        out.team = g_team.load(std::memory_order_relaxed);
        const auto after = g_generation.load(std::memory_order_acquire);
        const auto pin_after = g_pin_sequence.load(std::memory_order_acquire);
        if (before == after && pin_before == pin_after &&
            !g_pin_active.load(std::memory_order_relaxed)) {
            out.generation = after;
            return out;
        }
    }
}

void pin(const Snapshot& value) noexcept {
    lock_writer();
    g_pin_sequence.fetch_add(1, std::memory_order_acq_rel);
    g_pinned_pawn.store(value.pawn, std::memory_order_relaxed);
    g_pinned_controller.store(value.controller, std::memory_order_relaxed);
    g_pinned_slot.store(value.slot, std::memory_order_relaxed);
    g_pinned_team.store(value.team, std::memory_order_relaxed);
    g_pinned_generation.store(value.generation, std::memory_order_relaxed);
    g_pin_active.store(value.pawn != nullptr, std::memory_order_relaxed);
    g_pin_sequence.fetch_add(1, std::memory_order_release);
    unlock_writer();
}

void unpin() noexcept {
    lock_writer();
    g_pin_sequence.fetch_add(1, std::memory_order_acq_rel);
    g_pin_active.store(false, std::memory_order_relaxed);
    g_pinned_pawn.store(nullptr, std::memory_order_relaxed);
    g_pinned_controller.store(nullptr, std::memory_order_relaxed);
    g_pinned_slot.store(-1, std::memory_order_relaxed);
    g_pinned_team.store(0, std::memory_order_relaxed);
    g_pinned_generation.store(0, std::memory_order_relaxed);
    g_pin_sequence.fetch_add(1, std::memory_order_release);
    unlock_writer();
}

bool pinned() noexcept {
    for (;;) {
        const auto before = g_pin_sequence.load(std::memory_order_acquire);
        if ((before & 1U) != 0) {
            continue;
        }
        const bool result = g_pin_active.load(std::memory_order_relaxed);
        const auto after = g_pin_sequence.load(std::memory_order_acquire);
        if (before == after) {
            return result;
        }
    }
}

void invalidate() noexcept {
    unpin();
    publish(Snapshot{});
}

} // namespace MirvPovContext
