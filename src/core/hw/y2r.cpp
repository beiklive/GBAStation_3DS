// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define Y2R_USE_NEON 1
#else
#define Y2R_USE_NEON 0
#endif
#include "common/assert.h"
#include "common/color.h"
#include "common/common_types.h"
#include "common/microprofile.h"
#include "common/vector_math.h"
#include "core/core.h"
#include "core/hle/service/cam/y2r_u.h"
#include "core/hw/y2r.h"
#include "core/memory.h"

namespace HW::Y2R {

using namespace Service::Y2R;
using Clock = std::chrono::steady_clock;

static const std::size_t MAX_TILES = 1024 / 8;
static const std::size_t TILE_SIZE = 8 * 8;
using ImageTile = std::array<u32, TILE_SIZE>;

std::mutex diagnostics_mutex;
Diagnostics diagnostics;

void StoreDirectConfig(Diagnostics& target, const ConversionConfiguration& cvt) {
    target.last_direct_width = cvt.input_line_width;
    target.last_direct_lines = cvt.input_lines;
    target.last_direct_dst_unit = cvt.dst.transfer_unit;
    target.last_direct_dst_gap = cvt.dst.gap;
    target.last_direct_input_format = static_cast<u8>(cvt.input_format);
    target.last_direct_output_format = static_cast<u8>(cvt.output_format);
    target.last_direct_rotation = static_cast<u8>(cvt.rotation);
    target.last_direct_block_alignment = static_cast<u8>(cvt.block_alignment);
}

void StoreFallbackConfig(Diagnostics& target, const ConversionConfiguration& cvt) {
    target.last_fallback_width = cvt.input_line_width;
    target.last_fallback_lines = cvt.input_lines;
    target.last_fallback_y_unit = cvt.src_Y.transfer_unit;
    target.last_fallback_y_gap = cvt.src_Y.gap;
    target.last_fallback_u_unit = cvt.src_U.transfer_unit;
    target.last_fallback_u_gap = cvt.src_U.gap;
    target.last_fallback_v_unit = cvt.src_V.transfer_unit;
    target.last_fallback_v_gap = cvt.src_V.gap;
    target.last_fallback_yuyv_unit = cvt.src_YUYV.transfer_unit;
    target.last_fallback_yuyv_gap = cvt.src_YUYV.gap;
    target.last_fallback_dst_unit = cvt.dst.transfer_unit;
    target.last_fallback_dst_gap = cvt.dst.gap;
    target.last_fallback_input_format = static_cast<u8>(cvt.input_format);
    target.last_fallback_output_format = static_cast<u8>(cvt.output_format);
    target.last_fallback_rotation = static_cast<u8>(cvt.rotation);
    target.last_fallback_block_alignment = static_cast<u8>(cvt.block_alignment);
}

void AddDiagnostics(bool direct, u64 pixels, double elapsed_ms,
                    const ConversionConfiguration* fallback_cvt = nullptr) {
    std::scoped_lock lock(diagnostics_mutex);
    diagnostics.conversions++;
    diagnostics.pixels += pixels;
    diagnostics.total_ms += elapsed_ms;
    if (direct) {
        diagnostics.direct_conversions++;
        diagnostics.direct_pixels += pixels;
        diagnostics.direct_ms += elapsed_ms;
        if (fallback_cvt) {
            StoreDirectConfig(diagnostics, *fallback_cvt);
        }
    } else {
        diagnostics.fallback_conversions++;
        diagnostics.fallback_ms += elapsed_ms;
        if (fallback_cvt) {
            StoreFallbackConfig(diagnostics, *fallback_cvt);
        }
    }
}

void RecordPreConversionFlush(bool invalidate_only, u32 bytes, double elapsed_ms) {
    std::scoped_lock lock(diagnostics_mutex);
    diagnostics.pre_flushes++;
    diagnostics.pre_flush_bytes += bytes;
    diagnostics.pre_flush_ms += elapsed_ms;
    if (invalidate_only) {
        diagnostics.pre_flush_invalidate_only++;
    }
}

Diagnostics GetAndResetDiagnostics() {
    std::scoped_lock lock(diagnostics_mutex);
    Diagnostics result = diagnostics;
    diagnostics = {};
    return result;
}

[[nodiscard]] static constexpr unsigned int BytesPerPixel(OutputFormat format) {
    switch (format) {
    case OutputFormat::RGBA8:
        return 4;
    case OutputFormat::RGB8:
        return 3;
    case OutputFormat::RGB5A1:
    case OutputFormat::RGB565:
        return 2;
    }
    return 0;
}

[[nodiscard]] static bool HasRowDmaLayout(const ConversionBuffer& buf, unsigned int row_bytes) {
    return row_bytes > 0 && buf.transfer_unit == row_bytes;
}

[[nodiscard]] static bool HasWritableDmaLayout(const ConversionBuffer& buf, unsigned int bpp) {
    return bpp > 0 && buf.transfer_unit >= bpp && buf.transfer_unit % bpp == 0;
}

[[nodiscard]] static u8 ClampToByte(s32 value) {
    if (static_cast<u32>(value) <= 0xFF) {
        return static_cast<u8>(value);
    }
    return value < 0 ? 0 : 0xFF;
}

[[nodiscard]] static Common::Vec4<u8> ConvertPixel(s32 y, s32 u, s32 v,
                                                   const CoefficientSet& coefficients, u8 alpha) {
    const auto& c = coefficients;
    const s32 cY = c[0] * y;

    s32 r = cY + c[1] * v;
    s32 g = cY - c[2] * v - c[3] * u;
    s32 b = cY + c[4] * u;

    const s32 rounding_offset = 0x18;
    r = (r >> 3) + c[5] + rounding_offset;
    g = (g >> 3) + c[6] + rounding_offset;
    b = (b >> 3) + c[7] + rounding_offset;

    return {ClampToByte(r >> 5), ClampToByte(g >> 5), ClampToByte(b >> 5), alpha};
}

struct ConversionLuts {
    std::array<s32, 256> y;
    std::array<s32, 256> rv;
    std::array<s32, 256> gv;
    std::array<s32, 256> gu;
    std::array<s32, 256> bu;
    s32 r_offset{};
    s32 g_offset{};
    s32 b_offset{};
};

[[nodiscard]] static ConversionLuts BuildConversionLuts(const CoefficientSet& coefficients) {
    ConversionLuts luts;
    for (std::size_t i = 0; i < 256; ++i) {
        const s32 value = static_cast<s32>(i);
        luts.y[i] = coefficients[0] * value;
        luts.rv[i] = coefficients[1] * value;
        luts.gv[i] = -coefficients[2] * value;
        luts.gu[i] = -coefficients[3] * value;
        luts.bu[i] = coefficients[4] * value;
    }
    constexpr s32 rounding_offset = 0x18;
    luts.r_offset = coefficients[5] + rounding_offset;
    luts.g_offset = coefficients[6] + rounding_offset;
    luts.b_offset = coefficients[7] + rounding_offset;
    return luts;
}

[[nodiscard]] static Common::Vec4<u8> ConvertPixel(const ConversionLuts& luts, u8 y, u8 u, u8 v,
                                                   u8 alpha) {
    const s32 cY = luts.y[y];
    const s32 r = ((cY + luts.rv[v]) >> 3) + luts.r_offset;
    const s32 g = ((cY + luts.gv[v] + luts.gu[u]) >> 3) + luts.g_offset;
    const s32 b = ((cY + luts.bu[u]) >> 3) + luts.b_offset;
    return {ClampToByte(r >> 5), ClampToByte(g >> 5), ClampToByte(b >> 5), alpha};
}

[[nodiscard]] static u32 ConvertPixelRGBA8Packed(const ConversionLuts& luts, u8 y, u8 u, u8 v,
                                                 u8 alpha) {
    const s32 cY = luts.y[y];
    const s32 r = ((cY + luts.rv[v]) >> 3) + luts.r_offset;
    const s32 g = ((cY + luts.gv[v] + luts.gu[u]) >> 3) + luts.g_offset;
    const s32 b = ((cY + luts.bu[u]) >> 3) + luts.b_offset;
    return static_cast<u32>(alpha) | (static_cast<u32>(ClampToByte(b >> 5)) << 8) |
           (static_cast<u32>(ClampToByte(g >> 5)) << 16) |
           (static_cast<u32>(ClampToByte(r >> 5)) << 24);
}

static void StoreRGB8(u8* output, u8 r, u8 g, u8 b) {
    output[2] = r;
    output[1] = g;
    output[0] = b;
}

static void ConvertYToRGB8(const ConversionLuts& luts, u8 y, s32 r_uv, s32 g_uv, s32 b_uv,
                           u8* output) {
    const s32 cY = luts.y[y];
    const s32 r = ((cY + r_uv) >> 3) + luts.r_offset;
    const s32 g = ((cY + g_uv) >> 3) + luts.g_offset;
    const s32 b = ((cY + b_uv) >> 3) + luts.b_offset;
    StoreRGB8(output, ClampToByte(r >> 5), ClampToByte(g >> 5), ClampToByte(b >> 5));
}

static void ConvertYUV420QuadRGB8(const ConversionLuts& luts, u8 y00, u8 y01, u8 y10, u8 y11,
                                  u8 u, u8 v, u8* out00, u8* out01, u8* out10, u8* out11) {
    const s32 r_uv = luts.rv[v];
    const s32 g_uv = luts.gv[v] + luts.gu[u];
    const s32 b_uv = luts.bu[u];
    ConvertYToRGB8(luts, y00, r_uv, g_uv, b_uv, out00);
    ConvertYToRGB8(luts, y01, r_uv, g_uv, b_uv, out01);
    ConvertYToRGB8(luts, y10, r_uv, g_uv, b_uv, out10);
    ConvertYToRGB8(luts, y11, r_uv, g_uv, b_uv, out11);
}

#if Y2R_USE_NEON
[[nodiscard]] static uint8x8_t PackY8(u8 y0, u8 y1, u8 y2, u8 y3, u8 y4, u8 y5, u8 y6, u8 y7) {
    const u64 packed = static_cast<u64>(y0) | (static_cast<u64>(y1) << 8) |
                       (static_cast<u64>(y2) << 16) | (static_cast<u64>(y3) << 24) |
                       (static_cast<u64>(y4) << 32) | (static_cast<u64>(y5) << 40) |
                       (static_cast<u64>(y6) << 48) | (static_cast<u64>(y7) << 56);
    return vcreate_u8(packed);
}

[[nodiscard]] static uint8x8_t ClampS32PairToU8(int32x4_t low, int32x4_t high) {
    const int16x8_t as_s16 = vcombine_s16(vqmovn_s32(low), vqmovn_s32(high));
    return vqmovun_s16(as_s16);
}

static void ConvertYUV420OctetRGB8Neon(const ConversionLuts& luts, u16 y_coefficient,
                                       uint8x8_t y_values, u8 u0, u8 v0, u8 u1, u8 v1,
                                       u8* output) {
    const uint16x8_t y16 = vmovl_u8(y_values);
    const int32x4_t cy_low =
        vreinterpretq_s32_u32(vmull_n_u16(vget_low_u16(y16), y_coefficient));
    const int32x4_t cy_high =
        vreinterpretq_s32_u32(vmull_n_u16(vget_high_u16(y16), y_coefficient));

    const int32x4_t r_low = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_low, vdupq_n_s32(luts.rv[v0])), 3),
                  vdupq_n_s32(luts.r_offset)),
        5);
    const int32x4_t r_high = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_high, vdupq_n_s32(luts.rv[v1])), 3),
                  vdupq_n_s32(luts.r_offset)),
        5);

    const int32x4_t g_uv_low = vdupq_n_s32(luts.gv[v0] + luts.gu[u0]);
    const int32x4_t g_uv_high = vdupq_n_s32(luts.gv[v1] + luts.gu[u1]);
    const int32x4_t g_low = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_low, g_uv_low), 3), vdupq_n_s32(luts.g_offset)), 5);
    const int32x4_t g_high = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_high, g_uv_high), 3), vdupq_n_s32(luts.g_offset)), 5);

    const int32x4_t b_low = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_low, vdupq_n_s32(luts.bu[u0])), 3),
                  vdupq_n_s32(luts.b_offset)),
        5);
    const int32x4_t b_high = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_high, vdupq_n_s32(luts.bu[u1])), 3),
                  vdupq_n_s32(luts.b_offset)),
        5);

    uint8x8x3_t rgb;
    rgb.val[0] = ClampS32PairToU8(b_low, b_high);
    rgb.val[1] = ClampS32PairToU8(g_low, g_high);
    rgb.val[2] = ClampS32PairToU8(r_low, r_high);
    vst3_u8(output, rgb);
}

