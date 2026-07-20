// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include "GBAStation/switch_libnx.h"

namespace SwitchFrontend::InputMapping {

void Reload();
u64 ButtonMask(std::string_view key, u64 fallback);
u64 MenuHotkeyMask();
u64 FastForwardHotkeyMask();
bool FastForwardToggleMode();
bool FastForwardEnabled();

} // namespace SwitchFrontend::InputMapping
