#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <windows.h>

#include "gui/gui.h"
#include "mirror_thread.h"

#ifdef _DEBUG
#define GL_CALL(call)                                                                                                                      \
    do {                                                                                                                                   \
        call;                                                                                                                              \
        GLenum gl_err = glGetError();                                                                                                      \
        if (gl_err != GL_NO_ERROR) {                                                                                                       \
            Log("OpenGL Error 0x" + std::to_string(gl_err) + " in " + #call + " at " + __FILE__ + ":" + std::to_string(__LINE__));         \
        }                                                                                                                                  \
    } while (0)
#else
#define GL_CALL(call) call
#endif

struct MirrorInstance;
struct UserImageInstance;

// Cached mirror render data to minimize lock contention
// All border rendering is now done by mirror_thread - render_thread just blits finalTexture
struct MirrorRenderData {
    GLuint texture;
    int tex_w, tex_h;
    const MirrorConfig* config;
    // Pre-computed from render cache (populated by capture thread)
    float vertices[24];
    int outW, outH;
    bool cacheValid;
    // GPU fence for cross-context synchronization - copied from instance during lock
    GLsync gpuFence;
    int screenX = 0;
    int screenY = 0;
    int screenW = 0;
    int screenH = 0;
    bool hasFrameContent = false;
};

struct FilterShaderLocs {
    GLint screenTexture, targetColor, outputColor, sensitivity, sourceRect;
};

struct RenderShaderLocs {
    GLint filterTexture, borderWidth, outputColor, borderColor, screenPixel;
};

struct BackgroundShaderLocs {
    GLint backgroundTexture;
    GLint opacity;
};

struct SolidColorShaderLocs {
    GLint color;
};

struct ImageRenderShaderLocs {
    GLint imageTexture;
    GLint enableColorKey;
    GLint numColorKeys;
    GLint colorKeys;
    GLint sensitivities;
    GLint opacity;
};

struct PassthroughShaderLocs {
    GLint screenTexture, sourceRect, opacity;
};

#define MAX_GRADIENT_STOPS 8
struct GradientShaderLocs {
    GLint numStops;
    GLint stopColors;
    GLint stopPositions;
    GLint angle;
    GLint time;
    GLint animationType;
    GLint animationSpeed;
    GLint colorFade;
};

struct GLState {
    GLint p;
    GLint t;
    GLint t0;
    GLint ab;
    GLint va;
    GLint fb;
    GLint read_fb, draw_fb;
    GLint at;

    GLboolean be;
    GLboolean de;
    GLboolean sc;
    GLboolean ce;
    GLboolean ste;
    GLboolean srgb_enabled;
    GLboolean depth_mask;

    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;
    GLint draw_buffer;
    GLint read_buffer;

    GLint vp[4];
    GLint sb[4];

    GLfloat cc[4];
    GLfloat lw;
    GLboolean color_mask[4];
    GLint unpack_row_length;
    GLint unpack_skip_pixels;
    GLint unpack_skip_rows;
    GLint pack_alignment;
    GLint unpack_alignment;
};

extern GLuint g_filterProgram;
extern GLuint g_renderProgram;
extern GLuint g_backgroundProgram;
extern GLuint g_solidColorProgram;
extern GLuint g_imageRenderProgram;
extern GLuint g_passthroughProgram;
extern GLuint g_gradientProgram;

extern FilterShaderLocs g_filterShaderLocs;
extern RenderShaderLocs g_renderShaderLocs;
extern BackgroundShaderLocs g_backgroundShaderLocs;
extern SolidColorShaderLocs g_solidColorShaderLocs;
extern ImageRenderShaderLocs g_imageRenderShaderLocs;
extern PassthroughShaderLocs g_passthroughShaderLocs;
extern GradientShaderLocs g_gradientShaderLocs;

// --- Global GUI State for Render Thread ---
// These atomics are set by main thread and read by render.cpp to populate FrameRenderRequest
extern std::atomic<bool> g_shouldRenderGui;
extern std::atomic<bool> g_showPerformanceOverlay;
extern std::atomic<bool> g_showProfiler;
extern std::atomic<bool> g_showEyeZoom;
extern std::atomic<float> g_eyeZoomFadeOpacity;
extern std::atomic<int> g_eyeZoomAnimatedViewportX;
extern std::atomic<bool> g_isTransitioningFromEyeZoom;
extern std::atomic<bool> g_showTextureGrid;
extern std::atomic<int> g_textureGridModeWidth;
extern std::atomic<int> g_textureGridModeHeight;

// Used by dllmain.cpp to pass snapshot texture to OBS render thread
GLuint GetEyeZoomSnapshotTexture();
int GetEyeZoomSnapshotWidth();
int GetEyeZoomSnapshotHeight();

extern std::unordered_map<std::string, MirrorInstance> g_mirrorInstances;

struct BackgroundTextureInstance {
    GLuint textureId = 0;

    bool isAnimated = false;
    std::vector<GLuint> frameTextures;
    std::vector<int> frameDelays;
    size_t currentFrame = 0;
    std::chrono::steady_clock::time_point lastFrameTime;
};

extern std::unordered_map<std::string, BackgroundTextureInstance> g_backgroundTextures;
extern std::unordered_map<std::string, UserImageInstance> g_userImages;
extern GLuint g_vao;
extern GLuint g_vbo;
extern GLuint g_debugVAO;
extern GLuint g_debugVBO;
extern GLuint g_sceneFBO;
extern GLuint g_sceneTexture;
extern int g_sceneW;
extern int g_sceneH;

// --- Mutex Protection for GPU Resource Maps ---
// These maps are accessed from multiple threads (render + GUI)
extern std::shared_mutex g_mirrorInstancesMutex;
extern std::mutex g_userImagesMutex;
extern std::mutex g_backgroundTexturesMutex;

extern std::vector<GLuint> g_texturesToDelete;
extern std::mutex g_texturesToDeleteMutex;
extern std::atomic<bool> g_hasTexturesToDelete;
extern std::atomic<bool> g_glInitialized;
extern std::atomic<bool> g_isGameFocused;
extern GameViewportGeometry g_lastFrameGeometry;
extern std::mutex g_geometryMutex;
extern std::atomic<GLuint> g_cachedGameTextureId;

enum class ResizeCorner;
extern std::string s_hoveredWindowOverlayName;
extern std::string s_draggedWindowOverlayName;
extern bool s_isWindowOverlayDragging;
extern bool s_isWindowOverlayResizing;

void InitializeShaders();
void CleanupShaders();

void DrawOverlayBorder(float nx1, float ny1, float nx2, float ny2, float borderWidth, float borderHeight, bool isDragging,
                       bool drawCorners);

void RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH);

