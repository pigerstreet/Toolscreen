#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

#include "gui/gui.h"

// Config access: Reader threads use GetConfigSnapshot() for safe, lock-free access.
// g_config is the mutable draft, only touched by the GUI/main thread.

struct MirrorInstance {
    GLuint fbo = 0;
    GLuint fboTexture = 0; // Front buffer (read by main thread for display)
    int fbo_w = 0;
    int fbo_h = 0;
    std::chrono::steady_clock::time_point lastUpdateTime{};
    int forceUpdateFrames = 0;
    std::vector<unsigned char> pixelBuffer;
    GLuint tempCaptureTexture = 0;
    int tempCaptureTextureW = 0;
    int tempCaptureTextureH = 0;

    // Double-buffering for threaded capture
    GLuint fboBack = 0;                      // Back FBO (written by capture thread)
    GLuint fboTextureBack = 0;               // Back texture (written by capture thread)
    std::atomic<bool> captureReady{ false };
    bool hasValidContent = false;

    // This prevents race conditions when rawOutput setting changes mid-frame
    bool capturedAsRawOutput = false;
    bool capturedAsRawOutputBack = false;

    // Desired rawOutput state - written by main thread, read by capture thread
    // This ensures capture thread always uses the latest value
    std::atomic<bool> desiredRawOutput{ false };

    // The capture thread renders here, the render thread just blits this
    GLuint finalFbo = 0;
    GLuint finalTexture = 0;
    GLuint finalFboBack = 0;                // Back FBO (capture thread writes)
    GLuint finalTextureBack = 0;            // Back texture (capture thread writes)
    int final_w = 0, final_h = 0;
    int final_w_back = 0, final_h_back = 0;

    bool hasFrameContent = false;     // Front buffer has content (read by render thread)
    bool hasFrameContentBack = false; // Back buffer has content (written by capture thread)

    // Cross-context GPU synchronization fences
    // These ensure the render thread waits for capture thread's GPU work to complete
    // fences work across shared contexts via glWaitSync.
    GLsync gpuFence = nullptr;     // Front buffer fence (render thread waits on this)
    GLsync gpuFenceBack = nullptr; // Back buffer fence (capture thread sets this)

    // Cached render state - computed by capture thread, used by render thread
    // This minimizes per-frame calculations on the render thread
    struct CachedMirrorRenderState {
        float outputScale = -1.0f;
        bool outputSeparateScale = false;
        float outputScaleX = 1.0f;
        float outputScaleY = 1.0f;
        int outputX = 0, outputY = 0;
        std::string outputRelativeTo;
        int gameW = 0, gameH = 0;
        int screenW = 0, screenH = 0;
        int finalX = 0, finalY = 0, finalW = 0, finalH = 0;
        int fbo_w = 0, fbo_h = 0;

        float vertices[24];
        int outW = 0, outH = 0;

        int mirrorScreenX = 0, mirrorScreenY = 0;
        int mirrorScreenW = 0, mirrorScreenH = 0;

        bool isValid = false;
    };
    CachedMirrorRenderState cachedRenderState;
    CachedMirrorRenderState cachedRenderStateBack;

    MirrorInstance() = default;

    MirrorInstance(const MirrorInstance& other)
        : fbo(other.fbo),
          fboTexture(other.fboTexture),
          fbo_w(other.fbo_w),
          fbo_h(other.fbo_h),
          lastUpdateTime(other.lastUpdateTime),
          forceUpdateFrames(other.forceUpdateFrames),
          pixelBuffer(other.pixelBuffer),
          tempCaptureTexture(other.tempCaptureTexture),
          tempCaptureTextureW(other.tempCaptureTextureW),
          tempCaptureTextureH(other.tempCaptureTextureH),
          fboBack(other.fboBack),
          fboTextureBack(other.fboTextureBack),
          captureReady(other.captureReady.load(std::memory_order_relaxed)),
          hasValidContent(other.hasValidContent),
          capturedAsRawOutput(other.capturedAsRawOutput),
          capturedAsRawOutputBack(other.capturedAsRawOutputBack),
          desiredRawOutput(other.desiredRawOutput.load(std::memory_order_relaxed)),
          finalFbo(other.finalFbo),
          finalTexture(other.finalTexture),
          finalFboBack(other.finalFboBack),
          finalTextureBack(other.finalTextureBack),
          final_w(other.final_w),
          final_h(other.final_h),
          final_w_back(other.final_w_back),
          final_h_back(other.final_h_back),
          hasFrameContent(other.hasFrameContent),
          hasFrameContentBack(other.hasFrameContentBack),
          gpuFence(nullptr),
          gpuFenceBack(nullptr), // Fences are GPU resources, don't copy
          cachedRenderState(other.cachedRenderState),
          cachedRenderStateBack(other.cachedRenderStateBack) {}

