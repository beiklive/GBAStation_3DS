// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/game_db.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include <json.hpp>

#include "common/logging/log.h"

namespace SwitchFrontend::GameDatabase {
namespace {

constexpr std::array<const char*, 2> DatabasePaths{{
    "sdmc:/GBAStation/data/GameData_3DS.json",
    "/GBAStation/data/GameData_3DS.json",
}};

std::string NormalizePath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.rfind("sdmc:", 0) == 0) {
        path.erase(0, 5);
    }
    while (path.size() > 1 && path[0] == '/' && path[1] == '/') {
        path.erase(0, 1);
    }
    return path;
}

bool IsKnownLayout(const std::string& layout) {
    constexpr std::array<const char*, 7> Layouts{{
        "vertical", "horizontal", "priority_top", "hybrid", "top", "bottom", "custom",
    }};
    return std::find(Layouts.begin(), Layouts.end(), layout) != Layouts.end();
}

int ParseOrientation(const nlohmann::json& item) {
    std::string value = item.value("ndsScreenOrientation", "0");
    if (value == "90") return 90;
    if (value == "180") return 180;
    if (value == "270") return 270;
    return 0;
}

GBAStationDisplaySettings ReadDisplaySettings(const nlohmann::json& item) {
    GBAStationDisplaySettings settings;
    const std::string layout = item.value("ndsScreenLayout", settings.screen_layout);
    if (IsKnownLayout(layout)) {
        settings.screen_layout = layout;
    }
    settings.screen_orientation = ParseOrientation(item);
    settings.internal_resolution = std::clamp(item.value("ndsInternalResolution", 1), 1, 4);
    settings.integer_scale = item.value("ndsIntegerScale", true);
    settings.screen_gap = std::clamp(item.value("ndsScreenGap", 0), -256, 256);
    settings.top_scale = std::clamp(item.value("ndsTopScale", 1.0f), 1.0f, 10.0f);
    settings.top_offset_x = std::clamp(item.value("ndsTopOffsetX", 0.0f), -1024.0f, 1024.0f);
    settings.top_offset_y = std::clamp(item.value("ndsTopOffsetY", 0.0f), -1024.0f, 1024.0f);
    settings.bottom_scale = std::clamp(item.value("ndsBottomScale", 1.0f), 1.0f, 10.0f);
    settings.bottom_offset_x =
        std::clamp(item.value("ndsBottomOffsetX", 0.0f), -1024.0f, 1024.0f);
    settings.bottom_offset_y =
        std::clamp(item.value("ndsBottomOffsetY", 0.0f), -1024.0f, 1024.0f);
    settings.bottom_opacity = std::clamp(item.value("ndsBottomOpacity", 1.0f), 0.0f, 1.0f);
    settings.overlay_enabled = item.value("overlayEnabled", false);
    settings.overlay_path = item.value("overlayPath", "");
    return settings;
}

void WriteDisplaySettings(nlohmann::json& item, const GBAStationDisplaySettings& settings) {
    item["ndsScreenLayout"] = IsKnownLayout(settings.screen_layout) ? settings.screen_layout
                                                                    : "priority_top";
    item["ndsScreenOrientation"] = std::to_string(settings.screen_orientation);
    item["ndsInternalResolution"] = std::clamp(settings.internal_resolution, 1, 4);
    item["ndsIntegerScale"] = settings.integer_scale;
    item["ndsScreenGap"] = std::clamp(settings.screen_gap, -256, 256);
    item["ndsTopScale"] = std::clamp(settings.top_scale, 1.0f, 10.0f);
    item["ndsTopOffsetX"] = std::clamp(settings.top_offset_x, -1024.0f, 1024.0f);
    item["ndsTopOffsetY"] = std::clamp(settings.top_offset_y, -1024.0f, 1024.0f);
    item["ndsBottomScale"] = std::clamp(settings.bottom_scale, 1.0f, 10.0f);
    item["ndsBottomOffsetX"] = std::clamp(settings.bottom_offset_x, -1024.0f, 1024.0f);
    item["ndsBottomOffsetY"] = std::clamp(settings.bottom_offset_y, -1024.0f, 1024.0f);
    item["ndsBottomOpacity"] = std::clamp(settings.bottom_opacity, 0.0f, 1.0f);
}

