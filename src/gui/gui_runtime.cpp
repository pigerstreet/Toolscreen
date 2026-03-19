#include "gui_internal.h"

#include "common/i18n.h"
#include "common/profiler.h"
#include "common/utils.h"
#include "imgui_cache.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "platform/resource.h"
#include "render/obs_thread.h"
#include "render/render.h"
#include "runtime/logic_thread.h"
#include "third_party/stb_image.h"

#include <GL/glew.h>
#include <array>
#include <algorithm>
#include <chrono>

static std::recursive_mutex s_imguiContextMutex;

namespace {

struct KeyboardLayoutFontRefreshRequest {
    bool pending = false;
    bool force = false;
    ImVec2 windowSize = ImVec2(0.0f, 0.0f);
    float keyHeight = 0.0f;
    float keyboardScale = 1.0f;
};

struct KeyboardLayoutFontRefreshState {
    bool valid = false;
    std::string fontPath;
    ImVec2 windowSize = ImVec2(0.0f, 0.0f);
    float guiScaleFactor = 1.0f;
    float keyHeight = 0.0f;
    float keyboardScale = 1.0f;
    float primarySize = 0.0f;
    float secondarySize = 0.0f;
};

struct MainGuiFontRefreshState {
    bool valid = false;
    float guiScaleFactor = 1.0f;
    std::string fontPath;
};

struct MainGuiFontRefreshRequest {
    bool pending = false;
    bool force = false;
};

KeyboardLayoutFontRefreshRequest s_pendingKeyboardLayoutFontRefresh;
KeyboardLayoutFontRefreshState s_keyboardLayoutFontRefreshState;
MainGuiFontRefreshState s_mainGuiFontRefreshState;
MainGuiFontRefreshRequest s_pendingMainGuiFontRefresh;

constexpr std::array<const char*, 5> kLocalizedFallbackFontPaths = {
    "c:\\Windows\\Fonts\\msyh.ttc",
    "c:\\Windows\\Fonts\\msyhbd.ttc",
    "c:\\Windows\\Fonts\\Deng.ttf",
    "c:\\Windows\\Fonts\\simhei.ttf",
    "c:\\Windows\\Fonts\\simsun.ttc",
};

bool FontFileExists(const char* path) {
    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

const ImWchar* GetGlyphRangesOrDefault(ImFontAtlas* atlas, const std::vector<ImWchar>& glyphRanges) {
    if (!glyphRanges.empty()) {
        return glyphRanges.data();
    }
    return atlas->GetGlyphRangesDefault();
}

void AddMergedLocalizedFallbackFont(ImFontAtlas* atlas, float size, const std::vector<ImWchar>& glyphRanges) {
    ImFontConfig mergeCfg{};
    mergeCfg.MergeMode = true;
    mergeCfg.PixelSnapH = true;
    mergeCfg.OversampleH = 2;
    mergeCfg.OversampleV = 2;

    const ImWchar* ranges = GetGlyphRangesOrDefault(atlas, glyphRanges);
    for (const char* fallbackPath : kLocalizedFallbackFontPaths) {
        if (!FontFileExists(fallbackPath)) {
            continue;
        }

        if (atlas->AddFontFromFileTTF(fallbackPath, size, &mergeCfg, ranges) != nullptr) {
            return;
        }
    }
}

}

std::recursive_mutex& GetImGuiContextMutex() { return s_imguiContextMutex; }

static std::string ResolveGuiFontPath(float baseFontSize) {
    const std::string configuredFontPath = g_config.fontPath;

    auto isStable = [](const std::string& path, float size) -> bool {
        if (path.empty()) return false;
        ImFontAtlas testAtlas;
        ImFont* font = testAtlas.AddFontFromFileTTF(path.c_str(), size);
        if (!font) return false;
        return testAtlas.Build();
    };

    std::string usePath = configuredFontPath.empty() ? ConfigDefaults::CONFIG_FONT_PATH : configuredFontPath;
    if (!isStable(usePath, baseFontSize)) { usePath = ConfigDefaults::CONFIG_FONT_PATH; }
    return usePath;
}

static ImFont* AddFontWithFallback(ImFontAtlas* atlas, const std::string& fontPath, float size, const ImFontConfig* config = nullptr,
                                   const ImWchar* glyphRanges = nullptr) {
    ImFont* font = atlas->AddFontFromFileTTF(fontPath.c_str(), size, config, glyphRanges);
    if (!font && fontPath != ConfigDefaults::CONFIG_FONT_PATH) {
        font = atlas->AddFontFromFileTTF(ConfigDefaults::CONFIG_FONT_PATH.c_str(), size, config, glyphRanges);
    }
    return font;
}

static ImFont* AddKeyboardFontWithFallback(ImFontAtlas* atlas, const std::string& fontPath, float preferredSize, const ImFontConfig& config) {
    static constexpr float kSteps[] = { 1.00f, 0.90f, 0.80f, 0.70f, 0.62f, 0.55f };
    for (float step : kSteps) {
        ImFont* font = AddFontWithFallback(atlas, fontPath, preferredSize * step, &config);
        if (font) { return font; }
    }
    return nullptr;
}

static void RebuildImGuiFontAtlas(float scaleFactor, float keyboardPrimarySize, float keyboardSecondarySize) {
    ImGuiIO& io = ImGui::GetIO();

    const float baseFontSize = 16.0f * scaleFactor;
    const std::string usePath = ResolveGuiFontPath(baseFontSize);
    const std::vector<ImWchar> localizedGlyphRanges = BuildTranslationGlyphRanges();
    const ImWchar* localizedRanges = GetGlyphRangesOrDefault(io.Fonts, localizedGlyphRanges);

    io.Fonts->Clear();

    ImFont* baseFont = AddFontWithFallback(io.Fonts, usePath, baseFontSize, nullptr, localizedRanges);
    if (!baseFont) {
        Log("GUI: Failed to load configured font, using ImGui default font");
        baseFont = io.Fonts->AddFontDefault();
    }
    AddMergedLocalizedFallbackFont(io.Fonts, baseFontSize, localizedGlyphRanges);
    io.FontDefault = baseFont;

    ImFontConfig keyFontCfg{};
    keyFontCfg.OversampleH = 4;
    keyFontCfg.OversampleV = 2;
    keyFontCfg.PixelSnapH = true;

    const float primarySize = (keyboardPrimarySize > 0.0f) ? keyboardPrimarySize : (baseFontSize * 2.80f);
    const float secondarySize = (keyboardSecondarySize > 0.0f) ? keyboardSecondarySize : (baseFontSize * 2.00f);

    g_keyboardLayoutPrimaryFont = AddKeyboardFontWithFallback(io.Fonts, usePath, primarySize, keyFontCfg);
    g_keyboardLayoutSecondaryFont = AddKeyboardFontWithFallback(io.Fonts, usePath, secondarySize, keyFontCfg);

    if (!g_keyboardLayoutPrimaryFont) g_keyboardLayoutPrimaryFont = baseFont;
    if (!g_keyboardLayoutSecondaryFont) g_keyboardLayoutSecondaryFont = baseFont;

    InitializeOverlayTextFont(usePath, 16.0f, scaleFactor);
    io.Fonts->Build();

    if (io.BackendRendererUserData != nullptr) {
        ImGui_ImplOpenGL3_DestroyDeviceObjects();
        ImGui_ImplOpenGL3_CreateDeviceObjects();
    }
}

static void ConfigureImGuiFontsAndStyle(float scaleFactor) {
    RebuildImGuiFontAtlas(scaleFactor, 0.0f, 0.0f);

    ImGuiStyle defaultStyle;
    ImGui::GetStyle() = defaultStyle;
    LoadTheme();
    ApplyAppearanceConfig();
    ImGui::GetStyle().ScaleAllSizes(scaleFactor);

    s_mainGuiFontRefreshState.valid = true;
    s_mainGuiFontRefreshState.guiScaleFactor = scaleFactor;
    s_mainGuiFontRefreshState.fontPath = ResolveGuiFontPath(16.0f * scaleFactor);
}

static ImGuiContext* s_mainThreadImGuiContext = nullptr;
static HWND s_mainThreadImGuiHwnd = NULL;
static HGLRC s_mainThreadImGuiGlContext = NULL;

float ComputeGuiScaleFactorFromCachedWindowSize() {
    int screenWidth = GetCachedWindowWidth();
    int screenHeight = GetCachedWindowHeight();
    if (screenWidth < 1) screenWidth = 1;
    if (screenHeight < 1) screenHeight = 1;

    const float widthScale = static_cast<float>(screenWidth) / 1920.0f;
    const float heightScale = static_cast<float>(screenHeight) / 1080.0f;
    float scaleFactor = (std::min)(widthScale, heightScale);
    scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
    if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }
    return scaleFactor;
}

