// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tico/overlay/overlay_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string_view>

#include <imgui.h>

#include "tico/switch_libnx.h"

namespace SwitchFrontend::OverlayUI {
namespace {

enum class MenuItem {
    Resume,
    SaveState,
    LoadState,
    Cheats,
    Display,
    Reset,
    Exit,
    Count,
};

struct MenuLabel {
    const char* chinese;
    const char* english;
};

constexpr std::array<MenuLabel, static_cast<int>(MenuItem::Count)> MenuLabels{{
    {"返回游戏", "Resume"},
    {"保存状态", "Save State"},
    {"读取状态", "Load State"},
    {"金手指设置", "Cheats"},
    {"画面设置", "Display"},
    {"重置游戏", "Reset Game"},
    {"退出游戏", "Exit Game"},
}};

constexpr float BaseWidth = 1280.0f;
constexpr float BaseHeight = 720.0f;
constexpr float AnimationDuration = 0.22f;
constexpr float ToastHoldDuration = 2.0f;
constexpr float ToastFadeDuration = 0.25f;

bool visible{};
bool content_focus{};
int selected_item{};
int selected_slot{};
float animation_time{};
NavInput pending_nav{};
std::string game_title{"Nintendo 3DS"};
SlotOccupiedFn slot_occupied;

std::mutex toast_mutex;
std::string toast_message;
float toast_time{};
ToastCorner toast_corner{ToastCorner::BottomRight};

float EaseOutCubic(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    const float inverse = 1.0f - value;
    return 1.0f - inverse * inverse * inverse;
}

ImU32 Color(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

void DrawText(ImDrawList* draw_list, ImVec2 position, ImU32 color, std::string_view text,
              float size = 0.0f) {
    ImFont* font = ImGui::GetFont();
    const float font_size = size > 0.0f ? size : ImGui::GetFontSize();
    draw_list->AddText(font, font_size, {position.x + 1.5f, position.y + 1.5f},
                       Color(0, 0, 0, 110), text.data(), text.data() + text.size());
    draw_list->AddText(font, font_size, position, color, text.data(), text.data() + text.size());
}

void DrawCenteredText(ImDrawList* draw_list, ImVec2 center, ImU32 color, std::string_view text,
                      float size) {
    ImFont* font = ImGui::GetFont();
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(),
                                              text.data() + text.size());
    DrawText(draw_list, {center.x - extent.x * 0.5f, center.y - extent.y * 0.5f}, color, text,
             size);
}

std::string CleanTitle(std::string title) {
    for (char& c : title) {
        if (c == '\n' || c == '\r' || c == '\t') {
            c = ' ';
        }
    }
    const auto first = title.find_first_not_of(' ');
    if (first == std::string::npos) {
        return "Nintendo 3DS";
    }
    const auto last = title.find_last_not_of(' ');
    title = title.substr(first, last - first + 1);
    if (title.size() > 72) {
        title.resize(69);
        title += "...";
    }
    return title;
}

bool ItemHasContent(MenuItem item) {
    return item == MenuItem::SaveState || item == MenuItem::LoadState ||
           item == MenuItem::Cheats || item == MenuItem::Display;
}

Action MakeSaveAction(int slot) {
    return static_cast<Action>(static_cast<int>(Action::SaveStateSlot1) + slot - 1);
}

Action MakeLoadAction(int slot) {
    return static_cast<Action>(static_cast<int>(Action::LoadStateSlot1) + slot - 1);
}

bool SlotOccupied(int slot) {
    return slot_occupied && slot_occupied(slot);
}

void DrawButtonHint(ImDrawList* draw_list, ImVec2& cursor, const char* button,
                    const char* description, float scale, float alpha) {
    const float radius = 13.0f * scale;
    const ImU32 circle = Color(225, 235, 246, static_cast<int>(235.0f * alpha));
    const ImU32 symbol = Color(25, 32, 42, static_cast<int>(255.0f * alpha));
    draw_list->AddCircleFilled({cursor.x + radius, cursor.y + radius}, radius, circle, 24);
    DrawCenteredText(draw_list, {cursor.x + radius, cursor.y + radius}, symbol, button,
                     15.0f * scale);
    cursor.x += radius * 2.0f + 9.0f * scale;
    DrawText(draw_list, {cursor.x, cursor.y + 2.0f * scale},
             Color(218, 226, 238, static_cast<int>(230.0f * alpha)), description,
             20.0f * scale);
    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(20.0f * scale, FLT_MAX, 0.0f,
                                                        description);
    cursor.x += size.x + 28.0f * scale;
}

void DrawStatus(ImDrawList* draw_list, float scale, float alpha) {
    char time_text[16] = "--:--";
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    if (localtime_r(&now, &local_time)) {
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
    }

    u32 battery = 0;
    const bool has_battery = psmGetBatteryChargePercentage(&battery) == 0;
    char status[48]{};
    if (has_battery) {
        std::snprintf(status, sizeof(status), "%s   %u%%", time_text, battery);
    } else {
        std::snprintf(status, sizeof(status), "%s", time_text);
    }
    const float font_size = 20.0f * scale;
    const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, status);
    DrawText(draw_list, {BaseWidth * scale - size.x - 48.0f * scale, 42.0f * scale},
             Color(210, 221, 235, static_cast<int>(220.0f * alpha)), status, font_size);
}

void DrawLeftMenu(ImDrawList* draw_list, float scale, float alpha, float slide) {
    constexpr float left_x = 42.0f;
    constexpr float start_y = 130.0f;
    constexpr float width = 294.0f;
    constexpr float row_height = 68.0f;
    constexpr float row_step = 74.0f;

    for (int index = 0; index < static_cast<int>(MenuItem::Count); ++index) {
        const float y = (start_y + row_step * index + slide) * scale;
        const bool selected = index == selected_item;
        const ImVec2 p0{left_x * scale, y};
        const ImVec2 p1{(left_x + width) * scale, y + row_height * scale};

        if (index == static_cast<int>(MenuItem::Reset)) {
            const float separator_y = y - 10.0f * scale;
            draw_list->AddLine({p0.x + 12.0f * scale, separator_y},
                               {p1.x - 12.0f * scale, separator_y},
                               Color(255, 255, 255, static_cast<int>(35.0f * alpha)),
                               1.0f * scale);
        }

        if (selected) {
            draw_list->AddRectFilled(p0, p1,
                                     Color(34, 104, 176, static_cast<int>(72.0f * alpha)),
                                     8.0f * scale);
            draw_list->AddRect(p0, p1,
                               Color(85, 178, 255, static_cast<int>(235.0f * alpha)),
                               8.0f * scale, 0, 2.0f * scale);
            draw_list->AddRectFilled(p0, {p0.x + 5.0f * scale, p1.y},
                                     Color(85, 190, 255, static_cast<int>(255.0f * alpha)),
                                     8.0f * scale);
        }

        const MenuLabel& label = MenuLabels[index];
        const ImU32 primary = selected
                                  ? Color(255, 255, 255, static_cast<int>(255.0f * alpha))
                                  : Color(218, 225, 236, static_cast<int>(210.0f * alpha));
        DrawText(draw_list, {p0.x + 22.0f * scale, p0.y + 10.0f * scale}, primary,
                 label.chinese, 24.0f * scale);
        DrawText(draw_list, {p0.x + 22.0f * scale, p0.y + 39.0f * scale},
                 Color(150, 169, 192, static_cast<int>((selected ? 225.0f : 170.0f) * alpha)),
                 label.english, 15.0f * scale);
    }
}

void DrawSlotPage(ImDrawList* draw_list, float scale, float alpha, bool loading) {
    const float origin_x = 390.0f * scale;
    const float origin_y = 154.0f * scale;
    const float card_width = 390.0f * scale;
    const float card_height = 72.0f * scale;
    const float column_gap = 24.0f * scale;
    const float row_gap = 14.0f * scale;

    DrawText(draw_list, {origin_x, 112.0f * scale}, Color(242, 247, 255, static_cast<int>(255.0f * alpha)),
             loading ? "读取状态" : "保存状态", 30.0f * scale);

    for (int slot_index = 0; slot_index < StateSlotCount; ++slot_index) {
        const int column = slot_index % 2;
        const int row = slot_index / 2;
        const float x = origin_x + column * (card_width + column_gap);
        const float y = origin_y + row * (card_height + row_gap);
        const bool selected = content_focus && selected_slot == slot_index;
        const bool occupied = SlotOccupied(slot_index + 1);
        const ImVec2 p0{x, y};
        const ImVec2 p1{x + card_width, y + card_height};

        draw_list->AddRectFilled(
            p0, p1,
            selected ? Color(35, 99, 164, static_cast<int>(105.0f * alpha))
                     : Color(31, 35, 43, static_cast<int>(215.0f * alpha)),
            7.0f * scale);
        draw_list->AddRect(
            p0, p1,
            selected ? Color(87, 187, 255, static_cast<int>(245.0f * alpha))
                     : Color(255, 255, 255, static_cast<int>(25.0f * alpha)),
            7.0f * scale, 0, selected ? 2.0f * scale : 1.0f * scale);

        char slot_label[32]{};
        std::snprintf(slot_label, sizeof(slot_label), "存档位 %d", slot_index + 1);
        DrawText(draw_list, {x + 20.0f * scale, y + 13.0f * scale},
                 Color(242, 246, 252, static_cast<int>(245.0f * alpha)), slot_label,
                 23.0f * scale);
        DrawText(draw_list, {x + 20.0f * scale, y + 43.0f * scale},
                 occupied ? Color(103, 210, 143, static_cast<int>(230.0f * alpha))
                          : Color(145, 157, 174, static_cast<int>(190.0f * alpha)),
                 occupied ? "已有状态" : "空", 16.0f * scale);

        if (loading && !occupied) {
            DrawText(draw_list, {x + card_width - 82.0f * scale, y + 27.0f * scale},
                     Color(126, 136, 150, static_cast<int>(160.0f * alpha)), "不可用",
                     15.0f * scale);
        }
    }
}

void DrawPlaceholderPage(ImDrawList* draw_list, float scale, float alpha, MenuItem item) {
    const bool cheats = item == MenuItem::Cheats;
    DrawText(draw_list, {390.0f * scale, 112.0f * scale},
             Color(242, 247, 255, static_cast<int>(255.0f * alpha)),
             cheats ? "金手指设置" : "画面设置", 30.0f * scale);

    const ImVec2 p0{390.0f * scale, 166.0f * scale};
    const ImVec2 p1{1200.0f * scale, 508.0f * scale};
    draw_list->AddRectFilled(p0, p1, Color(29, 33, 41, static_cast<int>(215.0f * alpha)),
                             10.0f * scale);
    draw_list->AddRect(p0, p1, Color(255, 255, 255, static_cast<int>(24.0f * alpha)),
                       10.0f * scale);
    DrawCenteredText(draw_list, {(p0.x + p1.x) * 0.5f, 292.0f * scale},
                     Color(205, 215, 229, static_cast<int>(235.0f * alpha)),
                     "此功能将在后续版本中提供", 26.0f * scale);
    DrawCenteredText(draw_list, {(p0.x + p1.x) * 0.5f, 338.0f * scale},
                     Color(129, 148, 173, static_cast<int>(205.0f * alpha)),
                     cheats ? "Cheat options are not available yet"
                            : "Personal display options are not available yet",
                     17.0f * scale);
}

void DrawActionPage(ImDrawList* draw_list, float scale, float alpha, MenuItem item) {
    const bool reset = item == MenuItem::Reset;
    DrawText(draw_list, {390.0f * scale, 112.0f * scale},
             Color(242, 247, 255, static_cast<int>(255.0f * alpha)),
             reset ? "重置游戏" : "退出游戏", 30.0f * scale);

    const ImVec2 p0{390.0f * scale, 166.0f * scale};
    const ImVec2 p1{1200.0f * scale, 420.0f * scale};
    draw_list->AddRectFilled(p0, p1,
                             reset ? Color(45, 38, 28, static_cast<int>(225.0f * alpha))
                                   : Color(48, 29, 33, static_cast<int>(225.0f * alpha)),
                             10.0f * scale);
    draw_list->AddRect(p0, p1,
                       reset ? Color(240, 183, 82, static_cast<int>(100.0f * alpha))
                             : Color(242, 102, 119, static_cast<int>(115.0f * alpha)),
                       10.0f * scale, 0, 1.5f * scale);
    DrawCenteredText(draw_list, {(p0.x + p1.x) * 0.5f, 252.0f * scale},
                     Color(242, 245, 250, static_cast<int>(245.0f * alpha)),
                     reset ? "按 A 重新启动当前游戏" : "按 A 关闭游戏并返回启动器",
                     26.0f * scale);
    DrawCenteredText(draw_list, {(p0.x + p1.x) * 0.5f, 314.0f * scale},
                     Color(170, 181, 198, static_cast<int>(220.0f * alpha)),
                     reset ? "未保存的游戏进度可能丢失" : "退出前会先安全关闭模拟器资源",
                     18.0f * scale);
}

void DrawRightPage(ImDrawList* draw_list, float scale, float alpha) {
    const MenuItem item = static_cast<MenuItem>(selected_item);
    switch (item) {
    case MenuItem::SaveState:
        DrawSlotPage(draw_list, scale, alpha, false);
        break;
    case MenuItem::LoadState:
        DrawSlotPage(draw_list, scale, alpha, true);
        break;
    case MenuItem::Cheats:
    case MenuItem::Display:
        DrawPlaceholderPage(draw_list, scale, alpha, item);
        break;
    case MenuItem::Reset:
    case MenuItem::Exit:
        DrawActionPage(draw_list, scale, alpha, item);
        break;
    case MenuItem::Resume:
    default:
        DrawText(draw_list, {390.0f * scale, 112.0f * scale},
                 Color(242, 247, 255, static_cast<int>(255.0f * alpha)), "返回游戏",
                 30.0f * scale);
        DrawCenteredText(draw_list, {795.0f * scale, 290.0f * scale},
                         Color(213, 223, 236, static_cast<int>(235.0f * alpha)),
                         "按 A 或 B 继续游戏", 28.0f * scale);
        DrawCenteredText(draw_list, {795.0f * scale, 340.0f * scale},
                         Color(130, 151, 178, static_cast<int>(205.0f * alpha)),
                         "Press A or B to resume", 18.0f * scale);
        break;
    }
}

void DrawFooter(ImDrawList* draw_list, float scale, float alpha) {
    draw_list->AddLine({42.0f * scale, 638.0f * scale}, {1238.0f * scale, 638.0f * scale},
                       Color(255, 255, 255, static_cast<int>(28.0f * alpha)), 1.0f * scale);
    ImVec2 cursor{62.0f * scale, 659.0f * scale};
    DrawButtonHint(draw_list, cursor, "A", content_focus ? "确定" : "选择", scale, alpha);
    DrawButtonHint(draw_list, cursor, "B", content_focus ? "返回" : "继续游戏", scale, alpha);
    DrawButtonHint(draw_list, cursor, "ZL+ZR", "关闭菜单", scale, alpha);
}

void DrawToast(ImDrawList* draw_list, float scale, float delta_time) {
    std::lock_guard lock{toast_mutex};
    if (toast_message.empty() || toast_time <= 0.0f) {
        return;
    }
    toast_time = std::max(0.0f, toast_time - delta_time);
    float alpha = 1.0f;
    if (toast_time < ToastFadeDuration) {
        alpha = toast_time / ToastFadeDuration;
    }

    const float font_size = 20.0f * scale;
    const ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f,
                                                             toast_message.c_str());
    const float width = text_size.x + 44.0f * scale;
    const float height = 54.0f * scale;
    float x = 34.0f * scale;
    float y = 34.0f * scale;
    if (toast_corner == ToastCorner::TopRight || toast_corner == ToastCorner::BottomRight) {
        x = BaseWidth * scale - width - 34.0f * scale;
    }
    if (toast_corner == ToastCorner::BottomLeft || toast_corner == ToastCorner::BottomRight) {
        y = BaseHeight * scale - height - 34.0f * scale;
    }
    draw_list->AddRectFilled({x, y}, {x + width, y + height},
                             Color(24, 29, 37, static_cast<int>(235.0f * alpha)),
                             8.0f * scale);
    draw_list->AddRect({x, y}, {x + width, y + height},
                       Color(88, 181, 245, static_cast<int>(150.0f * alpha)), 8.0f * scale);
    DrawText(draw_list, {x + 22.0f * scale, y + 15.0f * scale},
             Color(240, 245, 252, static_cast<int>(255.0f * alpha)), toast_message,
             font_size);
}

