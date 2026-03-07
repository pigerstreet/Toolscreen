#pragma once

#include <format>
#include <nlohmann/json.hpp>
#include <string>

void LoadLangs();

nlohmann::json GetLangs();

bool LoadTranslation(const std::string& lang);

std::string tr(const char* key);

template <typename... Args>
inline std::string tr(const char* key, Args&&... args) {
    return std::vformat(tr(key), std::make_format_args(std::forward<Args>(args)...));
}

template <typename... Args>
inline char* trc(const char* key, Args&&... args) {
    std::string s   = tr(key, std::forward<Args>(args)...);
    char*       ret = new char[s.size() + 1];
    strcpy(ret, s.c_str());
    return ret;
}