#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

// Forward declarations - render thread looks up configs directly from g_config
struct ModeConfig;
struct MirrorConfig;
struct ImageConfig;
struct GLState;
struct GameViewportGeometry;

constexpr int RENDER_THREAD_FBO_COUNT = 3;

// Lightweight struct - render thread looks up active elements from g_config directly
struct FrameRenderRequest {
    uint64_t frameNumber = 0;

    int fullW = 0;
    int fullH = 0;

    int gameW = 0;
    int gameH = 0;
    int finalX = 0;
    int finalY = 0;
    int finalW = 0;
    int finalH = 0;

    GLuint gameTextureId = 0;

    // Mode ID - render thread looks up ModeConfig and collects active elements
    std::string modeId;

    bool isAnimating = false;
    float overlayOpacity = 1.0f;

    bool obsDetected = false;
    bool excludeOnlyOnMyScreen = false;
    bool skipAnimation = false;
    bool relativeStretching = false;

    float transitionProgress = 1.0f;
    int fromX = 0;
    int fromY = 0;
    int fromW = 0;
    int fromH = 0;
    int toX = 0;
    int toY = 0;
    int toW = 0;
    int toH = 0;

    // Render thread will render game at animatedX/Y/W/H position and include overlays
    bool isObsPass = false;

    int animatedX = 0;
    int animatedY = 0;
    int animatedW = 0;
    int animatedH = 0;

    float bgR = 0.0f;
    float bgG = 0.0f;
    float bgB = 0.0f;

    // These allow background and border to be rendered on the render thread
    bool backgroundIsImage = false;

    bool borderEnabled = false;
    float borderR = 1.0f;
    float borderG = 1.0f;
    float borderB = 1.0f;
    int borderWidth = 0;
    int borderRadius = 0;

    bool transitioningToFullscreen = false;
    bool fromBackgroundIsImage = false;
    float fromBgR = 0.0f;
    float fromBgG = 0.0f;
    float fromBgB = 0.0f;
    bool fromBorderEnabled = false;
    float fromBorderR = 1.0f;
    float fromBorderG = 1.0f;
    float fromBorderB = 1.0f;
    int fromBorderWidth = 0;
    int fromBorderRadius = 0;
    std::string fromModeId;

    bool fromSlideMirrorsIn = false;
    bool toSlideMirrorsIn = false;
    float mirrorSlideProgress = 1.0f;

    int letterboxExtendX = 0;
    int letterboxExtendY = 0;

    // GPU fence for game texture synchronization (OBS pass)
    // The main thread creates this fence after the game finishes rendering.
    // The render thread waits on this to ensure game is done before reading.
    // This is separate from the mirror thread's blit fence.
    GLsync gameTextureFence = nullptr;

    // These control whether to render GUI elements on the render thread
    bool shouldRenderGui = false;
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    bool showEyeZoom = false;
    float eyeZoomFadeOpacity = 1.0f;
    int eyeZoomAnimatedViewportX = -1;
    bool isTransitioningFromEyeZoom = false;
    GLuint eyeZoomSnapshotTexture = 0;
    int eyeZoomSnapshotWidth = 0;
    int eyeZoomSnapshotHeight = 0;
    bool showTextureGrid = false;
    int textureGridModeWidth = 0;
    int textureGridModeHeight = 0;

    bool showWelcomeToast = false;
    bool welcomeToastIsFullscreen = false;

    bool isWindowed = false;
    int windowW = 0;
    int windowH = 0;
    bool isPre113Windowed = false;
    bool isRawWindowedMode = false;
};

extern std::atomic<bool> g_renderThreadRunning;
extern std::atomic<uint64_t> g_renderFrameNumber;
extern std::atomic<bool> g_eyeZoomFontNeedsReload;

// Start the render thread (call from main thread after GL context is available)
void StartRenderThread(void* gameGLContext);

// Stop the render thread (call before DLL unload)
void StopRenderThread();

void SubmitFrameForRendering(const FrameRenderRequest& request);

// Wait for the render thread to complete a frame
int WaitForRenderComplete(int timeoutMs = 16);

GLuint GetCompletedRenderTexture();

// Get the fence associated with the completed render texture
// The main thread should wait on this fence before reading the texture
// Returns nullptr if no fence is available
// The caller must NOT delete this fence; it is managed by the render thread.
GLsync GetCompletedRenderFence();

// === Cross-context producer/consumer safety ===
// The render thread renders into a ring of FBO textures. The main thread samples one of those
// textures during the final composite. If the render thread laps the main thread (e.g. scheduler
// jitter / very high FPS), it can start writing into a texture that the main thread is still
// To prevent that, the main thread can publish a GLsync fence after it finishes sampling a
// completed texture; the render thread waits on that fence before reusing the corresponding FBO.
struct CompletedRenderFrame {
    GLuint texture = 0;
    GLsync fence = nullptr; // Fence signaling render-thread completion of this texture
    int fboIndex = -1;      // Which internal render-thread FBO owns `texture` (-1 if unknown)
};

CompletedRenderFrame GetCompletedRenderFrame();

// Main thread: publish a fence that signals when it has finished sampling the completed texture.
// Render thread: waits on this before reusing that FBO as a render target.
void SubmitRenderFBOConsumerFence(int fboIndex, GLsync consumerFence);

GLuint GetCompletedObsTexture();

// Get the fence associated with the completed OBS render texture
// The caller should wait on this fence before reading the texture
// Returns nullptr if no fence is available
// The caller must NOT delete this fence; it is managed by the render thread.
GLsync GetCompletedObsFence();

// Collect current internal texture IDs that should never be selected by game-texture calibration.
// Thread-safe snapshot: IDs are published through atomics.
void GetRenderThreadCalibrationExcludeTextureIds(std::vector<GLuint>& outTextureIds);

struct ObsFrameContext {
    int fullW = 0, fullH = 0;
    int gameW = 0, gameH = 0;
    GLuint gameTextureId = 0;
    std::string modeId;
    bool relativeStretching = false;
    float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f;

    bool isWindowed = false;
    bool isRawWindowedMode = false;
    int windowW = 0;
    int windowH = 0;

    bool shouldRenderGui = false;
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    bool isEyeZoom = false;
    bool isTransitioningFromEyeZoom = false;
    int eyeZoomAnimatedViewportX = 0;
    GLuint eyeZoomSnapshotTexture = 0;
    int eyeZoomSnapshotWidth = 0;
    int eyeZoomSnapshotHeight = 0;
    bool showTextureGrid = false;

    bool showWelcomeToast = false;
    bool welcomeToastIsFullscreen = false;
};

// Lightweight OBS submission struct - avoids building full FrameRenderRequest on main thread
// The render thread will call BuildObsFrameRequest with this context
struct ObsFrameSubmission {
    ObsFrameContext context;
    GLsync gameTextureFence = nullptr;
    bool isDualRenderingPath = false;
};

// Lightweight OBS submission - defers BuildObsFrameRequest to render thread
// This is more efficient as it avoids lock-free reads and struct building on main thread
void SubmitObsFrameContext(const ObsFrameSubmission& submission);

FrameRenderRequest BuildObsFrameRequest(const ObsFrameContext& ctx, bool isDualRenderingPath);