void WriteOverlaySettings(nlohmann::json& item, const GBAStationDisplaySettings& settings) {
    item["overlayEnabled"] = settings.overlay_enabled;
    item["overlayPath"] = settings.overlay_path;
}

void WriteAllDisplaySettings(nlohmann::json& item, const GBAStationDisplaySettings& settings) {
    item["ndsScreenLayout"] = IsKnownLayout(settings.screen_layout) ? settings.screen_layout
                                                                    : "priority_top";
    item["ndsScreenOrientation"] = std::to_string(settings.screen_orientation);
    item["ndsInternalResolution"] = std::clamp(settings.internal_resolution, 1, 4);
    item["ndsIntegerScale"] = settings.integer_scale;
    item["ndsScreenGap"] = std::clamp(settings.screen_gap, -256, 256);
    item["ndsTopScale"] = std::clamp(settings.top_scale, 1.0f, 10.0f);
    item["ndsTopOffsetX"] = std::clamp(settings.top_offset_x, -1024.0f, 1024.0f);
    item["ndsTopOffsetY"] = std::clamp(settings.top_offset_y, -1024.0f, 1024.0f);
    item["ndsBottomScale"] = std::clamp(settings.bottom_scale, 1.0f, 10.0f);
    item["ndsBottomOffsetX"] = std::clamp(settings.bottom_offset_x, -1024.0f, 1024.0f);
    item["ndsBottomOffsetY"] = std::clamp(settings.bottom_offset_y, -1024.0f, 1024.0f);
    item["ndsBottomOpacity"] = std::clamp(settings.bottom_opacity, 0.0f, 1.0f);
    WriteOverlaySettings(item, settings);
}

bool ReadDatabase(const char* path, nlohmann::json& data) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    data = nlohmann::json::parse(input, nullptr, false);
    return data.is_array();
}

bool WriteDatabase(const char* path, const nlohmann::json& data) {
    mkdir("sdmc:/GBAStation", 0777);
    mkdir("sdmc:/GBAStation/data", 0777);
    const std::string temporary = std::string(path) + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            LOG_ERROR(Frontend, "3DS GameDB failed to open temporary file {} errno={}",
                      temporary, errno);
            return false;
        }
        output << data.dump(4) << '\n';
        if (!output.good()) {
            LOG_ERROR(Frontend, "3DS GameDB failed while writing {} errno={}", temporary,
                      errno);
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path) != 0) {
        std::ifstream temporary_input(temporary, std::ios::binary);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << temporary_input.rdbuf();
        const bool copied = temporary_input.good() || temporary_input.eof();
        const bool written = output.good();
        output.close();
        temporary_input.close();
        std::remove(temporary.c_str());
        if (!copied || !written) {
            LOG_ERROR(Frontend, "3DS GameDB fallback copy failed path={} errno={}", path,
                      errno);
        }
        return copied && written;
    }
    return true;
}

std::string CurrentTimestamp() {
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (!local) {
        return {};
    }
    char text[32]{};
    std::strftime(text, sizeof(text), "%y-%m-%d %H-%M-%S", local);
    return text;
}

bool LoadWritableDatabase(nlohmann::json& data, const char*& target_path) {
    data = nlohmann::json::array();
    target_path = DatabasePaths.front();
    for (const char* path : DatabasePaths) {
        if (ReadDatabase(path, data)) {
            target_path = path;
            return true;
        }
    }
    return false;
}

nlohmann::json& FindOrCreateRecord(nlohmann::json& data, const std::string& rom_path,
                                   const std::string& title) {
    const std::string normalized_rom = NormalizePath(rom_path);
    for (auto& item : data) {
        if (item.is_object() && NormalizePath(item.value("path", "")) == normalized_rom) {
            if (!title.empty()) item["title"] = title;
            return item;
        }
    }
    data.push_back(nlohmann::json::object({{"path", rom_path}, {"title", title}}));
    return data.back();
}

} // namespace

