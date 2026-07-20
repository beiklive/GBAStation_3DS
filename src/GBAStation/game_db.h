// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "GBAStation/display_settings.h"

namespace SwitchFrontend::GameDatabase {

struct GameRecord {
    bool found{};
    std::string title;
    GBAStationDisplaySettings display;
};

GameRecord LoadGameRecord(const std::string& rom_path);
bool SaveDisplaySettings(const std::string& rom_path, const std::string& title,
                         const GBAStationDisplaySettings& settings);

} // namespace SwitchFrontend::GameDatabase
