#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// Need gui.h for Color and MirrorCaptureConfig used as value types in ThreadedMirrorConfig
#include "gui/gui.h"

struct MirrorInstance;

// Thread runs independently, capturing game content to back-buffer FBOs

// Is the mirror capture thread currently running
extern std::atomic<bool> g_mirrorCaptureRunning;

// Capture thread only captures while this is true - if it becomes false, capture is aborted
extern std::atomic<bool> g_safeToCapture;

// When enabled, SwapBuffers consumes the copied game texture directly for the on-screen mirror path.
// SubmitFrameCapture publishes the ready frame immediately and skips mirror-thread queue submission.
extern std::atomic<bool> g_sameThreadMirrorPipelineActive;

// Updated by UpdateMirrorCaptureConfigs() (logic thread) and read by SwapBuffers hook to
extern std::atomic<int> g_activeMirrorCaptureCount;

// Maximum requested FPS among active mirrors (summary of ThreadedMirrorConfig::fps).
extern std::atomic<int> g_activeMirrorCaptureMaxFps;

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

// Game state for capture thread (main thread writes, capture thread reads)
extern std::atomic<int> g_captureGameW;
extern std::atomic<int> g_captureGameH;
extern std::atomic<GLuint> g_captureGameTexture;

// Screen/viewport geometry for render cache computation (main thread writes, capture thread reads)
extern std::atomic<int> g_captureScreenW;
extern std::atomic<int> g_captureScreenH;
extern std::atomic<int> g_captureFinalX;
extern std::atomic<int> g_captureFinalY;
extern std::atomic<int> g_captureFinalW;
extern std::atomic<int> g_captureFinalH;

// Frame capture notification - sent from SwapBuffers to mirror thread
// SwapBuffers only creates fence - mirror thread does the actual GPU blit
struct FrameCaptureNotification {
    GLuint gameTextureId; // Game texture to copy from (mirror thread does the blit)
    GLsync fence;         // Fence to wait on before reading game texture
    int width;
    int height;
    int textureIndex; // Which copy texture (0 or 1) this notification refers to - fixes race condition
};

// Lock-free SPSC (Single Producer Single Consumer) ring buffer for capture notifications
// This allows the render thread to push without any locking
constexpr int CAPTURE_QUEUE_SIZE = 2; // Only need 1 pending frame (size must be power of 2)
extern FrameCaptureNotification g_captureQueue[CAPTURE_QUEUE_SIZE];
extern std::atomic<int> g_captureQueueHead; // Write index (render thread only)
extern std::atomic<int> g_captureQueueTail; // Read index (capture thread only)

// Lock-free queue operations (inline for performance)
inline bool CaptureQueuePush(const FrameCaptureNotification& notif) {
    int head = g_captureQueueHead.load(std::memory_order_relaxed);
    int nextHead = (head + 1) % CAPTURE_QUEUE_SIZE;

    if (nextHead == g_captureQueueTail.load(std::memory_order_acquire)) {
        return false;
    }

    g_captureQueue[head] = notif;
    g_captureQueueHead.store(nextHead, std::memory_order_release);
    return true;
}

inline bool CaptureQueuePop(FrameCaptureNotification& notif) {
    int tail = g_captureQueueTail.load(std::memory_order_relaxed);

    if (tail == g_captureQueueHead.load(std::memory_order_acquire)) {
        return false;
    }

    notif = g_captureQueue[tail];
    g_captureQueueTail.store((tail + 1) % CAPTURE_QUEUE_SIZE, std::memory_order_release);
    return true;
}

// Start the mirror capture thread (call from main thread after GPU init)
// MUST be called from main thread where game context is current
void StartMirrorCaptureThread(void* gameGLContext);

// Stop the mirror capture thread
void StopMirrorCaptureThread();

// Call this from main render thread each frame
void SwapMirrorBuffers();

// Render active mirror captures on the current GL thread using the shared mirror resources.
// Returns true when at least one mirror produced a fresh front buffer during this call.
bool RenderMirrorCapturesOnCurrentThread(const std::vector<MirrorConfig>& activeMirrors, GLuint sourceTexture, int gameW, int gameH,
                                         int screenW, int screenH, int finalX, int finalY, int finalW, int finalH);

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

// Start async GPU blit to copy game texture (called from SwapBuffers, non-blocking)
// The GPU executes the blit in background. Consumers call GetGameCopyTexture/Fence to access.
void SubmitFrameCapture(GLuint gameTexture, int width, int height);

// These provide access to the copied game texture for render_thread/OBS to use
// The copy is made by mirror thread (deferred from SwapBuffers)
GLuint GetGameCopyTexture();

// --- Ready Frame Accessors (for OBS render thread) ---
// These return GUARANTEED COMPLETE frames - GPU fence has signaled, safe to read without waiting
// Updated by mirror thread after fence signals, read by OBS without any fence wait
GLuint GetReadyGameTexture();
int GetReadyGameWidth();
int GetReadyGameHeight();

// --- Fallback Frame Accessors (for render_thread when ready frame not available) ---
// These return the last copy texture info, but require fence wait before use
GLuint GetFallbackGameTexture();
int GetFallbackGameWidth();
int GetFallbackGameHeight();
GLsync GetFallbackCopyFence();   // Fence to wait on before using fallback texture

// No fence wait needed - this is a simple and reliable fallback
GLuint GetSafeReadTexture();

// Note: OBS capture is now handled by obs_thread.h/cpp via glBlitFramebuffer hook