Action HandleNavigation() {
    const NavInput nav = pending_nav;
    pending_nav = {};
    const MenuItem item = static_cast<MenuItem>(selected_item);

    if (content_focus) {
        if (item == MenuItem::SaveState || item == MenuItem::LoadState) {
            if (nav.up) {
                selected_slot = (selected_slot - 2 + StateSlotCount) % StateSlotCount;
            }
            if (nav.down) {
                selected_slot = (selected_slot + 2) % StateSlotCount;
            }
            if (nav.left) {
                if ((selected_slot % 2) == 0) {
                    content_focus = false;
                } else {
                    --selected_slot;
                }
            }
            if (nav.right && (selected_slot % 2) == 0) {
                ++selected_slot;
            }
            if (nav.accept) {
                const int slot = selected_slot + 1;
                if (item == MenuItem::LoadState && !SlotOccupied(slot)) {
                    ShowToast("该存档位为空");
                    return Action::None;
                }
                return item == MenuItem::SaveState ? MakeSaveAction(slot) : MakeLoadAction(slot);
            }
        } else if (nav.accept) {
            ShowToast("此功能将在后续版本中提供");
        }

        if (nav.cancel) {
            content_focus = false;
        }
        return Action::None;
    }

    if (nav.up) {
        selected_item = (selected_item - 1 + static_cast<int>(MenuItem::Count)) %
                        static_cast<int>(MenuItem::Count);
    }
    if (nav.down) {
        selected_item = (selected_item + 1) % static_cast<int>(MenuItem::Count);
    }
    if (nav.right && ItemHasContent(static_cast<MenuItem>(selected_item))) {
        content_focus = true;
        selected_slot = 0;
    }
    if (nav.cancel) {
        return Action::Resume;
    }
    if (!nav.accept) {
        return Action::None;
    }

    switch (static_cast<MenuItem>(selected_item)) {
    case MenuItem::Resume:
        return Action::Resume;
    case MenuItem::SaveState:
    case MenuItem::LoadState:
    case MenuItem::Cheats:
    case MenuItem::Display:
        content_focus = true;
        selected_slot = 0;
        return Action::None;
    case MenuItem::Reset:
        return Action::Reset;
    case MenuItem::Exit:
        return Action::Exit;
    default:
        return Action::None;
    }
}