void DiscardAllGPUImages();
void CleanupGPUResources();
void UploadDecodedImageToGPU(const DecodedImageData& imgData);
void UploadDecodedImageToGPU_Internal(const DecodedImageData& imgData);
void InitializeGPUResources();
void CreateMirrorGPUResources(const MirrorConfig& conf);

// Mirror Capture Thread functions are declared in mirror_thread.h

void InvalidateConfigLookupCaches();

void RenderMirrors(const std::vector<MirrorConfig>& activeMirrors, const GameViewportGeometry& geo, int fullW, int fullH,
                   float modeOpacity = 1.0f, bool excludeOnlyOnMyScreen = false);
void RenderImages(const std::vector<ImageConfig>& activeImages, int fullW, int fullH, float modeOpacity = 1.0f,
                  bool excludeOnlyOnMyScreen = false);
void CollectActiveElementsForMode(const Config& config, const std::string& modeId, bool onlyOnMyScreenPass, uint64_t configVersion,
                                  std::vector<MirrorConfig>& outMirrors, std::vector<ImageConfig>& outImages,
                                  std::vector<const WindowOverlayConfig*>& outWindowOverlays);
void RenderMode(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation = false,
                bool excludeOnlyOnMyScreen = false);
void RenderModeWithOpacity(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, float opacity,
                           bool skipBackgroundClear = false);
void RenderDebugBordersForMirror(const MirrorConfig* conf, Color captureColor, Color outputColor, GLint originalVAO);
void handleEyeZoomMode(const GLState& s, const EyeZoomConfig& zoomConfig, int fullW, int fullH, float opacity = 1.0f,
                       int animatedViewportX = -1, bool useSnapshot = false, GLuint preferredGameTexture = 0,
                       int preferredGameW = 0, int preferredGameH = 0);
void InitializeOverlayTextFont(const std::string& fontPath, float baseFontSize, float scaleFactor);
void SetOverlayTextFontSize(int sizePixels);

void CalculateImageDimensions(const ImageConfig& img, int& outW, int& outH);

void SaveGLState(GLState* s);
void RestoreGLState(const GLState& s);

typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
extern GLVIEWPORTPROC oglViewport;

struct ModeConfig;

void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode);
void UpdateModeTransition();
bool IsModeTransitionActive();
GameTransitionType GetGameTransitionType();
OverlayTransitionType GetOverlayTransitionType();
BackgroundTransitionType GetBackgroundTransitionType();
std::string GetModeTransitionFromModeId();

// Struct to hold all transition state atomically
struct ModeTransitionState {
    bool active;
    int width;
    int height;
    int x;
    int y;
    GameTransitionType gameTransition;
    OverlayTransitionType overlayTransition;
    BackgroundTransitionType backgroundTransition;
    float progress;
    float moveProgress;
    int targetWidth;
    int targetHeight;
    int targetX;
    int targetY;
    int fromWidth;
    int fromHeight;
    int fromX;
    int fromY;
    std::string fromModeId;
};

// Get all transition state in a single atomic operation to avoid race conditions
ModeTransitionState GetModeTransitionState();

void RenderTextureGridOverlay(bool showTextureGrid, int modeWidth = 0, int modeHeight = 0);
void RenderCachedTextureGridLabels();

void GetAnimatedModePosition(int& outX, int& outY);

// Wait for the async overlay blit fence to complete (for delayRenderingUntilBlitted setting)
// Returns true if fence was waited on, false if no fence was pending
bool WaitForOverlayBlitFence();


