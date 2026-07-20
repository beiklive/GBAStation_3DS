// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Memory {
class MemorySystem;
}

namespace Service::Y2R {
struct ConversionConfiguration;
} // namespace Service::Y2R

namespace HW::Y2R {
struct Diagnostics {
    u64 conversions{};
    u64 direct_conversions{};
    u64 fallback_conversions{};
    u64 pixels{};
    u64 direct_pixels{};
    double total_ms{};
    double direct_ms{};
    double fallback_ms{};
    u64 pre_flushes{};
    u64 pre_flush_bytes{};
    u64 pre_flush_invalidate_only{};
    double pre_flush_ms{};
    u16 last_direct_width{};
    u16 last_direct_lines{};
    u16 last_direct_dst_unit{};
    u16 last_direct_dst_gap{};
    u8 last_direct_input_format{};
    u8 last_direct_output_format{};
    u8 last_direct_rotation{};
    u8 last_direct_block_alignment{};
    u16 last_fallback_width{};
    u16 last_fallback_lines{};
    u16 last_fallback_y_unit{};
    u16 last_fallback_y_gap{};
    u16 last_fallback_u_unit{};
    u16 last_fallback_u_gap{};
    u16 last_fallback_v_unit{};
    u16 last_fallback_v_gap{};
    u16 last_fallback_yuyv_unit{};
    u16 last_fallback_yuyv_gap{};
    u16 last_fallback_dst_unit{};
    u16 last_fallback_dst_gap{};
    u8 last_fallback_input_format{};
    u8 last_fallback_output_format{};
    u8 last_fallback_rotation{};
    u8 last_fallback_block_alignment{};
};

Diagnostics GetAndResetDiagnostics();
void RecordPreConversionFlush(bool invalidate_only, u32 bytes, double elapsed_ms);

void PerformConversion(Memory::MemorySystem& memory, Service::Y2R::ConversionConfiguration cvt);
} // namespace HW::Y2R