[[nodiscard]] static int32x4_t PackRepeatedPairS32(s32 a, s32 b) {
    return vcombine_s32(vdup_n_s32(a), vdup_n_s32(b));
}

static void ConvertYUV422OctetRGB8Neon(const ConversionLuts& luts, u16 y_coefficient,
                                       uint8x8_t y_values, u8 u0, u8 v0, u8 u1, u8 v1, u8 u2,
                                       u8 v2, u8 u3, u8 v3, u8* output) {
    const uint16x8_t y16 = vmovl_u8(y_values);
    const int32x4_t cy_low =
        vreinterpretq_s32_u32(vmull_n_u16(vget_low_u16(y16), y_coefficient));
    const int32x4_t cy_high =
        vreinterpretq_s32_u32(vmull_n_u16(vget_high_u16(y16), y_coefficient));

    const int32x4_t r_uv_low = PackRepeatedPairS32(luts.rv[v0], luts.rv[v1]);
    const int32x4_t r_uv_high = PackRepeatedPairS32(luts.rv[v2], luts.rv[v3]);
    const int32x4_t r_low = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_low, r_uv_low), 3), vdupq_n_s32(luts.r_offset)), 5);
    const int32x4_t r_high = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_high, r_uv_high), 3), vdupq_n_s32(luts.r_offset)), 5);

    const int32x4_t g_uv_low =
        PackRepeatedPairS32(luts.gv[v0] + luts.gu[u0], luts.gv[v1] + luts.gu[u1]);
    const int32x4_t g_uv_high =
        PackRepeatedPairS32(luts.gv[v2] + luts.gu[u2], luts.gv[v3] + luts.gu[u3]);
    const int32x4_t g_low = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_low, g_uv_low), 3), vdupq_n_s32(luts.g_offset)), 5);
    const int32x4_t g_high = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_high, g_uv_high), 3), vdupq_n_s32(luts.g_offset)), 5);

    const int32x4_t b_uv_low = PackRepeatedPairS32(luts.bu[u0], luts.bu[u1]);
    const int32x4_t b_uv_high = PackRepeatedPairS32(luts.bu[u2], luts.bu[u3]);
    const int32x4_t b_low = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_low, b_uv_low), 3), vdupq_n_s32(luts.b_offset)), 5);
    const int32x4_t b_high = vshrq_n_s32(
        vaddq_s32(vshrq_n_s32(vaddq_s32(cy_high, b_uv_high), 3), vdupq_n_s32(luts.b_offset)), 5);

    uint8x8x3_t rgb;
    rgb.val[0] = ClampS32PairToU8(b_low, b_high);
    rgb.val[1] = ClampS32PairToU8(g_low, g_high);
    rgb.val[2] = ClampS32PairToU8(r_low, r_high);
    vst3_u8(output, rgb);
}
#endif

static void ConvertYUV422PairRGBA8Packed(const ConversionLuts& luts, u8 y0, u8 y1, u8 u, u8 v,
                                         u8 alpha, u32& out0, u32& out1) {
    const s32 r_uv = luts.rv[v];
    const s32 g_uv = luts.gv[v] + luts.gu[u];
    const s32 b_uv = luts.bu[u];

    const auto convert_y = [&](u8 y) {
        const s32 cY = luts.y[y];
        const s32 r = ((cY + r_uv) >> 3) + luts.r_offset;
        const s32 g = ((cY + g_uv) >> 3) + luts.g_offset;
        const s32 b = ((cY + b_uv) >> 3) + luts.b_offset;
        return static_cast<u32>(alpha) | (static_cast<u32>(ClampToByte(b >> 5)) << 8) |
               (static_cast<u32>(ClampToByte(g >> 5)) << 16) |
               (static_cast<u32>(ClampToByte(r >> 5)) << 24);
    };

    out0 = convert_y(y0);
    out1 = convert_y(y1);
}

