// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/input_mapping.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SwitchFrontend::InputMapping {
namespace {

std::unordered_map<std::string, std::string> values;

std::string Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string DecodeValue(std::string_view encoded) {
    std::string value = Trim(encoded);
    if (value.size() > 2 && value[1] == '|') {
        const char type = value[0];
        value.erase(0, 2);
        if (type == 's') {
            std::string decoded;
            decoded.reserve(value.size());
            bool escaped = false;
            for (const char c : value) {
                if (escaped) {
                    decoded.push_back(c);
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else {
                    decoded.push_back(c);
                }
            }
            if (escaped) {
                decoded.push_back('\\');
            }
            return decoded;
        }
    }
    return value;
}

u64 TokenMask(std::string_view token) {
    const std::string normalized = Trim(token);
    token = normalized;
    if (token == "PAD_A") return HidNpadButton_A;
    if (token == "PAD_B") return HidNpadButton_B;
    if (token == "PAD_X") return HidNpadButton_X;
    if (token == "PAD_Y") return HidNpadButton_Y;
    if (token == "PAD_UP") return HidNpadButton_Up;
    if (token == "PAD_DOWN") return HidNpadButton_Down;
    if (token == "PAD_LEFT") return HidNpadButton_Left;
    if (token == "PAD_RIGHT") return HidNpadButton_Right;
    if (token == "PAD_LB") return HidNpadButton_L;
    if (token == "PAD_RB") return HidNpadButton_R;
    if (token == "PAD_LT" || token == "PAD_ZL") return HidNpadButton_ZL;
    if (token == "PAD_RT" || token == "PAD_ZR") return HidNpadButton_ZR;
    if (token == "PAD_START") return HidNpadButton_Plus;
    if (token == "PAD_BACK") return HidNpadButton_Minus;
    if (token == "PAD_LSB" || token == "PAD_L3") return HidNpadButton_StickL;
    if (token == "PAD_RSB" || token == "PAD_R3") return HidNpadButton_StickR;
    if (token == "PAD_LEFTSTICKUP") return HidNpadButton_StickLUp;
    if (token == "PAD_LEFTSTICKDOWN") return HidNpadButton_StickLDown;
    if (token == "PAD_LEFTSTICKLEFT") return HidNpadButton_StickLLeft;
    if (token == "PAD_LEFTSTICKRIGHT") return HidNpadButton_StickLRight;
    if (token == "PAD_RIGHTSTICKUP") return HidNpadButton_StickRUp;
    if (token == "PAD_RIGHTSTICKDOWN") return HidNpadButton_StickRDown;
    if (token == "PAD_RIGHTSTICKLEFT") return HidNpadButton_StickRLeft;
    if (token == "PAD_RIGHTSTICKRIGHT") return HidNpadButton_StickRRight;
    return 0;
}

u64 ParseComboMask(std::string_view combo) {
    const std::string value = Trim(combo);
    if (value.empty() || value == "none") {
        return 0;
    }
    u64 mask = 0;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const std::size_t end = value.find('+', begin);
        const std::string_view token = std::string_view(value).substr(
            begin, end == std::string::npos ? value.size() - begin : end - begin);
        mask |= TokenMask(token);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return mask;
}

std::vector<u64> ParseBindingMasks(std::string_view value, u64 fallback) {
    const std::string trimmed = Trim(value);
    if (trimmed.empty() || trimmed == "none") {
        return {};
    }
    std::vector<u64> masks;
    std::size_t begin = 0;
    while (begin <= trimmed.size()) {
        const std::size_t end = trimmed.find('|', begin);
        const std::string_view combo = std::string_view(trimmed).substr(
            begin, end == std::string::npos ? trimmed.size() - begin : end - begin);
        const u64 mask = ParseComboMask(combo);
        if (mask != 0) {
            masks.push_back(mask);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (masks.empty() && fallback != 0) {
        masks.push_back(fallback);
    }
    return masks;
}

u64 ParseMask(std::string_view value, u64 fallback) {
    u64 mask = 0;
    for (const u64 alternative : ParseBindingMasks(value, fallback)) {
        mask |= alternative;
    }
    return mask;
}

bool BindingPressed(std::string_view key, u64 fallback, const PadState& pad) {
    const auto it = values.find(std::string(key));
    const std::vector<u64> masks =
        it == values.end() ? std::vector<u64>{fallback} : ParseBindingMasks(it->second, fallback);
    const u64 buttons = padGetButtons(&pad);
    for (const u64 mask : masks) {
        if (mask != 0 && (buttons & mask) == mask) {
            return true;
        }
    }
    return false;
}

} // namespace

void Reload() {
    values.clear();
    constexpr std::array<const char*, 2> paths{{
        "sdmc:/GBAStation/config/config.cfg",
        "/GBAStation/config/config.cfg",
    }};
    for (const char* path : paths) {
        std::FILE* file = std::fopen(path, "rb");
        if (!file) {
            continue;
        }
        char line[512]{};
        while (std::fgets(line, sizeof(line), file)) {
            const std::string_view text{line};
            const std::size_t equal = text.find('=');
            if (equal == std::string_view::npos) {
                continue;
            }
            const std::string key = Trim(text.substr(0, equal));
            values[key] = DecodeValue(text.substr(equal + 1));
        }
        std::fclose(file);
        break;
    }
}

u64 ButtonMask(std::string_view key, u64 fallback) {
    const auto it = values.find(std::string(key));
    return it == values.end() ? fallback : ParseMask(it->second, fallback);
}

u64 MenuHotkeyMask() {
    const auto it = values.find("3ds.hotkey.menu.pad");
    return it == values.end() ? (HidNpadButton_ZL | HidNpadButton_ZR)
                              : ParseMask(it->second, HidNpadButton_ZL | HidNpadButton_ZR);
}

u64 FastForwardHotkeyMask() {
    const auto it = values.find("3ds.handle.fastforward");
    return it == values.end() ? HidNpadButton_StickL
                              : ParseMask(it->second, HidNpadButton_StickL);
}

u64 PointerModeHotkeyMask() {
    const auto it = values.find("3ds.hotkey.pointer_mode.pad");
    return it == values.end() ? 0 : ParseMask(it->second, 0);
}

u64 PointerClickHotkeyMask() {
    const auto it = values.find("3ds.hotkey.pointer_click.pad");
    return it == values.end() ? 0 : ParseMask(it->second, 0);
}

u64 MicInputHotkeyMask() {
    if (const auto it = values.find("3ds.hotkey.mic_input.pad"); it != values.end()) {
        return ParseMask(it->second, 0);
    }
    const auto legacy = values.find("3ds.hotkey.mic_blow.pad");
    return legacy == values.end() ? 0 : ParseMask(legacy->second, 0);
}

bool FastForwardToggleMode() {
    const auto it = values.find("fastforward.mode");
    return it != values.end() && it->second == "toggle";
}

bool FastForwardEnabled() {
    const auto it = values.find("fastforward.enabled");
    return it == values.end() || (it->second != "0" && it->second != "false" &&
                                  it->second != "off");
}

bool MenuHotkeyPressed(const PadState& pad) {
    return BindingPressed("3ds.hotkey.menu.pad", HidNpadButton_ZL | HidNpadButton_ZR, pad);
}

bool FastForwardHotkeyPressed(const PadState& pad) {
    return BindingPressed("3ds.handle.fastforward", HidNpadButton_StickL, pad);
}

bool PointerModeHotkeyPressed(const PadState& pad) {
    return BindingPressed("3ds.hotkey.pointer_mode.pad", 0, pad);
}

bool PointerClickHotkeyPressed(const PadState& pad) {
    return BindingPressed("3ds.hotkey.pointer_click.pad", 0, pad);
}

bool MicInputHotkeyPressed(const PadState& pad) {
    if (values.find("3ds.hotkey.mic_input.pad") != values.end()) {
        return BindingPressed("3ds.hotkey.mic_input.pad", 0, pad);
    }
    return BindingPressed("3ds.hotkey.mic_blow.pad", 0, pad);
}

} // namespace SwitchFrontend::InputMapping
