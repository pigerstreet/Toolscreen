#pragma once

#include "imgui.h"
#include <chrono>
#include <vector>

class ImGuiDrawDataCache {
public:
    ImGuiDrawDataCache();
    ~ImGuiDrawDataCache();

    void CacheFromCurrent();

    bool IsValid() const { return m_valid; }

    ImDrawData* GetCachedDrawData();

    void Clear();

    bool ShouldUpdate() const;

    void MarkUpdated();

    void Invalidate();

private:
    ImDrawList* CloneDrawList(const ImDrawList* src);

    bool m_valid = false;
    bool m_forceUpdate = true;
    std::chrono::steady_clock::time_point m_lastUpdateTime;
    
    ImDrawData m_cachedDrawData;
    
    std::vector<ImDrawList*> m_ownedDrawLists;
    
    static constexpr int UPDATE_INTERVAL_MS = 33;
};

extern ImGuiDrawDataCache g_imguiCache;

inline bool ShouldUpdateImGui() { return g_imguiCache.ShouldUpdate(); }
inline void CacheImGuiDrawData() { g_imguiCache.CacheFromCurrent(); g_imguiCache.MarkUpdated(); }
inline void RenderCachedImGuiDrawData(void (*renderFunc)(ImDrawData*)) {
    if (g_imguiCache.IsValid()) {
        renderFunc(g_imguiCache.GetCachedDrawData());
    }
}
inline void InvalidateImGuiCache() { g_imguiCache.Invalidate(); }