[[nodiscard]] static bool IsAlignedForU32(const void* ptr) {
    return (reinterpret_cast<std::uintptr_t>(ptr) & (alignof(u32) - 1)) == 0;
}

template <OutputFormat output_format>
static void WritePixel(u8* output, const Common::Vec4<u8>& color) {
    if constexpr (output_format == OutputFormat::RGBA8) {
        Common::Color::EncodeRGBA8(color, output);
    } else if constexpr (output_format == OutputFormat::RGB8) {
        Common::Color::EncodeRGB8(color, output);
    } else if constexpr (output_format == OutputFormat::RGB5A1) {
        Common::Color::EncodeRGB5A1(color, output);
    } else if constexpr (output_format == OutputFormat::RGB565) {
        Common::Color::EncodeRGB565(color, output);
    } else {
        UNREACHABLE_MSG("Unknown Y2R output format {}", output_format);
    }
}

class DmaOutputWriter {
public:
    bool Init(Memory::MemorySystem& memory, const ConversionBuffer& buffer, unsigned int bpp_) {
        if (!HasWritableDmaLayout(buffer, bpp_)) {
            return false;
        }
        output = memory.GetPointer(buffer.address);
        if (!output) {
            return false;
        }
        transfer_unit = buffer.transfer_unit;
        gap = buffer.gap;
        bpp = bpp_;
        remaining_in_unit = transfer_unit;
        return true;
    }

    template <OutputFormat output_format>
    void Write(const Common::Vec4<u8>& color) {
        WritePixel<output_format>(output, color);
        output += bpp;
        remaining_in_unit -= bpp;
        if (remaining_in_unit == 0) {
            output += gap;
            remaining_in_unit = transfer_unit;
        }
    }

    void WriteRGBA8Packed(u32 color) {
        std::memcpy(output, &color, sizeof(color));
        output += sizeof(color);
        remaining_in_unit -= sizeof(color);
        if (remaining_in_unit == 0) {
            output += gap;
            remaining_in_unit = transfer_unit;
        }
    }

private:
    u8* output{};
    unsigned int transfer_unit{};
    unsigned int gap{};
    unsigned int bpp{};
    unsigned int remaining_in_unit{};
};

[[nodiscard]] static unsigned int MortonToLinear(unsigned int index) {
    const unsigned int x = (index & 1) | ((index >> 1) & 2) | ((index >> 2) & 4);
    const unsigned int y = ((index >> 1) & 1) | ((index >> 2) & 2) | ((index >> 3) & 4);
    return y * 8 + x;
}

constexpr std::array<unsigned int, TILE_SIZE> MakeMortonToLinearTable() {
    std::array<unsigned int, TILE_SIZE> table{};
    for (unsigned int i = 0; i < TILE_SIZE; ++i) {
        const unsigned int x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
        const unsigned int y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
        table[i] = y * 8 + x;
    }
    return table;
}

constexpr auto MortonToLinearTable = MakeMortonToLinearTable();

constexpr std::array<unsigned int, TILE_SIZE / 2> MakeMortonPairXTable() {
    std::array<unsigned int, TILE_SIZE / 2> table{};
    for (unsigned int i = 0; i < TILE_SIZE; i += 2) {
        table[i / 2] = MortonToLinearTable[i] & 7;
    }
    return table;
}

constexpr std::array<unsigned int, TILE_SIZE / 2> MakeMortonPairYTable() {
    std::array<unsigned int, TILE_SIZE / 2> table{};
    for (unsigned int i = 0; i < TILE_SIZE; i += 2) {
        table[i / 2] = MortonToLinearTable[i] >> 3;
    }
    return table;
}

constexpr auto MortonPairXTable = MakeMortonPairXTable();
constexpr auto MortonPairYTable = MakeMortonPairYTable();

constexpr std::array<unsigned int, TILE_SIZE / 4> MakeMortonQuadXTable() {
    std::array<unsigned int, TILE_SIZE / 4> table{};
    for (unsigned int i = 0; i < TILE_SIZE; i += 4) {
        table[i / 4] = MortonToLinearTable[i] & 7;
    }
    return table;
}

constexpr std::array<unsigned int, TILE_SIZE / 4> MakeMortonQuadYTable() {
    std::array<unsigned int, TILE_SIZE / 4> table{};
    for (unsigned int i = 0; i < TILE_SIZE; i += 4) {
        table[i / 4] = MortonToLinearTable[i] >> 3;
    }
    return table;
}

constexpr auto MortonQuadXTable = MakeMortonQuadXTable();
constexpr auto MortonQuadYTable = MakeMortonQuadYTable();

[[nodiscard]] static bool DirectConvertYUV420Indiv8RGB8(Memory::MemorySystem& memory,
                                                        const ConversionConfiguration& cvt) {
    const unsigned int width = cvt.input_line_width;
    const unsigned int height = cvt.input_lines;
    if ((width & 1) != 0 || (height & 1) != 0) {
        return false;
    }
    if (!HasRowDmaLayout(cvt.src_Y, width) || !HasRowDmaLayout(cvt.src_U, width / 2) ||
        !HasRowDmaLayout(cvt.src_V, width / 2)) {
        return false;
    }
    if (!HasWritableDmaLayout(cvt.dst, 3) || cvt.dst.gap != 0) {
        return false;
    }

    const u8* src_y = memory.GetPointer(cvt.src_Y.address);
    const u8* src_u = memory.GetPointer(cvt.src_U.address);
    const u8* src_v = memory.GetPointer(cvt.src_V.address);
    u8* output_base = memory.GetPointer(cvt.dst.address);
    if (!src_y || !src_u || !src_v || !output_base) {
        return false;
    }

    const std::size_t y_stride = cvt.src_Y.transfer_unit + cvt.src_Y.gap;
    const std::size_t uv_stride = cvt.src_U.transfer_unit + cvt.src_U.gap;
    const std::size_t v_stride = cvt.src_V.transfer_unit + cvt.src_V.gap;
    const ConversionLuts luts = BuildConversionLuts(cvt.coefficients);

    if (cvt.block_alignment == BlockAlignment::Linear) {
        const std::size_t dst_stride = static_cast<std::size_t>(width) * 3;
        for (unsigned int y = 0; y < height; y += 2) {
            const u8* y_row0 = src_y + y * y_stride;
            const u8* y_row1 = y_row0 + y_stride;
            const u8* u_row = src_u + (y / 2) * uv_stride;
            const u8* v_row = src_v + (y / 2) * v_stride;
            u8* out_row0 = output_base + y * dst_stride;
            u8* out_row1 = out_row0 + dst_stride;
            unsigned int x = 0;
#if Y2R_USE_NEON
            const u16 y_coefficient = static_cast<u16>(cvt.coefficients[0]);
            for (; x + 7 < width; x += 8) {
                const unsigned int uv_x = x / 2;
                ConvertYUV422OctetRGB8Neon(luts, y_coefficient, vld1_u8(y_row0 + x), u_row[uv_x],
                                           v_row[uv_x], u_row[uv_x + 1], v_row[uv_x + 1],
                                           u_row[uv_x + 2], v_row[uv_x + 2], u_row[uv_x + 3],
                                           v_row[uv_x + 3], out_row0 + x * 3);
                ConvertYUV422OctetRGB8Neon(luts, y_coefficient, vld1_u8(y_row1 + x), u_row[uv_x],
                                           v_row[uv_x], u_row[uv_x + 1], v_row[uv_x + 1],
                                           u_row[uv_x + 2], v_row[uv_x + 2], u_row[uv_x + 3],
                                           v_row[uv_x + 3], out_row1 + x * 3);
            }
#endif
            for (; x < width; x += 2) {
                const unsigned int uv_x = x / 2;
                ConvertYUV420QuadRGB8(luts, y_row0[x], y_row0[x + 1], y_row1[x], y_row1[x + 1],
                                      u_row[uv_x], v_row[uv_x], out_row0 + x * 3,
                                      out_row0 + (x + 1) * 3, out_row1 + x * 3,
                                      out_row1 + (x + 1) * 3);
            }
        }
        return true;
    }

    if (cvt.block_alignment == BlockAlignment::Block8x8 && (height & 7) == 0) {
        u8* out = output_base;
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                std::array<const u8*, 8> y_rows{};
                std::array<const u8*, 4> u_rows{};
                std::array<const u8*, 4> v_rows{};
                for (unsigned int local_y = 0; local_y < 8; ++local_y) {
                    y_rows[local_y] = src_y + (strip_y + local_y) * y_stride;
                }
                for (unsigned int local_uv_y = 0; local_uv_y < 4; ++local_uv_y) {
                    const unsigned int y = (strip_y / 2) + local_uv_y;
                    u_rows[local_uv_y] = src_u + y * uv_stride;
                    v_rows[local_uv_y] = src_v + y * v_stride;
                }

#if Y2R_USE_NEON
                for (unsigned int quad = 0; quad < TILE_SIZE / 4; quad += 2) {
                    const unsigned int local_x0 = MortonQuadXTable[quad];
                    const unsigned int local_y0 = MortonQuadYTable[quad];
                    const unsigned int local_x1 = MortonQuadXTable[quad + 1];
                    const unsigned int local_y1 = MortonQuadYTable[quad + 1];
                    ASSERT(local_y0 == local_y1);

                    const unsigned int x0 = tile_x + local_x0;
                    const unsigned int x1 = tile_x + local_x1;
                    const u8* y_row0 = y_rows[local_y0];
                    const u8* y_row1 = y_rows[local_y0 + 1];
                    const u8* u_row = u_rows[local_y0 / 2];
                    const u8* v_row = v_rows[local_y0 / 2];
                    const unsigned int uv_x0 = x0 / 2;
                    const unsigned int uv_x1 = x1 / 2;
                    const uint8x8_t y_values = PackY8(y_row0[x0], y_row0[x0 + 1], y_row1[x0],
                                                      y_row1[x0 + 1], y_row0[x1], y_row0[x1 + 1],
                                                      y_row1[x1], y_row1[x1 + 1]);
                    ConvertYUV420OctetRGB8Neon(
                        luts, static_cast<u16>(cvt.coefficients[0]), y_values, u_row[uv_x0],
                        v_row[uv_x0], u_row[uv_x1], v_row[uv_x1], out);
                    out += 24;
                }
#else
                for (unsigned int quad = 0; quad < TILE_SIZE / 4; ++quad) {
                    const unsigned int local_x = MortonQuadXTable[quad];
                    const unsigned int local_y = MortonQuadYTable[quad];
                    const unsigned int x = tile_x + local_x;
                    const u8* y_row0 = y_rows[local_y];
                    const u8* y_row1 = y_rows[local_y + 1];
                    const u8* u_row = u_rows[local_y / 2];
                    const u8* v_row = v_rows[local_y / 2];
                    const unsigned int uv_x = x / 2;
                    ConvertYUV420QuadRGB8(luts, y_row0[x], y_row0[x + 1], y_row1[x],
                                          y_row1[x + 1], u_row[uv_x], v_row[uv_x], out,
                                          out + 3, out + 6, out + 9);
                    out += 12;
                }
#endif
            }
        }
        return true;
    }

    return false;
}

