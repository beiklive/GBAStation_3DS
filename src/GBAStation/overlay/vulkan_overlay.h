// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "GBAStation/display_settings.h"
#include "GBAStation/switch_libnx.h"

namespace Vulkan {
class RendererVulkan;
}

namespace SwitchFrontend::VulkanOverlay {

struct CheatEntry {
    std::string name;
    std::string description;
    bool enabled{};
};

using CheatToggleCallback = std::function<bool(std::size_t index, bool enabled)>;

// Initializes the GBAStation menu against Azahar's Vulkan renderer and registers
// its present-time draw callback.
bool Init(Vulkan::RendererVulkan& renderer);

// Polls the pad: toggles the menu on ZL+ZR and feeds D-pad/stick navigation.
void Update(PadState* pad);

bool IsVisible();
bool ShouldExit();

// Returns and clears the last queued OverlayUI::Action (0 when none).
int ConsumeAction();

void SetDisplaySettings(const GBAStationDisplaySettings& settings);
GBAStationDisplaySettings GetDisplaySettings();
void SetFastForwardActive(bool active);
void SetFpsOverlay(bool visible, float fps);
// Replaces the menu's immutable cheat snapshot. The callback is invoked from the
// frontend input thread when the user toggles an entry.
void SetCheats(std::vector<CheatEntry> cheats, CheatToggleCallback on_toggle);
void PrepareForShutdown();

void Shutdown();

} // namespace SwitchFrontend::VulkanOverlay
