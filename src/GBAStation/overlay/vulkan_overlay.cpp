// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/overlay/vulkan_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "GBAStation/input_mapping.h"
#include "GBAStation/overlay/overlay_ui.h"
#include "common/logging/log.h"
#include "video_core/overlay.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"

namespace SwitchFrontend::VulkanOverlay {
namespace {

constexpr const char* Tag = "[gbastation-dekopon-menu]";
constexpr std::array<float, 10> FastForwardValues{{
    0.1f, 0.5f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f, 4.0f, 5.0f,
}};
constexpr std::array<const char*, 7> LayoutIds{{
    "vertical", "horizontal", "priority_top", "hybrid", "top", "bottom", "custom",
}};

enum class Page {
    Resume,
    Display,
    Reset,
    Exit,
    Count,
};

enum class ResumePage {
    Main,
    SaveSlots,
    LoadSlots,
    Cheats,
};

constexpr int DisplayFastForwardIndex = 1;
constexpr int DisplayInternalResolutionIndex = 3;
constexpr int DisplayLayoutIndex = 4;
constexpr int DisplayOrientationIndex = 5;
constexpr int DisplayIntegerScaleIndex = 6;
constexpr int DisplayGapIndex = 7;
constexpr int DisplaySyncDisplayIndex = 12;
constexpr int DisplaySyncOverlayIndex = 13;

struct PreviousNavigation {
    bool up{};
    bool down{};
    bool left{};
    bool right{};
    bool accept{};
    bool cancel{};
    bool shoulder_left{};
    bool shoulder_right{};
};

std::atomic_bool initialized{};
std::atomic_bool visible{};
std::atomic_bool exit_requested{};
std::atomic_int pending_action{};
Page page = Page::Resume;
ResumePage resume_page = ResumePage::Main;
int selected = 0;
bool tabs_focused = true;
bool previous_combo{};
PreviousNavigation previous_navigation{};

std::atomic<float> fast_forward_multiplier{4.0f};
std::atomic_int display_layout{2};
std::atomic_int display_orientation{};
std::atomic_int display_internal_resolution{1};
std::atomic_bool display_integer_scale{true};
std::atomic_int display_gap{};
std::atomic<float> display_top_scale{1.0f};
std::atomic<float> display_top_offset_x{};
std::atomic<float> display_top_offset_y{};
std::atomic<float> display_bottom_scale{1.0f};
std::atomic<float> display_bottom_offset_x{};
std::atomic<float> display_bottom_offset_y{};
std::atomic<float> display_bottom_opacity{1.0f};
std::atomic_bool display_overlay_enabled{};
std::string display_overlay_path;

int LayoutIndex(const std::string& layout) {
    const auto it = std::find(LayoutIds.begin(), LayoutIds.end(), layout);
    return it == LayoutIds.end() ? 2 : static_cast<int>(std::distance(LayoutIds.begin(), it));
}

void PublishAction(OverlayUI::Action action, bool close_menu) {
    pending_action.store(static_cast<int>(action), std::memory_order_release);
    if (action == OverlayUI::Action::Exit) {
        exit_requested.store(true, std::memory_order_release);
    }
    if (close_menu) {
        visible.store(false, std::memory_order_release);
        VideoCore::SetOverlayMenuState({});
    }
}

std::string FastForwardValue() {
    char value[16]{};
    std::snprintf(value, sizeof(value), "%.2fx", fast_forward_multiplier.load());
    return value;
}

std::string LayoutValue() {
    switch (display_layout.load(std::memory_order_acquire)) {
    case 0: return "竖排";
    case 1: return "横排";
    case 2: return "主屏优先";
    case 3: return "混合";
    case 4: return "上屏";
    case 5: return "下屏";
    case 6: return "自定义";
    default: return "主屏优先";
    }
}

std::string OrientationValue() {
    switch (display_orientation.load(std::memory_order_acquire)) {
    case 1: return "90";
    case 2: return "180";
    case 3: return "270";
    default: return "0";
    }
}

VideoCore::OverlayMenuItem Header(std::string label) {
    return {std::move(label), "", false, VideoCore::OverlayMenuItemKind::Header, false};
}

VideoCore::OverlayMenuItem Row(std::string label, std::string value = {}, bool is_action = true) {
    return {std::move(label), std::move(value), is_action, VideoCore::OverlayMenuItemKind::Row,
            false};
}

VideoCore::OverlayMenuItem Selector(std::string label, std::string value) {
    return {std::move(label), std::move(value), false, VideoCore::OverlayMenuItemKind::Row, true};
}

VideoCore::OverlayMenuItem Disabled(std::string label, std::string value = "预留") {
    return {std::move(label), std::move(value), false, VideoCore::OverlayMenuItemKind::Disabled,
            false};
}

OverlayUI::Action SaveActionForSlot(int slot) {
    return static_cast<OverlayUI::Action>(
        static_cast<int>(OverlayUI::Action::SaveStateSlot1) + std::clamp(slot, 1, 10) - 1);
}

OverlayUI::Action LoadActionForSlot(int slot) {
    return static_cast<OverlayUI::Action>(
        static_cast<int>(OverlayUI::Action::LoadStateSlot1) + std::clamp(slot, 1, 10) - 1);
}

std::vector<VideoCore::OverlayMenuItem> BuildResumeItems() {
    if (resume_page == ResumePage::SaveSlots || resume_page == ResumePage::LoadSlots) {
        std::vector<VideoCore::OverlayMenuItem> items;
        items.reserve(OverlayUI::StateSlotCount + 1);
        const bool saving = resume_page == ResumePage::SaveSlots;
        for (int slot = 1; slot <= OverlayUI::StateSlotCount; ++slot) {
            const bool occupied = OverlayUI::IsSlotOccupied(slot);
            char label[32]{};
            std::snprintf(label, sizeof(label), "状态槽 %d", slot);
            if (saving || occupied) {
                items.push_back(Row(label, occupied ? "已有存档" : "空", true));
            } else {
                items.push_back(Disabled(label, "空"));
            }
        }
        items.push_back(Row("返回上级"));
        return items;
    }

    if (resume_page == ResumePage::Cheats) {
        std::vector<VideoCore::OverlayMenuItem> items;
        const auto cheats = OverlayUI::GetCheats();
        items.reserve(cheats.size() + 2);
        items.push_back(Header("金手指列表"));
        if (cheats.empty()) {
            items.push_back(Disabled("暂无金手指功能", ""));
        } else {
            for (const auto& cheat : cheats) {
                items.push_back(Row(cheat.name.empty() ? "金手指" : cheat.name,
                                    cheat.enabled ? "开启" : "关闭", true));
            }
        }
        items.push_back(Row("返回上级"));
        return items;
    }

    return {
        Row("返回游戏"),
        Row("保存状态", "进入"),
        Row("读取状态", "进入"),
        Row("金手指", "进入"),
    };
}

std::vector<VideoCore::OverlayMenuItem> BuildDisplayItems() {
    return {
        Header("快进"),
        Selector("快进倍率", FastForwardValue()),
        Header("画面"),
        Selector("渲染分辨率", std::to_string(display_internal_resolution.load()) + "x"),
        Selector("屏幕布局", LayoutValue()),
        Selector("画面旋转", OrientationValue() + "度"),
        Selector("画面整数倍", display_integer_scale.load() ? "开启" : "关闭"),
        Selector("屏幕间距", std::to_string(display_gap.load())),
        Header("扩展"),
        Disabled("遮罩设置"),
        Disabled("着色器设置"),
        Header("同步"),
        Row("同步画面设置"),
        Row("同步遮罩设置"),
        Disabled("同步着色器设置"),
    };
}

std::vector<VideoCore::OverlayMenuItem> BuildCurrentItems() {
    switch (page) {
    case Page::Resume:
        return BuildResumeItems();
    case Page::Display:
        return BuildDisplayItems();
    case Page::Reset:
        return {Row("重置游戏")};
    case Page::Exit:
        return {Row("退出游戏")};
    default:
        return {};
    }
}

bool IsSelectable(const std::vector<VideoCore::OverlayMenuItem>& items, int index) {
    return index >= 0 && index < static_cast<int>(items.size()) &&
           items[index].kind != VideoCore::OverlayMenuItemKind::Header &&
           items[index].kind != VideoCore::OverlayMenuItemKind::Disabled;
}

int FirstSelectable(const std::vector<VideoCore::OverlayMenuItem>& items) {
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (IsSelectable(items, i)) {
            return i;
        }
    }
    return 0;
}

void EnsureSelected(const std::vector<VideoCore::OverlayMenuItem>& items) {
    if (!items.empty() && !IsSelectable(items, selected)) {
        selected = FirstSelectable(items);
    }
    if (items.empty()) {
        selected = 0;
    } else {
        selected = std::clamp(selected, 0, static_cast<int>(items.size()) - 1);
    }
}

void StepContentSelection(int direction) {
    const auto items = BuildCurrentItems();
    if (items.empty()) {
        selected = 0;
        return;
    }
    int next = selected;
    for (int attempts = 0; attempts < static_cast<int>(items.size()); ++attempts) {
        next = (next + direction + static_cast<int>(items.size())) % static_cast<int>(items.size());
        if (IsSelectable(items, next)) {
            selected = next;
            return;
        }
    }
    selected = FirstSelectable(items);
}

void Repaint() {
    VideoCore::OverlayMenuState state;
    state.visible = visible.load(std::memory_order_acquire);
    const auto items = BuildCurrentItems();
    EnsureSelected(items);
    state.selected = selected;
    state.selected_tab = static_cast<int>(page);
    state.tabs_focused = tabs_focused;

    if (!state.visible) {
        VideoCore::SetOverlayMenuState(state);
        return;
    }

    state.title = "GBAStation 菜单";
    state.tabs = {
        {"继续游戏", "\xEE\x97\x84"},
        {"画面设置", "\xEE\x8C\xB3"},
        {"重置游戏", "\xEE\x97\x95"},
        {"退出游戏", "\xEE\xA1\xB9"},
    };

    switch (page) {
    case Page::Resume:
        state.hint = tabs_focused ? "A 进入   B 关闭   L/R 切换标签"
                                  : "A 确定   B 返回   ↑/↓ 选择";
        state.items = items;
        break;
    case Page::Display:
        state.hint = tabs_focused ? "A 进入   B 关闭   L/R 切换标签"
                                  : "L/R 调整   A 确定   B 返回";
        state.items = items;
        break;
    case Page::Reset:
        state.hint = tabs_focused ? "A 进入   B 关闭   L/R 切换标签"
                                  : "A 确定   B 返回";
        state.items = items;
        break;
    case Page::Exit:
        state.hint = tabs_focused ? "A 进入   B 关闭   L/R 切换标签"
                                  : "A 确定   B 返回";
        state.items = items;
        break;
    default:
        break;
    }
    VideoCore::SetOverlayMenuState(state);
}

void OpenMenu() {
    page = Page::Resume;
    resume_page = ResumePage::Main;
    selected = 0;
    tabs_focused = true;
    visible.store(true, std::memory_order_release);
    previous_navigation = {};
    Repaint();
}

void CloseMenu() {
    visible.store(false, std::memory_order_release);
    previous_navigation = {};
    tabs_focused = true;
    VideoCore::SetOverlayMenuState({});
}

void ToggleMenu() {
    if (visible.load(std::memory_order_acquire)) {
        PublishAction(OverlayUI::Action::Resume, true);
    } else {
        OpenMenu();
    }
}

void StepFastForward(int direction) {
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
    PublishAction(OverlayUI::Action::FastForwardMultiplierChanged, false);
}

void StepDisplayValue(int direction) {
    switch (selected) {
    case DisplayFastForwardIndex:
        StepFastForward(direction);
        return;
    case DisplayInternalResolutionIndex:
        display_internal_resolution.store(
            (display_internal_resolution.load(std::memory_order_relaxed) + direction - 1 + 4) % 4 +
                1,
            std::memory_order_release);
        PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
        return;
    case DisplayIntegerScaleIndex:
        display_integer_scale.store(!display_integer_scale.load(std::memory_order_relaxed),
                                    std::memory_order_release);
        PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
        return;
    case DisplayLayoutIndex:
        display_layout.store((display_layout.load(std::memory_order_relaxed) + direction +
                              static_cast<int>(LayoutIds.size())) %
                                 static_cast<int>(LayoutIds.size()),
                             std::memory_order_release);
        PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
        return;
    case DisplayOrientationIndex:
        display_orientation.store(
            (display_orientation.load(std::memory_order_relaxed) + direction + 4) % 4,
            std::memory_order_release);
        PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
        return;
    case DisplayGapIndex:
        display_gap.store(std::clamp(display_gap.load(std::memory_order_relaxed) + direction, -256,
                                     256),
                          std::memory_order_release);
        PublishAction(OverlayUI::Action::DisplaySettingsChanged, false);
        return;
    default:
        return;
    }
}

void ActivateCurrent() {
    if (tabs_focused) {
        tabs_focused = false;
        selected = FirstSelectable(BuildCurrentItems());
        Repaint();
        return;
    }

    if (page == Page::Resume) {
        if (resume_page == ResumePage::Main) {
            if (selected == 0) {
                PublishAction(OverlayUI::Action::Resume, true);
            } else if (selected == 1) {
                resume_page = ResumePage::SaveSlots;
                selected = 0;
                Repaint();
            } else if (selected == 2) {
                resume_page = ResumePage::LoadSlots;
                selected = 0;
                Repaint();
            } else if (selected == 3) {
                resume_page = ResumePage::Cheats;
                selected = FirstSelectable(BuildCurrentItems());
                Repaint();
            }
            return;
        }

        if (resume_page == ResumePage::SaveSlots) {
            if (selected >= 0 && selected < OverlayUI::StateSlotCount) {
                PublishAction(SaveActionForSlot(selected + 1), true);
            } else {
                resume_page = ResumePage::Main;
                selected = 1;
                Repaint();
            }
            return;
        }

        if (resume_page == ResumePage::LoadSlots) {
            if (selected >= 0 && selected < OverlayUI::StateSlotCount) {
                PublishAction(LoadActionForSlot(selected + 1), true);
            } else {
                resume_page = ResumePage::Main;
                selected = 2;
                Repaint();
            }
            return;
        }

        const auto cheats = OverlayUI::GetCheats();
        if (selected > 0 && selected <= static_cast<int>(cheats.size())) {
            const bool toggled = OverlayUI::ToggleCheat(selected - 1);
            OverlayUI::ShowToast(toggled ? "金手指设置已保存" : "金手指设置失败");
        } else {
            resume_page = ResumePage::Main;
            selected = 3;
        }
        Repaint();
        return;
    }

    if (page == Page::Display) {
        if (selected == DisplaySyncDisplayIndex) {
            PublishAction(OverlayUI::Action::SyncDisplaySettings, false);
        } else if (selected == DisplaySyncOverlayIndex) {
            PublishAction(OverlayUI::Action::SyncOverlaySettings, false);
        }
        return;
    }

    if (page == Page::Reset) {
        PublishAction(OverlayUI::Action::Reset, true);
    } else if (page == Page::Exit) {
        PublishAction(OverlayUI::Action::Exit, true);
    }
}

void GoBack() {
    if (!tabs_focused && page == Page::Resume && resume_page != ResumePage::Main) {
        resume_page = ResumePage::Main;
        selected = 0;
        Repaint();
    } else if (!tabs_focused) {
        tabs_focused = true;
        selected = 0;
        Repaint();
    } else {
        PublishAction(OverlayUI::Action::Resume, true);
    }
}

bool Rising(bool current, bool previous) {
    return current && !previous;
}

} // namespace

