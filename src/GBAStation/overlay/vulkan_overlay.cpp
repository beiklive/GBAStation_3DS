// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "GBAStation/input_mapping.h"
#include "GBAStation/overlay/overlay_ui.h"
#include "GBAStation/overlay/vulkan_menu_renderer.h"
#include "GBAStation/overlay/vulkan_overlay.h"
#include "audio_core/libnx_sink.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"

namespace SwitchFrontend::VulkanOverlay {
namespace {

constexpr const char* Tag = "[gbastation-3ds-menu]";
constexpr int MenuItemCount = static_cast<int>(VulkanMenuRenderer::Item::Count);
constexpr int DisplayControlCount = 7;
constexpr int CustomLayoutControlCount = 6;
constexpr float SelectorInitialDelayMs = 320.0f;
constexpr float NavigationInitialDelayMs = 280.0f;
constexpr const char* OverlayRoot = "sdmc:/GBAStation/overlays";
constexpr std::array<float, 10> FastForwardValues{{
    0.1f, 0.5f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f, 4.0f, 5.0f,
}};
constexpr std::array<const char*, 8> LayoutIds{{
    "vertical", "horizontal", "priority_top", "priority_bottom",
    "hybrid", "top", "bottom", "custom",
}};

std::atomic_bool initialized{};
std::atomic_bool visible{};
std::atomic_bool exit_requested{};
std::atomic_bool content_focused{};
std::atomic_int pending_action{};
std::atomic_int selected_item{};
std::atomic_int content_focus{};
std::atomic_bool custom_layout_sidebar{};
std::atomic_int custom_layout_focus{};
std::atomic_int display_layout{2};
std::atomic_int display_orientation{};
std::atomic<float> fast_forward_multiplier{4.0f};
std::atomic_bool display_integer_scale{true};
std::atomic_int display_gap{};
std::atomic<float> display_top_scale{1.0f};
std::atomic<float> display_top_offset_x{};
std::atomic<float> display_top_offset_y{};
std::atomic<float> display_bottom_scale{1.0f};
std::atomic<float> display_bottom_offset_x{};
std::atomic<float> display_bottom_offset_y{};
std::atomic_bool display_overlay_enabled{};
std::atomic_bool overlay_sidebar{};
std::atomic_int overlay_focus{};
std::atomic_bool file_picker{};
std::atomic_int file_picker_focus{};

std::mutex display_data_mutex;
std::string display_overlay_path;
std::string file_picker_path{OverlayRoot};
std::vector<VulkanMenuRenderer::FileEntry> file_entries;

bool previous_combo{};
vk::Device device;

struct PreviousNavigation {
    bool up{};
    bool down{};
    bool left{};
    bool right{};
    bool accept{};
    bool cancel{};
};
PreviousNavigation previous_navigation{};
u64 selector_repeat_start{};
u64 selector_repeat_last{};
int selector_repeat_direction{};
u64 navigation_repeat_start{};
u64 navigation_repeat_last{};
int navigation_repeat_direction{};

bool ItemHasContent(int item) {
    return item >= static_cast<int>(VulkanMenuRenderer::Item::SaveState) &&
           item <= static_cast<int>(VulkanMenuRenderer::Item::Display);
}

int LayoutIndex(const std::string& layout) {
    const auto it = std::find(LayoutIds.begin(), LayoutIds.end(), layout);
    return it == LayoutIds.end() ? 2 : static_cast<int>(std::distance(LayoutIds.begin(), it));
}

float StepFloat(std::atomic<float>& value, int direction, float step, float minimum,
                float maximum) {
    const float next = std::clamp(value.load(std::memory_order_relaxed) + direction * step,
                                  minimum, maximum);
    value.store(next, std::memory_order_release);
    return next;
}

bool CustomLayoutEnabled() {
    return display_layout.load(std::memory_order_acquire) == 7;
}

int NextDisplayFocus(int focus, int direction) {
    for (int i = 0; i < DisplayControlCount; ++i) {
        focus = (focus + direction + DisplayControlCount) % DisplayControlCount;
        if (focus != 3 || CustomLayoutEnabled()) {
            return focus;
        }
    }
    return focus;
}

int UpdateHeldNavigation(bool up, bool down) {
    const int direction = up == down ? 0 : (down ? 1 : -1);
    if (direction == 0) {
        navigation_repeat_direction = 0;
        return 0;
    }
    const u64 now = armGetSystemTick();
    if (navigation_repeat_direction != direction) {
        navigation_repeat_direction = direction;
        navigation_repeat_start = now;
        navigation_repeat_last = now;
        return 0;
    }
    const float held_ms =
        static_cast<float>(armTicksToNs(now - navigation_repeat_start)) / 1000000.0f;
    if (held_ms < NavigationInitialDelayMs) {
        return 0;
    }
    const float interval_ms =
        std::max(48.0f, 128.0f - (held_ms - NavigationInitialDelayMs) * 0.12f);
    const float since_last_ms =
        static_cast<float>(armTicksToNs(now - navigation_repeat_last)) / 1000000.0f;
    if (since_last_ms < interval_ms) {
        return 0;
    }
    navigation_repeat_last = now;
    return direction;
}

bool EndsWithPng(const std::string& value) {
    if (value.size() < 4) {
        return false;
    }
    std::string suffix = value.substr(value.size() - 4);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix == ".png";
}

void ReloadFilePicker(const std::string& requested_path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path directory = requested_path.empty() ? fs::path{OverlayRoot} : fs::path{requested_path};
    if (!fs::is_directory(directory, ec)) {
        directory = fs::path{OverlayRoot};
    }
    fs::create_directories(fs::path{OverlayRoot}, ec);

    std::vector<VulkanMenuRenderer::FileEntry> next;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const bool is_directory = it->is_directory(ec);
        const std::string path = it->path().string();
        if (!is_directory && !EndsWithPng(path)) {
            continue;
        }
        next.push_back({it->path().filename().string(), path, is_directory});
    }
    std::sort(next.begin(), next.end(), [](const auto& left_entry, const auto& right_entry) {
        if (left_entry.directory != right_entry.directory) {
            return left_entry.directory > right_entry.directory;
        }
        return left_entry.name < right_entry.name;
    });
    std::lock_guard lock{display_data_mutex};
    file_picker_path = directory.string();
    file_entries = std::move(next);
    file_picker_focus.store(0, std::memory_order_release);
}

int UpdateHeldAdjustment(bool active, bool decrease, bool increase) {
    const int direction = increase ? 1 : (decrease ? -1 : 0);
    if (!active || direction == 0) {
        selector_repeat_direction = 0;
        return 0;
    }
    const u64 now = armGetSystemTick();
    if (selector_repeat_direction != direction) {
        selector_repeat_direction = direction;
        selector_repeat_start = now;
        selector_repeat_last = now;
        return 0;
    }
    const float held_ms = static_cast<float>(armTicksToNs(now - selector_repeat_start)) / 1000000.0f;
    if (held_ms < SelectorInitialDelayMs) {
        return 0;
    }
    const float interval_ms =
        std::max(52.0f, 180.0f - (held_ms - SelectorInitialDelayMs) * 0.25f);
    const float since_last_ms =
        static_cast<float>(armTicksToNs(now - selector_repeat_last)) / 1000000.0f;
    if (since_last_ms < interval_ms) {
        return 0;
    }
    selector_repeat_last = now;
    return direction;
}

void PublishAction(OverlayUI::Action action, bool close_menu) {
    pending_action.store(static_cast<int>(action), std::memory_order_release);
    if (action == OverlayUI::Action::Exit) {
        exit_requested.store(true, std::memory_order_release);
    }
    if (close_menu) {
        visible.store(false, std::memory_order_release);
        content_focused.store(false, std::memory_order_release);
    }
}

OverlayUI::Action SaveAction(int slot) {
    return static_cast<OverlayUI::Action>(
        static_cast<int>(OverlayUI::Action::SaveStateSlot1) + slot);
}

OverlayUI::Action LoadAction(int slot) {
    return static_cast<OverlayUI::Action>(
        static_cast<int>(OverlayUI::Action::LoadStateSlot1) + slot);
}

void DrawCallback(vk::CommandBuffer command_buffer, vk::Image image, vk::Extent2D extent,
                  vk::Format format) {
    if (!initialized.load(std::memory_order_acquire)) {
        return;
    }
    VulkanMenuRenderer::State state{};
    state.menu_visible = visible.load(std::memory_order_acquire);
    state.item = static_cast<VulkanMenuRenderer::Item>(std::clamp(
        selected_item.load(std::memory_order_relaxed), 0, MenuItemCount - 1));
    state.content_focused = content_focused.load(std::memory_order_relaxed);
    state.content_focus = content_focus.load(std::memory_order_relaxed);
    state.custom_layout_sidebar = custom_layout_sidebar.load(std::memory_order_relaxed);
    state.custom_layout_focus = custom_layout_focus.load(std::memory_order_relaxed);
    state.overlay_sidebar = overlay_sidebar.load(std::memory_order_relaxed);
    state.overlay_focus = overlay_focus.load(std::memory_order_relaxed);
    state.file_picker = file_picker.load(std::memory_order_relaxed);
    state.file_picker_focus = file_picker_focus.load(std::memory_order_relaxed);
    {
        std::lock_guard lock{display_data_mutex};
        state.file_picker_path = file_picker_path;
        state.file_entries = file_entries;
    }
    state.toast = OverlayUI::GetToast();
    for (int slot = 0; slot < static_cast<int>(state.occupied.size()); ++slot) {
        state.occupied[slot] = OverlayUI::IsSlotOccupied(slot + 1);
    }
    state.display = GetDisplaySettings();
    if (!state.menu_visible && !state.display.overlay_enabled && state.toast.empty()) {
        return;
    }
    VulkanMenuRenderer::Draw(command_buffer, image, extent, format, state);
}

void ResetCallback() {
    VulkanMenuRenderer::ResetSwapchain();
}

void HandleDisplayAdjustment(int row, int direction) {
    switch (row) {
    case 0:
    {
        int index = 8;
        const float current = fast_forward_multiplier.load(std::memory_order_relaxed);
        for (int i = 0; i < static_cast<int>(FastForwardValues.size()); ++i) {
            if (std::fabs(FastForwardValues[i] - current) < 0.01f) {
                index = i;
                break;
            }
        }
        index = (index + direction + static_cast<int>(FastForwardValues.size())) %
                static_cast<int>(FastForwardValues.size());
        fast_forward_multiplier.store(FastForwardValues[index], std::memory_order_release);
        AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Slider);
        PublishAction(OverlayUI::Action::FastForwardMultiplierChanged, false);
        return;
    }
    case 1:
        display_integer_scale.store(!display_integer_scale.load(std::memory_order_relaxed),
                                    std::memory_order_release);
        break;
    case 2:
        display_layout.store((display_layout.load(std::memory_order_relaxed) + direction +
                              static_cast<int>(LayoutIds.size())) %
                                 static_cast<int>(LayoutIds.size()),
                             std::memory_order_release);
        break;
    case 4:
        display_orientation.store(
            (display_orientation.load(std::memory_order_relaxed) + direction + 4) % 4,
            std::memory_order_release);
        break;
    case 5:
        display_gap.store(std::clamp(display_gap.load(std::memory_order_relaxed) + direction,
                                     -256, 256),
                          std::memory_order_release);
        break;
    default:
        return;
    }
    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Slider);
    PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
}