void RequestDynamicGuiFontRefresh(bool forceRefresh) {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    s_pendingMainGuiFontRefresh.pending = true;
    s_pendingMainGuiFontRefresh.force = s_pendingMainGuiFontRefresh.force || forceRefresh;
}

void ApplyDynamicGuiFontRefresh() {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    if (ImGui::GetCurrentContext() == nullptr) { return; }

    const float guiScaleFactor = ComputeGuiScaleFactorFromCachedWindowSize();
    const std::string fontPath = ResolveGuiFontPath(16.0f * guiScaleFactor);
    const bool overlayFontReloadRequested = g_eyeZoomFontNeedsReload.exchange(false, std::memory_order_acq_rel);
    const bool hasPendingRequest = s_pendingMainGuiFontRefresh.pending;
    const bool scaleChanged = !s_mainGuiFontRefreshState.valid || fabsf(guiScaleFactor - s_mainGuiFontRefreshState.guiScaleFactor) > 0.001f;
    const bool fontPathChanged = !s_mainGuiFontRefreshState.valid || fontPath != s_mainGuiFontRefreshState.fontPath;
    const bool mustRefresh = s_pendingMainGuiFontRefresh.force || scaleChanged || fontPathChanged || overlayFontReloadRequested;

    s_pendingMainGuiFontRefresh.pending = false;
    s_pendingMainGuiFontRefresh.force = false;
    if (!hasPendingRequest && !mustRefresh) { return; }
    if (!mustRefresh) { return; }

    ConfigureImGuiFontsAndStyle(guiScaleFactor);
    if (s_keyboardLayoutFontRefreshState.valid) {
        s_pendingKeyboardLayoutFontRefresh.pending = true;
        s_pendingKeyboardLayoutFontRefresh.force = true;
        s_pendingKeyboardLayoutFontRefresh.windowSize = s_keyboardLayoutFontRefreshState.windowSize;
        s_pendingKeyboardLayoutFontRefresh.keyHeight = s_keyboardLayoutFontRefreshState.keyHeight;
        s_pendingKeyboardLayoutFontRefresh.keyboardScale = s_keyboardLayoutFontRefreshState.keyboardScale;
    }
    InvalidateImGuiCache();
}