bool IsActionInRange(Action action, Action first, Action last) {
    return static_cast<int>(action) >= static_cast<int>(first) &&
           static_cast<int>(action) <= static_cast<int>(last);
}

} // namespace

void SetVisible(bool new_visible) {
    if (visible == new_visible) {
        return;
    }
    visible = new_visible;
    animation_time = 0.0f;
    if (visible) {
        selected_item = 0;
        selected_slot = 0;
        content_focus = false;
        pending_nav = {};
    }
}

Action Render(int display_w, int display_h) {
    const float delta_time = std::max(ImGui::GetIO().DeltaTime, 1.0f / 240.0f);
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const float scale = std::min(static_cast<float>(display_w) / BaseWidth,
                                 static_cast<float>(display_h) / BaseHeight);

    if (!visible) {
        DrawToast(draw_list, scale, delta_time);
        return Action::None;
    }

    animation_time = std::min(animation_time + delta_time, AnimationDuration);
    const float progress = EaseOutCubic(animation_time / AnimationDuration);
    const float slide = (1.0f - progress) * BaseHeight;
    const int alpha = static_cast<int>(255.0f * progress);

    draw_list->AddRectFilled({0.0f, 0.0f}, {static_cast<float>(display_w), static_cast<float>(display_h)},
                             Color(4, 7, 12, static_cast<int>(190.0f * progress)));
    const ImVec2 panel_min{22.0f * scale, (22.0f + slide) * scale};
    const ImVec2 panel_max{(BaseWidth - 22.0f) * scale, (BaseHeight - 22.0f + slide) * scale};
    draw_list->AddRectFilled(panel_min, panel_max, Color(18, 22, 29, alpha), 14.0f * scale);
    draw_list->AddRect(panel_min, panel_max, Color(255, 255, 255, static_cast<int>(24.0f * progress)),
                       14.0f * scale);

    DrawText(draw_list, {44.0f * scale, (42.0f + slide) * scale},
             Color(245, 248, 253, alpha), "GBAStation 3DS", 31.0f * scale);
    DrawText(draw_list, {44.0f * scale, (78.0f + slide) * scale},
             Color(135, 162, 195, static_cast<int>(220.0f * progress)), game_title,
             18.0f * scale);
    DrawStatus(draw_list, scale, progress);

    DrawLeftMenu(draw_list, scale, progress, slide);
    draw_list->AddLine({360.0f * scale, (112.0f + slide) * scale},
                       {360.0f * scale, (618.0f + slide) * scale},
                       Color(255, 255, 255, static_cast<int>(28.0f * progress)), 1.0f * scale);

    if (slide < 1.0f) {
        DrawRightPage(draw_list, scale, progress);
        DrawFooter(draw_list, scale, progress);
    }
    DrawToast(draw_list, scale, delta_time);

    return HandleNavigation();
}