void HandleOverlayToggle() {
    display_overlay_enabled.store(
        !display_overlay_enabled.load(std::memory_order_relaxed), std::memory_order_release);
    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
    PublishAction(OverlayUI::Action::OverlaySettingsChanged, false);
}

void HandleCustomLayoutAdjustment(int row, int direction) {
    switch (row) {
    case 0:
        StepFloat(display_top_scale, direction, 0.1f, 1.0f, 10.0f);
        break;
    case 1:
        StepFloat(display_top_offset_x, direction, 1.0f, -1024.0f, 1024.0f);
        break;
    case 2:
        StepFloat(display_top_offset_y, direction, 1.0f, -1024.0f, 1024.0f);
        break;
    case 3:
        StepFloat(display_bottom_scale, direction, 0.1f, 1.0f, 10.0f);
        break;
    case 4:
        StepFloat(display_bottom_offset_x, direction, 1.0f, -1024.0f, 1024.0f);
        break;
    case 5:
        StepFloat(display_bottom_offset_y, direction, 1.0f, -1024.0f, 1024.0f);
        break;
    default:
        return;
    }
    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Slider);
    PublishAction(OverlayUI::Action::CustomLayoutChanged, false);
}

void ResetCustomLayoutValue(int row) {
    switch (row) {
    case 0: display_top_scale.store(1.0f, std::memory_order_release); break;
    case 1: display_top_offset_x.store(0.0f, std::memory_order_release); break;
    case 2: display_top_offset_y.store(0.0f, std::memory_order_release); break;
    case 3: display_bottom_scale.store(1.0f, std::memory_order_release); break;
    case 4: display_bottom_offset_x.store(0.0f, std::memory_order_release); break;
    case 5: display_bottom_offset_y.store(0.0f, std::memory_order_release); break;
    default: return;
    }
    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
    PublishAction(OverlayUI::Action::CustomLayoutChanged, false);
}

