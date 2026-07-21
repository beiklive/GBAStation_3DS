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
bool SyncDisplaySettings(const GBAStationDisplaySettings& settings, bool include_screen,
                         bool include_overlay, int& updated_count);
bool UpdatePlayStats(const std::string& rom_path, const std::string& title,
                     bool increment_count, int additional_seconds);
bool AddInstalledTitle(const std::string& content_path, const std::string& title,
                       const std::string& three_ds_id);

} // namespace SwitchFrontend::GameDatabase