[[nodiscard]] static bool DirectConvertYUV422Indiv8RGB8(Memory::MemorySystem& memory,
                                                        const ConversionConfiguration& cvt) {
    const unsigned int width = cvt.input_line_width;
    const unsigned int height = cvt.input_lines;
    if ((width & 1) != 0) {
        return false;
    }
    if (!HasRowDmaLayout(cvt.src_Y, width) || !HasRowDmaLayout(cvt.src_U, width / 2) ||
        !HasRowDmaLayout(cvt.src_V, width / 2)) {
        return false;
    }
    if (!HasWritableDmaLayout(cvt.dst, 3) || cvt.dst.gap != 0) {
        return false;
    }

    const u8* src_y = memory.GetPointer(cvt.src_Y.address);
    const u8* src_u = memory.GetPointer(cvt.src_U.address);
    const u8* src_v = memory.GetPointer(cvt.src_V.address);
    u8* output_base = memory.GetPointer(cvt.dst.address);
    if (!src_y || !src_u || !src_v || !output_base) {
        return false;
    }

    const std::size_t y_stride = cvt.src_Y.transfer_unit + cvt.src_Y.gap;
    const std::size_t uv_stride = cvt.src_U.transfer_unit + cvt.src_U.gap;
    const std::size_t v_stride = cvt.src_V.transfer_unit + cvt.src_V.gap;
    const ConversionLuts luts = BuildConversionLuts(cvt.coefficients);

    const auto write_pair = [&](const u8* y_row, const u8* u_row, const u8* v_row, unsigned int x,
                                u8* out) {
        const unsigned int uv_x = x / 2;
        const u8 u = u_row[uv_x];
        const u8 v = v_row[uv_x];
        const s32 r_uv = luts.rv[v];
        const s32 g_uv = luts.gv[v] + luts.gu[u];
        const s32 b_uv = luts.bu[u];
        ConvertYToRGB8(luts, y_row[x], r_uv, g_uv, b_uv, out);
        ConvertYToRGB8(luts, y_row[x + 1], r_uv, g_uv, b_uv, out + 3);
    };

    if (cvt.block_alignment == BlockAlignment::Linear) {
        const std::size_t dst_stride = static_cast<std::size_t>(width) * 3;
        for (unsigned int y = 0; y < height; ++y) {
            const u8* y_row = src_y + y * y_stride;
            const u8* u_row = src_u + y * uv_stride;
            const u8* v_row = src_v + y * v_stride;
            u8* out_row = output_base + y * dst_stride;
            unsigned int x = 0;
#if Y2R_USE_NEON
            const u16 y_coefficient = static_cast<u16>(cvt.coefficients[0]);
            for (; x + 7 < width; x += 8) {
                const unsigned int uv_x = x / 2;
                ConvertYUV422OctetRGB8Neon(luts, y_coefficient, vld1_u8(y_row + x), u_row[uv_x],
                                           v_row[uv_x], u_row[uv_x + 1], v_row[uv_x + 1],
                                           u_row[uv_x + 2], v_row[uv_x + 2], u_row[uv_x + 3],
                                           v_row[uv_x + 3], out_row + x * 3);
            }
#endif
            for (; x < width; x += 2) {
                write_pair(y_row, u_row, v_row, x, out_row + x * 3);
            }
        }
        return true;
    }

    if (cvt.block_alignment == BlockAlignment::Block8x8 && (height & 7) == 0) {
        u8* out = output_base;
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                std::array<const u8*, 8> y_rows{};
                std::array<const u8*, 8> u_rows{};
                std::array<const u8*, 8> v_rows{};
                for (unsigned int local_y = 0; local_y < 8; ++local_y) {
                    const unsigned int y = strip_y + local_y;
                    y_rows[local_y] = src_y + y * y_stride;
                    u_rows[local_y] = src_u + y * uv_stride;
                    v_rows[local_y] = src_v + y * v_stride;
                }

#if Y2R_USE_NEON
                const u16 y_coefficient = static_cast<u16>(cvt.coefficients[0]);
                for (unsigned int pair = 0; pair < TILE_SIZE / 2; pair += 4) {
                    const unsigned int lx0 = MortonPairXTable[pair];
                    const unsigned int ly0 = MortonPairYTable[pair];
                    const unsigned int lx1 = MortonPairXTable[pair + 1];
                    const unsigned int ly1 = MortonPairYTable[pair + 1];
                    const unsigned int lx2 = MortonPairXTable[pair + 2];
                    const unsigned int ly2 = MortonPairYTable[pair + 2];
                    const unsigned int lx3 = MortonPairXTable[pair + 3];
                    const unsigned int ly3 = MortonPairYTable[pair + 3];
                    const unsigned int x0 = tile_x + lx0;
                    const unsigned int x1 = tile_x + lx1;
                    const unsigned int x2 = tile_x + lx2;
                    const unsigned int x3 = tile_x + lx3;
                    const u8* y_row0 = y_rows[ly0];
                    const u8* y_row1 = y_rows[ly1];
                    const u8* y_row2 = y_rows[ly2];
                    const u8* y_row3 = y_rows[ly3];
                    const u8* u_row0 = u_rows[ly0];
                    const u8* u_row1 = u_rows[ly1];
                    const u8* u_row2 = u_rows[ly2];
                    const u8* u_row3 = u_rows[ly3];
                    const u8* v_row0 = v_rows[ly0];
                    const u8* v_row1 = v_rows[ly1];
                    const u8* v_row2 = v_rows[ly2];
                    const u8* v_row3 = v_rows[ly3];
                    const unsigned int uv_x0 = x0 / 2;
                    const unsigned int uv_x1 = x1 / 2;
                    const unsigned int uv_x2 = x2 / 2;
                    const unsigned int uv_x3 = x3 / 2;
                    const uint8x8_t y_values = PackY8(y_row0[x0], y_row0[x0 + 1], y_row1[x1],
                                                      y_row1[x1 + 1], y_row2[x2], y_row2[x2 + 1],
                                                      y_row3[x3], y_row3[x3 + 1]);
                    ConvertYUV422OctetRGB8Neon(
                        luts, y_coefficient, y_values, u_row0[uv_x0], v_row0[uv_x0],
                        u_row1[uv_x1], v_row1[uv_x1], u_row2[uv_x2], v_row2[uv_x2],
                        u_row3[uv_x3], v_row3[uv_x3], out);
                    out += 24;
                }
#else
                for (unsigned int pair = 0; pair < TILE_SIZE / 2; ++pair) {
                    const unsigned int local_x = MortonPairXTable[pair];
                    const unsigned int local_y = MortonPairYTable[pair];
                    const unsigned int x = tile_x + local_x;
                    write_pair(y_rows[local_y], u_rows[local_y], v_rows[local_y], x, out);
                    out += 6;
                }
#endif
            }
        }
        return true;
    }

    return false;
}

