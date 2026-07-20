// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "GBAStation/emu_window_switch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "common/settings.h"
#include "core/3ds.h"
#include "core/frontend/framebuffer_layout.h"

namespace SwitchFrontend {
namespace {
constexpr u32 DefaultWidth = 1280;
constexpr u32 DefaultHeight = 720;
constexpr u32 TouchWidth = 1280;
constexpr u32 TouchHeight = 720;
constexpr float CursorMaxSpeed = 220.0f;
constexpr float CursorDeadzone = 0.24f;
constexpr float CursorDefaultFrameTime = 1.0f / 60.0f;
constexpr float CursorMaxFrameTime = 1.0f / 30.0f;
constexpr float StickScale = 1.0f / 32767.0f;

struct Region {
    float x;
    float y;
    float width;
    float height;
};

float FitScale(const Region& region, float native_width, float native_height,
               bool integer_scale) {
    float scale = std::max(0.01f, std::min(region.width / native_width,
                                           region.height / native_height));
    if (integer_scale && scale >= 1.0f) {
        scale = std::floor(scale);
    }
    return std::max(0.01f, scale);
}

Common::Rectangle<u32> PlaceScreen(const Region& region, float native_width, float native_height,
                                   bool integer_scale, float scale_multiplier = 1.0f,
                                   float offset_x = 0.0f, float offset_y = 0.0f) {
    float scale = FitScale(region, native_width, native_height, integer_scale);
    scale *= std::max(0.01f, scale_multiplier);
    float screen_width = std::min(native_width * scale, region.width);
    float screen_height = std::min(native_height * scale, region.height);
    float left = region.x + (region.width - screen_width) * 0.5f + offset_x;
    float top = region.y + (region.height - screen_height) * 0.5f + offset_y;
    left = std::clamp(left, region.x, region.x + std::max(0.0f, region.width - screen_width));
    top = std::clamp(top, region.y, region.y + std::max(0.0f, region.height - screen_height));
    const u32 x = static_cast<u32>(std::max(0.0f, std::round(left)));
    const u32 y = static_cast<u32>(std::max(0.0f, std::round(top)));
    const u32 right = static_cast<u32>(std::max<float>(x + 1, std::round(left + screen_width)));
    const u32 bottom = static_cast<u32>(std::max<float>(y + 1, std::round(top + screen_height)));
    return {x, y, right, bottom};
}

Common::Rectangle<u32> PlaceCustomScreen(float framebuffer_width, float framebuffer_height,
                                         float native_width, float native_height, float scale,
                                         float center_x, float center_y, float offset_x,
                                         float offset_y) {
    scale = std::clamp(scale, 1.0f, 10.0f);
    const float screen_width = native_width * scale;
    const float screen_height = native_height * scale;
    constexpr float movement_margin = 64.0f;
    if (screen_width + movement_margin * 2.0f <= framebuffer_width) {
        center_x = std::clamp(center_x, screen_width * 0.5f + movement_margin,
                              framebuffer_width - screen_width * 0.5f - movement_margin);
    } else if (screen_width <= framebuffer_width) {
        center_x = framebuffer_width * 0.5f;
    }
    if (screen_height + movement_margin * 2.0f <= framebuffer_height) {
        center_y = std::clamp(center_y, screen_height * 0.5f + movement_margin,
                              framebuffer_height - screen_height * 0.5f - movement_margin);
    } else if (screen_height <= framebuffer_height) {
        center_y = framebuffer_height * 0.5f;
    }
    float left = center_x - screen_width * 0.5f + offset_x;
    float top = center_y - screen_height * 0.5f + offset_y;
    // Framebuffer rectangles may extend past the right/bottom edge and are clipped by the
    // presenter. Keeping the requested position is what makes offsets useful at large scales.
    left = std::max(0.0f, left);
    top = std::max(0.0f, top);
    const u32 x = static_cast<u32>(std::round(left));
    const u32 y = static_cast<u32>(std::round(top));
    const u32 right = static_cast<u32>(std::round(left + screen_width));
    const u32 bottom = static_cast<u32>(std::round(top + screen_height));
    return {x, y, std::max(x + 1, right), std::max(y + 1, bottom)};
}

Layout::FramebufferLayout BuildLandscapeLayout(u32 width, u32 height,
                                               const GBAStationDisplaySettings& settings) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float gap = static_cast<float>(std::clamp(settings.screen_gap, -256, 256));
    const float positive_gap = std::max(0.0f, gap);
    const Region full{0.0f, 0.0f, w, h};
    Layout::FramebufferLayout layout{width, height, true, true, {}, {}, true};