void ResetScreenGap() {
    if (display_gap.exchange(0, std::memory_order_acq_rel) == 0) {
        return;
    }
    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
    PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
}

} // namespace

bool Init(Vulkan::RendererVulkan& renderer) {
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }
    const Vulkan::Instance& renderer_instance = renderer.GetVulkanInstance();
    device = renderer_instance.GetDevice();
    if (!device || !VulkanMenuRenderer::Init(renderer_instance)) {
        return false;
    }
    visible.store(false);
    exit_requested.store(false);
    content_focused.store(false);
    pending_action.store(0);
    selected_item.store(0);
    content_focus.store(0);
    custom_layout_sidebar.store(false);
    custom_layout_focus.store(0);
    overlay_sidebar.store(false);
    overlay_focus.store(0);
    file_picker.store(false);
    file_picker_focus.store(0);
    previous_combo = false;
    previous_navigation = {};
    selector_repeat_start = 0;
    selector_repeat_last = 0;
    selector_repeat_direction = 0;
    navigation_repeat_start = 0;
    navigation_repeat_last = 0;
    navigation_repeat_direction = 0;
    Vulkan::SetOverlayResetCallback(&ResetCallback);
    Vulkan::SetOverlayDrawCallback(&DrawCallback);
    initialized.store(true, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "{} NDS-style Vulkan menu initialized", Tag);
    return true;
}

