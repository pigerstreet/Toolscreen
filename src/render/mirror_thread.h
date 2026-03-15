#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// Need gui.h for Color and MirrorCaptureConfig used as value types in ThreadedMirrorConfig
#include "gui/gui.h"

struct MirrorInstance;

// Mirror capture resources and same-thread mirror compositing support.
extern std::atomic<int> g_activeMirrorCaptureCount;

// Maximum requested FPS among active mirrors (summary of ThreadedMirrorConfig::fps).
extern std::atomic<int> g_activeMirrorCaptureMaxFps;

constexpr int kMirrorRealtimeSliderValue = 500;
constexpr int kMirrorRealtimeFps = 10000;

inline bool IsMirrorRealtimeFps(int fps) {
    return fps >= kMirrorRealtimeFps;
}

inline bool MirrorUsesEveryFrameUpdates(int fps) {
    return fps <= 0 || IsMirrorRealtimeFps(fps);
}

// Named ThreadedMirrorConfig to avoid conflict with MirrorCaptureConfig in gui.h
struct ThreadedMirrorConfig {
    std::string name;
    int captureWidth = 0;
    int captureHeight = 0;

    MirrorBorderType borderType = MirrorBorderType::Dynamic;
    int dynamicBorderThickness = 0;
    MirrorBorderShape staticBorderShape = MirrorBorderShape::Rectangle;
    Color staticBorderColor = { 1.0f, 1.0f, 1.0f };
    int staticBorderThickness = 2;
    int staticBorderRadius = 0;
    int staticBorderOffsetX = 0;
    int staticBorderOffsetY = 0;
    int staticBorderWidth = 0;
    int staticBorderHeight = 0;

    int fps = 0;
    bool rawOutput = false;
    bool colorPassthrough = false;
    std::vector<Color> targetColors;
    Color outputColor;
    Color borderColor;
    float colorSensitivity = 0.0f;
    std::vector<MirrorCaptureConfig> input;
    uint64_t sourceRectLayoutHash = 0;
    std::chrono::steady_clock::time_point lastCaptureTime;

    float outputScale = 1.0f;
    bool outputSeparateScale = false;
    float outputScaleX = 1.0f;
    float outputScaleY = 1.0f;
    int outputX = 0, outputY = 0;
    std::string outputRelativeTo;
};

// External access to threaded mirror configs (protected by mutex)
extern std::vector<ThreadedMirrorConfig> g_threadedMirrorConfigs;
extern std::mutex g_threadedMirrorConfigMutex;

// Call this from the current GL render path each frame.
void SwapMirrorBuffers();

// Render active mirror captures on the current GL thread using the mirror resources.
// Returns true when at least one mirror produced a fresh front buffer during this call.
bool RenderMirrorCapturesOnCurrentThread(const std::vector<ThreadedMirrorConfig>& activeMirrorConfigs, GLuint sourceTexture, int gameW,
                                         int gameH, int screenW, int screenH, int finalX, int finalY, int finalW, int finalH);

void BuildThreadedMirrorConfigs(const std::vector<MirrorConfig>& activeMirrors, std::vector<ThreadedMirrorConfig>& outConfigs);

// Update capture configs from main thread (call when active mirrors change)
void UpdateMirrorCaptureConfigs(const std::vector<MirrorConfig>& activeMirrors);

void UpdateMirrorFPS(const std::string& mirrorName, int fps);

void UpdateMirrorOutputPosition(const std::string& mirrorName, int x, int y, float scale, bool separateScale, float scaleX, float scaleY,
                                const std::string& relativeTo);

void UpdateMirrorGroupOutputPosition(const std::vector<std::string>& mirrorIds, int x, int y, float scale, bool separateScale, float scaleX,
                                     float scaleY, const std::string& relativeTo);

void UpdateMirrorInputRegions(const std::string& mirrorName, const std::vector<MirrorCaptureConfig>& inputRegions);

void UpdateMirrorCaptureSettings(const std::string& mirrorName, int captureWidth, int captureHeight, const MirrorBorderConfig& border,
                                 const MirrorColors& colors, float colorSensitivity, bool rawOutput, bool colorPassthrough);

// Invalidate cached mirror textures/state for mirrors that are no longer active in the current mode.
void InvalidateMirrorTextureCaches(const std::vector<std::string>& mirrorNames);

void SetGlobalMirrorGammaMode(MirrorGammaMode mode);
MirrorGammaMode GetGlobalMirrorGammaMode();

void InitCaptureTexture(int width, int height);
void EnsureCaptureTextureInitialized(int width, int height);
void CleanupCaptureTexture();

// Copy the current game texture into the shared mirror capture textures.
void SubmitFrameCapture(GLuint gameTexture, int width, int height);

// These provide access to the copied game texture for OBS and other synchronous consumers.
GLuint GetGameCopyTexture();

// --- Ready Frame Accessors ---
// These return the most recent completed copy and are safe to sample immediately.
GLuint GetReadyGameTexture();
int GetReadyGameWidth();
int GetReadyGameHeight();

// --- Fallback Frame Accessors ---
// These expose the latest copy metadata even if the ready frame was invalidated during resize.
GLuint GetFallbackGameTexture();
int GetFallbackGameWidth();
int GetFallbackGameHeight();

// No fence wait needed - this is a simple and reliable fallback
GLuint GetSafeReadTexture();

// Note: OBS capture is now handled by obs_thread.h/cpp via glBlitFramebuffer hook