    if (settings.screen_layout == "vertical") {
        const float row_height = std::max(1.0f, (h - gap) * 0.5f);
        const Region top{0.0f, 0.0f, w, row_height};
        const Region bottom{0.0f, row_height + gap, w, row_height};
        layout.top_screen = PlaceScreen(top, Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
        layout.bottom_screen = PlaceScreen(bottom, Core::kScreenBottomWidth,
                                           Core::kScreenBottomHeight, settings.integer_scale);
    } else if (settings.screen_layout == "horizontal") {
        const float available = std::max(2.0f, w - gap);
        const float top_width = available * 5.0f / 9.0f;
        const Region top{0.0f, 0.0f, top_width, h};
        const Region bottom{top_width + gap, 0.0f, available - top_width, h};
        layout.top_screen = PlaceScreen(top, Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
        layout.bottom_screen = PlaceScreen(bottom, Core::kScreenBottomWidth,
                                           Core::kScreenBottomHeight, settings.integer_scale);
    } else if (settings.screen_layout == "priority_bottom") {
        const float small_width = std::max(1.0f, (w - positive_gap) / 3.0f);
        const Region bottom{0.0f, 0.0f, w - small_width - positive_gap, h};
        const Region top{w - small_width, 0.0f, small_width, h};
        layout.bottom_screen = PlaceScreen(bottom, Core::kScreenBottomWidth,
                                           Core::kScreenBottomHeight, settings.integer_scale);
        layout.top_screen = PlaceScreen(top, Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
    } else if (settings.screen_layout == "hybrid") {
        const float right_width = std::max(1.0f, (w - positive_gap) / 3.0f);
        const float right_x = w - right_width;
        const float row_height = std::max(1.0f, (h - positive_gap) * 0.5f);
        layout.top_screen = PlaceScreen({0.0f, 0.0f, right_x - positive_gap, h},
                                        Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
        layout.additional_screen_enabled = true;
        layout.additional_screen_is_bottom = false;
        layout.additional_screen = PlaceScreen({right_x, 0.0f, right_width, row_height},
                                               Core::kScreenTopWidth, Core::kScreenTopHeight,
                                               settings.integer_scale);
        layout.bottom_screen = PlaceScreen(
            {right_x, row_height + positive_gap, right_width, row_height},
            Core::kScreenBottomWidth, Core::kScreenBottomHeight, settings.integer_scale);
    } else if (settings.screen_layout == "top") {
        layout.top_screen = PlaceScreen(full, Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
        layout.bottom_screen = PlaceScreen(full, Core::kScreenBottomWidth,
                                           Core::kScreenBottomHeight, settings.integer_scale);
        layout.bottom_screen_enabled = false;
    } else if (settings.screen_layout == "bottom") {
        layout.top_screen = PlaceScreen(full, Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
        layout.bottom_screen = PlaceScreen(full, Core::kScreenBottomWidth,
                                           Core::kScreenBottomHeight, settings.integer_scale);
        layout.top_screen_enabled = false;
    } else if (settings.screen_layout == "custom") {
        // Match nds_stub's custom canvas: both screens share the vertical center and use
        // independent offsets, with the large screen on the left and small screen on the right.
        const bool portrait_canvas = h > w;
        const float top_center_x = portrait_canvas ? w * 0.5f : w * (352.0f / 1280.0f);
        const float bottom_center_x = portrait_canvas ? w * 0.5f : w * (928.0f / 1280.0f);
        const float top_center_y = portrait_canvas ? h * 0.5f - Core::kScreenTopHeight * 0.5f
                                                   : h * 0.5f;
        const float bottom_center_y =
            portrait_canvas ? h * 0.5f + Core::kScreenBottomHeight * 0.5f : h * 0.5f;
        layout.top_screen = PlaceCustomScreen(
            w, h, Core::kScreenTopWidth, Core::kScreenTopHeight, settings.top_scale,
            top_center_x, top_center_y, settings.top_offset_x, settings.top_offset_y);
        layout.bottom_screen = PlaceCustomScreen(
            w, h, Core::kScreenBottomWidth, Core::kScreenBottomHeight, settings.bottom_scale,
            bottom_center_x, bottom_center_y, settings.bottom_offset_x,
            settings.bottom_offset_y);
    } else {
        const float small_width = std::max(1.0f, (w - positive_gap) / 3.0f);
        const Region top{0.0f, 0.0f, w - small_width - positive_gap, h};
        const Region bottom{w - small_width, 0.0f, small_width, h};
        layout.top_screen = PlaceScreen(top, Core::kScreenTopWidth, Core::kScreenTopHeight,
                                        settings.integer_scale);
        layout.bottom_screen = PlaceScreen(bottom, Core::kScreenBottomWidth,
                                           Core::kScreenBottomHeight, settings.integer_scale);
    }
    layout.render_3d_mode = Settings::values.render_3d.GetValue();
    return layout;
}

Layout::FramebufferLayout BuildDisplayLayout(u32 width, u32 height,
                                             const GBAStationDisplaySettings& settings) {
    const int orientation = settings.screen_orientation;
    if (orientation == 90 || orientation == 270) {
        auto layout = BuildLandscapeLayout(height, width, settings);
        layout.is_rotated = false;
        layout = Layout::reverseLayout(layout);
        if (orientation == 270) {
            layout = Layout::rotate180Layout(layout);
        }
        return layout;
    }
    auto layout = BuildLandscapeLayout(width, height, settings);
    if (orientation == 180) {
        layout = Layout::rotate180Layout(layout);
    }
    return layout;
}

std::pair<float, float> ApplyCursorResponse(float x_axis, float y_axis) {
    const float magnitude = std::min(std::sqrt(x_axis * x_axis + y_axis * y_axis), 1.0f);
    if (magnitude < CursorDeadzone) {
        return {0.0f, 0.0f};
    }

    const float normalized = (magnitude - CursorDeadzone) / (1.0f - CursorDeadzone);
    const float curved = normalized * normalized * normalized;
    const float scale = curved / magnitude;
    return {x_axis * scale, y_axis * scale};
}
} // namespace

EmuWindowSwitch::EmuWindowSwitch(NWindow* window_)
    : window{window_ ? window_ : nwindowGetDefault()},
      cursor_x{static_cast<float>(Core::kScreenBottomWidth) * 0.5f},
      cursor_y{static_cast<float>(Core::kScreenBottomHeight) * 0.5f} {
    window_info.type = Frontend::WindowSystemType::Switch;
    window_info.render_surface = window;
    window_info.render_surface_scale = 1.0f;
    padInitializeDefault(&cursor_pad);
    hidInitializeTouchScreen();
    RefreshDimensions();
}

void EmuWindowSwitch::PollEvents() {
    RefreshDimensions();
    if (input_suppressed) {
        if (physical_touch_pressed || cursor_touch_pressed) {
            TouchReleased();
            physical_touch_pressed = false;
            cursor_touch_pressed = false;
        }
        return;
    }
    const bool physical_touch_active = PollTouch();
    if (!physical_touch_active) {
        PollControllerCursor();
    }
}

void EmuWindowSwitch::SetInputSuppressed(bool suppressed) {
    input_suppressed = suppressed;
}

void EmuWindowSwitch::SetDisplaySettings(const GBAStationDisplaySettings& settings) {
    display_settings = settings;
    RefreshDimensions();
}

void EmuWindowSwitch::RefreshDimensions() {
    u32 width = DefaultWidth;
    u32 height = DefaultHeight;
    if (window) {
        static_cast<void>(nwindowGetDimensions(window, &width, &height));
        if (width == 0 || height == 0) {
            width = DefaultWidth;
            height = DefaultHeight;
        }
    }
    NotifyFramebufferLayoutChanged(BuildDisplayLayout(width, height, display_settings));
}

unsigned EmuWindowSwitch::ScaleTouchX(u32 touch_x) const {
    const auto& layout = GetFramebufferLayout();
    if (layout.width <= 1) {
        return 0;
    }

    const u64 scaled = static_cast<u64>(touch_x) * layout.width / TouchWidth;
    return static_cast<unsigned>(std::min<u64>(scaled, layout.width - 1));
}

unsigned EmuWindowSwitch::ScaleTouchY(u32 touch_y) const {
    const auto& layout = GetFramebufferLayout();
    if (layout.height <= 1) {
        return 0;
    }

    const u64 scaled = static_cast<u64>(touch_y) * layout.height / TouchHeight;
    return static_cast<unsigned>(std::min<u64>(scaled, layout.height - 1));
}

bool EmuWindowSwitch::PollTouch() {
    if (!GetFramebufferLayout().bottom_screen_enabled) {
        if (physical_touch_pressed || cursor_touch_pressed) {
            TouchReleased();
            physical_touch_pressed = false;
            cursor_touch_pressed = false;
        }
        return false;
    }

    HidTouchScreenState state{};
    if (!hidGetTouchScreenStates(&state, 1) || state.count <= 0) {
        if (physical_touch_pressed) {
            TouchReleased();
            physical_touch_pressed = false;
        }
        return false;
    }

    const HidTouchState& touch = state.touches[0];
    const unsigned x = ScaleTouchX(touch.x);
    const unsigned y = ScaleTouchY(touch.y);

    if (cursor_touch_pressed) {
        TouchReleased();
        cursor_touch_pressed = false;
    }

    if (physical_touch_pressed) {
        TouchMoved(x, y);
    } else {
        physical_touch_pressed = TouchPressed(x, y);
    }
    return true;
}

void EmuWindowSwitch::PollControllerCursor() {
    padUpdate(&cursor_pad);
    const u64 buttons = padGetButtons(&cursor_pad);
    const u64 buttons_down = padGetButtonsDown(&cursor_pad);
    const auto now = std::chrono::steady_clock::now();
    float delta_time = CursorDefaultFrameTime;
    if (last_cursor_update.time_since_epoch().count() != 0) {
        delta_time = std::chrono::duration<float>(now - last_cursor_update).count();
        delta_time = std::clamp(delta_time, 0.0f, CursorMaxFrameTime);
    }
    last_cursor_update = now;

    const bool toggle_cursor = (buttons_down & HidNpadButton_StickR) != 0;

    if (toggle_cursor) {
        cursor_visible = !cursor_visible;
        if (!cursor_visible && cursor_touch_pressed) {
            TouchReleased();
            cursor_touch_pressed = false;
        }
    }

    if (!cursor_visible || !GetFramebufferLayout().bottom_screen_enabled) {
        return;
    }

    const HidAnalogStickState stick = padGetStickPos(&cursor_pad, 1);
    float x_axis = std::clamp(static_cast<float>(stick.x) * StickScale, -1.0f, 1.0f);
    float y_axis = std::clamp(static_cast<float>(stick.y) * StickScale, -1.0f, 1.0f);

    std::tie(x_axis, y_axis) = ApplyCursorResponse(x_axis, y_axis);

    cursor_x = std::clamp(cursor_x + x_axis * CursorMaxSpeed * delta_time, 0.0f,
                          static_cast<float>(Core::kScreenBottomWidth - 1));
    cursor_y = std::clamp(cursor_y - y_axis * CursorMaxSpeed * delta_time, 0.0f,
                          static_cast<float>(Core::kScreenBottomHeight - 1));

    const auto [framebuffer_x, framebuffer_y] = CursorFramebufferPosition();
    if (buttons & HidNpadButton_ZR) {
        if (cursor_touch_pressed) {
            TouchMoved(framebuffer_x, framebuffer_y);
        } else {
            cursor_touch_pressed = TouchPressed(framebuffer_x, framebuffer_y);
        }
    } else if (cursor_touch_pressed) {
        TouchReleased();
        cursor_touch_pressed = false;
    }
}

std::pair<unsigned, unsigned> EmuWindowSwitch::CursorFramebufferPosition() const {
    const auto& bottom = GetFramebufferLayout().bottom_screen;
    const float projected_x =
        cursor_x * static_cast<float>(bottom.GetWidth()) / Core::kScreenBottomWidth;
    const float projected_y =
        cursor_y * static_cast<float>(bottom.GetHeight()) / Core::kScreenBottomHeight;
    const auto x = static_cast<unsigned>(bottom.left + static_cast<u32>(projected_x));
    const auto y = static_cast<unsigned>(bottom.top + static_cast<u32>(projected_y));
    return {std::min<unsigned>(x, bottom.right - 1), std::min<unsigned>(y, bottom.bottom - 1)};
}

Frontend::EmuWindow::CursorInfo EmuWindowSwitch::GetCursorInfo() const {
    if (!cursor_visible || !GetFramebufferLayout().bottom_screen_enabled) {
        return {};
    }

    const auto& bottom = GetFramebufferLayout().bottom_screen;
    return {
        .visible = true,
        .projected_x = cursor_x * static_cast<float>(bottom.GetWidth()) / Core::kScreenBottomWidth,
        .projected_y =
            cursor_y * static_cast<float>(bottom.GetHeight()) / Core::kScreenBottomHeight,
    };
}

} // namespace SwitchFrontend