bool Init([[maybe_unused]] Vulkan::RendererVulkan& renderer) {
    initialized.store(true, std::memory_order_release);
    visible.store(false, std::memory_order_release);
    exit_requested.store(false, std::memory_order_release);
    pending_action.store(0, std::memory_order_release);
    previous_combo = false;
    previous_navigation = {};
    VideoCore::SetOverlayMenuState({});
    LOG_INFO(Render_Vulkan, "{} initialized with video_core overlay", Tag);
    return true;
}

void Update(PadState* pad) {
    if (!initialized.load(std::memory_order_acquire) || !pad) {
        return;
    }

    const u64 held = padGetButtons(pad);
    const bool combo = InputMapping::MenuHotkeyPressed(*pad);
    if (combo && !previous_combo) {
        ToggleMenu();
        previous_combo = combo;
        return;
    }
    previous_combo = combo;

    if (!visible.load(std::memory_order_acquire)) {
        previous_navigation = {};
        return;
    }

    const PreviousNavigation nav{
        .up = (held & (HidNpadButton_AnyUp | HidNpadButton_StickLUp)) != 0,
        .down = (held & (HidNpadButton_AnyDown | HidNpadButton_StickLDown)) != 0,
        .left = (held & (HidNpadButton_AnyLeft | HidNpadButton_StickLLeft)) != 0,
        .right = (held & (HidNpadButton_AnyRight | HidNpadButton_StickLRight)) != 0,
        .accept = (held & HidNpadButton_A) != 0,
        .cancel = (held & HidNpadButton_B) != 0,
        .shoulder_left = (held & HidNpadButton_L) != 0,
        .shoulder_right = (held & HidNpadButton_R) != 0,
    };

    bool changed = false;
    const int tab_count = static_cast<int>(Page::Count);

    const auto step_tab = [&](int direction) {
        const int next = (static_cast<int>(page) + direction + tab_count) % tab_count;
        page = static_cast<Page>(next);
        resume_page = ResumePage::Main;
        selected = 0;
        tabs_focused = true;
    };

    if (tabs_focused && Rising(nav.up, previous_navigation.up)) {
        step_tab(-1);
        changed = true;
    }
    if (tabs_focused && Rising(nav.down, previous_navigation.down)) {
        step_tab(1);
        changed = true;
    }
    if (!tabs_focused && Rising(nav.up, previous_navigation.up)) {
        StepContentSelection(-1);
        changed = true;
    }
    if (!tabs_focused && Rising(nav.down, previous_navigation.down)) {
        StepContentSelection(1);
        changed = true;
    }
    if (tabs_focused && Rising(nav.right, previous_navigation.right)) {
        tabs_focused = false;
        selected = FirstSelectable(BuildCurrentItems());
        changed = true;
    }
    if (!tabs_focused && Rising(nav.left, previous_navigation.left)) {
        tabs_focused = true;
        selected = 0;
        changed = true;
    }
    if (tabs_focused && Rising(nav.shoulder_left, previous_navigation.shoulder_left)) {
        step_tab(-1);
        changed = true;
    }
    if (tabs_focused && Rising(nav.shoulder_right, previous_navigation.shoulder_right)) {
        step_tab(1);
        changed = true;
    }
    if (!tabs_focused && page == Page::Display &&
        (Rising(nav.shoulder_left, previous_navigation.shoulder_left) ||
         Rising(nav.shoulder_right, previous_navigation.shoulder_right))) {
        const auto items = BuildCurrentItems();
        if (IsSelectable(items, selected) && items[selected].uses_lr) {
            StepDisplayValue(nav.shoulder_right ? 1 : -1);
            changed = true;
        }
    }
    if (Rising(nav.cancel, previous_navigation.cancel)) {
        GoBack();
        previous_navigation = nav;
        return;
    }
    if (Rising(nav.accept, previous_navigation.accept)) {
        ActivateCurrent();
        previous_navigation = nav;
        return;
    }

    previous_navigation = nav;
    if (changed) {
        Repaint();
    }
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
    fast_forward_multiplier.store(std::clamp(settings.fast_forward_multiplier, 0.1f, 5.0f),
                                  std::memory_order_release);
    display_internal_resolution.store(std::clamp(settings.internal_resolution, 1, 4),
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
    display_bottom_offset_x.store(std::clamp(settings.bottom_offset_x, -1024.0f, 1024.0f),
                                  std::memory_order_release);
    display_bottom_offset_y.store(std::clamp(settings.bottom_offset_y, -1024.0f, 1024.0f),
                                  std::memory_order_release);
    display_bottom_opacity.store(std::clamp(settings.bottom_opacity, 0.0f, 1.0f),
                                 std::memory_order_release);
    display_overlay_enabled.store(settings.overlay_enabled, std::memory_order_release);
    display_overlay_path = settings.overlay_path;
    if (visible.load(std::memory_order_acquire)) {
        Repaint();
    }
}

GBAStationDisplaySettings GetDisplaySettings() {
    GBAStationDisplaySettings settings;
    settings.fast_forward_multiplier = fast_forward_multiplier.load(std::memory_order_acquire);
    settings.internal_resolution = display_internal_resolution.load(std::memory_order_acquire);
    settings.screen_layout = LayoutIds[std::clamp(display_layout.load(std::memory_order_acquire), 0,
                                                  static_cast<int>(LayoutIds.size()) - 1)];
    constexpr std::array<int, 4> Orientations{{0, 90, 180, 270}};
    settings.screen_orientation =
        Orientations[std::clamp(display_orientation.load(std::memory_order_acquire), 0, 3)];
    settings.integer_scale = display_integer_scale.load(std::memory_order_acquire);
    settings.screen_gap = display_gap.load(std::memory_order_acquire);
    settings.top_scale = display_top_scale.load(std::memory_order_acquire);
    settings.top_offset_x = display_top_offset_x.load(std::memory_order_acquire);
    settings.top_offset_y = display_top_offset_y.load(std::memory_order_acquire);
    settings.bottom_scale = display_bottom_scale.load(std::memory_order_acquire);
    settings.bottom_offset_x = display_bottom_offset_x.load(std::memory_order_acquire);
    settings.bottom_offset_y = display_bottom_offset_y.load(std::memory_order_acquire);
    settings.bottom_opacity = display_bottom_opacity.load(std::memory_order_acquire);
    settings.overlay_enabled = display_overlay_enabled.load(std::memory_order_acquire);
    settings.overlay_path = display_overlay_path;
    return settings;
}

void SetFastForwardActive([[maybe_unused]] bool active) {}

void SetFpsOverlay(bool visible, [[maybe_unused]] float fps) {
    VideoCore::SetFpsOverlayState(visible, fps);
}

void PrepareForShutdown() {
    visible.store(false, std::memory_order_release);
    exit_requested.store(true, std::memory_order_release);
    pending_action.store(0, std::memory_order_release);
    previous_navigation = {};
    VideoCore::SetOverlayMenuState({});
}

void Shutdown() {
    PrepareForShutdown();
    initialized.store(false, std::memory_order_release);
    exit_requested.store(false, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "{} shutdown complete", Tag);
}

} // namespace SwitchFrontend::VulkanOverlay
