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
u64 PointerModeHotkeyMask();
u64 PointerClickHotkeyMask();
u64 SwapScreensHotkeyMask();
u64 MicInputHotkeyMask();
bool MenuHotkeyPressed(const PadState& pad);
bool FastForwardHotkeyPressed(const PadState& pad);
bool PointerModeHotkeyPressed(const PadState& pad);
bool PointerClickHotkeyPressed(const PadState& pad);
bool SwapScreensHotkeyPressed(const PadState& pad);
bool MicInputHotkeyPressed(const PadState& pad);
bool FastForwardToggleMode();
bool FastForwardEnabled();

} // namespace SwitchFrontend::InputMapping