template <OutputFormat output_format, std::size_t sample_stride>
[[nodiscard]] static bool DirectConvertYUV420Indiv(Memory::MemorySystem& memory,
                                                   const ConversionConfiguration& cvt) {
    const unsigned int width = cvt.input_line_width;
    const unsigned int height = cvt.input_lines;
    if ((width & 1) != 0 || (height & 1) != 0) {
        return false;
    }

    const unsigned int dst_bpp = BytesPerPixel(output_format);
    if (!HasRowDmaLayout(cvt.src_Y, width * sample_stride) ||
        !HasRowDmaLayout(cvt.src_U, (width / 2) * sample_stride) ||
        !HasRowDmaLayout(cvt.src_V, (width / 2) * sample_stride)) {
        return false;
    }

    const u8* src_y = memory.GetPointer(cvt.src_Y.address);
    const u8* src_u = memory.GetPointer(cvt.src_U.address);
    const u8* src_v = memory.GetPointer(cvt.src_V.address);
    if (!src_y || !src_u || !src_v) {
        return false;
    }

    DmaOutputWriter output;
    if (!output.Init(memory, cvt.dst, dst_bpp)) {
        return false;
    }

    const std::size_t y_stride = cvt.src_Y.transfer_unit + cvt.src_Y.gap;
    const std::size_t uv_stride = cvt.src_U.transfer_unit + cvt.src_U.gap;
    const std::size_t v_stride = cvt.src_V.transfer_unit + cvt.src_V.gap;
    const u8 alpha = static_cast<u8>(cvt.alpha);
    const ConversionLuts luts = BuildConversionLuts(cvt.coefficients);

    const auto write_pixel = [&](unsigned int x, unsigned int y) {
        const u8* y_row = src_y + y * y_stride;
        const u8* u_row = src_u + (y / 2) * uv_stride;
        const u8* v_row = src_v + (y / 2) * v_stride;
        const auto color = ConvertPixel(luts, y_row[x * sample_stride],
                                        u_row[(x / 2) * sample_stride],
                                        v_row[(x / 2) * sample_stride], alpha);
        output.Write<output_format>(color);
    };

    if (cvt.block_alignment == BlockAlignment::Linear) {
        for (unsigned int y = 0; y < height; ++y) {
            for (unsigned int x = 0; x < width; ++x) {
                write_pixel(x, y);
            }
        }
    } else {
        if ((height & 7) != 0) {
            return false;
        }
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                for (unsigned int i = 0; i < TILE_SIZE; ++i) {
                    const unsigned int linear = MortonToLinear(i);
                    write_pixel(tile_x + (linear & 7), strip_y + (linear >> 3));
                }
            }
        }
    }
    return true;
}

template <OutputFormat output_format, std::size_t sample_stride>
[[nodiscard]] static bool DirectConvertYUV422Indiv(Memory::MemorySystem& memory,
                                                   const ConversionConfiguration& cvt) {
    const unsigned int width = cvt.input_line_width;
    const unsigned int height = cvt.input_lines;
    if ((width & 1) != 0) {
        return false;
    }

    const unsigned int dst_bpp = BytesPerPixel(output_format);
    if (!HasRowDmaLayout(cvt.src_Y, width * sample_stride) ||
        !HasRowDmaLayout(cvt.src_U, (width / 2) * sample_stride) ||
        !HasRowDmaLayout(cvt.src_V, (width / 2) * sample_stride)) {
        return false;
    }

    const u8* src_y = memory.GetPointer(cvt.src_Y.address);
    const u8* src_u = memory.GetPointer(cvt.src_U.address);
    const u8* src_v = memory.GetPointer(cvt.src_V.address);
    if (!src_y || !src_u || !src_v) {
        return false;
    }

    DmaOutputWriter output;
    if (!output.Init(memory, cvt.dst, dst_bpp)) {
        return false;
    }

    const std::size_t y_stride = cvt.src_Y.transfer_unit + cvt.src_Y.gap;
    const std::size_t uv_stride = cvt.src_U.transfer_unit + cvt.src_U.gap;
    const std::size_t v_stride = cvt.src_V.transfer_unit + cvt.src_V.gap;
    const u8 alpha = static_cast<u8>(cvt.alpha);
    const ConversionLuts luts = BuildConversionLuts(cvt.coefficients);

    const auto write_pixel = [&](unsigned int x, unsigned int y) {
        const u8* y_row = src_y + y * y_stride;
        const u8* u_row = src_u + y * uv_stride;
        const u8* v_row = src_v + y * v_stride;
        const auto color = ConvertPixel(luts, y_row[x * sample_stride],
                                        u_row[(x / 2) * sample_stride],
                                        v_row[(x / 2) * sample_stride], alpha);
        output.Write<output_format>(color);
    };

    if (cvt.block_alignment == BlockAlignment::Linear) {
        for (unsigned int y = 0; y < height; ++y) {
            for (unsigned int x = 0; x < width; ++x) {
                write_pixel(x, y);
            }
        }
    } else {
        if ((height & 7) != 0) {
            return false;
        }
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                for (unsigned int i = 0; i < TILE_SIZE; ++i) {
                    const unsigned int linear = MortonToLinear(i);
                    write_pixel(tile_x + (linear & 7), strip_y + (linear >> 3));
                }
            }
        }
    }
    return true;
}