void Update(PadState* pad) {
    if (!initialized.load(std::memory_order_acquire) || !pad) {
        return;
    }
    const u64 held = padGetButtons(pad);
    const u64 menu_mask = InputMapping::MenuHotkeyMask();
    const bool combo = menu_mask != 0 && (held & menu_mask) == menu_mask;
    if (combo && !previous_combo) {
        const bool next_visible = !visible.load(std::memory_order_relaxed);
        if (!next_visible && custom_layout_sidebar.exchange(false, std::memory_order_acq_rel)) {
            pending_action.store(static_cast<int>(OverlayUI::Action::CustomLayoutCommitted),
                                 std::memory_order_release);
            selector_repeat_direction = 0;
        }
        if (!next_visible && (overlay_sidebar.exchange(false, std::memory_order_acq_rel) ||
                             file_picker.exchange(false, std::memory_order_acq_rel))) {
            pending_action.store(static_cast<int>(OverlayUI::Action::OverlaySettingsCommitted),
                                 std::memory_order_release);
        }
        visible.store(next_visible, std::memory_order_release);
        AudioCore::PlayLibnxUiSound(next_visible ? AudioCore::LibnxUiSound::Click
                                                : AudioCore::LibnxUiSound::Back);
        content_focused.store(false, std::memory_order_release);
        if (next_visible) {
            selected_item.store(0, std::memory_order_release);
            content_focus.store(0, std::memory_order_release);
            custom_layout_sidebar.store(false, std::memory_order_release);
            custom_layout_focus.store(0, std::memory_order_release);
            overlay_sidebar.store(false, std::memory_order_release);
            overlay_focus.store(0, std::memory_order_release);
            file_picker.store(false, std::memory_order_release);
        }
    }
    previous_combo = combo;

    const bool up = (held & (HidNpadButton_AnyUp | HidNpadButton_StickLUp)) != 0;
    const bool down = (held & (HidNpadButton_AnyDown | HidNpadButton_StickLDown)) != 0;
    const bool left =
        (held & (HidNpadButton_AnyLeft | HidNpadButton_StickLLeft | HidNpadButton_L)) != 0;
    const bool right =
        (held & (HidNpadButton_AnyRight | HidNpadButton_StickLRight | HidNpadButton_R)) != 0;
    const bool shoulder_left = (held & HidNpadButton_L) != 0;
    const bool shoulder_right = (held & HidNpadButton_R) != 0;
    const bool accept = (held & HidNpadButton_A) != 0;
    const bool cancel = (held & HidNpadButton_B) != 0;

    if (visible.load(std::memory_order_acquire)) {
        if (file_picker.load(std::memory_order_acquire)) {
            const int repeated_navigation = UpdateHeldNavigation(up, down);
            int focus = file_picker_focus.load(std::memory_order_relaxed);
            std::vector<VulkanMenuRenderer::FileEntry> entries;
            {
                std::lock_guard lock{display_data_mutex};
                entries = file_entries;
            }
            const int count = static_cast<int>(entries.size());
            const int previous_focus = focus;
            if (count > 0 && ((up && !previous_navigation.up) || repeated_navigation < 0)) {
                focus = (focus - 1 + count) % count;
            }
            if (count > 0 && ((down && !previous_navigation.down) || repeated_navigation > 0)) {
                focus = (focus + 1) % count;
            }
            file_picker_focus.store(focus, std::memory_order_release);
            if (focus != previous_focus) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Focus);
            }
            if (cancel && !previous_navigation.cancel) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Back);
                file_picker.store(false, std::memory_order_release);
                overlay_sidebar.store(true, std::memory_order_release);
                navigation_repeat_direction = 0;
            } else if (accept && !previous_navigation.accept && count > 0) {
                const auto entry = entries[std::clamp(focus, 0, count - 1)];
                if (entry.directory) {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    ReloadFilePicker(entry.path);
                } else if (EndsWithPng(entry.path)) {
                    {
                        std::lock_guard lock{display_data_mutex};
                        display_overlay_path = entry.path;
                    }
                    display_overlay_enabled.store(true, std::memory_order_release);
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    file_picker.store(false, std::memory_order_release);
                    overlay_sidebar.store(true, std::memory_order_release);
                    PublishAction(OverlayUI::Action::OverlaySettingsChanged, false);
                }
            }
            previous_navigation = {up, down, left, right, accept, cancel};
            return;
        }

        if (overlay_sidebar.load(std::memory_order_acquire)) {
            const int repeated_navigation = UpdateHeldNavigation(up, down);
            int focus = overlay_focus.load(std::memory_order_relaxed);
            const int previous_focus = focus;
            if ((up && !previous_navigation.up) || repeated_navigation < 0) {
                focus = (focus + 1) % 2;
            }
            if ((down && !previous_navigation.down) || repeated_navigation > 0) {
                focus = (focus + 1) % 2;
            }
            overlay_focus.store(focus, std::memory_order_release);
            if (focus != previous_focus) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Focus);
            }
            if (cancel && !previous_navigation.cancel) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Back);
                overlay_sidebar.store(false, std::memory_order_release);
                navigation_repeat_direction = 0;
                PublishAction(OverlayUI::Action::OverlaySettingsCommitted, false);
            } else if (accept && !previous_navigation.accept) {
                if (focus == 0) {
                    HandleOverlayToggle();
                } else {
                    std::string start_path;
                    {
                        std::lock_guard lock{display_data_mutex};
                        start_path = display_overlay_path;
                    }
                    if (!start_path.empty()) {
                        start_path = std::filesystem::path{start_path}.parent_path().string();
                    }
                    ReloadFilePicker(start_path);
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    overlay_sidebar.store(false, std::memory_order_release);
                    file_picker.store(true, std::memory_order_release);
                }
            }
            previous_navigation = {up, down, left, right, accept, cancel};
            return;
        }

        if (custom_layout_sidebar.load(std::memory_order_acquire)) {
            const int repeated_navigation = UpdateHeldNavigation(up, down);
            const int repeated_direction =
                UpdateHeldAdjustment(true, shoulder_left, shoulder_right);
            int focus = custom_layout_focus.load(std::memory_order_relaxed);
            const int previous_focus = focus;
            if ((up && !previous_navigation.up) || repeated_navigation < 0) {
                focus = (focus - 1 + CustomLayoutControlCount) % CustomLayoutControlCount;
            }
            if ((down && !previous_navigation.down) || repeated_navigation > 0) {
                focus = (focus + 1) % CustomLayoutControlCount;
            }
            custom_layout_focus.store(focus, std::memory_order_release);
            if (focus != previous_focus) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Focus);
            }
            if (cancel && !previous_navigation.cancel) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Back);
                custom_layout_sidebar.store(false, std::memory_order_release);
                selector_repeat_direction = 0;
                PublishAction(OverlayUI::Action::CustomLayoutCommitted, false);
            } else if ((left && !previous_navigation.left) ||
                       (right && !previous_navigation.right) || repeated_direction != 0) {
                HandleCustomLayoutAdjustment(
                    focus, repeated_direction != 0 ? repeated_direction : (right ? 1 : -1));
            } else if (accept && !previous_navigation.accept) {
                ResetCustomLayoutValue(focus);
            }
            previous_navigation = {up, down, left, right, accept, cancel};
            return;
        }

        int selected = selected_item.load(std::memory_order_relaxed);
        int focus = content_focus.load(std::memory_order_relaxed);
        const bool in_content = content_focused.load(std::memory_order_relaxed);
        const int repeated_navigation = UpdateHeldNavigation(up, down);

        if (!in_content) {
            UpdateHeldAdjustment(false, false, false);
            const int previous_selected = selected;
            if ((up && !previous_navigation.up) || repeated_navigation < 0) {
                selected = (selected - 1 + MenuItemCount) % MenuItemCount;
            }
            if ((down && !previous_navigation.down) || repeated_navigation > 0) {
                selected = (selected + 1) % MenuItemCount;
            }
            selected_item.store(selected, std::memory_order_release);
            if (selected != previous_selected) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Focus);
            }

            if (cancel && !previous_navigation.cancel) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Back);
                PublishAction(OverlayUI::Action::Resume, true);
            } else if ((right && !previous_navigation.right) ||
                       (accept && !previous_navigation.accept && ItemHasContent(selected))) {
                if (ItemHasContent(selected)) {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    content_focus.store(0, std::memory_order_release);
                    content_focused.store(true, std::memory_order_release);
                }
            } else if (accept && !previous_navigation.accept) {
                if (selected == static_cast<int>(VulkanMenuRenderer::Item::Resume)) {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    PublishAction(OverlayUI::Action::Resume, true);
                } else if (selected == static_cast<int>(VulkanMenuRenderer::Item::Reset)) {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    PublishAction(OverlayUI::Action::Reset, true);
                } else if (selected == static_cast<int>(VulkanMenuRenderer::Item::Exit)) {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    PublishAction(OverlayUI::Action::Exit, true);
                }
            }
        } else {
            const auto item = static_cast<VulkanMenuRenderer::Item>(selected);
            const int repeated_direction = UpdateHeldAdjustment(
                item == VulkanMenuRenderer::Item::Display, shoulder_left, shoulder_right);
            const int count = (item == VulkanMenuRenderer::Item::SaveState ||
                               item == VulkanMenuRenderer::Item::LoadState)
                                  ? 10
                                  : (item == VulkanMenuRenderer::Item::Display ? DisplayControlCount
                                                                               : 1);
            const int previous_focus = focus;
            if ((up && !previous_navigation.up) || repeated_navigation < 0) {
                focus = item == VulkanMenuRenderer::Item::Display
                            ? NextDisplayFocus(focus, -1)
                            : (focus - 1 + count) % count;
            }
            if ((down && !previous_navigation.down) || repeated_navigation > 0) {
                focus = item == VulkanMenuRenderer::Item::Display
                            ? NextDisplayFocus(focus, 1)
                            : (focus + 1) % count;
            }
            content_focus.store(focus, std::memory_order_release);
            if (focus != previous_focus) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Focus);
            }

            if ((cancel && !previous_navigation.cancel) ||
                (left && !previous_navigation.left &&
                 item != VulkanMenuRenderer::Item::Display)) {
                AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Back);
                content_focused.store(false, std::memory_order_release);
            } else if (item == VulkanMenuRenderer::Item::Display &&
                       ((left && !previous_navigation.left) ||
                        (right && !previous_navigation.right) || repeated_direction != 0)) {
                HandleDisplayAdjustment(
                    focus, repeated_direction != 0 ? repeated_direction : (right ? 1 : -1));
            } else if (accept && !previous_navigation.accept) {
                if (item == VulkanMenuRenderer::Item::SaveState) {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                    PublishAction(SaveAction(focus), true);
                } else if (item == VulkanMenuRenderer::Item::LoadState) {
                    if (OverlayUI::IsSlotOccupied(focus + 1)) {
                        AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                        PublishAction(LoadAction(focus), true);
                    } else {
                        AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Error);
                        OverlayUI::ShowToast("该存档位为空");
                    }
                } else if (item == VulkanMenuRenderer::Item::Display) {
                    if (focus == 1) {
                        HandleDisplayAdjustment(focus, 1);
                    } else if (focus == 3) {
                        if (CustomLayoutEnabled()) {
                            AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                            custom_layout_focus.store(0, std::memory_order_release);
                            custom_layout_sidebar.store(true, std::memory_order_release);
                            selector_repeat_direction = 0;
                        } else {
                            AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Error);
                        }
                    } else if (focus == 5) {
                        ResetScreenGap();
                    } else if (focus == 6) {
                        AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Click);
                        overlay_focus.store(0, std::memory_order_release);
                        overlay_sidebar.store(true, std::memory_order_release);
                        navigation_repeat_direction = 0;
                    }
                } else {
                    AudioCore::PlayLibnxUiSound(AudioCore::LibnxUiSound::Error);
                    OverlayUI::ShowToast("暂无可用金手指");
                }
            }
        }
    }
    previous_navigation = {up, down, left, right, accept, cancel};
}