void RequestKeyboardLayoutFontRefresh(const ImVec2& windowSize, float keyHeight, float keyboardScale, bool forceRefresh) {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    if (windowSize.x <= 0.0f || windowSize.y <= 0.0f || keyHeight <= 0.0f) { return; }

    s_pendingKeyboardLayoutFontRefresh.pending = true;
    s_pendingKeyboardLayoutFontRefresh.force = s_pendingKeyboardLayoutFontRefresh.force || forceRefresh;
    s_pendingKeyboardLayoutFontRefresh.windowSize = windowSize;
    s_pendingKeyboardLayoutFontRefresh.keyHeight = keyHeight;
    s_pendingKeyboardLayoutFontRefresh.keyboardScale = keyboardScale;
}

void ApplyPendingKeyboardLayoutFontRefresh() {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    if (!s_pendingKeyboardLayoutFontRefresh.pending || ImGui::GetCurrentContext() == nullptr) { return; }

    const KeyboardLayoutFontRefreshRequest request = s_pendingKeyboardLayoutFontRefresh;
    s_pendingKeyboardLayoutFontRefresh.pending = false;
    s_pendingKeyboardLayoutFontRefresh.force = false;

    const float guiScaleFactor = ComputeGuiScaleFactorFromCachedWindowSize();
    const float baseFontSize = 16.0f * guiScaleFactor;
    const std::string fontPath = ResolveGuiFontPath(baseFontSize);

    const float widthScale = request.windowSize.x / 1180.0f;
    const float heightScale = request.windowSize.y / 720.0f;
    const float windowFactor = std::clamp((std::min)(widthScale, heightScale), 0.85f, 1.08f);

    float desiredPrimary = roundf(request.keyHeight * (0.50f + 0.02f * request.keyboardScale) * windowFactor);
    float desiredSecondary = roundf(request.keyHeight * (0.34f + 0.02f * request.keyboardScale) * windowFactor);

    desiredPrimary = std::clamp(desiredPrimary, baseFontSize * 1.00f, baseFontSize * 2.05f);
    desiredSecondary = std::clamp(desiredSecondary, baseFontSize * 0.82f, (std::max)(baseFontSize * 0.82f, desiredPrimary - 2.0f));

    const bool settingsChanged = !s_keyboardLayoutFontRefreshState.valid || request.force ||
        fabsf(desiredPrimary - s_keyboardLayoutFontRefreshState.primarySize) > 0.5f ||
        fabsf(desiredSecondary - s_keyboardLayoutFontRefreshState.secondarySize) > 0.5f ||
        fabsf(guiScaleFactor - s_keyboardLayoutFontRefreshState.guiScaleFactor) > 0.001f ||
        fontPath != s_keyboardLayoutFontRefreshState.fontPath;
    if (!settingsChanged) { return; }

    RebuildImGuiFontAtlas(guiScaleFactor, desiredPrimary, desiredSecondary);

    s_keyboardLayoutFontRefreshState.valid = true;
    s_keyboardLayoutFontRefreshState.fontPath = fontPath;
    s_keyboardLayoutFontRefreshState.windowSize = request.windowSize;
    s_keyboardLayoutFontRefreshState.guiScaleFactor = guiScaleFactor;
    s_keyboardLayoutFontRefreshState.keyHeight = request.keyHeight;
    s_keyboardLayoutFontRefreshState.keyboardScale = request.keyboardScale;
    s_keyboardLayoutFontRefreshState.primarySize = desiredPrimary;
    s_keyboardLayoutFontRefreshState.secondarySize = desiredSecondary;

    InvalidateImGuiCache();
}