    MirrorInstance(MirrorInstance&& other) noexcept
        : fbo(other.fbo),
          fboTexture(other.fboTexture),
          fbo_w(other.fbo_w),
          fbo_h(other.fbo_h),
          lastUpdateTime(other.lastUpdateTime),
          forceUpdateFrames(other.forceUpdateFrames),
          pixelBuffer(std::move(other.pixelBuffer)),
          tempCaptureTexture(other.tempCaptureTexture),
          tempCaptureTextureW(other.tempCaptureTextureW),
          tempCaptureTextureH(other.tempCaptureTextureH),
          fboBack(other.fboBack),
          fboTextureBack(other.fboTextureBack),
          captureReady(other.captureReady.load(std::memory_order_relaxed)),
          hasValidContent(other.hasValidContent),
          capturedAsRawOutput(other.capturedAsRawOutput),
          capturedAsRawOutputBack(other.capturedAsRawOutputBack),
          desiredRawOutput(other.desiredRawOutput.load(std::memory_order_relaxed)),
          finalFbo(other.finalFbo),
          finalTexture(other.finalTexture),
          finalFboBack(other.finalFboBack),
          finalTextureBack(other.finalTextureBack),
          final_w(other.final_w),
          final_h(other.final_h),
          final_w_back(other.final_w_back),
          final_h_back(other.final_h_back),
          hasFrameContent(other.hasFrameContent),
          hasFrameContentBack(other.hasFrameContentBack),
          gpuFence(other.gpuFence),
          gpuFenceBack(other.gpuFenceBack),
          cachedRenderState(std::move(other.cachedRenderState)),
          cachedRenderStateBack(std::move(other.cachedRenderStateBack)) {
        // Transfer fence ownership
        other.gpuFence = nullptr;
        other.gpuFenceBack = nullptr;
    }

    MirrorInstance& operator=(const MirrorInstance& other) {
        if (this != &other) {
            fbo = other.fbo;
            fboTexture = other.fboTexture;
            fbo_w = other.fbo_w;
            fbo_h = other.fbo_h;
            lastUpdateTime = other.lastUpdateTime;
            forceUpdateFrames = other.forceUpdateFrames;
            pixelBuffer = other.pixelBuffer;
            tempCaptureTexture = other.tempCaptureTexture;
            tempCaptureTextureW = other.tempCaptureTextureW;
            tempCaptureTextureH = other.tempCaptureTextureH;
            fboBack = other.fboBack;
            fboTextureBack = other.fboTextureBack;
            captureReady.store(other.captureReady.load(std::memory_order_relaxed), std::memory_order_relaxed);
            hasValidContent = other.hasValidContent;
            capturedAsRawOutput = other.capturedAsRawOutput;
            capturedAsRawOutputBack = other.capturedAsRawOutputBack;
            desiredRawOutput.store(other.desiredRawOutput.load(std::memory_order_relaxed), std::memory_order_relaxed);
            finalFbo = other.finalFbo;
            finalTexture = other.finalTexture;
            finalFboBack = other.finalFboBack;
            finalTextureBack = other.finalTextureBack;
            final_w = other.final_w;
            final_h = other.final_h;
            final_w_back = other.final_w_back;
            final_h_back = other.final_h_back;
            hasFrameContent = other.hasFrameContent;
            hasFrameContentBack = other.hasFrameContentBack;
            cachedRenderState = other.cachedRenderState;
            cachedRenderStateBack = other.cachedRenderStateBack;
            // Fences are GPU resources - don't copy, just clear ours
            gpuFence = nullptr;
            gpuFenceBack = nullptr;
        }
        return *this;
    }