void SetGameTitle(std::string title) {
    game_title = CleanTitle(std::move(title));
}

void ShowToast(std::string message, ToastCorner corner) {
    std::lock_guard lock{toast_mutex};
    toast_message = std::move(message);
    toast_corner = corner;
    toast_time = toast_message.empty() ? 0.0f : ToastHoldDuration + ToastFadeDuration;
}

bool HasTransientContent() {
    std::lock_guard lock{toast_mutex};
    return !toast_message.empty() && toast_time > 0.0f;
}

void SetSlotOccupiedCallback(SlotOccupiedFn callback) {
    slot_occupied = std::move(callback);
}

void FeedNav(const NavInput& nav) {
    pending_nav = nav;
}

bool IsSaveStateAction(Action action) {
    return IsActionInRange(action, Action::SaveStateSlot1, Action::SaveStateSlot10);
}

bool IsLoadStateAction(Action action) {
    return IsActionInRange(action, Action::LoadStateSlot1, Action::LoadStateSlot10);
}

int GetStateSlotForAction(Action action) {
    if (IsSaveStateAction(action)) {
        return static_cast<int>(action) - static_cast<int>(Action::SaveStateSlot1) + 1;
    }
    if (IsLoadStateAction(action)) {
        return static_cast<int>(action) - static_cast<int>(Action::LoadStateSlot1) + 1;
    }
    return 0;
}

} // namespace SwitchFrontend::OverlayUI