void HandleImGuiContextReset() {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    if (s_mainThreadImGuiContext) {
        ImGui::SetCurrentContext(s_mainThreadImGuiContext);
        Log("Performing deferred full ImGui context reset.");
        ClearSupporterTierTextureCache();
        g_keyboardLayoutPrimaryFont = nullptr;
        g_keyboardLayoutSecondaryFont = nullptr;
        s_pendingMainGuiFontRefresh = {};
        s_pendingKeyboardLayoutFontRefresh = {};
        s_keyboardLayoutFontRefreshState = {};
        s_mainGuiFontRefreshState = {};
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(s_mainThreadImGuiContext);
        s_mainThreadImGuiContext = nullptr;
        s_mainThreadImGuiHwnd = NULL;
        s_mainThreadImGuiGlContext = NULL;
    }
}

void InitializeImGuiContext(HWND hwnd) {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());
    const HGLRC currentGlContext = wglGetCurrentContext();
    if (s_mainThreadImGuiContext != nullptr && currentGlContext != NULL && s_mainThreadImGuiGlContext != NULL &&
        currentGlContext != s_mainThreadImGuiGlContext) {
        Log("Main-thread ImGui detected WGL context change; recreating context.");
        HandleImGuiContextReset();
    }

    if (s_mainThreadImGuiContext == nullptr) {
        Log("Re-creating ImGui context after full reset.");
        IMGUI_CHECKVERSION();
        s_mainThreadImGuiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(s_mainThreadImGuiContext);
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplOpenGL3_Init("#version 330");
        ConfigureImGuiFontsAndStyle(ComputeGuiScaleFactorFromCachedWindowSize());
        s_mainThreadImGuiHwnd = hwnd;
        s_mainThreadImGuiGlContext = currentGlContext;
        return;
    }

    ImGui::SetCurrentContext(s_mainThreadImGuiContext);
    if (hwnd != NULL && hwnd != s_mainThreadImGuiHwnd) {
        ImGui_ImplWin32_Shutdown();
        ImGui_ImplWin32_Init(hwnd);
        s_mainThreadImGuiHwnd = hwnd;
    }
    if (currentGlContext != NULL) {
        s_mainThreadImGuiGlContext = currentGlContext;
    }
}

bool IsGuiHotkeyPressed(WPARAM wParam) { return CheckHotkeyMatch(g_config.guiHotkey, wParam); }

std::atomic<bool> g_welcomeToastVisible{ false };
std::atomic<bool> g_configurePromptDismissedThisSession{ false };

std::atomic<bool> g_pieSpikeAlertActive{ false };
std::atomic<float> g_pieSpikeLastOrangeRatio{ 0.0f };
std::atomic<int64_t> g_pieSpikeLastAlertTimeMs{ 0 };
char g_pieSpikeMatchedName[64] = {};

