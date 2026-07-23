// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace SwitchFrontend {

constexpr float MinFastForwardMultiplier = 0.1f;
constexpr float MaxFastForwardMultiplier = 5.0f;

struct GBAStationDisplaySettings {
    float fast_forward_multiplier{4.0f};
    int internal_resolution{1};
    std::string screen_layout{"priority_top"};
    int screen_orientation{};
    bool integer_scale{true};
    int screen_gap{};
    float top_scale{1.0f};
    float top_offset_x{};
    float top_offset_y{};
    float bottom_scale{1.0f};
    float bottom_offset_x{};
    float bottom_offset_y{};
    float bottom_opacity{1.0f};
    bool overlay_enabled{};
    std::string overlay_path;
};

} // namespace SwitchFrontend
