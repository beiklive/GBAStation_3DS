// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "GBAStation/display_settings.h"
#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace SwitchFrontend::VulkanMenuRenderer {

struct FileEntry {
    std::string name;
    std::string path;
    std::string modified_time;
    u64 size{};
    bool directory{};
};

enum class Item {
    Resume,
    SaveState,
    LoadState,
    Cheats,
    Display,
    Reset,
    Exit,
    Count,
};

struct State {
    bool menu_visible{true};
    bool fast_forward_active{};
    bool show_fps{};
    float current_fps{};
    Item item{Item::Resume};
    bool content_focused{};
    int content_focus{};
    bool custom_layout_sidebar{};
    int custom_layout_focus{};
    bool overlay_sidebar{};
    int overlay_focus{};
    bool file_picker{};
    int file_picker_focus{};
    std::string file_picker_path;
    std::vector<FileEntry> file_entries;
    bool file_preview{};
    std::string file_preview_path;
    std::string toast;
    std::array<bool, 10> occupied{};
    GBAStationDisplaySettings display{};
};

bool Init(const Vulkan::Instance& instance);
void Draw(vk::CommandBuffer command_buffer, vk::Image image, vk::Extent2D extent,
          vk::Format format, const State& state);
void ResetSwapchain();
void Shutdown();

} // namespace SwitchFrontend::VulkanMenuRenderer