GameRecord LoadGameRecord(const std::string& rom_path) {
    const std::string normalized_rom = NormalizePath(rom_path);
    for (const char* path : DatabasePaths) {
        nlohmann::json data;
        if (!ReadDatabase(path, data)) {
            continue;
        }
        for (const auto& item : data) {
            if (!item.is_object() || NormalizePath(item.value("path", "")) != normalized_rom) {
                continue;
            }
            GameRecord record;
            record.found = true;
            record.title = item.value("title", "");
            record.display = ReadDisplaySettings(item);
            LOG_INFO(Frontend, "3DS GameDB matched {}", rom_path);
            return record;
        }
    }
    LOG_INFO(Frontend, "3DS GameDB has no record for {}", rom_path);
    return {};
}

bool SaveDisplaySettings(const std::string& rom_path, const std::string& title,
                         const GBAStationDisplaySettings& settings) {
    const char* target_path = nullptr;
    nlohmann::json data;
    LoadWritableDatabase(data, target_path);
    auto& matched = FindOrCreateRecord(data, rom_path, title);
    WriteAllDisplaySettings(matched, settings);
    const bool saved = WriteDatabase(target_path, data);
    LOG_INFO(Frontend, "3DS GameDB display settings save {} path={}", saved ? "ok" : "failed",
             target_path);
    return saved;
}

bool AddInstalledTitle(const std::string& content_path, const std::string& title,
                       const std::string& three_ds_id) {
    const char* target_path = nullptr;
    nlohmann::json data;
    LoadWritableDatabase(data, target_path);
    auto& item = FindOrCreateRecord(data, content_path, title);
    item["platform"] = 7;
    item["3ds_id"] = three_ds_id;
    item["core"] = "azahar";
    item["playCount"] = item.value("playCount", 0);
    item["playTime"] = item.value("playTime", 0);
    item["lastPlayed"] = item.value("lastPlayed", "");
    item["favourite"] = item.value("favourite", false);
    const bool saved = WriteDatabase(target_path, data);
    LOG_INFO(Frontend, "3DS GameDB installed title save {} id={} path={}",
             saved ? "ok" : "failed", three_ds_id, content_path);
    return saved;
}

bool SyncDisplaySettings(const GBAStationDisplaySettings& settings, bool include_screen,
                         bool include_overlay, int& updated_count) {
    updated_count = 0;
    const char* target_path = nullptr;
    nlohmann::json data;
    LoadWritableDatabase(data, target_path);
    for (auto& item : data) {
        if (!item.is_object()) {
            continue;
        }
        if (include_screen) {
            WriteDisplaySettings(item, settings);
        }
        if (include_overlay) {
            WriteOverlaySettings(item, settings);
        }
        ++updated_count;
    }
    const bool saved = WriteDatabase(target_path, data);
    LOG_INFO(Frontend,
             "3DS GameDB sync {} screen={} overlay={} count={} path={}",
             saved ? "ok" : "failed", include_screen ? 1 : 0, include_overlay ? 1 : 0,
             updated_count, target_path);
    return saved;
}

bool UpdatePlayStats(const std::string& rom_path, const std::string& title,
                     bool increment_count, int additional_seconds) {
    const char* target_path = nullptr;
    nlohmann::json data;
    LoadWritableDatabase(data, target_path);
    auto& item = FindOrCreateRecord(data, rom_path, title);
    if (increment_count) {
        item["playCount"] = std::max(0, item.value("playCount", 0)) + 1;
        item["lastPlayed"] = CurrentTimestamp();
    }
    if (additional_seconds > 0) {
        item["playTime"] = std::max(0, item.value("playTime", 0)) + additional_seconds;
    }
    const bool saved = WriteDatabase(target_path, data);
    LOG_INFO(Frontend,
             "3DS GameDB play stats save {} count_increment={} seconds={} path={}",
             saved ? "ok" : "failed", increment_count ? 1 : 0,
             std::max(0, additional_seconds), target_path);
    return saved;
}

} // namespace SwitchFrontend::GameDatabase
