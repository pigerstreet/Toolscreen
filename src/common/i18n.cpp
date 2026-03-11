#include "i18n.h"

#include "platform/resource.h"
#include "utils.h"

inline nlohmann::json                               g_langsJson;
inline nlohmann::json                               g_translationJson;
inline std::unordered_map<const char*, std::string> g_translationCache;

void LoadLangs() {
    try {
        HMODULE hModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&RenderWelcomeToast, &hModule
        );
        if (!hModule) {
            throw std::exception("GetModuleHandleExW failed");
        }

        HRSRC resSrc = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_LANG_LANGS), RT_RCDATA);
        if (!resSrc) {
            throw std::exception("FindResourceW failed");
        }

        HGLOBAL resHandle = LoadResource(hModule, resSrc);
        if (!resHandle) {
            throw std::exception("LoadResource failed");
        }

        auto* const resPtr = LockResource(resHandle);
        if (!resPtr) {
            throw std::exception("LockResource failed");
        }

        const auto resSize = SizeofResource(hModule, resSrc);
        if (resSize == 0) {
            throw std::exception("SizeofResource failed");
        }

        g_langsJson = nlohmann::json::parse((const char*)resPtr, (const char*)resPtr + resSize);
    } catch (const std::exception& e) {
        Log(std::string("Failed to load language list: ") + e.what());
    }
}

nlohmann::json GetLangs() {
    return g_langsJson;
}

bool LoadTranslation(const std::string& lang) {
    static const std::unordered_map<std::string, LPWSTR> langToResName = {
        {"en", MAKEINTRESOURCEW(IDR_LANG_EN)},
        {"zh_CN", MAKEINTRESOURCEW(IDR_LANG_ZH_CN)},
    };

    g_translationCache.clear();

    try {
        HMODULE hModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&RenderWelcomeToast, &hModule
        );
        if (!hModule) {
            throw std::exception("GetModuleHandleExW failed");
        }

        HRSRC resSrc = FindResourceW(hModule, langToResName.at(lang), RT_RCDATA);
        if (!resSrc) {
            throw std::exception("FindResourceW failed");
        }

        HGLOBAL resHandle = LoadResource(hModule, resSrc);
        if (!resHandle) {
            throw std::exception("LoadResource failed");
        }

        auto* const resPtr = LockResource(resHandle);
        if (!resPtr) {
            throw std::exception("LockResource failed");
        }

        const auto resSize = SizeofResource(hModule, resSrc);
        if (resSize == 0) {
            throw std::exception("SizeofResource failed");
        }

        g_translationJson = nlohmann::json::parse((const char*)resPtr, (const char*)resPtr + resSize);
    } catch (const std::exception& e) {
        Log("Failed to load translations of " + lang + ": " + e.what());
        return false;
    }
    return true;
}

std::string tr(const char* key) {
    auto cacheIt = g_translationCache.find(key);
    if (cacheIt != g_translationCache.end()) {
        return cacheIt->second;
    }

    if (!g_translationJson.contains(key)) {
        Log("Missing translation for key: " + std::string(key));
        return key;
    }
    if (!g_translationJson[key].is_string()) {
        Log(std::format("Translation for key '{}' is not a string", key));
        return key;
    }

    std::string result      = g_translationJson[key];
    g_translationCache[key] = result;
    return result;
}
