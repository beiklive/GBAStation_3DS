/* This file is part of the dynarmic project.
 * Copyright (c) 2026 BeikLive
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstdint>

namespace Dynarmic::Backend::Arm64 {

struct Arm64DispatchDiagnostics {
    std::uint64_t fast_dispatch_misses{};
    std::uint64_t fast_dispatch_updates{};
    std::uint64_t fast_dispatch_clears{};
};

Arm64DispatchDiagnostics GetAndResetArm64DispatchDiagnostics();

}  // namespace Dynarmic::Backend::Arm64