static ImU32 ScaleToastAlpha(ImU32 color, float alpha) {
    ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
    rgba.w *= std::clamp(alpha, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(rgba);
}

static void RenderFullscreenWelcomeToastImGui(float toastOpacity) {
    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());

    HWND hwnd = g_minecraftHwnd.load();
    if (!hwnd) { return; }

    InitializeImGuiContext(hwnd);
    if (ImGui::GetCurrentContext() == nullptr) { return; }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    SyncImGuiDisplayMetrics(hwnd);
    ApplyDynamicGuiFontRefresh();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        ImGui::Render();
        RenderImGuiWithStateProtection(true);
        return;
    }

    const float toastScale = (io.DisplaySize.y / 1080.0f) * 0.45f;
    const ImVec2 toastSize(700.0f * toastScale, 250.0f * toastScale);
    const float unit = toastSize.y / 250.0f;

    std::string titleText = tr("label.toolscreen");
    std::string hotkeyText = GetKeyComboString(g_config.guiHotkey);
    if (hotkeyText.empty()) {
        hotkeyText = tr("hotkeys.none");
    }
    const std::string messageText = tr("welcome_toast.fullscreen_press_to_configure", hotkeyText);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(toastSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("##fullscreen_welcome_toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const ImVec2 origin = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float rounding = 28.0f * unit;

        auto calcTextSize = [&](const std::string& text, float fontSize) {
            return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
        };
        auto addVec = [](const ImVec2& a, const ImVec2& b) {
            return ImVec2(a.x + b.x, a.y + b.y);
        };
        auto subVec = [](const ImVec2& a, const ImVec2& b) {
            return ImVec2(a.x - b.x, a.y - b.y);
        };
        auto fitTextSize = [&](const std::string& text, float desiredSize, float minSize, float maxWidth) {
            float fontSize = desiredSize;
            while (fontSize > minSize && calcTextSize(text, fontSize).x > maxWidth) {
                fontSize -= unit;
            }
            return fontSize;
        };
        auto drawCenteredText = [&](const std::string& text, float centerY, float fontSize, ImU32 color) {
            const ImVec2 textSize = calcTextSize(text, fontSize);
            const ImVec2 textPos(origin.x + (size.x - textSize.x) * 0.5f, origin.y + centerY - textSize.y * 0.5f);
            drawList->AddText(font, fontSize, addVec(textPos, ImVec2(2.0f * unit, 2.0f * unit)), ScaleToastAlpha(IM_COL32(20, 8, 31, 145), toastOpacity),
                              text.c_str());
            drawList->AddText(font, fontSize, textPos, color, text.c_str());
        };
        auto drawCenteredOutlinedText = [&](const std::string& text, float centerY, float fontSize, ImU32 fillColor, ImU32 outlineColor,
                                            float outlineOffset) {
            const ImVec2 textSize = calcTextSize(text, fontSize);
            const ImVec2 textPos(origin.x + (size.x - textSize.x) * 0.5f, origin.y + centerY - textSize.y * 0.5f);
            const ImVec2 offsets[] = {
                ImVec2(-outlineOffset, 0.0f), ImVec2(outlineOffset, 0.0f), ImVec2(0.0f, -outlineOffset), ImVec2(0.0f, outlineOffset),
                ImVec2(-outlineOffset, -outlineOffset), ImVec2(outlineOffset, -outlineOffset), ImVec2(-outlineOffset, outlineOffset),
                ImVec2(outlineOffset, outlineOffset),
            };

            for (const ImVec2& offset : offsets) {
                drawList->AddText(font, fontSize, addVec(textPos, offset), outlineColor, text.c_str());
            }
            drawList->AddText(font, fontSize, addVec(textPos, ImVec2(1.5f * unit, 1.5f * unit)), ScaleToastAlpha(IM_COL32(34, 15, 54, 120), toastOpacity),
                              text.c_str());
            drawList->AddText(font, fontSize, textPos, fillColor, text.c_str());
        };

        const ImVec2 panelMax = addVec(origin, size);
        drawList->AddRectFilled(origin, panelMax, ScaleToastAlpha(IM_COL32(36, 10, 58, 244), toastOpacity), rounding,
                                ImDrawFlags_RoundCornersBottomRight);
        drawList->AddRectFilledMultiColor(origin, panelMax,
                                          ScaleToastAlpha(IM_COL32(70, 45, 113, 208), toastOpacity),
                                          ScaleToastAlpha(IM_COL32(40, 14, 64, 72), toastOpacity),
                                          ScaleToastAlpha(IM_COL32(36, 10, 58, 0), toastOpacity),
                                          ScaleToastAlpha(IM_COL32(65, 41, 107, 146), toastOpacity));
        drawList->AddRectFilledMultiColor(origin, ImVec2(panelMax.x, origin.y + 118.0f * unit),
                                          ScaleToastAlpha(IM_COL32(82, 58, 130, 88), toastOpacity),
                                          ScaleToastAlpha(IM_COL32(63, 38, 102, 18), toastOpacity),
                                          ScaleToastAlpha(IM_COL32(0, 0, 0, 0), toastOpacity),
                                          ScaleToastAlpha(IM_COL32(0, 0, 0, 0), toastOpacity));

        const float titleFontSize = fitTextSize(titleText, 66.0f * unit, 34.0f * unit, size.x - 80.0f * unit);
        drawCenteredOutlinedText(titleText, 52.0f * unit, titleFontSize,
                                 ScaleToastAlpha(IM_COL32(243, 224, 151, 255), toastOpacity),
                                 ScaleToastAlpha(IM_COL32(74, 46, 35, 225), toastOpacity), 1.7f * unit);

        const float messageFontSize = fitTextSize(messageText, 48.0f * unit, 22.0f * unit, size.x - 78.0f * unit);
        drawCenteredText(messageText, 155.0f * unit, messageFontSize, ScaleToastAlpha(IM_COL32(247, 241, 224, 255), toastOpacity));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    ImGui::Render();
    RenderImGuiWithStateProtection(true);
}

void RenderWelcomeToast(bool isFullscreen) {
    if (isFullscreen && g_configurePromptDismissedThisSession.load(std::memory_order_relaxed)) { return; }

    static bool s_prevFullscreen = false;
    static std::chrono::steady_clock::time_point s_toast2StartTime{};
    static bool s_toast2FinishedThisFullscreen = false;

    if (isFullscreen && !s_prevFullscreen) {
        s_toast2StartTime = std::chrono::steady_clock::now();
        s_toast2FinishedThisFullscreen = false;
    }
    if (!isFullscreen) {
        s_toast2FinishedThisFullscreen = false;
    }
    s_prevFullscreen = isFullscreen;

    float toastOpacity = 1.0f;
    if (isFullscreen) {
        if (s_toast2FinishedThisFullscreen) { return; }

        constexpr float kToast2HoldSeconds = 10.0f;
        constexpr float kToast2FadeSeconds = 1.5f;

        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(now - s_toast2StartTime).count();

        if (elapsed <= kToast2HoldSeconds) {
            toastOpacity = 1.0f;
        } else {
            const float t = (elapsed - kToast2HoldSeconds) / kToast2FadeSeconds;
            const float clamped = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
            toastOpacity = 1.0f - clamped;
            if (toastOpacity <= 0.0f) {
                s_toast2FinishedThisFullscreen = true;
                return;
            }
        }
    }

    if (isFullscreen) {
        RenderFullscreenWelcomeToastImGui(toastOpacity);
        return;
    }

    static GLuint s_program = 0;
    static GLuint s_vao = 0;
    static GLuint s_vbo = 0;
    static GLint s_locTexture = -1;
    static GLint s_locOpacity = -1;

    static GLuint s_toast1Texture = 0;
    static int s_toast1Width = 0, s_toast1Height = 0;

    static HGLRC s_lastCtx = NULL;
    HGLRC currentCtx = wglGetCurrentContext();
    if (currentCtx != s_lastCtx) {
        s_lastCtx = currentCtx;
        s_program = 0;
        s_vao = 0;
        s_vbo = 0;
        s_locTexture = -1;
        s_locOpacity = -1;
        s_toast1Texture = 0;
        s_toast1Width = s_toast1Height = 0;
    }

    if (s_program == 0) {
        const char* vtxSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
})";
        const char* fragSrc = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D uTexture;
uniform float uOpacity;
void main() {
    vec4 c = texture(uTexture, TexCoord);
    FragColor = vec4(c.rgb, c.a * uOpacity);
})";

        s_program = CreateShaderProgram(vtxSrc, fragSrc);
        if (s_program != 0) {
            s_locTexture = glGetUniformLocation(s_program, "uTexture");
            s_locOpacity = glGetUniformLocation(s_program, "uOpacity");
            glUseProgram(s_program);
            glUniform1i(s_locTexture, 0);
            glUseProgram(0);
        }
    }

    if (s_vao == 0) {
        glGenVertexArrays(1, &s_vao);
    }
    if (s_vbo == 0) {
        glGenBuffers(1, &s_vbo);
    }
    if (s_vao != 0 && s_vbo != 0) {
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    auto ensureToastTexture = [&](int resourceId, GLuint& outTexture, int& outW, int& outH) {
        if (outTexture != 0 && outW > 0 && outH > 0) { return; }

        stbi_set_flip_vertically_on_load_thread(0);

        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&RenderWelcomeToast, &hModule);
        if (!hModule) { return; }

        HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!hResource) { return; }
        HGLOBAL hData = LoadResource(hModule, hResource);
        if (!hData) { return; }

        DWORD dataSize = SizeofResource(hModule, hResource);
        const unsigned char* rawData = (const unsigned char*)LockResource(hData);
        if (!rawData || dataSize == 0) { return; }

        int w = 0;
        int h = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load_from_memory(rawData, (int)dataSize, &w, &h, &channels, 4);
        if (!pixels || w <= 0 || h <= 0) { return; }

        glGenTextures(1, &outTexture);
        BindTextureDirect(GL_TEXTURE_2D, outTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        BindTextureDirect(GL_TEXTURE_2D, 0);

        outW = w;
        outH = h;
        stbi_image_free(pixels);
    };

    ensureToastTexture(IDR_TOAST1_PNG, s_toast1Texture, s_toast1Width, s_toast1Height);

    GLuint texture = s_toast1Texture;
    int imgW = s_toast1Width;
    int imgH = s_toast1Height;
    if (s_program == 0 || s_vao == 0 || s_vbo == 0 || texture == 0 || imgW <= 0 || imgH <= 0) { return; }

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    int vpW = viewport[2];
    int vpH = viewport[3];
    if (vpW <= 0 || vpH <= 0) { return; }

    GLint savedProgram = 0, savedVAO = 0, savedVBO = 0, savedFBO = 0, savedTex = 0, savedActiveTex = 0;
    GLboolean savedBlend = GL_FALSE, savedDepthTest = GL_FALSE, savedScissor = GL_FALSE, savedStencil = GL_FALSE;
    GLint savedBlendSrcRGB = 0, savedBlendDstRGB = 0, savedBlendSrcA = 0, savedBlendDstA = 0;
    GLint savedViewport[4];
    GLboolean savedColorMask[4];
    GLint savedUnpackRowLength = 0, savedUnpackSkipPixels = 0, savedUnpackSkipRows = 0, savedUnpackAlignment = 0;

    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedVBO);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFBO);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
    savedBlend = glIsEnabled(GL_BLEND);
    savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
    savedScissor = glIsEnabled(GL_SCISSOR_TEST);
    savedStencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDstA);
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &savedUnpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &savedUnpackSkipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &savedUnpackSkipRows);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlignment);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    float scaleFactor = (static_cast<float>(vpH) / 1080.0f) * 0.45f;
    float drawW = (float)imgW * scaleFactor;
    float drawH = (float)imgH * scaleFactor;

    float px1 = 0.0f, py1 = 0.0f;
    float px2 = drawW, py2 = drawH;
    float nx1 = (px1 / vpW) * 2.0f - 1.0f;
    float nx2 = (px2 / vpW) * 2.0f - 1.0f;
    float ny_top = 1.0f - (py1 / vpH) * 2.0f;
    float ny_bot = 1.0f - (py2 / vpH) * 2.0f;

    float verts[] = {
        nx1, ny_bot, 0.0f, 1.0f,
        nx2, ny_bot, 1.0f, 1.0f,
        nx2, ny_top, 1.0f, 0.0f,
        nx1, ny_bot, 0.0f, 1.0f,
        nx2, ny_top, 1.0f, 0.0f,
        nx1, ny_top, 0.0f, 0.0f,
    };

    glUseProgram(s_program);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, texture);
    glUniform1f(s_locOpacity, toastOpacity);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glUseProgram(savedProgram);
    glBindVertexArray(savedVAO);
    glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
    glBindFramebuffer(GL_FRAMEBUFFER, savedFBO);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, savedTex);
    glActiveTexture(savedActiveTex);
    if (oglViewport)
        oglViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    else
        glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, savedUnpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, savedUnpackSkipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, savedUnpackSkipRows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlignment);

    if (savedBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (savedDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (savedScissor)
        glEnable(GL_SCISSOR_TEST);
    else
        glDisable(GL_SCISSOR_TEST);
    if (savedStencil)
        glEnable(GL_STENCIL_TEST);
    else
        glDisable(GL_STENCIL_TEST);
    glBlendFuncSeparate(savedBlendSrcRGB, savedBlendDstRGB, savedBlendSrcA, savedBlendDstA);
}

void RenderPerformanceOverlay(bool showPerformanceOverlay) {
    if (!showPerformanceOverlay) return;

    static auto lastOverlayUpdate = std::chrono::steady_clock::now();
    static float cachedFrameTime = 0.0f;
    static float cachedOriginalFrameTime = 0.0f;
    static int cachedObsTargetFramerate = ConfigDefaults::CONFIG_OBS_FRAMERATE;

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastOverlayUpdate);

    if (timeSinceLastUpdate.count() >= 500) {
        cachedFrameTime = static_cast<float>(g_lastFrameTimeMs.load());
        cachedOriginalFrameTime = static_cast<float>(g_originalFrameTimeMs.load());
        cachedObsTargetFramerate = GetObsTargetFramerate();
        lastOverlayUpdate = currentTime;
    }

    ImGui::SetNextWindowPos(ImVec2(5.0f, 5.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("DebugOverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Render Hook Overhead: %.2f ms", cachedFrameTime);
    ImGui::Text("Original Frame Time: %.2f ms", cachedOriginalFrameTime);
    ImGui::Text("OBS Target Framerate: %d fps", cachedObsTargetFramerate);
    ImGui::End();
}

void RenderProfilerOverlay(bool showProfiler, bool showPerformanceOverlay) {
    if (!showProfiler) return;

    auto displayData = Profiler::GetInstance().GetProfileData();

    ImGui::SetNextWindowPos(ImVec2(5.0f, showPerformanceOverlay ? 80.0f : 5.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("ProfilerOverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::SetWindowFontScale(g_config.debug.profilerScale);

    ImGui::Text("Toolscreen Profiler (Hierarchical)");
    ImGui::Separator();

    auto renderTreeSection = [](const char* sectionTitle, const std::vector<std::pair<std::string, Profiler::ProfileEntry>>& entries,
                                ImVec4 headerColor) {
        if (entries.empty()) return;

        ImGui::PushStyleColor(ImGuiCol_Text, headerColor);
        ImGui::Text("%s", sectionTitle);
        ImGui::PopStyleColor();

        if (ImGui::BeginTable("##ProfilerTable", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 280.0f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Calls/f", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Of Parent", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Of Total", ImGuiTableColumnFlags_WidthFixed, 60.0f);

            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& path = entries[i].first;
                const auto& entry = entries[i].second;
                const std::string& displayName = entry.displayName.empty() ? path : entry.displayName;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                std::string indent;
                for (int depth = 0; depth < entry.depth; ++depth) {
                    indent += "  ";
                }

                bool isLastAtDepth = true;
                for (size_t j = i + 1; j < entries.size(); ++j) {
                    if (entries[j].second.depth == entry.depth) {
                        isLastAtDepth = false;
                        break;
                    } else if (entries[j].second.depth < entry.depth) {
                        break;
                    }
                }

                if (entry.depth > 0) {
                    if (isLastAtDepth) {
                        indent += "└─ ";
                    } else {
                        indent += "├─ ";
                    }
                }

                bool isUnspecified = (displayName == "[Unspecified]");
                if (isUnspecified) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                } else if (entry.depth == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
                } else if (entry.depth == 1) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 1.0f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                }

                ImGui::Text("%s%s", indent.c_str(), displayName.c_str());
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(1);
                if (entry.rollingAverageTime >= 0.0001) {
                    ImGui::Text("%.4fms", entry.rollingAverageTime);
                } else {
                    ImGui::Text("<0.0001");
                }

                ImGui::TableSetColumnIndex(2);
                if (entry.rollingSelfTime >= 0.0001) {
                    ImGui::Text("%.4fms", entry.rollingSelfTime);
                } else {
                    ImGui::Text("<0.0001");
                }

                ImGui::TableSetColumnIndex(3);
                if (entry.rollingAverageCalls >= 100.0) {
                    ImGui::Text("%.0f", entry.rollingAverageCalls);
                } else if (entry.rollingAverageCalls >= 10.0) {
                    ImGui::Text("%.1f", entry.rollingAverageCalls);
                } else if (entry.rollingAverageCalls > 0.0) {
                    ImGui::Text("%.2f", entry.rollingAverageCalls);
                } else {
                    ImGui::Text("0");
                }

                ImGui::TableSetColumnIndex(4);
                if (entry.maxTimeInLastSecond >= 0.0001) {
                    ImGui::Text("%.4fms", entry.maxTimeInLastSecond);
                } else {
                    ImGui::Text("<0.0001");
                }

                ImGui::TableSetColumnIndex(5);
                if (entry.parentPercentage >= 1.0) {
                    ImGui::Text("%.0f%%", entry.parentPercentage);
                } else if (entry.parentPercentage >= 0.1) {
                    ImGui::Text("%.1f%%", entry.parentPercentage);
                } else {
                    ImGui::Text("<1%%");
                }

                ImGui::TableSetColumnIndex(6);
                if (entry.totalPercentage >= 1.0) {
                    ImGui::Text("%.0f%%", entry.totalPercentage);
                } else if (entry.totalPercentage >= 0.1) {
                    ImGui::Text("%.1f%%", entry.totalPercentage);
                } else {
                    ImGui::Text("<1%%");
                }
            }

            ImGui::EndTable();
        }
    };

    renderTreeSection("Render Thread", displayData.renderThread, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));

    if (!displayData.otherThreads.empty()) {
        ImGui::Separator();
        renderTreeSection("Other Threads", displayData.otherThreads, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
    }

    ImGui::End();
}

void RenderImGuiWithStateProtection(bool useFullProtection) {
    if (useFullProtection) {
        GLint last_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
        GLint last_vertex_array;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
        GLint last_array_buffer;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
        GLint last_element_buffer;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_buffer);
        GLint last_texture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
        GLint last_active_texture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
        GLboolean last_blend = glIsEnabled(GL_BLEND);
        GLint last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha;
        glGetIntegerv(GL_BLEND_SRC_RGB, &last_blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &last_blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &last_blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &last_blend_dst_alpha);
        GLint last_viewport[4];
        glGetIntegerv(GL_VIEWPORT, last_viewport);
        GLboolean last_depth_test = glIsEnabled(GL_DEPTH_TEST);
        GLboolean last_cull_face = glIsEnabled(GL_CULL_FACE);
        GLboolean last_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
        GLint last_scissor_box[4];
        glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);
        GLint last_framebuffer;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glUseProgram(last_program);
        glBindVertexArray(last_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_buffer);
        glActiveTexture(last_active_texture);
        BindTextureDirect(GL_TEXTURE_2D, last_texture);
        if (oglViewport)
            oglViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        else
            glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        glScissor(last_scissor_box[0], last_scissor_box[1], last_scissor_box[2], last_scissor_box[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

        if (last_depth_test)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (last_cull_face)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (last_scissor_test)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);

        if (last_blend) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
        } else {
            glDisable(GL_BLEND);
        }
    } else {
        GLint last_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
        GLint last_vertex_array;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
        GLint last_array_buffer;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
        GLboolean last_blend = glIsEnabled(GL_BLEND);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glUseProgram(last_program);
        glBindVertexArray(last_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
        if (last_blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }
}

void SyncImGuiDisplayMetrics(HWND hwnd) {
    if (!ImGui::GetCurrentContext()) { return; }

    int clientWidth = 0;
    int clientHeight = 0;
    if (hwnd != NULL) {
        RECT clientRect{};
        if (GetClientRect(hwnd, &clientRect)) {
            clientWidth = clientRect.right - clientRect.left;
            clientHeight = clientRect.bottom - clientRect.top;
        }
    }

    if (clientWidth <= 0 || clientHeight <= 0) {
        clientWidth = GetCachedWindowWidth();
        clientHeight = GetCachedWindowHeight();
    }

    if (clientWidth <= 0 || clientHeight <= 0) { return; }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(clientWidth), static_cast<float>(clientHeight));
}

void HandleConfigLoadFailed(HDC hDc, BOOL (*oWglSwapBuffers)(HDC)) {
    (void)hDc;
    (void)oWglSwapBuffers;

    std::lock_guard<std::recursive_mutex> imguiLock(GetImGuiContextMutex());

    if (ImGui::GetCurrentContext() == nullptr) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ConfigureImGuiFontsAndStyle(ComputeGuiScaleFactorFromCachedWindowSize() * 1.25f);
        ImGui_ImplWin32_Init(g_minecraftHwnd.load());
        ImGui_ImplOpenGL3_Init("#version 330");
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    SyncImGuiDisplayMetrics(g_minecraftHwnd.load());
    ImGui::NewFrame();

    RenderConfigErrorGUI();

    ImGui::Render();
    RenderImGuiWithStateProtection(true);
}