    MirrorInstance& operator=(MirrorInstance&& other) noexcept {
        if (this != &other) {
            fbo = other.fbo;
            fboTexture = other.fboTexture;
            fbo_w = other.fbo_w;
            fbo_h = other.fbo_h;
            lastUpdateTime = other.lastUpdateTime;
            forceUpdateFrames = other.forceUpdateFrames;
            pixelBuffer = std::move(other.pixelBuffer);
            tempCaptureTexture = other.tempCaptureTexture;
            tempCaptureTextureW = other.tempCaptureTextureW;
            tempCaptureTextureH = other.tempCaptureTextureH;
            fboBack = other.fboBack;
            fboTextureBack = other.fboTextureBack;
            captureReady.store(other.captureReady.load(std::memory_order_relaxed), std::memory_order_relaxed);
            hasValidContent = other.hasValidContent;
            capturedAsRawOutput = other.capturedAsRawOutput;
            capturedAsRawOutputBack = other.capturedAsRawOutputBack;
            desiredRawOutput.store(other.desiredRawOutput.load(std::memory_order_relaxed), std::memory_order_relaxed);
            finalFbo = other.finalFbo;
            finalTexture = other.finalTexture;
            finalFboBack = other.finalFboBack;
            finalTextureBack = other.finalTextureBack;
            final_w = other.final_w;
            final_h = other.final_h;
            final_w_back = other.final_w_back;
            final_h_back = other.final_h_back;
            hasFrameContent = other.hasFrameContent;
            hasFrameContentBack = other.hasFrameContentBack;
            cachedRenderState = std::move(other.cachedRenderState);
            cachedRenderStateBack = std::move(other.cachedRenderStateBack);
            // Transfer fence ownership
            gpuFence = other.gpuFence;
            gpuFenceBack = other.gpuFenceBack;
            other.gpuFence = nullptr;
            other.gpuFenceBack = nullptr;
        }
        return *this;
    }
};

struct UserImageInstance {
    GLuint textureId = 0;
    int width = 0;
    int height = 0;
    bool isFullyTransparent = false;

    // Render-thread-only texture sampling state cache.
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;

    bool isAnimated = false;
    std::vector<GLuint> frameTextures;
    std::vector<int> frameDelays;
    size_t currentFrame = 0;
    std::chrono::steady_clock::time_point lastFrameTime;

    struct CachedImageRenderState {
        int crop_left = -1;
        int crop_right = -1;
        int crop_top = -1;
        int crop_bottom = -1;
        float scale = -1.0f;
        int x = 0;
        int y = 0;
        std::string relativeTo;
        int screenWidth = 0;
        int screenHeight = 0;

        int displayW = 0;
        int displayH = 0;
        float tx1 = 0, ty1 = 0, tx2 = 0, ty2 = 0;
        float nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;

        bool isValid = false;
    } cachedRenderState;
};

extern std::ofstream logFile;
extern std::mutex g_logFileMutex;
extern std::wstring g_toolscreenPath;
extern std::wstring g_modeFilePath;
extern std::wstring g_stateFilePath;
extern std::atomic<bool> g_isStateOutputAvailable;
extern std::atomic<bool> g_stopMonitoring;
extern std::atomic<bool> g_stopImageMonitoring;
extern std::atomic<bool> g_isShuttingDown;
extern Config g_config;

extern std::atomic<bool> g_allImagesLoaded;
extern std::mutex g_decodedImagesMutex;
extern std::vector<DecodedImageData> g_decodedImagesQueue;
extern std::atomic<HWND> g_minecraftHwnd;
extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;
extern std::mutex g_hotkeyMainKeysMutex;
extern std::atomic<HCURSOR> g_specialCursorHandle;

void Log(const std::string& message);
void Log(const std::wstring& message);
void APIENTRY BindTextureDirect(GLenum target, GLuint texture);
void InvalidateTrackedGameTextureId(bool clearSwapThread = false);

void StartLogThread(); // Start background log writer thread
void StopLogThread();  // Stop background log writer thread (flushes first)
void FlushLogs();

void LogCategory(const char* category, const std::string& message);

std::wstring Utf8ToWide(const std::string& utf8_string);
std::string WideToUtf8(const std::wstring& wstr);
std::wstring GetToolscreenPath();

bool CompressFileToGzip(const std::wstring& srcPath, const std::wstring& dstPath);

inline std::string GetKeyComboString(const std::vector<DWORD>& keys) {
    std::string keyStr;
    for (size_t k = 0; k < keys.size(); ++k) {
        keyStr += VkToString(keys[k]);
        if (k < keys.size() - 1) keyStr += "+";
    }
    return keyStr;
}

