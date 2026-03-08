#pragma once


#include "fake_cursor.h"
#include "gui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"
#include "profiler.h"
#include "render.h"
#include "stb_image.h"
#include "utils.h"
#include "window_overlay.h"
#include <GL/glew.h>
#include <Shlwapi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <shared_mutex>
#include <string>
#include <thread>


void HelpMarker(const char* desc);

bool Spinner(const char* id_label, int* v, int step = 1, int min_val = INT_MIN, int max_val = INT_MAX, float inputWidth = 80.0f,
             float margin = 0.0f);

bool SpinnerFloat(const char* id_label, float* v, float step = 0.1f, float min_val = 0.0f, float max_val = FLT_MAX,
                  const char* format = "%.1f");


inline const std::vector<std::pair<const char*, const char*>>& GetRelativeToOptions() {
    static const std::vector<std::pair<const char*, const char*>> options = {
                                                                                {"topLeftViewport", trc("position.top_left_viewport")},
                                                                                {"topRightViewport", trc("position.top_right_viewport")},
                                                                                {"bottomLeftViewport", trc("position.bottom_left_viewport")},
                                                                                {"bottomRightViewport", trc("position.bottom_right_viewport")},
                                                                                {"centerViewport", trc("position.center_viewport")},
                                                                                {"pieLeft", trc("position.pie_left")},
                                                                                {"pieRight", trc("position.pie_right")},
                                                                                {"topLeftScreen", trc("position.top_left_screen")},
                                                                                {"topRightScreen", trc("position.top_right_screen")},
                                                                                {"bottomLeftScreen", trc("position.bottom_left_screen")},
                                                                                {"bottomRightScreen", trc("position.bottom_right_screen")},
                                                                                {"centerScreen", trc("position.center_screen")}
                                                                            };
    return options;
}

inline const std::vector<std::pair<const char*, const char*>>& GetImageRelativeToOptions() {
    static const std::vector<std::pair<const char*, const char*>> options = { { "topLeft", trc("position.top_left") },
                                                                              { "topRight", trc("position.top_right") },
                                                                              { "bottomLeft", trc("position.bottom_left") },
                                                                              { "bottomRight", trc("position.bottom_right") },
                                                                              { "center", trc("position.center") } };
    return options;
}

inline const char* GetFriendlyName(const std::string& key, const std::vector<std::pair<const char*, const char*>>& options) {
    for (const auto& option : options) {
        if (key == option.first) return option.second;
    }
    return "Unknown";
}

inline const std::vector<const char*>& GetValidGameStates() {
    static const std::vector<const char*> states = { "wall",  "inworld,cursor_free", "inworld,cursor_grabbed",
                                                     "title", "waiting",             "generating" };
    return states;
}

inline const std::vector<const char*>& GetGuiGameStates() {
    static const std::vector<const char*> states = { "wall",  "inworld,cursor_free", "inworld,cursor_grabbed",
                                                     "title", "generating" };
    return states;
}

inline const std::vector<std::pair<const char*, const char*>>& GetGameStateDisplayNames() {
    static const std::vector<std::pair<const char*, const char*>> names = { { "wall", "Wall Screen" },
                                                                            { "inworld,cursor_free", "In World (Cursor Free)" },
                                                                            { "inworld,cursor_grabbed", "In World (Cursor Grabbed)" },
                                                                            { "title", "Title Screen" },
                                                                            { "waiting", "Waiting Screen" },
                                                                            { "generating", "World Generation" } };
    return names;
}

inline const char* GetGameStateFriendlyName(const std::string& gameState) {
    for (const auto& pair : GetGameStateDisplayNames()) {
        if (gameState == pair.first) return pair.second;
    }
    return gameState.c_str();
}


struct ImagePickerResult {
    bool completed = false;
    bool success = false;
    std::string path;
    std::string error;
};

std::string ValidateImageFile(const std::string& path, const std::wstring& toolscreenPath);
ImagePickerResult OpenImagePickerAndValidate(HWND ownerHwnd, const std::wstring& initialDir, const std::wstring& toolscreenPath);
void ClearExpiredImageErrors();
void SetImageError(const std::string& key, const std::string& error);
std::string GetImageError(const std::string& key);
void ClearImageError(const std::string& key);

void StartAsyncImagePicker(const std::string& pickerId, const std::wstring& initialDir);
bool CheckAsyncImagePicker(const std::string& pickerId, std::string& outPath, std::string& outError);


bool HasDuplicateModeName(const std::string& name, size_t currentIndex);
bool HasDuplicateMirrorName(const std::string& name, size_t currentIndex);
bool HasDuplicateImageName(const std::string& name, size_t currentIndex);
bool HasDuplicateWindowOverlayName(const std::string& name, size_t currentIndex);
bool HasDuplicateEyeZoomOverlayName(const std::string& name, size_t currentIndex);


void RenderTransitionSettingsHorizontalNoBackground(ModeConfig& mode, const std::string& idSuffix);
void RenderTransitionSettingsHorizontal(ModeConfig& mode, const std::string& idSuffix);


std::vector<ModeConfig> GetDefaultModes();
std::vector<MirrorConfig> GetDefaultMirrors();
std::vector<ImageConfig> GetDefaultImages();
std::vector<WindowOverlayConfig> GetDefaultWindowOverlays();
std::vector<HotkeyConfig> GetDefaultHotkeys();
CursorsConfig GetDefaultCursors();
EyeZoomConfig GetDefaultEyeZoomConfig();


struct ExclusionBindState {
    int hotkey_idx = -1;
    int exclusion_idx = -1;
};

struct AltBindState {
    int hotkey_idx = -1;
    int alt_idx = -1;
};

extern int s_mainHotkeyToBind;
extern int s_sensHotkeyToBind;
extern ExclusionBindState s_exclusionToBind;
extern AltBindState s_altHotkeyToBind;


void RenderModesTab();
void RenderMirrorsTab();
void RenderImagesTab();
void RenderWindowOverlaysTab();
void RenderHotkeysTab();
void RenderMouseTab();
void RenderSettingsTab();
void RenderRebindsTab();


