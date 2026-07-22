// SPDX-FileCopyrightText: Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/overlay_font.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

#ifdef __SWITCH__
#include "GBAStation/switch_libnx.h"
#endif

namespace Vulkan::OverlayFont {
namespace {

constexpr int RuntimeAtlasWidth = kAtlasWidth;
constexpr int RuntimeAtlasHeight = kAtlasHeight;
constexpr float RuntimeBakePixelHeight = 32.0f;
constexpr std::array<u32, 4> MaterialIconCodepoints{{
    0xE5C4, // keyboard_return
    0xE333, // settings_overscan
    0xE5D5, // refresh
    0xE879, // exit_to_app
}};

struct RuntimeAtlas {
    std::vector<unsigned char> pixels;
    std::unordered_map<u32, Glyph> glyphs;
    float ascent = kAscent;
    float descent = kDescent;
    float line_height = kLineHeight;
    bool ready = false;
};

RuntimeAtlas runtime;

const Glyph& StaticGlyphFor(u32 codepoint) {
    const int index = codepoint < kFirstChar || codepoint > kLastChar
                          ? 0
                          : static_cast<int>(codepoint) - kFirstChar;
    return kGlyphs[index];
}

bool DecodeUtf8(std::string_view text, std::vector<u32>& out) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char* end = ptr + text.size();
    while (ptr < end) {
        u32 cp = 0;
        const unsigned char c = *ptr;
        if (c < 0x80) {
            cp = c;
            ++ptr;
        } else if ((c & 0xE0) == 0xC0 && ptr + 1 < end) {
            cp = ((c & 0x1F) << 6) | (ptr[1] & 0x3F);
            ptr += 2;
        } else if ((c & 0xF0) == 0xE0 && ptr + 2 < end) {
            cp = ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
            ptr += 3;
        } else if ((c & 0xF8) == 0xF0 && ptr + 3 < end) {
            cp = ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                 ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
            ptr += 4;
        } else {
            return false;
        }
        out.push_back(cp);
    }
    return true;
}

std::vector<u32> BuildTextCodepoints() {
    std::vector<u32> codepoints;
    for (u32 cp = 32; cp <= 126; ++cp) {
        codepoints.push_back(cp);
    }

    constexpr std::array<std::string_view, 35> strings{{
        "GBAStation 菜单",
        "继续",
        "画面",
        "重置",
        "退出",
        "返回游戏",
        "画面设置",
        "重置游戏",
        "退出游戏",
        "快进倍率",
        "内部渲染",
        "整数缩放",
        "屏幕布局",
        "画面旋转",
        "屏幕间距",
        "应用设置",
        "返回标签",
        "开启",
        "关闭",
        "竖排",
        "横排",
        "主屏优先",
        "混合",
        "上屏",
        "下屏",
        "自定义",
        "确定",
        "调整",
        "选择",
        "切换标签",
        "进入设置",
        "关闭菜单",
        "返回",
        "度",
        "←→↑↓",
    }};
    for (std::string_view text : strings) {
        DecodeUtf8(text, codepoints);
    }

    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}

std::vector<unsigned char> LoadFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in) {
        data.clear();
    }
    return data;
}

std::vector<unsigned char> LoadMaterialFont() {
    constexpr std::array<const char*, 4> paths{{
        "romfs:/material/MaterialIcons-Regular.ttf",
        "romfs:/rescources/material/MaterialIcons-Regular.ttf",
        "sdmc:/GBAStation/resources/material/MaterialIcons-Regular.ttf",
        "/GBAStation/resources/material/MaterialIcons-Regular.ttf",
    }};
    for (const char* path : paths) {
        std::vector<unsigned char> data = LoadFile(path);
        if (!data.empty()) {
            LOG_INFO(Render_Vulkan, "Overlay material icon font loaded from {}", path);
            return data;
        }
    }
    LOG_WARNING(Render_Vulkan, "Overlay material icon font not found; tab icons will be hidden");
    return {};
}

#ifdef __SWITCH__
std::vector<unsigned char> CopySharedFont(PlSharedFontType type) {
    PlFontData font{};
    if (plGetSharedFontByType(&font, type) != 0 || !font.address || font.size == 0) {
        return {};
    }
    std::vector<unsigned char> data(font.size);
    std::memcpy(data.data(), font.address, font.size);
    return data;
}

std::vector<unsigned char> LoadSwitchChineseFont() {
    std::vector<unsigned char> data;
    if (plInitialize(PlServiceType_User) == 0) {
        data = CopySharedFont(PlSharedFontType_ChineseSimplified);
        if (data.empty()) {
            data = CopySharedFont(PlSharedFontType_ExtChineseSimplified);
        }
        if (data.empty()) {
            data = CopySharedFont(PlSharedFontType_Standard);
        }
        plExit();
    }
    if (data.empty()) {
        LOG_WARNING(Render_Vulkan, "Overlay Switch shared font unavailable; using ASCII fallback");
    }
    return data;
}
#else
std::vector<unsigned char> LoadSwitchChineseFont() {
    return {};
}
#endif

float FontAscentForPixelHeight(const std::vector<unsigned char>& font_data, float pixel_height) {
    if (font_data.empty()) {
        return 0.0f;
    }
    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, font_data.data(), stbtt_GetFontOffsetForIndex(font_data.data(), 0))) {
        return 0.0f;
    }
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    return static_cast<float>(ascent) * stbtt_ScaleForPixelHeight(&info, pixel_height);
}

