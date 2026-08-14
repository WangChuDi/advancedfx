#pragma once

#include "../MirvPovContext.h"

namespace live_hud::pov {

using Domain = MirvPovContext::Domain;
using Snapshot = MirvPovContext::Snapshot;
using Scope = MirvPovContext::Scope;

inline bool active() noexcept { return MirvPovContext::active(); }
inline bool active(Domain domains) noexcept {
    return MirvPovContext::active(domains);
}
inline Domain domains() noexcept { return MirvPovContext::domains(); }
inline void publish(const Snapshot& snapshot) noexcept {
    MirvPovContext::publish(snapshot);
}
inline Snapshot snapshot() noexcept { return MirvPovContext::snapshot(); }
inline void pin(const Snapshot& snapshot) noexcept {
    MirvPovContext::pin(snapshot);
}
inline void unpin() noexcept { MirvPovContext::unpin(); }
inline bool pinned() noexcept { return MirvPovContext::pinned(); }
inline void invalidate() noexcept { MirvPovContext::invalidate(); }

} // namespace live_hud::pov
