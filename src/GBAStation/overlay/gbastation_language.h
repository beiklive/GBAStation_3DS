/// @file gbastation_language.h
/// @brief Lightweight zh/en translation helper for the 3DS overlay menus.
///
/// Language comes from the launcher's UI.language (config.cfg) via
/// GBAStationConfig::GetConfiguredSystemLanguage(). Returns the input
/// Chinese string verbatim when the language is not English.

#pragma once

#include <string>
#include <unordered_map>

#include "GBAStation/overlay/gbastation_config.h"

namespace SwitchFrontend {

class GBAStationLanguage {
public:
    static GBAStationLanguage& Instance() {
        static GBAStationLanguage instance;
        return instance;
    }

    /// Returns true when the effective UI language is English.
    bool IsEnglish() {
        EnsureLoaded();
        return is_english_;
    }

    /// Translate a Chinese UI string; returns the input unchanged for zh.
    /// The returned pointer stays valid until the next Tr() call.
    const char* Tr(const std::string& zh) {
        EnsureLoaded();
        if (!is_english_) {
            return zh.c_str();
        }
        const auto it = table_.find(zh);
        if (it != table_.end()) {
            return it->second.c_str();
        }
        return zh.c_str();
    }

    const char* Tr(const char* zh) {
        return Tr(zh ? std::string(zh) : std::string());
    }

private:
    GBAStationLanguage() = default;

    void EnsureLoaded() {
        if (loaded_) {
            return;
        }
        loaded_ = true;
        const std::string language = GBAStationConfig::GetConfiguredSystemLanguage();
        is_english_ = (language == "en-US" || language == "en" || language == "English");
        if (!is_english_) {
            return;
        }
        table_ = {
            {"返回游戏", "Resume Game"},
            {"保存状态", "Save State"},
            {"读取状态", "Load State"},
            {"金手指", "Cheats"},
            {"画面设置", "Display Settings"},
            {"运行设置", "Runtime Settings"},
            {"重置游戏", "Reset Game"},
            {"退出游戏", "Exit Game"},
            {"继续当前游戏。", "Resume the current game."},
            {"创建即时存档。", "Create an instant save."},
            {"读取即时存档。", "Load an instant save."},
            {"管理游戏金手指。", "Manage game cheats."},
            {"调整画面比例和缩放方式。", "Adjust aspect ratio and scaling."},
            {"调整可即时生效的核心选项。", "Adjust core options applied immediately."},
            {"重新启动当前游戏。", "Restart the current game."},
            {"关闭模拟器并返回 GBAStation。", "Close the emulator and return to GBAStation."},
            {"下屏优先", "Bottom Priority"},
            {"仅上屏", "Top Only"},
            {"仅下屏", "Bottom Only"},
            {"自定义", "Custom"},
            {"上屏优先", "Top Priority"},
            {"自定义画面布局", "Custom Layout"},
            {"上屏布局", "Top Screen Layout"},
            {"下屏布局", "Bottom Screen Layout"},
            {"X 偏移", "X Offset"},
            {"Y 偏移", "Y Offset"},
            {"透明度", "Opacity"},
            {"档位已有状态空存档槽继续按确定返回列表不可用", "Slot In Use | Empty | Continue | Confirm | Back to List | Unavailable"},
            {"自定义画面布局调整当前项上屏布局下屏布局缩放偏移", "Custom layout | Adjust item | Top layout | Bottom layout | Scale | Offset"},
            {"同步遮罩同步画面设置执行已同步到个游戏失败", "Sync masks | Sync display settings | Executed | Synced to games | Failed"},
            {"CPU时钟频率视频节流", "CPU clock | Video throttle"},
            {"安全关闭模拟器未保存的游戏进度可能丢失", "Shut down safely? Unsaved progress may be lost"},
            {"保存", "Save"},
            {"读取", "Load"},
            {"返回", "Back"},
            {"返回列表", "Back to List"},
            {"确认", "Confirm"},
            {"确定", "OK"},
            {"取消", "Cancel"},
            {"关闭", "Off"},
            {"开启", "On"},
            {"默认", "Default"},
            {"自动", "Auto"},
            {"选择", "Select"},
            {"设置", "Settings"},
            {"显示", "Display"},
            {"大小", "Size"},
            {"拉伸", "Stretch"},
            {"原始", "Original"},
            {"重置", "Reset"},
            {"更改", "Change"},
            {"已保存", "Saved"},
            {"空存档槽", "Empty Slot"},
            {"存档槽 ", "Slot "},
            {"不可用", "Unavailable"},
            {"继续", "Continue"},
            {"执行", "Execute"},
            {"已同步到", "Synced to"},
            {"个游戏", " games"},
            {"失败", "Failed"},
        };
    }

    bool loaded_ = false;
    bool is_english_ = false;
    std::unordered_map<std::string, std::string> table_;
};

} // namespace SwitchFrontend

/// Global shorthand: translate a Chinese UI string for the 3DS menus.
#define GBA_L(zh) SwitchFrontend::GBAStationLanguage::Instance().Tr(zh)