[[nodiscard]] static bool DirectConvertYUV422Indiv8RGBA8(Memory::MemorySystem& memory,
                                                         const ConversionConfiguration& cvt) {
    const unsigned int width = cvt.input_line_width;
    const unsigned int height = cvt.input_lines;
    if ((width & 1) != 0) {
        return false;
    }

    if (!HasRowDmaLayout(cvt.src_Y, width) || !HasRowDmaLayout(cvt.src_U, width / 2) ||
        !HasRowDmaLayout(cvt.src_V, width / 2)) {
        return false;
    }

    const u8* src_y = memory.GetPointer(cvt.src_Y.address);
    const u8* src_u = memory.GetPointer(cvt.src_U.address);
    const u8* src_v = memory.GetPointer(cvt.src_V.address);
    if (!src_y || !src_u || !src_v) {
        return false;
    }

    u8* output_base = memory.GetPointer(cvt.dst.address);
    if (!output_base) {
        return false;
    }

    DmaOutputWriter output;
    if (!output.Init(memory, cvt.dst, 4)) {
        return false;
    }

    const std::size_t y_stride = cvt.src_Y.transfer_unit + cvt.src_Y.gap;
    const std::size_t uv_stride = cvt.src_U.transfer_unit + cvt.src_U.gap;
    const std::size_t v_stride = cvt.src_V.transfer_unit + cvt.src_V.gap;
    const u8 alpha = static_cast<u8>(cvt.alpha);
    const ConversionLuts luts = BuildConversionLuts(cvt.coefficients);

    if (cvt.block_alignment == BlockAlignment::Linear &&
        HasRowDmaLayout(cvt.dst, width * sizeof(u32)) && IsAlignedForU32(output_base)) {
        const std::size_t dst_stride = cvt.dst.transfer_unit + cvt.dst.gap;
        for (unsigned int y = 0; y < height; ++y) {
            const u8* y_row = src_y + y * y_stride;
            const u8* u_row = src_u + y * uv_stride;
            const u8* v_row = src_v + y * v_stride;
            auto* out = reinterpret_cast<u32*>(output_base + y * dst_stride);
            for (unsigned int x = 0; x < width; x += 2) {
                const unsigned int uv_x = x / 2;
                const u8 u = u_row[uv_x];
                const u8 v = v_row[uv_x];
                ConvertYUV422PairRGBA8Packed(luts, y_row[x], y_row[x + 1], u, v, alpha, out[x],
                                             out[x + 1]);
            }
        }
        return true;
    }

    if (cvt.block_alignment == BlockAlignment::Block8x8 && (height & 7) == 0 && cvt.dst.gap == 0 &&
        IsAlignedForU32(output_base)) {
        auto* out = reinterpret_cast<u32*>(output_base);
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                std::array<const u8*, 8> y_rows{};
                std::array<const u8*, 8> u_rows{};
                std::array<const u8*, 8> v_rows{};
                for (unsigned int local_y = 0; local_y < 8; ++local_y) {
                    const unsigned int y = strip_y + local_y;
                    y_rows[local_y] = src_y + y * y_stride;
                    u_rows[local_y] = src_u + y * uv_stride;
                    v_rows[local_y] = src_v + y * v_stride;
                }

                for (unsigned int pair = 0; pair < TILE_SIZE / 2; ++pair) {
                    const unsigned int local_x = MortonPairXTable[pair];
                    const unsigned int local_y = MortonPairYTable[pair];
                    const unsigned int x = tile_x + local_x;
                    const u8* y_row = y_rows[local_y];
                    const u8* u_row = u_rows[local_y];
                    const u8* v_row = v_rows[local_y];
                    const unsigned int uv_x = x / 2;
                    const u8 u = u_row[uv_x];
                    const u8 v = v_row[uv_x];
                    u32 color0;
                    u32 color1;
                    ConvertYUV422PairRGBA8Packed(luts, y_row[x], y_row[x + 1], u, v, alpha,
                                                 color0, color1);
                    *out++ = color0;
                    *out++ = color1;
                }
            }
        }
        return true;
    }

    const auto write_pair = [&](unsigned int x, unsigned int y) {
        const u8* y_row = src_y + y * y_stride;
        const u8* u_row = src_u + y * uv_stride;
        const u8* v_row = src_v + y * v_stride;
        const unsigned int uv_x = x / 2;
        const u8 u = u_row[uv_x];
        const u8 v = v_row[uv_x];
        u32 color0;
        u32 color1;
        ConvertYUV422PairRGBA8Packed(luts, y_row[x], y_row[x + 1], u, v, alpha, color0, color1);
        output.WriteRGBA8Packed(color0);
        output.WriteRGBA8Packed(color1);
    };

    if (cvt.block_alignment == BlockAlignment::Linear) {
        for (unsigned int y = 0; y < height; ++y) {
            for (unsigned int x = 0; x < width; x += 2) {
                write_pair(x, y);
            }
        }
    } else {
        if ((height & 7) != 0) {
            return false;
        }
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                for (unsigned int i = 0; i < TILE_SIZE; i += 2) {
                    const unsigned int linear = MortonToLinearTable[i];
                    write_pair(tile_x + (linear & 7), strip_y + (linear >> 3));
                }
            }
        }
    }
    return true;
}

template <OutputFormat output_format>
[[nodiscard]] static bool DirectConvertYUYV422(Memory::MemorySystem& memory,
                                               const ConversionConfiguration& cvt) {
    const unsigned int width = cvt.input_line_width;
    const unsigned int height = cvt.input_lines;
    if ((width & 1) != 0) {
        return false;
    }

    const unsigned int dst_bpp = BytesPerPixel(output_format);
    if (!HasRowDmaLayout(cvt.src_YUYV, width * 2)) {
        return false;
    }

    const u8* src = memory.GetPointer(cvt.src_YUYV.address);
    if (!src) {
        return false;
    }

    DmaOutputWriter output;
    if (!output.Init(memory, cvt.dst, dst_bpp)) {
        return false;
    }

    const std::size_t src_stride = cvt.src_YUYV.transfer_unit + cvt.src_YUYV.gap;
    const u8 alpha = static_cast<u8>(cvt.alpha);
    const ConversionLuts luts = BuildConversionLuts(cvt.coefficients);

    const auto write_pixel = [&](unsigned int x, unsigned int y) {
        const u8* src_row = src + y * src_stride;
        const unsigned int pair = (x / 2) * 4;
        const auto color =
            ConvertPixel(luts, src_row[x * 2], src_row[pair + 1], src_row[pair + 3], alpha);
        output.Write<output_format>(color);
    };

    if (cvt.block_alignment == BlockAlignment::Linear) {
        for (unsigned int y = 0; y < height; ++y) {
            for (unsigned int x = 0; x < width; ++x) {
                write_pixel(x, y);
            }
        }
    } else {
        if ((height & 7) != 0) {
            return false;
        }
        for (unsigned int strip_y = 0; strip_y < height; strip_y += 8) {
            for (unsigned int tile_x = 0; tile_x < width; tile_x += 8) {
                for (unsigned int i = 0; i < TILE_SIZE; ++i) {
                    const unsigned int linear = MortonToLinear(i);
                    write_pixel(tile_x + (linear & 7), strip_y + (linear >> 3));
                }
            }
        }
    }
    return true;
}

template <OutputFormat output_format>
[[nodiscard]] static bool DirectConvertOutput(Memory::MemorySystem& memory,
                                              const ConversionConfiguration& cvt) {
    switch (cvt.input_format) {
    case InputFormat::YUV420_Indiv8:
        if constexpr (output_format == OutputFormat::RGB8) {
            if (DirectConvertYUV420Indiv8RGB8(memory, cvt)) {
                return true;
            }
        }
        return DirectConvertYUV420Indiv<output_format, 1>(memory, cvt);
    case InputFormat::YUV422_Indiv8:
        if constexpr (output_format == OutputFormat::RGB8) {
            if (DirectConvertYUV422Indiv8RGB8(memory, cvt)) {
                return true;
            }
        }
        if constexpr (output_format == OutputFormat::RGBA8) {
            if (DirectConvertYUV422Indiv8RGBA8(memory, cvt)) {
                return true;
            }
        }
        return DirectConvertYUV422Indiv<output_format, 1>(memory, cvt);
    case InputFormat::YUV420_Indiv16:
        return DirectConvertYUV420Indiv<output_format, 2>(memory, cvt);
    case InputFormat::YUV422_Indiv16:
        return DirectConvertYUV422Indiv<output_format, 2>(memory, cvt);
    case InputFormat::YUYV422_Interleaved:
        return DirectConvertYUYV422<output_format>(memory, cvt);
    default:
        return false;
    }
}

[[nodiscard]] static bool TryDirectConvert(Memory::MemorySystem& memory,
                                           const ConversionConfiguration& cvt) {
    if (cvt.rotation != Rotation::None) {
        return false;
    }
    if (cvt.block_alignment != BlockAlignment::Linear &&
        cvt.block_alignment != BlockAlignment::Block8x8) {
        return false;
    }

    switch (cvt.output_format) {
    case OutputFormat::RGBA8:
        return DirectConvertOutput<OutputFormat::RGBA8>(memory, cvt);
    case OutputFormat::RGB8:
        return DirectConvertOutput<OutputFormat::RGB8>(memory, cvt);
    case OutputFormat::RGB5A1:
        return DirectConvertOutput<OutputFormat::RGB5A1>(memory, cvt);
    case OutputFormat::RGB565:
        return DirectConvertOutput<OutputFormat::RGB565>(memory, cvt);
    default:
        return false;
    }
}

