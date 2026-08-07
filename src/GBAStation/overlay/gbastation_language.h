/// @file gbastation_language.h
/// @brief JSON-backed zh/en/ja translation helper for the 3DS overlay menus.
///
/// Language comes from the launcher's UI.language (config.cfg) via
/// GBAStationConfig::GetConfiguredSystemLanguage(). Translation tables are
/// loaded from romfs:/rescources/lang/<locale>.json. Falls back to the
/// input Chinese string when a key is missing.

#pragma once

#include <string>
#include <unordered_map>

#include <json.hpp>

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
        return CurrentLocale() == "en-US";
    }

    /// Returns true when the effective UI language is Japanese.
    bool IsJapanese() {
        return CurrentLocale() == "ja-JP";
    }

    /// Translate a Chinese UI string; returns the input unchanged when the
    /// language is Chinese or the key is missing. The returned pointer stays
    /// valid until the next Tr() call (backed by the persistent table_).
    const char* Tr(const std::string& zh) {
        if (CurrentLocale() == "zh-CN") {
            return zh.c_str();
        }
        EnsureTable();
        const auto it = table_.find(zh);
        if (it != table_.end() && !it->second.empty()) {
            return it->second.c_str();
        }
        return zh.c_str();
    }

    /// const char* overload: returns the caller's literal directly for the
    /// Chinese fallback so no temporary std::string (and dangling pointer)
    /// is ever created.
    const char* Tr(const char* zh) {
        if (!zh) {
            return zh;
        }
        if (CurrentLocale() == "zh-CN") {
            return zh;
        }
        EnsureTable();
        const auto it = table_.find(zh);
        if (it != table_.end() && !it->second.empty()) {
            return it->second.c_str();
        }
        return zh;
    }

private:
    GBAStationLanguage() = default;

    /// Resolve the effective locale from the launcher config on every call so
    /// early static-initialization lookups (before ReloadConfig) still pick up
    /// the final language later.
    std::string CurrentLocale() {
        const std::string language = GBAStationConfig::GetConfiguredSystemLanguage();
        if (language == "en-US" || language == "en" || language == "English") {
            return "en-US";
        }
        if (language == "ja-JP" || language == "ja" || language == "Japanese") {
            return "ja-JP";
        }
        return "zh-CN";
    }

    void EnsureTable() {
        if (table_loaded_) {
            return;
        }
        table_loaded_ = true;

        const std::string locale = CurrentLocale();
        const std::string path = "romfs:/rescources/lang/" + locale + ".json";
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) {
            return;
        }
        std::fseek(fp, 0, SEEK_END);
        const long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (size <= 0) {
            std::fclose(fp);
            return;
        }
        std::string content(static_cast<std::size_t>(size), '\0');
        const std::size_t read = std::fread(content.data(), 1, content.size(), fp);
        std::fclose(fp);
        content.resize(read);

        try {
            nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                return;
            }
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_string()) {
                    table_[it.key()] = it.value().get<std::string>();
                }
            }
        } catch (...) {
        }
    }

    bool table_loaded_ = false;
    std::unordered_map<std::string, std::string> table_;
};

} // namespace SwitchFrontend

/// Global shorthand: translate a Chinese UI string for the 3DS menus.
#define GBA_L(zh) SwitchFrontend::GBAStationLanguage::Instance().Tr(zh)