bool IsVisible() {
    return visible.load(std::memory_order_acquire);
}

bool ShouldExit() {
    return exit_requested.load(std::memory_order_acquire);
}

int ConsumeAction() {
    return pending_action.exchange(0, std::memory_order_acq_rel);
}

void SetDisplaySettings(const GBAStationDisplaySettings& settings) {
    fast_forward_multiplier.store(
        std::clamp(settings.fast_forward_multiplier, 0.1f, 5.0f),
        std::memory_order_release);
    display_layout.store(LayoutIndex(settings.screen_layout), std::memory_order_release);
    int orientation = 0;
    if (settings.screen_orientation == 90) orientation = 1;
    if (settings.screen_orientation == 180) orientation = 2;
    if (settings.screen_orientation == 270) orientation = 3;
    display_orientation.store(orientation, std::memory_order_release);
    display_integer_scale.store(settings.integer_scale, std::memory_order_release);
    display_gap.store(std::clamp(settings.screen_gap, -256, 256), std::memory_order_release);
    display_top_scale.store(std::clamp(settings.top_scale, 1.0f, 10.0f),
                            std::memory_order_release);
    display_top_offset_x.store(std::clamp(settings.top_offset_x, -1024.0f, 1024.0f),
                               std::memory_order_release);
    display_top_offset_y.store(std::clamp(settings.top_offset_y, -1024.0f, 1024.0f),
                               std::memory_order_release);
    display_bottom_scale.store(std::clamp(settings.bottom_scale, 1.0f, 10.0f),
                               std::memory_order_release);
    display_bottom_offset_x.store(
        std::clamp(settings.bottom_offset_x, -1024.0f, 1024.0f), std::memory_order_release);
    display_bottom_offset_y.store(
        std::clamp(settings.bottom_offset_y, -1024.0f, 1024.0f), std::memory_order_release);
    display_overlay_enabled.store(settings.overlay_enabled, std::memory_order_release);
    {
        std::lock_guard lock{display_data_mutex};
        display_overlay_path = settings.overlay_path;
    }
}