/// Converts a image strip from the source YUV format into individual 8x8 RGB32 tiles.
template <InputFormat input_format>
static void ConvertYUVToRGB(const u8* input_Y, const u8* input_U, const u8* input_V,
                            ImageTile output[], unsigned int width, unsigned int height,
                            const CoefficientSet& coefficients) {

    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            s32 Y;
            s32 U;
            s32 V;
            if constexpr (input_format == InputFormat::YUV422_Indiv8 ||
                          input_format == InputFormat::YUV422_Indiv16) {
                Y = input_Y[y * width + x];
                U = input_U[(y * width + x) / 2];
                V = input_V[(y * width + x) / 2];
            } else if constexpr (input_format == InputFormat::YUV420_Indiv8 ||
                                 input_format == InputFormat::YUV420_Indiv16) {
                Y = input_Y[y * width + x];
                U = input_U[((y / 2) * width + x) / 2];
                V = input_V[((y / 2) * width + x) / 2];
            } else if constexpr (input_format == InputFormat::YUYV422_Interleaved) {
                Y = input_Y[(y * width + x) * 2];
                U = input_Y[(y * width + (x / 2) * 2) * 2 + 1];
                V = input_Y[(y * width + (x / 2) * 2) * 2 + 3];
            } else {
                UNREACHABLE_MSG("Unknown Y2R input format {}", input_format);
                return;
            }

            // This conversion process is bit-exact with hardware, as far as could be tested.
            auto& c = coefficients;
            s32 cY = c[0] * Y;

            s32 r = cY + c[1] * V;
            s32 g = cY - c[2] * V - c[3] * U;
            s32 b = cY + c[4] * U;

            const s32 rounding_offset = 0x18;
            r = (r >> 3) + c[5] + rounding_offset;
            g = (g >> 3) + c[6] + rounding_offset;
            b = (b >> 3) + c[7] + rounding_offset;

            unsigned int tile = x / 8;
            unsigned int tile_x = x % 8;
            u32* out = &output[tile][y * 8 + tile_x];
            *out = ((u32)std::clamp(r >> 5, 0, 0xFF) << 24) |
                   ((u32)std::clamp(g >> 5, 0, 0xFF) << 16) |
                   ((u32)std::clamp(b >> 5, 0, 0xFF) << 8);
        }
    }
}

/// Simulates an incoming CDMA transfer. The N parameter is used to automatically convert 16-bit
/// formats to 8-bit.
template <std::size_t N>
static void ReceiveData(Memory::MemorySystem& memory, u8* output, ConversionBuffer& buf,
                        std::size_t amount_of_data) {
    const u8* input = memory.GetPointer(buf.address);

    std::size_t output_unit = buf.transfer_unit / N;
    ASSERT(amount_of_data % output_unit == 0);

    while (amount_of_data > 0) {
        for (std::size_t i = 0; i < output_unit; ++i) {
            output[i] = input[i * N];
        }

        output += output_unit;
        input += buf.transfer_unit + buf.gap;

        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
        amount_of_data -= output_unit;
    }
}

/// Convert intermediate RGB32 format to the final output format while simulating an outgoing CDMA
/// transfer.
template <OutputFormat output_format>
static void SendData(Memory::MemorySystem& memory, const u32* input, ConversionBuffer& buf,
                     int amount_of_data, u8 alpha) {

    u8* output = memory.GetPointer(buf.address);

    while (amount_of_data > 0) {
        u8* unit_end = output + buf.transfer_unit;
        while (output < unit_end) {
            u32 color = *input++;
            Common::Vec4<u8> col_vec{(u8)(color >> 24), (u8)(color >> 16), (u8)(color >> 8), alpha};

            if constexpr (output_format == OutputFormat::RGBA8) {
                Common::Color::EncodeRGBA8(col_vec, output);
                output += 4;
            } else if constexpr (output_format == OutputFormat::RGB8) {
                Common::Color::EncodeRGB8(col_vec, output);
                output += 3;
            } else if constexpr (output_format == OutputFormat::RGB5A1) {
                Common::Color::EncodeRGB5A1(col_vec, output);
                output += 2;
            } else if constexpr (output_format == OutputFormat::RGB565) {
                Common::Color::EncodeRGB565(col_vec, output);
                output += 2;
            } else {
                UNREACHABLE_MSG("Unknown Y2R output format {}", output_format);
            }

            amount_of_data -= 1;
        }

        output += buf.gap;
        buf.address += buf.transfer_unit + buf.gap;
        buf.image_size -= buf.transfer_unit;
    }
}

static const u8 linear_lut[TILE_SIZE] = {
    // clang-format off
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63,
    // clang-format on
};

static const u8 morton_lut[TILE_SIZE] = {
    // clang-format off
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
    // clang-format on
};

static void RotateTile0(const ImageTile& input, ImageTile& output, int height,
                        const u8 out_map[64]) {
    for (int i = 0; i < height * 8; ++i) {
        output[out_map[i]] = input[i];
    }
}

static void RotateTile90(const ImageTile& input, ImageTile& output, int height,
                         const u8 out_map[64]) {
    int out_i = 0;
    for (int x = 0; x < 8; ++x) {
        for (int y = height - 1; y >= 0; --y) {
            output[out_map[out_i++]] = input[y * 8 + x];
        }
    }
}

static void RotateTile180(const ImageTile& input, ImageTile& output, int height,
                          const u8 out_map[64]) {
    int out_i = 0;
    for (int i = height * 8 - 1; i >= 0; --i) {
        output[out_map[out_i++]] = input[i];
    }
}

static void RotateTile270(const ImageTile& input, ImageTile& output, int height,
                          const u8 out_map[64]) {
    int out_i = 0;
    for (int x = 8 - 1; x >= 0; --x) {
        for (int y = 0; y < height; ++y) {
            output[out_map[out_i++]] = input[y * 8 + x];
        }
    }
}

static void WriteTileToOutput(u32* output, const ImageTile& tile, int height, int line_stride) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 8; ++x) {
            output[y * line_stride + x] = tile[y * 8 + x];
        }
    }
}

MICROPROFILE_DEFINE(Y2R_PerformConversion, "Y2R", "PerformConversion", MP_RGB(185, 66, 245));

/**
 * Performs a Y2R colorspace conversion.
 *
 * The Y2R hardware implements hardware-accelerated YUV to RGB colorspace conversions. It is most
 * commonly used for video playback or to display camera input to the screen.
 *
 * The conversion process is quite configurable, and can be divided in distinct steps. From
 * observation, it appears that the hardware buffers a single 8-pixel tall strip of image data
 * internally and converts it in one go before writing to the output and loading the next strip.
 *
 * The steps taken to convert one strip of image data are:
 *
 * - The hardware receives data via CDMA (http://3dbrew.org/wiki/Corelink_DMA_Engines), which is
 *   presumably stored in one or more internal buffers. This process can be done in several separate
 *   transfers, as long as they don't exceed the size of the internal image buffer. This allows
 *   flexibility in input strides.
 * - The input data is decoded into a YUV tuple. Several formats are suported, see the `InputFormat`
 *   enum.
 * - The YUV tuple is converted, using fixed point calculations, to RGB. This step can be configured
 *   using a set of coefficients to support different colorspace standards. See `CoefficientSet`.
 * - The strip can be optionally rotated 90, 180 or 270 degrees. Since each strip is processed
 *   independently, this notably rotates each *strip*, not the entire image. This means that for 90
 *   or 270 degree rotations, the output will be in terms of several 8 x height images, and for any
 *   non-zero rotation the strips will have to be re-arranged so that the parts of the image will
 *   not be shuffled together. This limitation makes this a feature of somewhat dubious utility. 90
 *   or 270 degree rotations in images with non-even height don't seem to work properly.
 * - The data is converted to the output RGB format. See the `OutputFormat` enum.
 * - The data can be output either linearly line-by-line or in the swizzled 8x8 tile format used by
 *   the PICA. This is decided by the `BlockAlignment` enum. If 8x8 alignment is used, then the
 *   image must have a height divisible by 8. The image width must always be divisible by 8.
 * - The final data is then CDMAed out to main memory and the next image strip is processed. This
 *   offers the same flexibility as the input stage.
 *
 * In this implementation, to avoid the combinatorial explosion of parameter combinations, common
 * intermediate formats are used and where possible tables or parameters are used instead of
 * diverging code paths to keep the amount of branches in check. Some steps are also merged to
 * increase efficiency.
 *
 * Output for all valid settings combinations matches hardware, however output in some edge-cases
 * differs:
 *
 * - `Block8x8` alignment with non-mod8 height produces different garbage patterns on the last
 *   strip, especially when combined with rotation.
 * - Hardware, when using `Linear` alignment with a non-even height and 90 or 270 degree rotation
 *   produces misaligned output on the last strip. This implmentation produces output with the
 *   correct "expected" alignment.
 *
 * Hardware behaves strangely (doesn't fire the completion interrupt, for example) in these cases,
 * so they are believed to be invalid configurations anyway.
 */
