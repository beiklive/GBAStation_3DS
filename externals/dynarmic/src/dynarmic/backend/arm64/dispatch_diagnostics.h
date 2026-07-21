/* This file is part of the dynarmic project.
 * Copyright (c) 2026 BeikLive
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstdint>

namespace Dynarmic::Backend::Arm64 {

struct Arm64DispatchDiagnostics {
    std::uint64_t fast_dispatch_misses{};
    std::uint64_t fast_dispatch_updates{};
    std::uint64_t fast_dispatch_clears{};
    std::uint64_t fast_dispatch_false_misses{};
    std::uint64_t dispatcher_cache_hits{};
    std::uint64_t dispatcher_cache_misses{};
    std::uint64_t dispatcher_cache_collisions{};
    std::array<std::uint64_t, 4> top_dispatcher_descriptors{};
    std::array<std::uint64_t, 4> top_dispatcher_descriptor_counts{};
};

Arm64DispatchDiagnostics GetAndResetArm64DispatchDiagnostics();

}  // namespace Dynarmic::Backend::Arm64
