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
        EnsureLoaded();
        return locale_ == "en-US" || locale_ == "English";
    }

    /// Returns true when the effective UI language is Japanese.
    bool IsJapanese() {
        EnsureLoaded();
        return locale_ == "ja-JP" || locale_ == "Japanese";
    }

    /// Translate a Chinese UI string; returns the input unchanged when the
    /// language is Chinese or the key is missing. The returned pointer stays
    /// valid until the next Tr() call (backed by the persistent table_).
    const char* Tr(const std::string& zh) {
        EnsureLoaded();
        if (locale_ == "zh-CN" || locale_ == "Chinese") {
            return zh.c_str();
        }
        const auto it = table_.find(zh);
        if (it != table_.end() && !it->second.empty()) {
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
        if (language == "en-US" || language == "en" || language == "English") {
            locale_ = "en-US";
        } else if (language == "ja-JP" || language == "ja" || language == "Japanese") {
            locale_ = "ja-JP";
        } else {
            locale_ = "zh-CN";
        }
        if (locale_ == "zh-CN") {
            return;
        }

        const std::string path = "romfs:/rescources/lang/" + locale_ + ".json";
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

    bool loaded_ = false;
    std::string locale_ = "zh-CN";
    std::unordered_map<std::string, std::string> table_;
};

} // namespace SwitchFrontend

/// Global shorthand: translate a Chinese UI string for the 3DS menus.
#define GBA_L(zh) SwitchFrontend::GBAStationLanguage::Instance().Tr(zh)
