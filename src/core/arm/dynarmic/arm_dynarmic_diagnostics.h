// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Core {

struct DynarmicDiagnostics {
    u64 jit_instances_created{};
    u64 instruction_cache_clears{};
    u64 cache_range_invalidations{};
    u64 cache_range_invalidation_bytes{};
    u64 last_code_cache_size{};
    u32 last_optimization_flags{};
    u32 last_hook_hint_instructions{};
    u32 last_always_little_endian{};
    u64 memory_read_callbacks{};
    u64 memory_write_callbacks{};
    u64 memory_exclusive_callbacks{};
    u64 memory_code_callbacks{};
    u64 fast_dispatch_misses{};
    u64 fast_dispatch_updates{};
    u64 fast_dispatch_clears{};
    u32 last_read_callback_addr{};
    u32 last_write_callback_addr{};
};

DynarmicDiagnostics GetAndResetDynarmicDiagnostics();

} // namespace Core