struct ModeViewportInfo {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;
    bool stretchEnabled = false;
};

bool GetMonitorRectForWindow(HWND hwnd, RECT& outRect);
bool GetMonitorSizeForWindow(HWND hwnd, int& outW, int& outH);
bool GetWindowClientRectInScreen(HWND hwnd, RECT& outRect);

UINT GetToolscreenBorderlessToggleMessageId();
bool RequestWindowClientResize(HWND hwnd, int width, int height, const char* source = nullptr);
bool GetRecentRequestedWindowClientResizes(int& outCurrentW, int& outCurrentH, int& outPreviousW, int& outPreviousH);
bool CenterWindowedRestoreOnCurrentMonitor(HWND hwnd, const char* source = nullptr);
void ToggleBorderlessWindowedFullscreen(HWND hwnd);
bool IsCursorVisible();
void WriteCurrentModeToFile(const std::string& modeId);
bool SwitchToMode(const std::string& newModeId, const std::string& source = "", bool forceCut = false);
bool IsHardcodedMode(const std::string& modeId);
bool EqualsIgnoreCase(const std::string& a, const std::string& b);
const ModeConfig* GetMode(const std::string& id);
const ModeConfig* GetMode_Internal(const std::string& id);
ModeConfig* GetModeMutable(const std::string& id);
MirrorConfig* GetMutableMirror(const std::string& name);

const ModeConfig* GetModeFromSnapshot(const Config& config, const std::string& id);
const MirrorConfig* GetMirrorFromSnapshot(const Config& config, const std::string& name);
bool isWallTitleOrWaiting(const std::string& state);
ModeViewportInfo GetCurrentModeViewport();
ModeViewportInfo GetCurrentModeViewport_Internal(); // Lock-free implementation using double-buffered mode ID

GLuint CompileShader(GLenum type, const char* source);
GLuint CreateShaderProgram(const char* vert, const char* frag);

void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath);
void LoadAllImages();

bool CheckHotkeyMatch(const std::vector<DWORD>& keys, WPARAM wParam, const std::vector<DWORD>& exclusionKeys = {},
                      bool triggerOnRelease = false, size_t minKeyCount = 0);

std::string FindHotkeyConflict(const std::vector<DWORD>& newKeys, const std::string& excludeLabel);

void BackupConfigFile();

void GetRelativeCoords(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX, int& outY);
void GetRelativeCoordsForImage(const std::string& type, int relX, int relY, int w, int h, int containerW, int containerH, int& outX,
                               int& outY);
void GetRelativeCoordsForImageWithViewport(const std::string& type, int relX, int relY, int w, int h, int gameX, int gameY, int gameW,
                                           int gameH, int fullW, int fullH, int& outX, int& outY);

inline bool IsViewportRelativeAnchor(const std::string& relativeTo) {
    if (relativeTo.length() > 8 && relativeTo.substr(relativeTo.length() - 8) == "Viewport") { return true; }
    return false;
}
void CalculateFinalScreenPos(const MirrorConfig* conf, const MirrorInstance& inst, int gameW, int gameH, int finalX, int finalY, int finalW,
                             int finalH, int fullW, int fullH, int& outScreenX, int& outScreenY);

void ScreenshotToClipboard(int width, int height);

DWORD WINAPI FileMonitorThread(LPVOID lpParam);
DWORD WINAPI ImageMonitorThread(LPVOID lpParam);

class SE_Exception : public std::exception {
  public:
    SE_Exception(unsigned int code, EXCEPTION_POINTERS* info) : m_code(code), m_info(info) {}

    unsigned int getCode() const { return m_code; }
    EXCEPTION_POINTERS* getInfo() const { return m_info; }

    const char* what() const noexcept override {
        static char buffer[128];
        snprintf(buffer, sizeof(buffer), "Structured Exception: 0x%08X", m_code);
        return buffer;
    }

  private:
    unsigned int m_code;
    EXCEPTION_POINTERS* m_info;
};

void LogException(const std::string& context, const std::exception& e);
void LogException(const std::string& context, DWORD exceptionCode, EXCEPTION_POINTERS* exceptionInfo = nullptr);
void InstallGlobalExceptionHandlers();
LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionInfo);
void SEHTranslator(unsigned int code, EXCEPTION_POINTERS* info);