GBAStationDisplaySettings GetDisplaySettings() {
    GBAStationDisplaySettings settings;
    settings.fast_forward_multiplier =
        fast_forward_multiplier.load(std::memory_order_acquire);
    settings.screen_layout = LayoutIds[std::clamp(display_layout.load(std::memory_order_acquire),
                                                  0, static_cast<int>(LayoutIds.size()) - 1)];
    constexpr std::array<int, 4> Orientations{{0, 90, 180, 270}};
    settings.screen_orientation = Orientations[std::clamp(
        display_orientation.load(std::memory_order_acquire), 0, 3)];
    settings.integer_scale = display_integer_scale.load(std::memory_order_acquire);
    settings.screen_gap = display_gap.load(std::memory_order_acquire);
    settings.top_scale = display_top_scale.load(std::memory_order_acquire);
    settings.top_offset_x = display_top_offset_x.load(std::memory_order_acquire);
    settings.top_offset_y = display_top_offset_y.load(std::memory_order_acquire);
    settings.bottom_scale = display_bottom_scale.load(std::memory_order_acquire);
    settings.bottom_offset_x = display_bottom_offset_x.load(std::memory_order_acquire);
    settings.bottom_offset_y = display_bottom_offset_y.load(std::memory_order_acquire);
    settings.overlay_enabled = display_overlay_enabled.load(std::memory_order_acquire);
    {
        std::lock_guard lock{display_data_mutex};
        settings.overlay_path = display_overlay_path;
    }
    return settings;
}

void Shutdown() {
    if (!initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    Vulkan::SetOverlayDrawCallback(nullptr);
    Vulkan::SetOverlayResetCallback(nullptr);
    if (device) {
        device.waitIdle();
    }
    VulkanMenuRenderer::Shutdown();
    device = VK_NULL_HANDLE;
    visible.store(false);
    exit_requested.store(false);
    content_focused.store(false);
    custom_layout_sidebar.store(false);
    overlay_sidebar.store(false);
    file_picker.store(false);
    selector_repeat_direction = 0;
    navigation_repeat_direction = 0;
    pending_action.store(0);
    selected_item.store(0);
    content_focus.store(0);
    LOG_INFO(Render_Vulkan, "{} shutdown complete", Tag);
}

} // namespace SwitchFrontend::VulkanOverlay