void PerformConversion(Memory::MemorySystem& memory, ConversionConfiguration cvt) {
    MICROPROFILE_SCOPE(Y2R_PerformConversion);
    const auto start_time = Clock::now();
    const u64 pixel_count = static_cast<u64>(cvt.input_line_width) * cvt.input_lines;

    ASSERT(cvt.input_line_width % 8 == 0);
    ASSERT(cvt.block_alignment != BlockAlignment::Block8x8 || cvt.input_lines % 8 == 0);

    if (TryDirectConvert(memory, cvt)) {
        const auto end_time = Clock::now();
        AddDiagnostics(true, pixel_count,
                       std::chrono::duration<double, std::milli>(end_time - start_time).count(),
                       &cvt);
        return;
    }

    // Tiles per row
    std::size_t num_tiles = cvt.input_line_width / 8;
    ASSERT(num_tiles <= MAX_TILES);

    // Buffer used as a CDMA source/target.
    std::unique_ptr<u8[]> data_buffer(new u8[cvt.input_line_width * 8 * 4]);
    // Intermediate storage for decoded 8x8 image tiles. Always stored as RGB32.
    std::unique_ptr<ImageTile[]> tiles(new ImageTile[num_tiles]);
    ImageTile tmp_tile;

    // LUT used to remap writes to a tile. Used to allow linear or swizzled output without
    // requiring two different code paths.
    const u8* tile_remap = nullptr;
    switch (cvt.block_alignment) {
    case BlockAlignment::Linear:
        tile_remap = linear_lut;
        break;
    case BlockAlignment::Block8x8:
        tile_remap = morton_lut;
        break;
    }

    for (unsigned int y = 0; y < cvt.input_lines; y += 8) {
        unsigned int row_height = std::min(cvt.input_lines - y, 8u);

        // Total size in pixels of incoming data required for this strip.
        const std::size_t row_data_size = row_height * cvt.input_line_width;

        u8* input_Y = data_buffer.get();
        u8* input_U = input_Y + 8 * cvt.input_line_width;
        u8* input_V = input_U + 8 * cvt.input_line_width / 2;

        switch (cvt.input_format) {
        case InputFormat::YUV422_Indiv8:
            ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 2);
            ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 2);
            ConvertYUVToRGB<InputFormat::YUV422_Indiv8>(input_Y, input_U, input_V, tiles.get(),
                                                        cvt.input_line_width, row_height,
                                                        cvt.coefficients);
            break;
        case InputFormat::YUV420_Indiv8:
            ReceiveData<1>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<1>(memory, input_U, cvt.src_U, row_data_size / 4);
            ReceiveData<1>(memory, input_V, cvt.src_V, row_data_size / 4);
            ConvertYUVToRGB<InputFormat::YUV420_Indiv8>(input_Y, input_U, input_V, tiles.get(),
                                                        cvt.input_line_width, row_height,
                                                        cvt.coefficients);
            break;
        case InputFormat::YUV422_Indiv16:
            ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 2);
            ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 2);
            ConvertYUVToRGB<InputFormat::YUV422_Indiv16>(input_Y, input_U, input_V, tiles.get(),
                                                         cvt.input_line_width, row_height,
                                                         cvt.coefficients);
            break;
        case InputFormat::YUV420_Indiv16:
            ReceiveData<2>(memory, input_Y, cvt.src_Y, row_data_size);
            ReceiveData<2>(memory, input_U, cvt.src_U, row_data_size / 4);
            ReceiveData<2>(memory, input_V, cvt.src_V, row_data_size / 4);
            ConvertYUVToRGB<InputFormat::YUV420_Indiv16>(input_Y, input_U, input_V, tiles.get(),
                                                         cvt.input_line_width, row_height,
                                                         cvt.coefficients);
            break;
        case InputFormat::YUYV422_Interleaved:
            input_U = nullptr;
            input_V = nullptr;
            ReceiveData<1>(memory, input_Y, cvt.src_YUYV, row_data_size * 2);
            ConvertYUVToRGB<InputFormat::YUYV422_Interleaved>(input_Y, input_U, input_V,
                                                              tiles.get(), cvt.input_line_width,
                                                              row_height, cvt.coefficients);
            break;
        default:
            UNREACHABLE_MSG("Unknown Y2R input format {}", cvt.input_format);
            return;
        }

        u32* output_buffer = reinterpret_cast<u32*>(data_buffer.get());

        for (std::size_t i = 0; i < num_tiles; ++i) {
            int image_strip_width = 0;
            int output_stride = 0;

            switch (cvt.rotation) {
            case Rotation::None:
                RotateTile0(tiles[i], tmp_tile, row_height, tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_90:
                RotateTile90(tiles[i], tmp_tile, row_height, tile_remap);
                image_strip_width = 8;
                output_stride = 8 * row_height;
                break;
            case Rotation::Clockwise_180:
                // For 180 and 270 degree rotations we also invert the order of tiles in the strip,
                // since the rotates are done individually on each tile.
                RotateTile180(tiles[num_tiles - i - 1], tmp_tile, row_height, tile_remap);
                image_strip_width = cvt.input_line_width;
                output_stride = 8;
                break;
            case Rotation::Clockwise_270:
                RotateTile270(tiles[num_tiles - i - 1], tmp_tile, row_height, tile_remap);
                image_strip_width = 8;
                output_stride = 8 * row_height;
                break;
            }

            switch (cvt.block_alignment) {
            case BlockAlignment::Linear:
                WriteTileToOutput(output_buffer, tmp_tile, row_height, image_strip_width);
                output_buffer += output_stride;
                break;
            case BlockAlignment::Block8x8:
                WriteTileToOutput(output_buffer, tmp_tile, 8, 8);
                output_buffer += TILE_SIZE;
                break;
            }
        }

        switch (cvt.output_format) {
        case OutputFormat::RGBA8:
            SendData<OutputFormat::RGBA8>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                          cvt.dst, static_cast<int>(row_data_size),
                                          static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB8:
            SendData<OutputFormat::RGB8>(memory, reinterpret_cast<u32*>(data_buffer.get()), cvt.dst,
                                         static_cast<int>(row_data_size),
                                         static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB5A1:
            SendData<OutputFormat::RGB5A1>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                           cvt.dst, static_cast<int>(row_data_size),
                                           static_cast<u8>(cvt.alpha));
            break;
        case OutputFormat::RGB565:
            SendData<OutputFormat::RGB565>(memory, reinterpret_cast<u32*>(data_buffer.get()),
                                           cvt.dst, static_cast<int>(row_data_size),
                                           static_cast<u8>(cvt.alpha));
            break;
        default:
            UNREACHABLE_MSG("Unknown Y2R output format {}", cvt.output_format);
            return;
        }
    }

    const auto end_time = Clock::now();
    AddDiagnostics(false, pixel_count,
                   std::chrono::duration<double, std::milli>(end_time - start_time).count(), &cvt);
}
} // namespace HW::Y2R