bool PackCodepoints(stbtt_pack_context& context, const std::vector<unsigned char>& font_data,
                    const std::vector<u32>& codepoints, float top_offset,
                    std::unordered_map<u32, Glyph>& out) {
    if (font_data.empty() || codepoints.empty()) {
        return false;
    }

    std::vector<int> stb_codepoints;
    stb_codepoints.reserve(codepoints.size());
    for (u32 cp : codepoints) {
        stb_codepoints.push_back(static_cast<int>(cp));
    }

    std::vector<stbtt_packedchar> chars(codepoints.size());
    stbtt_pack_range range{};
    range.font_size = RuntimeBakePixelHeight;
    range.array_of_unicode_codepoints = stb_codepoints.data();
    range.num_chars = static_cast<int>(stb_codepoints.size());
    range.chardata_for_range = chars.data();

    if (!stbtt_PackFontRanges(&context, font_data.data(), 0, &range, 1)) {
        return false;
    }

    for (std::size_t i = 0; i < codepoints.size(); ++i) {
        const stbtt_packedchar& ch = chars[i];
        Glyph glyph{};
        glyph.u0 = static_cast<float>(ch.x0) / RuntimeAtlasWidth;
        glyph.v0 = static_cast<float>(ch.y0) / RuntimeAtlasHeight;
        glyph.u1 = static_cast<float>(ch.x1) / RuntimeAtlasWidth;
        glyph.v1 = static_cast<float>(ch.y1) / RuntimeAtlasHeight;
        glyph.xoff = ch.xoff;
        glyph.yoff = ch.yoff + top_offset;
        glyph.w = ch.xoff2 - ch.xoff;
        glyph.h = ch.yoff2 - ch.yoff;
        glyph.xadvance = ch.xadvance;
        out[codepoints[i]] = glyph;
    }
    return true;
}

} // namespace

bool Initialize() {
    if (runtime.ready) {
        return true;
    }

    std::vector<unsigned char> font_data = LoadSwitchChineseFont();
    if (font_data.empty()) {
        return false;
    }

    RuntimeAtlas next;
    next.pixels.assign(RuntimeAtlasWidth * RuntimeAtlasHeight, 0);

    stbtt_fontinfo info{};
    if (!stbtt_InitFont(&info, font_data.data(), stbtt_GetFontOffsetForIndex(font_data.data(), 0))) {
        LOG_WARNING(Render_Vulkan, "Overlay failed to parse Switch shared font");
        return false;
    }

    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    const float scale = stbtt_ScaleForPixelHeight(&info, RuntimeBakePixelHeight);
    next.ascent = static_cast<float>(ascent) * scale;
    next.descent = -static_cast<float>(descent) * scale;
    next.line_height = static_cast<float>(ascent - descent + line_gap) * scale;

    stbtt_pack_context context{};
    if (!stbtt_PackBegin(&context, next.pixels.data(), RuntimeAtlasWidth, RuntimeAtlasHeight, 0, 1,
                         nullptr)) {
        LOG_WARNING(Render_Vulkan, "Overlay failed to begin runtime font packing");
        return false;
    }
    stbtt_PackSetOversampling(&context, 1, 1);
    stbtt_PackSetSkipMissingCodepoints(&context, 1);

    const bool text_ok =
        PackCodepoints(context, font_data, BuildTextCodepoints(), next.ascent, next.glyphs);

    std::vector<unsigned char> material_font = LoadMaterialFont();
    if (!material_font.empty()) {
        std::vector<u32> icon_codepoints(MaterialIconCodepoints.begin(), MaterialIconCodepoints.end());
        PackCodepoints(context, material_font, icon_codepoints,
                       FontAscentForPixelHeight(material_font, RuntimeBakePixelHeight),
                       next.glyphs);
    }

    stbtt_PackEnd(&context);

    next.pixels[0] = 255;
    if (!text_ok || next.glyphs.empty()) {
        LOG_WARNING(Render_Vulkan, "Overlay runtime font packing produced no text glyphs");
        return false;
    }

    next.ready = true;
    runtime = std::move(next);
    LOG_INFO(Render_Vulkan, "Overlay runtime font atlas ready: {} glyphs, {}x{}",
             runtime.glyphs.size(), RuntimeAtlasWidth, RuntimeAtlasHeight);
    return true;
}

void Shutdown() {
    runtime = {};
}

int AtlasWidth() {
    return runtime.ready ? RuntimeAtlasWidth : kAtlasWidth;
}

int AtlasHeight() {
    return runtime.ready ? RuntimeAtlasHeight : kAtlasHeight;
}

const unsigned char* AtlasData() {
    return runtime.ready ? runtime.pixels.data() : kAtlas;
}

std::size_t AtlasSize() {
    return static_cast<std::size_t>(AtlasWidth()) * static_cast<std::size_t>(AtlasHeight());
}

float BakePixelHeight() {
    return runtime.ready ? RuntimeBakePixelHeight : kBakePixelHeight;
}

float Ascent() {
    return runtime.ready ? runtime.ascent : kAscent;
}

float Descent() {
    return runtime.ready ? runtime.descent : kDescent;
}

float LineHeight() {
    return runtime.ready ? runtime.line_height : kLineHeight;
}

float WhiteU() {
    return runtime.ready ? 0.5f / RuntimeAtlasWidth : kWhiteU;
}

float WhiteV() {
    return runtime.ready ? 0.5f / RuntimeAtlasHeight : kWhiteV;
}

const Glyph& GlyphFor(u32 codepoint) {
    if (runtime.ready) {
        const auto it = runtime.glyphs.find(codepoint);
        if (it != runtime.glyphs.end()) {
            return it->second;
        }
    }
    return StaticGlyphFor(codepoint);
}

} // namespace Vulkan::OverlayFont
