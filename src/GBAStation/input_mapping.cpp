// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/input_mapping.h"

#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

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
        value.erase(0, 2);
    }
    return value;
}

u64 TokenMask(std::string_view token) {
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
    if (token == "PAD_LT") return HidNpadButton_ZL;
    if (token == "PAD_RT") return HidNpadButton_ZR;
    if (token == "PAD_START") return HidNpadButton_Plus;
    if (token == "PAD_BACK") return HidNpadButton_Minus;
    if (token == "PAD_LSB") return HidNpadButton_StickL;
    if (token == "PAD_RSB") return HidNpadButton_StickR;
    return 0;
}

u64 ParseMask(std::string_view encoded, u64 fallback) {
    const std::string value = DecodeValue(encoded);
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
    return mask == 0 ? fallback : mask;
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

bool FastForwardToggleMode() {
    const auto it = values.find("fastforward.mode");
    return it != values.end() && it->second == "toggle";
}

bool FastForwardEnabled() {
    const auto it = values.find("fastforward.enabled");
    return it == values.end() || (it->second != "0" && it->second != "false" &&
                                  it->second != "off");
}

} // namespace SwitchFrontend::InputMapping
