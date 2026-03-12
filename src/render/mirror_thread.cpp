#include "mirror_thread.h"
#include "gui/gui.h"
#include "runtime/logic_thread.h"
#include "common/profiler.h"
#include "render.h"
#include "shared_contexts.h"
#include "common/utils.h"
#include <array>
#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <thread>

// Thread runs independently, capturing game content to back-buffer FBOs
static std::thread g_mirrorCaptureThread;
std::atomic<bool> g_mirrorCaptureRunning{ false };
static std::atomic<bool> g_mirrorCaptureShouldStop{ false };

std::atomic<bool> g_safeToCapture{ false };
std::atomic<bool> g_sameThreadMirrorPipelineActive{ false };

// Updated by UpdateMirrorCaptureConfigs (logic thread) and read by SwapBuffers hook.
std::atomic<int> g_activeMirrorCaptureCount{ 0 };

// Summary for capture throttling: see mirror_thread.h
std::atomic<int> g_activeMirrorCaptureMaxFps{ 0 };

static HGLRC g_mirrorCaptureContext = NULL;
static HDC g_mirrorCaptureDC = NULL;
static bool g_mirrorContextIsShared = false;

// Fallback-mode DC ownership (see shared_contexts.h notes):
// Using the game's HDC on a different thread is undefined on some drivers and can trigger
// intermittent SEH/AVs or mirrors going black.
static HWND g_mirrorFallbackDummyHwnd = NULL;
static HDC g_mirrorFallbackDummyDC = NULL;
static HWND g_mirrorOwnedDCHwnd = NULL;

static bool MT_CreateFallbackDummyWindowWithMatchingPixelFormat(HDC gameHdc, const wchar_t* windowNameTag, HWND& outHwnd, HDC& outDc) {
    if (outHwnd && outDc) { return true; }
    if (!gameHdc) { return false; }

    int gamePf = GetPixelFormat(gameHdc);
    if (gamePf == 0) { return false; }

    PIXELFORMATDESCRIPTOR gamePfd = {};
    gamePfd.nSize = sizeof(gamePfd);
    gamePfd.nVersion = 1;
    if (DescribePixelFormat(gameHdc, gamePf, sizeof(gamePfd), &gamePfd) == 0) { return false; }

    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ToolscreenMirrorThreadDummy";
        s_atom = RegisterClassExW(&wc);
        if (!s_atom) {
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) { return false; }
        }
    }

    std::wstring wndName = L"ToolscreenMirrorThreadDummy_";
    wndName += (windowNameTag ? windowNameTag : L"mirror");

    outHwnd = CreateWindowExW(0, L"ToolscreenMirrorThreadDummy", wndName.c_str(), WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    if (!outHwnd) { return false; }

    outDc = GetDC(outHwnd);
    if (!outDc) {
        DestroyWindow(outHwnd);
        outHwnd = NULL;
        return false;
    }

    if (!SetPixelFormat(outDc, gamePf, &gamePfd)) {
        ReleaseDC(outHwnd, outDc);
        DestroyWindow(outHwnd);
        outDc = NULL;
        outHwnd = NULL;
        return false;
    }
    return true;
}

// Shared capture data (main thread writes, capture thread reads)
std::vector<ThreadedMirrorConfig> g_threadedMirrorConfigs;
std::mutex g_threadedMirrorConfigMutex;

// Incremented whenever g_threadedMirrorConfigs is mutated.
// The mirror capture thread uses this to refresh its local cache only when configs change
static std::atomic<uint64_t> g_threadedMirrorConfigsVersion{ 1 };

// Game state for capture thread
std::atomic<int> g_captureGameW{ 0 };
std::atomic<int> g_captureGameH{ 0 };
std::atomic<GLuint> g_captureGameTexture{ UINT_MAX };

std::atomic<int> g_captureScreenW{ 0 };
std::atomic<int> g_captureScreenH{ 0 };
std::atomic<int> g_captureFinalX{ 0 };
std::atomic<int> g_captureFinalY{ 0 };
std::atomic<int> g_captureFinalW{ 0 };
std::atomic<int> g_captureFinalH{ 0 };

// Lock-free SPSC ring buffer for capture notifications
FrameCaptureNotification g_captureQueue[CAPTURE_QUEUE_SIZE];
std::atomic<int> g_captureQueueHead{ 0 };
std::atomic<int> g_captureQueueTail{ 0 };

static std::mutex g_captureSignalMutex;
static std::condition_variable g_captureSignalCV;

// Double-buffered shared copy textures (render thread writes, capture thread reads)
static GLuint g_copyFBO = 0;
static GLuint g_copyTextures[2] = { 0, 0 };
static std::atomic<int> g_copyTextureWriteIndex{ 0 }; // Which texture render thread is writing to
static std::atomic<int> g_copyTextureReadIndex{ -1 }; // Which texture capture thread should read (-1 = none ready)
static int g_copyTextureW = 0, g_copyTextureH = 0;

// Track the last frame's copy fence for render_thread to wait on
// This is separate from the queue - render_thread needs synchronous access
static std::atomic<GLsync> g_lastCopyFence{ nullptr };
static std::atomic<int> g_lastCopyReadIndex{ -1 };
static std::atomic<int> g_lastCopyWidth{ 0 };
static std::atomic<int> g_lastCopyHeight{ 0 };
static std::atomic<bool> g_safeReadTextureValid{ false };

// These track the LAST FULLY COMPLETED frame - GPU fence has signaled, safe to read
// Updated by mirror thread after fence wait succeeds, read by OBS without waiting
static std::atomic<int> g_readyFrameIndex{ -1 };
static std::atomic<int> g_readyFrameWidth{ 0 };
static std::atomic<int> g_readyFrameHeight{ 0 };

static std::atomic<int> g_globalMirrorGammaMode{ static_cast<int>(MirrorGammaMode::Auto) };

void SetGlobalMirrorGammaMode(MirrorGammaMode mode) {
    g_globalMirrorGammaMode.store(static_cast<int>(mode), std::memory_order_release);
}

MirrorGammaMode GetGlobalMirrorGammaMode() {
    int v = g_globalMirrorGammaMode.load(std::memory_order_acquire);
    if (v < 0 || v > 2) return MirrorGammaMode::Auto;
    return static_cast<MirrorGammaMode>(v);
}

static void MT_LogSharedContextHealthOnce() {
    static std::atomic<bool> s_logged{ false };
    bool expected = false;
    if (!s_logged.compare_exchange_strong(expected, true)) { return; }

    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));

    LogCategory("init", std::string("Mirror Capture Thread: GL_VENDOR=") + (vendor ? vendor : "<null>"));
    LogCategory("init", std::string("Mirror Capture Thread: GL_RENDERER=") + (renderer ? renderer : "<null>"));
    LogCategory("init", std::string("Mirror Capture Thread: GL_VERSION=") + (version ? version : "<null>"));

    for (int i = 0; i < 2; i++) {
        GLuint tex = g_copyTextures[i];
        if (tex == 0) {
            LogCategory("init", "Mirror Capture Thread: g_copyTextures[" + std::to_string(i) + "] = 0 (not initialized yet)");
            continue;
        }

        GLboolean isTex = glIsTexture(tex);
        GLint w = 0, h = 0, ifmt = 0;
        BindTextureDirect(GL_TEXTURE_2D, tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        BindTextureDirect(GL_TEXTURE_2D, 0);

        LogCategory("init", "Mirror Capture Thread: shared copy tex[" + std::to_string(i) + "] id=" + std::to_string(tex) +
                                " glIsTexture=" + std::to_string((int)isTex) + " size=" + std::to_string(w) + "x" +
                                std::to_string(h) + " ifmt=" + std::to_string(ifmt));
    }

    while (glGetError() != GL_NO_ERROR) {
    }
}

// Note: OBS capture is now handled by obs_thread.cpp via glBlitFramebuffer hook

// MIRROR THREAD LOCAL SHADER PROGRAMS
// These shaders are created on the mirror thread context (not shared with main thread)

static const char* mt_passthrough_vert_shader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aSourceRect;
out vec2 TexCoord;
out vec4 SourceRect;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
    SourceRect = aSourceRect;
})";

static const char* mt_filter_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec4 SourceRect;
uniform sampler2D screenTexture;
uniform int u_gammaMode;
uniform vec3 u_targetColors[8];
uniform int u_targetColorCount;
uniform vec4 outputColor;
uniform float u_sensitivity;

vec3 SRGBToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, vec3(cutoff));
}
void main() {
    vec2 srcCoord = SourceRect.xy + TexCoord * SourceRect.zw;
    vec3 screenColor = texture(screenTexture, srcCoord).rgb;
    vec3 screenColorLinear = SRGBToLinear(screenColor);
    
    bool matches = false;
    for (int i = 0; i < u_targetColorCount; i++) {
        vec3 targetColorSRGB = u_targetColors[i];
        vec3 targetColorLinear = SRGBToLinear(targetColorSRGB);

        float dist;
        if (u_gammaMode == 2) {
            dist = distance(screenColor, targetColorLinear);
        } else if (u_gammaMode == 1) {
            dist = distance(screenColorLinear, targetColorLinear);
        } else {
            float distSRGB = distance(screenColor, targetColorSRGB);
            float distLinear = distance(screenColorLinear, targetColorLinear);
            dist = min(distSRGB, distLinear);
        }

        if (dist < u_sensitivity) {
            matches = true;
            break;
        }
    }
    
    if (matches) {
        FragColor = outputColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_filter_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec4 SourceRect;
uniform sampler2D screenTexture;
uniform int u_gammaMode;
uniform vec3 u_targetColors[8];
uniform int u_targetColorCount;
uniform float u_sensitivity;

vec3 SRGBToLinear(vec3 c) {
    bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
    vec3 low = c / 12.92;
    vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, vec3(cutoff));
}
void main() {
    vec2 srcCoord = SourceRect.xy + TexCoord * SourceRect.zw;
    vec3 screenColor = texture(screenTexture, srcCoord).rgb;
    vec3 screenColorLinear = SRGBToLinear(screenColor);
    
    bool matches = false;
    for (int i = 0; i < u_targetColorCount; i++) {
        vec3 targetColorSRGB = u_targetColors[i];
        vec3 targetColorLinear = SRGBToLinear(targetColorSRGB);

        float dist;
        if (u_gammaMode == 2) {
            dist = distance(screenColor, targetColorLinear);
        } else if (u_gammaMode == 1) {
            dist = distance(screenColorLinear, targetColorLinear);
        } else {
            float distSRGB = distance(screenColor, targetColorSRGB);
            float distLinear = distance(screenColorLinear, targetColorLinear);
            dist = min(distSRGB, distLinear);
        }

        if (dist < u_sensitivity) {
            matches = true;
            break;
        }
    }
    
    if (matches) {
        FragColor = vec4(screenColor, 1.0);
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
in vec4 SourceRect;
uniform sampler2D screenTexture;
void main() {
    vec2 srcCoord = SourceRect.xy + TexCoord * SourceRect.zw;
    // Force alpha=1 to avoid propagating undefined/junk alpha from game textures.
    vec4 c = texture(screenTexture, srcCoord);
    FragColor = vec4(c.rgb, 1.0);
})";

static const char* mt_background_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D backgroundTexture;
uniform float u_opacity;
void main() {
    vec4 texColor = texture(backgroundTexture, TexCoord);
    FragColor = vec4(texColor.rgb, texColor.a * u_opacity);
})";

static const char* mt_render_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_outputColor;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;

bool hasBorderSample(vec2 coord, vec2 pixel) {
    return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.5 ||
           texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.5 ||
           texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.5;
}

void main() {
    if (texture(filterTexture, TexCoord).a > 0.5) {
        FragColor = u_outputColor;
        return;
    }
    if (u_borderWidth == 1) {
        if (hasBorderSample(TexCoord, u_screenPixel)) {
            FragColor = u_borderColor;
            return;
        }
        discard;
    }
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(x, y) * u_screenPixel;
            maxA = max(maxA, texture(filterTexture, TexCoord + offset).a);
        }
    }
    if (maxA > 0.5) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

static const char* mt_render_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D filterTexture;
uniform int u_borderWidth;
uniform vec4 u_borderColor;
uniform vec2 u_screenPixel;

bool hasBorderSample(vec2 coord, vec2 pixel) {
    return texture(filterTexture, coord + vec2(-pixel.x, -pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(0.0, -pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(pixel.x, -pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(-pixel.x, 0.0)).a > 0.5 ||
           texture(filterTexture, coord + vec2(pixel.x, 0.0)).a > 0.5 ||
           texture(filterTexture, coord + vec2(-pixel.x, pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(0.0, pixel.y)).a > 0.5 ||
           texture(filterTexture, coord + vec2(pixel.x, pixel.y)).a > 0.5;
}

void main() {
    vec4 texColor = texture(filterTexture, TexCoord);
    if (texColor.a > 0.5) {
        FragColor = vec4(texColor.rgb, 1.0);
        return;
    }
    if (u_borderWidth == 1) {
        if (hasBorderSample(TexCoord, u_screenPixel)) {
            FragColor = u_borderColor;
            return;
        }
        discard;
    }
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
            if (x == 0 && y == 0) continue;
            vec2 offset = vec2(x, y) * u_screenPixel;
            maxA = max(maxA, texture(filterTexture, TexCoord + offset).a);
        }
    }
    if (maxA > 0.5) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

static const char* mt_dilate_horizontal_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D sourceTexture;
uniform int u_borderWidth;
uniform vec2 u_screenPixel;
void main() {
    float maxA = 0.0;
    for (int x = -u_borderWidth; x <= u_borderWidth; x++) {
        vec2 offset = vec2(float(x) * u_screenPixel.x, 0.0);
        maxA = max(maxA, texture(sourceTexture, TexCoord + offset).a);
    }
    FragColor = vec4(maxA, 0.0, 0.0, 1.0);
})";

static const char* mt_dilate_vertical_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D sourceTexture;
uniform sampler2D dilateTexture;
uniform int u_borderWidth;
uniform vec2 u_screenPixel;
uniform vec4 u_outputColor;
uniform vec4 u_borderColor;
void main() {
    float centerAlpha = texture(sourceTexture, TexCoord).a;
    if (centerAlpha > 0.5) {
        FragColor = u_outputColor;
        return;
    }
    float maxA = 0.0;
    for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
        vec2 offset = vec2(0.0, float(y) * u_screenPixel.y);
        maxA = max(maxA, texture(dilateTexture, TexCoord + offset).r);
    }
    if (maxA > 0.5) {
        FragColor = u_borderColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_dilate_vertical_passthrough_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D sourceTexture;
uniform sampler2D dilateTexture;
uniform int u_borderWidth;
uniform vec2 u_screenPixel;
uniform vec4 u_borderColor;
void main() {
    vec4 centerColor = texture(sourceTexture, TexCoord);
    if (centerColor.a > 0.5) {
        FragColor = vec4(centerColor.rgb, 1.0);
        return;
    }
    float maxA = 0.0;
    for (int y = -u_borderWidth; y <= u_borderWidth; y++) {
        vec2 offset = vec2(0.0, float(y) * u_screenPixel.y);
        maxA = max(maxA, texture(dilateTexture, TexCoord + offset).r);
    }
    if (maxA > 0.5) {
        FragColor = u_borderColor;
    } else {
        FragColor = vec4(0.0, 0.0, 0.0, 0.0);
    }
})";

static const char* mt_static_border_frag_shader = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform int u_shape;
uniform vec4 u_borderColor;
uniform float u_thickness;
uniform float u_radius;
uniform vec2 u_size;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    float maxR = min(b.x, b.y);
    r = clamp(r, 0.0, maxR);
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float sdEllipse(vec2 p, vec2 ab) {
    vec2 pn = p / ab;
    float d = length(pn) - 1.0;
    return d * min(ab.x, ab.y);
}

void main() {
    vec2 uv = TexCoord * 2.0 - 1.0;
    
    float aspectRatio = max(u_size.x, 1.0) / max(u_size.y, 1.0);
    vec2 aspectUV = uv;
    if (aspectRatio > 1.0) {
        aspectUV.x *= aspectRatio;
    } else {
        aspectUV.y /= aspectRatio;
    }
    
    float minSize = max(min(u_size.x, u_size.y), 1.0);
    float borderThickness = u_thickness / minSize * 2.0;
    
    float dist;
    
    if (u_shape == 0) {
        vec2 boxSize = vec2(1.0, 1.0);
        if (aspectRatio > 1.0) {
            boxSize.x = aspectRatio;
        } else {
            boxSize.y = 1.0 / aspectRatio;
        }
        float cornerRadius = u_radius / minSize * 2.0;
        dist = sdRoundedBox(aspectUV, boxSize, cornerRadius);
    } else {
        vec2 ellipseSize = vec2(1.0, 1.0);
        if (aspectRatio > 1.0) {
            ellipseSize.x = aspectRatio;
        } else {
            ellipseSize.y = 1.0 / aspectRatio;
        }
        dist = sdEllipse(aspectUV, ellipseSize);
    }
    
    float innerEdge = 0.0;
    float outerEdge = borderThickness;
    
    float epsilon = 0.01;
    
    if (dist >= innerEdge - epsilon && dist <= outerEdge + epsilon) {
        FragColor = u_borderColor;
    } else {
        discard;
    }
})";

// Local shader program handles (created on mirror thread context)
static GLuint mt_filterProgram = 0;
static GLuint mt_filterPassthroughProgram = 0;
static GLuint mt_passthroughProgram = 0;
static GLuint mt_backgroundProgram = 0;
static GLuint mt_renderProgram = 0;
static GLuint mt_renderPassthroughProgram = 0;
static GLuint mt_dilateHorizontalProgram = 0;
static GLuint mt_dilateVerticalProgram = 0;
static GLuint mt_dilateVerticalPassthroughProgram = 0;
static GLuint mt_staticBorderProgram = 0;

struct MT_FilterShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
    GLint gammaMode = -1;
    GLint targetColors = -1;
    GLint targetColorCount = -1;
    GLint outputColor = -1, sensitivity = -1;
};
struct MT_FilterPassthroughShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
    GLint gammaMode = -1;
    GLint targetColors = -1;
    GLint targetColorCount = -1;
    GLint sensitivity = -1;
};
struct MT_PassthroughShaderLocs {
    GLint screenTexture = -1, sourceRect = -1;
};
struct MT_BackgroundShaderLocs {
    GLint backgroundTexture = -1, opacity = -1;
};
struct MT_RenderShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, outputColor = -1, borderColor = -1, screenPixel = -1;
};
struct MT_RenderPassthroughShaderLocs {
    GLint filterTexture = -1, borderWidth = -1, borderColor = -1, screenPixel = -1;
};
struct MT_DilateHorizontalShaderLocs {
    GLint sourceTexture = -1, borderWidth = -1, screenPixel = -1;
};
struct MT_DilateVerticalShaderLocs {
    GLint sourceTexture = -1, dilateTexture = -1, borderWidth = -1, screenPixel = -1, outputColor = -1, borderColor = -1;
};
struct MT_DilateVerticalPassthroughShaderLocs {
    GLint sourceTexture = -1, dilateTexture = -1, borderWidth = -1, screenPixel = -1, borderColor = -1;
};
struct MT_StaticBorderShaderLocs {
    GLint shape = -1, borderColor = -1, thickness = -1, radius = -1, size = -1;
};

static MT_FilterShaderLocs mt_filterShaderLocs;
static MT_PassthroughShaderLocs mt_passthroughShaderLocs;
static MT_BackgroundShaderLocs mt_backgroundShaderLocs;
static MT_RenderShaderLocs mt_renderShaderLocs;
static MT_RenderPassthroughShaderLocs mt_renderPassthroughShaderLocs;
static MT_DilateHorizontalShaderLocs mt_dilateHorizontalShaderLocs;
static MT_DilateVerticalShaderLocs mt_dilateVerticalShaderLocs;
static MT_DilateVerticalPassthroughShaderLocs mt_dilateVerticalPassthroughShaderLocs;
static MT_StaticBorderShaderLocs mt_staticBorderShaderLocs;

static MT_FilterPassthroughShaderLocs mt_filterPassthroughShaderLocs;

static GLuint MT_CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, 512, NULL, log);
        Log("Mirror Thread: Shader compile error: " + std::string(log));
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint MT_CreateShaderProgram(const char* vertSrc, const char* fragSrc) {
    GLuint vs = MT_CompileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = MT_CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(p, 512, NULL, log);
        Log("Mirror Thread: Shader link error: " + std::string(log));
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static bool MT_InitializeShaders() {
    LogCategory("init", "Mirror Thread: Initializing local shaders...");

    mt_filterProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_filter_frag_shader);
    mt_filterPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_filter_passthrough_frag_shader);
    mt_passthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_passthrough_frag_shader);
    mt_backgroundProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_background_frag_shader);
    mt_renderProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_render_frag_shader);
    mt_renderPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_render_passthrough_frag_shader);
    mt_dilateHorizontalProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_dilate_horizontal_frag_shader);
    mt_dilateVerticalProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_dilate_vertical_frag_shader);
    mt_dilateVerticalPassthroughProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_dilate_vertical_passthrough_frag_shader);
    mt_staticBorderProgram = MT_CreateShaderProgram(mt_passthrough_vert_shader, mt_static_border_frag_shader);

    if (!mt_filterProgram || !mt_filterPassthroughProgram || !mt_passthroughProgram || !mt_backgroundProgram || !mt_renderProgram ||
        !mt_renderPassthroughProgram || !mt_dilateHorizontalProgram || !mt_dilateVerticalProgram ||
        !mt_dilateVerticalPassthroughProgram || !mt_staticBorderProgram) {
        Log("Mirror Thread: FATAL - Failed to create basic shader programs");
        return false;
    }

    mt_filterShaderLocs.screenTexture = glGetUniformLocation(mt_filterProgram, "screenTexture");
    mt_filterShaderLocs.sourceRect = glGetUniformLocation(mt_filterProgram, "u_sourceRect");
    mt_filterShaderLocs.gammaMode = glGetUniformLocation(mt_filterProgram, "u_gammaMode");
    mt_filterShaderLocs.targetColors = glGetUniformLocation(mt_filterProgram, "u_targetColors");
    mt_filterShaderLocs.targetColorCount = glGetUniformLocation(mt_filterProgram, "u_targetColorCount");
    mt_filterShaderLocs.outputColor = glGetUniformLocation(mt_filterProgram, "outputColor");
    mt_filterShaderLocs.sensitivity = glGetUniformLocation(mt_filterProgram, "u_sensitivity");

    mt_filterPassthroughShaderLocs.screenTexture = glGetUniformLocation(mt_filterPassthroughProgram, "screenTexture");
    mt_filterPassthroughShaderLocs.sourceRect = glGetUniformLocation(mt_filterPassthroughProgram, "u_sourceRect");
    mt_filterPassthroughShaderLocs.gammaMode = glGetUniformLocation(mt_filterPassthroughProgram, "u_gammaMode");
    mt_filterPassthroughShaderLocs.targetColors = glGetUniformLocation(mt_filterPassthroughProgram, "u_targetColors");
    mt_filterPassthroughShaderLocs.targetColorCount = glGetUniformLocation(mt_filterPassthroughProgram, "u_targetColorCount");
    mt_filterPassthroughShaderLocs.sensitivity = glGetUniformLocation(mt_filterPassthroughProgram, "u_sensitivity");

    mt_passthroughShaderLocs.screenTexture = glGetUniformLocation(mt_passthroughProgram, "screenTexture");
    mt_passthroughShaderLocs.sourceRect = glGetUniformLocation(mt_passthroughProgram, "u_sourceRect");

    mt_backgroundShaderLocs.backgroundTexture = glGetUniformLocation(mt_backgroundProgram, "backgroundTexture");
    mt_backgroundShaderLocs.opacity = glGetUniformLocation(mt_backgroundProgram, "u_opacity");

    mt_renderShaderLocs.filterTexture = glGetUniformLocation(mt_renderProgram, "filterTexture");
    mt_renderShaderLocs.borderWidth = glGetUniformLocation(mt_renderProgram, "u_borderWidth");
    mt_renderShaderLocs.outputColor = glGetUniformLocation(mt_renderProgram, "u_outputColor");
    mt_renderShaderLocs.borderColor = glGetUniformLocation(mt_renderProgram, "u_borderColor");
    mt_renderShaderLocs.screenPixel = glGetUniformLocation(mt_renderProgram, "u_screenPixel");

    mt_renderPassthroughShaderLocs.filterTexture = glGetUniformLocation(mt_renderPassthroughProgram, "filterTexture");
    mt_renderPassthroughShaderLocs.borderWidth = glGetUniformLocation(mt_renderPassthroughProgram, "u_borderWidth");
    mt_renderPassthroughShaderLocs.borderColor = glGetUniformLocation(mt_renderPassthroughProgram, "u_borderColor");
    mt_renderPassthroughShaderLocs.screenPixel = glGetUniformLocation(mt_renderPassthroughProgram, "u_screenPixel");

    mt_dilateHorizontalShaderLocs.sourceTexture = glGetUniformLocation(mt_dilateHorizontalProgram, "sourceTexture");
    mt_dilateHorizontalShaderLocs.borderWidth = glGetUniformLocation(mt_dilateHorizontalProgram, "u_borderWidth");
    mt_dilateHorizontalShaderLocs.screenPixel = glGetUniformLocation(mt_dilateHorizontalProgram, "u_screenPixel");

    mt_dilateVerticalShaderLocs.sourceTexture = glGetUniformLocation(mt_dilateVerticalProgram, "sourceTexture");
    mt_dilateVerticalShaderLocs.dilateTexture = glGetUniformLocation(mt_dilateVerticalProgram, "dilateTexture");
    mt_dilateVerticalShaderLocs.borderWidth = glGetUniformLocation(mt_dilateVerticalProgram, "u_borderWidth");
    mt_dilateVerticalShaderLocs.screenPixel = glGetUniformLocation(mt_dilateVerticalProgram, "u_screenPixel");
    mt_dilateVerticalShaderLocs.outputColor = glGetUniformLocation(mt_dilateVerticalProgram, "u_outputColor");
    mt_dilateVerticalShaderLocs.borderColor = glGetUniformLocation(mt_dilateVerticalProgram, "u_borderColor");

    mt_dilateVerticalPassthroughShaderLocs.sourceTexture = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "sourceTexture");
    mt_dilateVerticalPassthroughShaderLocs.dilateTexture = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "dilateTexture");
    mt_dilateVerticalPassthroughShaderLocs.borderWidth = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "u_borderWidth");
    mt_dilateVerticalPassthroughShaderLocs.screenPixel = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "u_screenPixel");
    mt_dilateVerticalPassthroughShaderLocs.borderColor = glGetUniformLocation(mt_dilateVerticalPassthroughProgram, "u_borderColor");

    mt_staticBorderShaderLocs.shape = glGetUniformLocation(mt_staticBorderProgram, "u_shape");
    mt_staticBorderShaderLocs.borderColor = glGetUniformLocation(mt_staticBorderProgram, "u_borderColor");
    mt_staticBorderShaderLocs.thickness = glGetUniformLocation(mt_staticBorderProgram, "u_thickness");
    mt_staticBorderShaderLocs.radius = glGetUniformLocation(mt_staticBorderProgram, "u_radius");
    mt_staticBorderShaderLocs.size = glGetUniformLocation(mt_staticBorderProgram, "u_size");

    glUseProgram(mt_filterProgram);
    glUniform1i(mt_filterShaderLocs.screenTexture, 0);
    if (mt_filterShaderLocs.gammaMode >= 0) { glUniform1i(mt_filterShaderLocs.gammaMode, 0); }

    glUseProgram(mt_filterPassthroughProgram);
    glUniform1i(mt_filterPassthroughShaderLocs.screenTexture, 0);
    if (mt_filterPassthroughShaderLocs.gammaMode >= 0) { glUniform1i(mt_filterPassthroughShaderLocs.gammaMode, 0); }

    glUseProgram(mt_passthroughProgram);
    glUniform1i(mt_passthroughShaderLocs.screenTexture, 0);

    glUseProgram(mt_backgroundProgram);
    glUniform1i(mt_backgroundShaderLocs.backgroundTexture, 0);

    glUseProgram(mt_renderProgram);
    glUniform1i(mt_renderShaderLocs.filterTexture, 0);

    glUseProgram(mt_renderPassthroughProgram);
    glUniform1i(mt_renderPassthroughShaderLocs.filterTexture, 0);

    glUseProgram(mt_dilateHorizontalProgram);
    glUniform1i(mt_dilateHorizontalShaderLocs.sourceTexture, 0);

    glUseProgram(mt_dilateVerticalProgram);
    glUniform1i(mt_dilateVerticalShaderLocs.sourceTexture, 0);
    glUniform1i(mt_dilateVerticalShaderLocs.dilateTexture, 1);

    glUseProgram(mt_dilateVerticalPassthroughProgram);
    glUniform1i(mt_dilateVerticalPassthroughShaderLocs.sourceTexture, 0);
    glUniform1i(mt_dilateVerticalPassthroughShaderLocs.dilateTexture, 1);

    glUseProgram(0);

    LogCategory("init", "Mirror Thread: Local shaders initialized successfully");
    return true;
}

static void MT_CleanupShaders() {
    if (mt_filterProgram) {
        glDeleteProgram(mt_filterProgram);
        mt_filterProgram = 0;
    }
    if (mt_filterPassthroughProgram) {
        glDeleteProgram(mt_filterPassthroughProgram);
        mt_filterPassthroughProgram = 0;
    }
    if (mt_passthroughProgram) {
        glDeleteProgram(mt_passthroughProgram);
        mt_passthroughProgram = 0;
    }
    if (mt_backgroundProgram) {
        glDeleteProgram(mt_backgroundProgram);
        mt_backgroundProgram = 0;
    }
    if (mt_renderProgram) {
        glDeleteProgram(mt_renderProgram);
        mt_renderProgram = 0;
    }
    if (mt_renderPassthroughProgram) {
        glDeleteProgram(mt_renderPassthroughProgram);
        mt_renderPassthroughProgram = 0;
    }
    if (mt_dilateHorizontalProgram) {
        glDeleteProgram(mt_dilateHorizontalProgram);
        mt_dilateHorizontalProgram = 0;
    }
    if (mt_dilateVerticalProgram) {
        glDeleteProgram(mt_dilateVerticalProgram);
        mt_dilateVerticalProgram = 0;
    }
    if (mt_dilateVerticalPassthroughProgram) {
        glDeleteProgram(mt_dilateVerticalPassthroughProgram);
        mt_dilateVerticalPassthroughProgram = 0;
    }
    if (mt_staticBorderProgram) {
        glDeleteProgram(mt_staticBorderProgram);
        mt_staticBorderProgram = 0;
    }
}

// Get the most recent copy texture (for OBS/render_thread to use)
GLuint GetGameCopyTexture() {
    int readIndex = g_lastCopyReadIndex.load(std::memory_order_acquire);
    if (readIndex < 0 || readIndex >= 2) return 0;
    return g_copyTextures[readIndex];
}

// These return GUARANTEED COMPLETE frames - no fence wait needed
// Updated by mirror thread after fence signals, so OBS can read without waiting

GLuint GetReadyGameTexture() {
    int idx = g_readyFrameIndex.load(std::memory_order_acquire);
    if (idx < 0 || idx >= 2) return 0;
    return g_copyTextures[idx];
}

int GetReadyGameWidth() { return g_readyFrameWidth.load(std::memory_order_acquire); }

int GetReadyGameHeight() { return g_readyFrameHeight.load(std::memory_order_acquire); }

// Fallback accessors - return last copy texture info (requires fence wait before use)
GLuint GetFallbackGameTexture() {
    int idx = g_lastCopyReadIndex.load(std::memory_order_acquire);
    if (idx < 0 || idx >= 2) return 0;
    return g_copyTextures[idx];
}

int GetFallbackGameWidth() { return g_lastCopyWidth.load(std::memory_order_acquire); }

int GetFallbackGameHeight() { return g_lastCopyHeight.load(std::memory_order_acquire); }

GLsync GetFallbackCopyFence() { return g_lastCopyFence.load(std::memory_order_acquire); }

// This is a guaranteed valid texture (may be 1 frame behind) - no fence wait needed
GLuint GetSafeReadTexture() {
    if (!g_safeReadTextureValid.load(std::memory_order_acquire)) return 0;
    int writeIndex = g_copyTextureWriteIndex.load(std::memory_order_acquire);
    int readIndex = 1 - writeIndex;
    if (g_copyTextures[readIndex] == 0) return 0;
    return g_copyTextures[readIndex];
}

void InitCaptureTexture(int width, int height) {
    // This MUST be called from the main render thread with GL context current

    g_copyTextureW = width;
    g_copyTextureH = height;

    glGenFramebuffers(1, &g_copyFBO);

    glGenTextures(2, g_copyTextures);
    for (int i = 0; i < 2; i++) {
        BindTextureDirect(GL_TEXTURE_2D, g_copyTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    BindTextureDirect(GL_TEXTURE_2D, 0);

    g_copyTextureWriteIndex.store(0);
    g_copyTextureReadIndex.store(-1);
    g_safeReadTextureValid.store(false, std::memory_order_release);

    LogCategory("init", "InitCaptureTexture: Created FBO and " + std::to_string(2) + " textures of " + std::to_string(width) + "x" +
                            std::to_string(height));
}

void EnsureCaptureTextureInitialized(int width, int height) {
    if (width <= 0 || height <= 0) { return; }
    if (g_copyFBO != 0 && g_copyTextures[0] != 0 && g_copyTextures[1] != 0) { return; }
    InitCaptureTexture(width, height);
}

void CleanupCaptureTexture() {
    // Cleanup capture resources - call from capture thread or main thread with GL context current
    // Drain the lock-free queue and delete any remaining fences
    FrameCaptureNotification notif;
    while (CaptureQueuePop(notif)) {
        if (notif.fence && glIsSync(notif.fence)) { glDeleteSync(notif.fence); }
    }

    // Also clear the render-thread fallback fence. This fence may have been created in a different
    // can cause driver instability on some systems.
    {
        GLsync old = g_lastCopyFence.exchange(nullptr, std::memory_order_acq_rel);
        if (old && glIsSync(old)) { glDeleteSync(old); }
        g_lastCopyReadIndex.store(-1, std::memory_order_release);
        g_lastCopyWidth.store(0, std::memory_order_release);
        g_lastCopyHeight.store(0, std::memory_order_release);
        g_readyFrameIndex.store(-1, std::memory_order_release);
        g_readyFrameWidth.store(0, std::memory_order_release);
        g_readyFrameHeight.store(0, std::memory_order_release);
        g_safeReadTextureValid.store(false, std::memory_order_release);
    }

    if (g_copyTextures[0] != 0 || g_copyTextures[1] != 0) {
        glDeleteTextures(2, g_copyTextures);
        g_copyTextures[0] = 0;
        g_copyTextures[1] = 0;
    }

    if (g_copyFBO != 0) {
        glDeleteFramebuffers(1, &g_copyFBO);
        g_copyFBO = 0;
    }

    Log("CleanupCaptureTexture: Cleaned up FBO and textures");
}

void SubmitFrameCapture(GLuint gameTexture, int width, int height) {
    // Called from SwapBuffers hook - does ASYNC GPU blit (non-blocking)
    // Consumers wait on the fence before reading the copy.

    if (g_copyFBO == 0) {
        return;
    }

    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    GLint prevActiveTexture = 0;
    GLint prevTexture2D = 0;
    GLboolean prevDither = GL_FALSE;
    GLboolean prevFramebufferSRGB = GL_FALSE;
    bool hasFramebufferSRGB = false;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture2D);

    // Some drivers apply dithering when converting to RGBA8 during blits/writes.
    prevDither = glIsEnabled(GL_DITHER);

    hasFramebufferSRGB = (GLEW_VERSION_3_0 || GLEW_EXT_framebuffer_sRGB || GLEW_ARB_framebuffer_sRGB);
    if (hasFramebufferSRGB) {
        prevFramebufferSRGB = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    }

    auto restoreState = [&]() {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);
        glActiveTexture(prevActiveTexture);
        BindTextureDirect(GL_TEXTURE_2D, prevTexture2D);

        if (prevDither)
            glEnable(GL_DITHER);
        else
            glDisable(GL_DITHER);

        if (hasFramebufferSRGB) {
            if (prevFramebufferSRGB)
                glEnable(GL_FRAMEBUFFER_SRGB);
            else
                glDisable(GL_FRAMEBUFFER_SRGB);
        }
    };

    glDisable(GL_DITHER);
    if (hasFramebufferSRGB) { glDisable(GL_FRAMEBUFFER_SRGB); }

    // Only resize the WRITE texture, not the read texture that other threads may be using
    int writeIndex = g_copyTextureWriteIndex.load(std::memory_order_acquire);
    bool dimensionsChanged = (width != g_copyTextureW || height != g_copyTextureH);

    if (dimensionsChanged) {
        // Note: We do NOT invalidate g_lastCopyReadIndex here - it continues pointing to
        // in an invalid state if blits fail due to race conditions.

        // glTexImage2D replaces the backing storage with undefined content, so any
        // thread reading the "ready" texture would get garbage/black data. This was
        // causing visual freezes on some devices: the render thread would keep blitting
        // the stale ready frame (now undefined) instead of showing new content.
        g_readyFrameIndex.store(-1, std::memory_order_release);
        g_readyFrameWidth.store(0, std::memory_order_release);
        g_readyFrameHeight.store(0, std::memory_order_release);
        g_safeReadTextureValid.store(false, std::memory_order_release);

        for (int i = 0; i < 2; i++) {
            BindTextureDirect(GL_TEXTURE_2D, g_copyTextures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }
        BindTextureDirect(GL_TEXTURE_2D, 0);

        // Use fence + flush instead of glFinish() to avoid blocking the game thread.
        // cause visible hitches on some GPU/driver combinations (especially iGPUs).
        // A fence only waits for the texture reallocation commands specifically.
        GLsync resizeFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush(); // Ensure fence and resize commands are submitted to GPU
        if (resizeFence) {
            glClientWaitSync(resizeFence, GL_SYNC_FLUSH_COMMANDS_BIT, 500000000ULL);
            if (glIsSync(resizeFence)) { glDeleteSync(resizeFence); }
        }

        g_copyTextureW = width;
        g_copyTextureH = height;
        LogCategory("texture_ops", "SubmitFrameCapture: Resized copy textures to " + std::to_string(width) + "x" + std::to_string(height));
    }

    static GLuint srcFBO = 0;
    if (srcFBO == 0) { glGenFramebuffers(1, &srcFBO); }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFBO);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gameTexture, 0);

    GLenum srcStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (srcStatus != GL_FRAMEBUFFER_COMPLETE) {
        static int s_srcIncompleteLog = 0;
        if ((++s_srcIncompleteLog % 240) == 1) {
            LogCategory("texture_ops",
                        "SubmitFrameCapture: Source FBO incomplete (status " + std::to_string(srcStatus) + ") gameTex=" +
                            std::to_string(gameTexture) + " size=" + std::to_string(width) + "x" + std::to_string(height));
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        restoreState();
        return;
    }


    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copyFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copyTextures[writeIndex], 0);

    GLenum dstStatus = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (dstStatus != GL_FRAMEBUFFER_COMPLETE) {
        static int s_dstIncompleteLog = 0;
        if ((++s_dstIncompleteLog % 240) == 1) {
            LogCategory("texture_ops",
                        "SubmitFrameCapture: Destination FBO incomplete (status " + std::to_string(dstStatus) + ") writeIdx=" +
                            std::to_string(writeIndex) + " dstTex=" + std::to_string(g_copyTextures[writeIndex]) + " size=" +
                            std::to_string(width) + "x" + std::to_string(height));
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        restoreState();
        return;
    }

    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    const bool sameThreadMirrorPipeline = g_sameThreadMirrorPipelineActive.load(std::memory_order_acquire);

    // Create synchronization fences after the blit commands.
    // The render-thread/consumer fence is always kept. The mirror-thread fence is only needed
    // when the mirror thread is responsible for publishing ready frames.
    GLsync fenceForMirrorThread = sameThreadMirrorPipeline ? nullptr : glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    GLsync fenceForRenderThread = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    // (glClientWaitSync/glWaitSync on a null/invalid fence can crash some drivers.)
    if ((!sameThreadMirrorPipeline && !fenceForMirrorThread) || !fenceForRenderThread) {
        if (fenceForMirrorThread && glIsSync(fenceForMirrorThread)) { glDeleteSync(fenceForMirrorThread); }
        if (fenceForRenderThread && glIsSync(fenceForRenderThread)) { glDeleteSync(fenceForRenderThread); }
        restoreState();
        return;
    }

    // Flush to ensure commands are submitted and fence is visible to other contexts
    glFlush();

    int nextWriteIndex = 1 - writeIndex;
    g_copyTextureWriteIndex.store(nextWriteIndex, std::memory_order_release);

    // Update accessor variables for render_thread/OBS to use
    // Delete old fence before storing new one (render thread fence management)
    GLsync oldFence = g_lastCopyFence.exchange(fenceForRenderThread, std::memory_order_acq_rel);
    if (oldFence && glIsSync(oldFence)) { glDeleteSync(oldFence); }
    g_lastCopyReadIndex.store(writeIndex, std::memory_order_release);
    g_lastCopyWidth.store(width, std::memory_order_release);
    g_lastCopyHeight.store(height, std::memory_order_release);
    g_safeReadTextureValid.store(true, std::memory_order_release);

    if (sameThreadMirrorPipeline) {
        g_readyFrameIndex.store(writeIndex, std::memory_order_release);
        g_readyFrameWidth.store(width, std::memory_order_release);
        g_readyFrameHeight.store(height, std::memory_order_release);
    } else {
        // Notify mirror thread (lock-free queue) - include texture index so mirror thread uses correct texture
        FrameCaptureNotification notif = { 0, fenceForMirrorThread, width, height, writeIndex };
        if (!CaptureQueuePush(notif)) {
            // Queue full - delete the fence since mirror thread won't get it
            if (glIsSync(fenceForMirrorThread)) { glDeleteSync(fenceForMirrorThread); }
        } else {
            // Wake mirror thread so it doesn't have to poll.
            g_captureSignalCV.notify_one();
        }
    }

    restoreState();
}

static void ComputeMirrorRenderCache(MirrorInstance* inst, const ThreadedMirrorConfig& conf, int gameW, int gameH, int screenW, int screenH,
                                     int finalX, int finalY, int finalW, int finalH, bool writeToBack) {
    auto& cache = writeToBack ? inst->cachedRenderStateBack : inst->cachedRenderState;

    float scaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
    float scaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
    if (cache.isValid && cache.outputScale == conf.outputScale && cache.outputSeparateScale == conf.outputSeparateScale &&
        cache.outputScaleX == conf.outputScaleX && cache.outputScaleY == conf.outputScaleY && cache.outputX == conf.outputX &&
        cache.outputY == conf.outputY && cache.outputRelativeTo == conf.outputRelativeTo && cache.gameW == gameW && cache.gameH == gameH &&
        cache.screenW == screenW && cache.screenH == screenH && cache.finalX == finalX && cache.finalY == finalY &&
        cache.finalW == finalW && cache.finalH == finalH && cache.fbo_w == inst->fbo_w && cache.fbo_h == inst->fbo_h) {
        return;
    }

    int outW = static_cast<int>(inst->fbo_w * scaleX);
    int outH = static_cast<int>(inst->fbo_h * scaleY);

    MirrorConfig tempConf;
    tempConf.output.scale = conf.outputScale;
    tempConf.output.separateScale = conf.outputSeparateScale;
    tempConf.output.scaleX = conf.outputScaleX;
    tempConf.output.scaleY = conf.outputScaleY;
    tempConf.output.x = conf.outputX;
    tempConf.output.y = conf.outputY;
    tempConf.output.relativeTo = conf.outputRelativeTo;

    int screenX, screenY;
    CalculateFinalScreenPos(&tempConf, *inst, gameW, gameH, finalX, finalY, finalW, finalH, screenW, screenH, screenX, screenY);

    float nx1 = (static_cast<float>(screenX) / screenW) * 2.0f - 1.0f;
    float ny2 = 1.0f - (static_cast<float>(screenY) / screenH) * 2.0f;
    float nx2 = (static_cast<float>(screenX + outW) / screenW) * 2.0f - 1.0f;
    float ny1 = 1.0f - (static_cast<float>(screenY + outH) / screenH) * 2.0f;

    float vertices[] = { nx1, ny1, 0, 0, nx2, ny1, 1, 0, nx2, ny2, 1, 1, nx1, ny1, 0, 0, nx2, ny2, 1, 1, nx1, ny2, 0, 1 };
    memcpy(cache.vertices, vertices, sizeof(vertices));

    cache.outputScale = conf.outputScale;
    cache.outputSeparateScale = conf.outputSeparateScale;
    cache.outputScaleX = conf.outputScaleX;
    cache.outputScaleY = conf.outputScaleY;
    cache.outputX = conf.outputX;
    cache.outputY = conf.outputY;
    cache.outputRelativeTo = conf.outputRelativeTo;
    cache.gameW = gameW;
    cache.gameH = gameH;
    cache.screenW = screenW;
    cache.screenH = screenH;
    cache.finalX = finalX;
    cache.finalY = finalY;
    cache.finalW = finalW;
    cache.finalH = finalH;
    cache.fbo_w = inst->fbo_w;
    cache.fbo_h = inst->fbo_h;
    cache.outW = outW;
    cache.outH = outH;
    cache.mirrorScreenX = screenX;
    cache.mirrorScreenY = screenY;
    cache.mirrorScreenW = outW;
    cache.mirrorScreenH = outH;
    cache.isValid = true;
}

struct MT_RenderToBufferStateCache {
    GLuint framebuffer = 0;
    bool framebufferValid = false;
    GLuint program = 0;
    bool programValid = false;
    GLuint texture = 0;
    bool textureValid = false;
    bool blendEnabled = false;
    bool blendValid = false;
    int viewportX = 0;
    int viewportY = 0;
    int viewportW = 0;
    int viewportH = 0;
    bool viewportValid = false;
    float clearColorR = 0.0f;
    float clearColorG = 0.0f;
    float clearColorB = 0.0f;
    float clearColorA = 0.0f;
    bool clearColorValid = false;

    int filterGammaMode = 0;
    bool filterGammaModeValid = false;
    int filterTargetColorCount = 0;
    bool filterTargetColorStateValid = false;
    std::array<float, 24> filterTargetColors{};
    float filterSensitivity = 0.0f;
    bool filterSensitivityValid = false;
    Color filterOutputColor{};
    bool filterOutputColorValid = false;

    int filterPassthroughGammaMode = 0;
    bool filterPassthroughGammaModeValid = false;
    int filterPassthroughTargetColorCount = 0;
    bool filterPassthroughTargetColorStateValid = false;
    std::array<float, 24> filterPassthroughTargetColors{};
    float filterPassthroughSensitivity = 0.0f;
    bool filterPassthroughSensitivityValid = false;

    float backgroundOpacity = 0.0f;
    bool backgroundOpacityValid = false;

    int renderBorderWidth = 0;
    bool renderBorderWidthValid = false;
    Color renderOutputColor{};
    bool renderOutputColorValid = false;
    Color renderBorderColor{};
    bool renderBorderColorValid = false;
    float renderScreenPixelX = 0.0f;
    float renderScreenPixelY = 0.0f;
    bool renderScreenPixelValid = false;

    int renderPassthroughBorderWidth = 0;
    bool renderPassthroughBorderWidthValid = false;
    Color renderPassthroughBorderColor{};
    bool renderPassthroughBorderColorValid = false;
    float renderPassthroughScreenPixelX = 0.0f;
    float renderPassthroughScreenPixelY = 0.0f;
    bool renderPassthroughScreenPixelValid = false;

    int dilateHorizontalBorderWidth = 0;
    bool dilateHorizontalBorderWidthValid = false;
    float dilateHorizontalScreenPixelX = 0.0f;
    float dilateHorizontalScreenPixelY = 0.0f;
    bool dilateHorizontalScreenPixelValid = false;

    int dilateVerticalBorderWidth = 0;
    bool dilateVerticalBorderWidthValid = false;
    float dilateVerticalScreenPixelX = 0.0f;
    float dilateVerticalScreenPixelY = 0.0f;
    bool dilateVerticalScreenPixelValid = false;
    Color dilateVerticalOutputColor{};
    bool dilateVerticalOutputColorValid = false;
    Color dilateVerticalBorderColor{};
    bool dilateVerticalBorderColorValid = false;

    int dilateVerticalPassthroughBorderWidth = 0;
    bool dilateVerticalPassthroughBorderWidthValid = false;
    float dilateVerticalPassthroughScreenPixelX = 0.0f;
    float dilateVerticalPassthroughScreenPixelY = 0.0f;
    bool dilateVerticalPassthroughScreenPixelValid = false;
    Color dilateVerticalPassthroughBorderColor{};
    bool dilateVerticalPassthroughBorderColorValid = false;

    GLuint sourceRectAttribBuffer = 0;
    bool sourceRectAttribBufferValid = false;
};

static void MT_BindFramebufferCached(GLuint framebuffer, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->framebufferValid || stateCache->framebuffer != framebuffer) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        if (stateCache) {
            stateCache->framebuffer = framebuffer;
            stateCache->framebufferValid = true;
        }
    }
}

static void MT_SetViewportCached(int x, int y, int width, int height, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->viewportValid || stateCache->viewportX != x || stateCache->viewportY != y ||
        stateCache->viewportW != width || stateCache->viewportH != height) {
        if (oglViewport) {
            oglViewport(x, y, width, height);
        } else {
            glViewport(x, y, width, height);
        }

        if (stateCache) {
            stateCache->viewportX = x;
            stateCache->viewportY = y;
            stateCache->viewportW = width;
            stateCache->viewportH = height;
            stateCache->viewportValid = true;
        }
    }
}

static void MT_UseProgramCached(GLuint program, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->programValid || stateCache->program != program) {
        glUseProgram(program);
        if (stateCache) {
            stateCache->program = program;
            stateCache->programValid = true;
        }
    }
}

static void MT_BindTextureCached(GLuint texture, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->textureValid || stateCache->texture != texture) {
        BindTextureDirect(GL_TEXTURE_2D, texture);
        if (stateCache) {
            stateCache->texture = texture;
            stateCache->textureValid = true;
        }
    }
}

static void MT_SetBlendEnabledCached(bool enabled, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->blendValid || stateCache->blendEnabled != enabled) {
        if (enabled) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
        } else {
            glDisable(GL_BLEND);
        }

        if (stateCache) {
            stateCache->blendEnabled = enabled;
            stateCache->blendValid = true;
        }
    }
}

static void MT_SetClearColorCached(float r, float g, float b, float a, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->clearColorValid || stateCache->clearColorR != r || stateCache->clearColorG != g ||
        stateCache->clearColorB != b || stateCache->clearColorA != a) {
        glClearColor(r, g, b, a);
        if (stateCache) {
            stateCache->clearColorR = r;
            stateCache->clearColorG = g;
            stateCache->clearColorB = b;
            stateCache->clearColorA = a;
            stateCache->clearColorValid = true;
        }
    }
}

static bool MT_ColorEquals(const Color& left, const Color& right) {
    return left.r == right.r && left.g == right.g && left.b == right.b && left.a == right.a;
}

static uint64_t MT_HashBytes(uint64_t seed, const void* data, size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        seed ^= static_cast<uint64_t>(bytes[i]);
        seed *= 1099511628211ull;
    }
    return seed;
}

static uint64_t MT_ComputeSourceRectLayoutHash(const ThreadedMirrorConfig& conf) {
    uint64_t hash = 1469598103934665603ull;
    hash = MT_HashBytes(hash, &conf.captureWidth, sizeof(conf.captureWidth));
    hash = MT_HashBytes(hash, &conf.captureHeight, sizeof(conf.captureHeight));

    const size_t inputCount = conf.input.size();
    hash = MT_HashBytes(hash, &inputCount, sizeof(inputCount));
    for (const auto& input : conf.input) {
        hash = MT_HashBytes(hash, &input.x, sizeof(input.x));
        hash = MT_HashBytes(hash, &input.y, sizeof(input.y));
        hash = MT_HashBytes(hash, input.relativeTo.data(), input.relativeTo.size());
    }

    return hash;
}

struct MT_SourceRectCacheEntry {
    uint64_t layoutHash = 0;
    int gameW = 0;
    int gameH = 0;
    std::vector<float> sourceRects;
};

struct MT_SourceRectGpuCacheEntry {
    GLuint instanceVbo = 0;
    size_t capacityBytes = 0;
    uint64_t layoutHash = 0;
    int gameW = 0;
    int gameH = 0;
};

static void MT_DeleteSourceRectGpuCacheEntry(MT_SourceRectGpuCacheEntry& cacheEntry) {
    if (cacheEntry.instanceVbo != 0) {
        glDeleteBuffers(1, &cacheEntry.instanceVbo);
        cacheEntry.instanceVbo = 0;
    }
    cacheEntry.capacityBytes = 0;
    cacheEntry.layoutHash = 0;
    cacheEntry.gameW = 0;
    cacheEntry.gameH = 0;
}

static void MT_BindSourceRectInstanceBufferCached(GLuint instanceVbo, MT_RenderToBufferStateCache* stateCache) {
    if (!stateCache || !stateCache->sourceRectAttribBufferValid || stateCache->sourceRectAttribBuffer != instanceVbo) {
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        if (stateCache) {
            stateCache->sourceRectAttribBuffer = instanceVbo;
            stateCache->sourceRectAttribBufferValid = true;
        }
    }
}

// Forward declaration needed for use in MT_GetCachedSourceRects before the full definition below
static void MT_GetRelativeCoordsNormalized(const std::string& anchor, int relX, int relY, int w, int h, int containerW,
                                           int containerH, int& outX, int& outY);

static const std::vector<float>& MT_GetCachedSourceRects(const ThreadedMirrorConfig& conf, int gameW, int gameH) {
    thread_local std::unordered_map<std::string, MT_SourceRectCacheEntry> s_sourceRectCache;

    MT_SourceRectCacheEntry& cacheEntry = s_sourceRectCache[conf.name];
    const size_t requiredFloatCount = conf.input.size() * 4;
    if (cacheEntry.layoutHash == conf.sourceRectLayoutHash && cacheEntry.gameW == gameW && cacheEntry.gameH == gameH &&
        cacheEntry.sourceRects.size() == requiredFloatCount) {
        return cacheEntry.sourceRects;
    }

    cacheEntry.layoutHash = conf.sourceRectLayoutHash;
    cacheEntry.gameW = gameW;
    cacheEntry.gameH = gameH;
    cacheEntry.sourceRects.resize(requiredFloatCount);

    const float sw = static_cast<float>(conf.captureWidth) / gameW;
    const float sh = static_cast<float>(conf.captureHeight) / gameH;
    for (size_t i = 0; i < conf.input.size(); ++i) {
        const auto& r = conf.input[i];
        int capX = 0;
        int capY = 0;
        MT_GetRelativeCoordsNormalized(r.relativeTo, r.x, r.y, conf.captureWidth, conf.captureHeight, gameW, gameH, capX, capY);
        const int capYGl = gameH - capY - conf.captureHeight;

        cacheEntry.sourceRects[i * 4 + 0] = static_cast<float>(capX) / gameW;
        cacheEntry.sourceRects[i * 4 + 1] = static_cast<float>(capYGl) / gameH;
        cacheEntry.sourceRects[i * 4 + 2] = sw;
        cacheEntry.sourceRects[i * 4 + 3] = sh;
    }

    return cacheEntry.sourceRects;
}

static int MT_UploadSourceRectInstances(MT_SourceRectGpuCacheEntry& gpuCache, const ThreadedMirrorConfig& conf, int gameW, int gameH,
                                        MT_RenderToBufferStateCache* stateCache) {
    const size_t instanceCount = conf.input.size();
    if (instanceCount == 0) { return 0; }

    if (gpuCache.instanceVbo == 0) {
        glGenBuffers(1, &gpuCache.instanceVbo);
        gpuCache.capacityBytes = 0;
        gpuCache.layoutHash = 0;
        gpuCache.gameW = 0;
        gpuCache.gameH = 0;
    }

    if (gpuCache.layoutHash != conf.sourceRectLayoutHash || gpuCache.gameW != gameW || gpuCache.gameH != gameH) {
        const std::vector<float>& sourceRects = MT_GetCachedSourceRects(conf, gameW, gameH);
        const size_t requiredBytes = sourceRects.size() * sizeof(float);
        glBindBuffer(GL_ARRAY_BUFFER, gpuCache.instanceVbo);
        if (gpuCache.capacityBytes < requiredBytes) {
            glBufferData(GL_ARRAY_BUFFER, requiredBytes, sourceRects.data(), GL_DYNAMIC_DRAW);
            gpuCache.capacityBytes = requiredBytes;
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, 0, requiredBytes, sourceRects.data());
        }

        gpuCache.layoutHash = conf.sourceRectLayoutHash;
        gpuCache.gameW = gameW;
        gpuCache.gameH = gameH;
    }

    MT_BindSourceRectInstanceBufferCached(gpuCache.instanceVbo, stateCache);

    return static_cast<int>(instanceCount);
}

static bool MT_CanRenderMirrorDirectToFinal(const ThreadedMirrorConfig& conf, bool useRawOutput, bool writeToBack) {
    if (writeToBack) { return false; }
    if (useRawOutput) { return true; }
    if (conf.borderType == MirrorBorderType::Static) { return true; }
    return conf.borderType == MirrorBorderType::Dynamic && conf.dynamicBorderThickness <= 0;
}

static bool MT_CanCompositeDynamicBorderOnScreen(const ThreadedMirrorConfig& conf, bool useRawOutput, bool writeToBack) {
    return !writeToBack && !useRawOutput && conf.borderType == MirrorBorderType::Dynamic && conf.dynamicBorderThickness == 1;
}

static bool MT_ShouldUseSeparableDynamicBorder(const ThreadedMirrorConfig& conf, bool useRawOutput) {
    return !useRawOutput && conf.borderType == MirrorBorderType::Dynamic && conf.dynamicBorderThickness > 1;
}

static bool MT_EnsureTempCaptureTexture(MirrorInstance* inst, int width, int height) {
    if (!inst || width <= 0 || height <= 0) { return false; }

    if (inst->tempCaptureTexture == 0) {
        glGenTextures(1, &inst->tempCaptureTexture);
        inst->tempCaptureTextureW = 0;
        inst->tempCaptureTextureH = 0;
    }

    if (inst->tempCaptureTextureW == width && inst->tempCaptureTextureH == height) { return true; }

    BindTextureDirect(GL_TEXTURE_2D, inst->tempCaptureTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    BindTextureDirect(GL_TEXTURE_2D, 0);

    inst->tempCaptureTextureW = width;
    inst->tempCaptureTextureH = height;
    return true;
}

static bool MT_RenderSeparableDynamicBorder(MirrorInstance* inst, const ThreadedMirrorConfig& conf, GLuint captureTexture,
                                            GLuint captureTempFbo, GLuint captureFinalFbo, GLuint* lastTempTextureId,
                                            GLuint finalTexture, int finalW, int finalH,
                                            MT_RenderToBufferStateCache* stateCache) {
    if (!inst || captureTempFbo == 0 || captureFinalFbo == 0 || finalTexture == 0 || !MT_EnsureTempCaptureTexture(inst, finalW, finalH)) {
        return false;
    }

    const float screenPixelX = 1.0f / static_cast<float>((std::max)(1, finalW));
    const float screenPixelY = 1.0f / static_cast<float>((std::max)(1, finalH));

    if (lastTempTextureId == nullptr || *lastTempTextureId != inst->tempCaptureTexture) {
        glBindFramebuffer(GL_FRAMEBUFFER, captureTempFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->tempCaptureTexture, 0);
        if (lastTempTextureId) { *lastTempTextureId = inst->tempCaptureTexture; }
        if (stateCache) {
            stateCache->framebufferValid = false;
            stateCache->textureValid = false;
        }
    }

    MT_BindFramebufferCached(captureTempFbo, stateCache);
    MT_SetViewportCached(0, 0, finalW, finalH, stateCache);
    MT_SetBlendEnabledCached(false, stateCache);

    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, captureTexture);
    MT_UseProgramCached(mt_dilateHorizontalProgram, stateCache);
    if (!stateCache || !stateCache->dilateHorizontalBorderWidthValid ||
        stateCache->dilateHorizontalBorderWidth != conf.dynamicBorderThickness) {
        glUniform1i(mt_dilateHorizontalShaderLocs.borderWidth, conf.dynamicBorderThickness);
        if (stateCache) {
            stateCache->dilateHorizontalBorderWidth = conf.dynamicBorderThickness;
            stateCache->dilateHorizontalBorderWidthValid = true;
        }
    }
    if (!stateCache || !stateCache->dilateHorizontalScreenPixelValid || stateCache->dilateHorizontalScreenPixelX != screenPixelX ||
        stateCache->dilateHorizontalScreenPixelY != screenPixelY) {
        glUniform2f(mt_dilateHorizontalShaderLocs.screenPixel, screenPixelX, screenPixelY);
        if (stateCache) {
            stateCache->dilateHorizontalScreenPixelX = screenPixelX;
            stateCache->dilateHorizontalScreenPixelY = screenPixelY;
            stateCache->dilateHorizontalScreenPixelValid = true;
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);

    MT_BindFramebufferCached(captureFinalFbo, stateCache);

    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, captureTexture);
    glActiveTexture(GL_TEXTURE1);
    BindTextureDirect(GL_TEXTURE_2D, inst->tempCaptureTexture);

    if (conf.colorPassthrough) {
        MT_UseProgramCached(mt_dilateVerticalPassthroughProgram, stateCache);
        if (!stateCache || !stateCache->dilateVerticalPassthroughBorderWidthValid ||
            stateCache->dilateVerticalPassthroughBorderWidth != conf.dynamicBorderThickness) {
            glUniform1i(mt_dilateVerticalPassthroughShaderLocs.borderWidth, conf.dynamicBorderThickness);
            if (stateCache) {
                stateCache->dilateVerticalPassthroughBorderWidth = conf.dynamicBorderThickness;
                stateCache->dilateVerticalPassthroughBorderWidthValid = true;
            }
        }
        if (!stateCache || !stateCache->dilateVerticalPassthroughScreenPixelValid ||
            stateCache->dilateVerticalPassthroughScreenPixelX != screenPixelX ||
            stateCache->dilateVerticalPassthroughScreenPixelY != screenPixelY) {
            glUniform2f(mt_dilateVerticalPassthroughShaderLocs.screenPixel, screenPixelX, screenPixelY);
            if (stateCache) {
                stateCache->dilateVerticalPassthroughScreenPixelX = screenPixelX;
                stateCache->dilateVerticalPassthroughScreenPixelY = screenPixelY;
                stateCache->dilateVerticalPassthroughScreenPixelValid = true;
            }
        }
        if (!stateCache || !stateCache->dilateVerticalPassthroughBorderColorValid ||
            !MT_ColorEquals(stateCache->dilateVerticalPassthroughBorderColor, conf.borderColor)) {
            glUniform4f(mt_dilateVerticalPassthroughShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g,
                        conf.borderColor.b, conf.borderColor.a);
            if (stateCache) {
                stateCache->dilateVerticalPassthroughBorderColor = conf.borderColor;
                stateCache->dilateVerticalPassthroughBorderColorValid = true;
            }
        }
    } else {
        MT_UseProgramCached(mt_dilateVerticalProgram, stateCache);
        if (!stateCache || !stateCache->dilateVerticalBorderWidthValid ||
            stateCache->dilateVerticalBorderWidth != conf.dynamicBorderThickness) {
            glUniform1i(mt_dilateVerticalShaderLocs.borderWidth, conf.dynamicBorderThickness);
            if (stateCache) {
                stateCache->dilateVerticalBorderWidth = conf.dynamicBorderThickness;
                stateCache->dilateVerticalBorderWidthValid = true;
            }
        }
        if (!stateCache || !stateCache->dilateVerticalScreenPixelValid || stateCache->dilateVerticalScreenPixelX != screenPixelX ||
            stateCache->dilateVerticalScreenPixelY != screenPixelY) {
            glUniform2f(mt_dilateVerticalShaderLocs.screenPixel, screenPixelX, screenPixelY);
            if (stateCache) {
                stateCache->dilateVerticalScreenPixelX = screenPixelX;
                stateCache->dilateVerticalScreenPixelY = screenPixelY;
                stateCache->dilateVerticalScreenPixelValid = true;
            }
        }
        if (!stateCache || !stateCache->dilateVerticalOutputColorValid ||
            !MT_ColorEquals(stateCache->dilateVerticalOutputColor, conf.outputColor)) {
            glUniform4f(mt_dilateVerticalShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                        conf.outputColor.a);
            if (stateCache) {
                stateCache->dilateVerticalOutputColor = conf.outputColor;
                stateCache->dilateVerticalOutputColorValid = true;
            }
        }
        if (!stateCache || !stateCache->dilateVerticalBorderColorValid ||
            !MT_ColorEquals(stateCache->dilateVerticalBorderColor, conf.borderColor)) {
            glUniform4f(mt_dilateVerticalShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                        conf.borderColor.a);
            if (stateCache) {
                stateCache->dilateVerticalBorderColor = conf.borderColor;
                stateCache->dilateVerticalBorderColorValid = true;
            }
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glActiveTexture(GL_TEXTURE1);
    BindTextureDirect(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    BindTextureDirect(GL_TEXTURE_2D, finalTexture);
    if (stateCache) { stateCache->textureValid = false; }
    return true;
}

static bool RenderMirrorToBuffer(MirrorInstance* inst, const ThreadedMirrorConfig& conf, GLuint validCopyTexture, GLuint captureVAO,
                                 GLuint captureVBO, MT_SourceRectGpuCacheEntry& sourceRectGpuCache, GLuint captureFbo,
                                 GLuint captureTempFbo, GLuint* captureTempTextureId, GLuint captureFinalFbo,
                                 MirrorGammaMode gammaMode, int gameW, int gameH, bool writeToBack,
                                 MT_RenderToBufferStateCache* stateCache = nullptr,
                                 bool fixedStateAlreadyPrepared = false) {
    PROFILE_SCOPE_CAT("Capture Single Mirror", "Mirror Thread");

    const GLuint captureTexture = writeToBack ? inst->fboTextureBack : inst->fboTexture;
    const GLuint finalTexture = writeToBack ? inst->finalTextureBack : inst->finalTexture;
    const int finalW = writeToBack ? inst->final_w_back : inst->final_w;
    const int finalH = writeToBack ? inst->final_h_back : inst->final_h;

    bool useRawOutput = inst->desiredRawOutput.load(std::memory_order_acquire);
    bool useColorPassthrough = conf.colorPassthrough;
    const bool renderDirectToFinal =
        captureFinalFbo != 0 && finalTexture != 0 && MT_CanRenderMirrorDirectToFinal(conf, useRawOutput, writeToBack);

    const GLuint initialTargetFbo = renderDirectToFinal ? captureFinalFbo : captureFbo;
    const int initialTargetW = renderDirectToFinal ? finalW : inst->fbo_w;
    const int initialTargetH = renderDirectToFinal ? finalH : inst->fbo_h;

    MT_BindFramebufferCached(initialTargetFbo, stateCache);
    MT_SetViewportCached(0, 0, initialTargetW, initialTargetH, stateCache);

    if (!fixedStateAlreadyPrepared) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glActiveTexture(GL_TEXTURE0);
        glBindVertexArray(captureVAO);
        glBindBuffer(GL_ARRAY_BUFFER, captureVBO);
    }

    MT_SetClearColorCached(0.0f, 0.0f, 0.0f, (renderDirectToFinal && useRawOutput) ? 1.0f : 0.0f, stateCache);
    glClear(GL_COLOR_BUFFER_BIT);

    MT_BindTextureCached(validCopyTexture, stateCache);

    if (useRawOutput) {
        MT_UseProgramCached(mt_passthroughProgram, stateCache);
    } else if (useColorPassthrough) {
        const int colorCount = (std::min)(static_cast<int>(conf.targetColors.size()), 8);
        std::array<float, 24> targetColorData{};
        for (int i = 0; i < colorCount; ++i) {
            targetColorData[static_cast<size_t>(i) * 3 + 0] = conf.targetColors[i].r;
            targetColorData[static_cast<size_t>(i) * 3 + 1] = conf.targetColors[i].g;
            targetColorData[static_cast<size_t>(i) * 3 + 2] = conf.targetColors[i].b;
        }

        MT_UseProgramCached(mt_filterPassthroughProgram, stateCache);
        if (mt_filterPassthroughShaderLocs.gammaMode >= 0 &&
            (!stateCache || !stateCache->filterPassthroughGammaModeValid ||
             stateCache->filterPassthroughGammaMode != static_cast<int>(gammaMode))) {
            glUniform1i(mt_filterPassthroughShaderLocs.gammaMode, static_cast<int>(gammaMode));
            if (stateCache) {
                stateCache->filterPassthroughGammaMode = static_cast<int>(gammaMode);
                stateCache->filterPassthroughGammaModeValid = true;
            }
        }

        if (!stateCache || !stateCache->filterPassthroughTargetColorStateValid ||
            stateCache->filterPassthroughTargetColorCount != colorCount ||
            !std::equal(targetColorData.begin(), targetColorData.begin() + (static_cast<size_t>(colorCount) * 3),
                        stateCache->filterPassthroughTargetColors.begin())) {
            glUniform1i(mt_filterPassthroughShaderLocs.targetColorCount, colorCount);
            if (colorCount > 0) {
                glUniform3fv(mt_filterPassthroughShaderLocs.targetColors, colorCount, targetColorData.data());
            }
            if (stateCache) {
                stateCache->filterPassthroughTargetColorCount = colorCount;
                stateCache->filterPassthroughTargetColors = targetColorData;
                stateCache->filterPassthroughTargetColorStateValid = true;
            }
        }

        if (!stateCache || !stateCache->filterPassthroughSensitivityValid ||
            stateCache->filterPassthroughSensitivity != conf.colorSensitivity) {
            glUniform1f(mt_filterPassthroughShaderLocs.sensitivity, conf.colorSensitivity);
            if (stateCache) {
                stateCache->filterPassthroughSensitivity = conf.colorSensitivity;
                stateCache->filterPassthroughSensitivityValid = true;
            }
        }
    } else {
        const int colorCount = (std::min)(static_cast<int>(conf.targetColors.size()), 8);
        std::array<float, 24> targetColorData{};
        for (int i = 0; i < colorCount; ++i) {
            targetColorData[static_cast<size_t>(i) * 3 + 0] = conf.targetColors[i].r;
            targetColorData[static_cast<size_t>(i) * 3 + 1] = conf.targetColors[i].g;
            targetColorData[static_cast<size_t>(i) * 3 + 2] = conf.targetColors[i].b;
        }

        MT_UseProgramCached(mt_filterProgram, stateCache);
        if (mt_filterShaderLocs.gammaMode >= 0 &&
            (!stateCache || !stateCache->filterGammaModeValid || stateCache->filterGammaMode != static_cast<int>(gammaMode))) {
            glUniform1i(mt_filterShaderLocs.gammaMode, static_cast<int>(gammaMode));
            if (stateCache) {
                stateCache->filterGammaMode = static_cast<int>(gammaMode);
                stateCache->filterGammaModeValid = true;
            }
        }

        if (!stateCache || !stateCache->filterTargetColorStateValid || stateCache->filterTargetColorCount != colorCount ||
            !std::equal(targetColorData.begin(), targetColorData.begin() + (static_cast<size_t>(colorCount) * 3),
                        stateCache->filterTargetColors.begin())) {
            glUniform1i(mt_filterShaderLocs.targetColorCount, colorCount);
            if (colorCount > 0) {
                glUniform3fv(mt_filterShaderLocs.targetColors, colorCount, targetColorData.data());
            }
            if (stateCache) {
                stateCache->filterTargetColorCount = colorCount;
                stateCache->filterTargetColors = targetColorData;
                stateCache->filterTargetColorStateValid = true;
            }
        }

        if (!stateCache || !stateCache->filterOutputColorValid || !MT_ColorEquals(stateCache->filterOutputColor, conf.outputColor)) {
            glUniform4f(mt_filterShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                        conf.outputColor.a);
            if (stateCache) {
                stateCache->filterOutputColor = conf.outputColor;
                stateCache->filterOutputColorValid = true;
            }
        }
        if (!stateCache || !stateCache->filterSensitivityValid || stateCache->filterSensitivity != conf.colorSensitivity) {
            glUniform1f(mt_filterShaderLocs.sensitivity, conf.colorSensitivity);
            if (stateCache) {
                stateCache->filterSensitivity = conf.colorSensitivity;
                stateCache->filterSensitivityValid = true;
            }
        }
    }

    if (useRawOutput) {
        MT_SetBlendEnabledCached(false, stateCache);
    } else {
        MT_SetBlendEnabledCached(true, stateCache);
    }

    int padding = renderDirectToFinal ? 0 : ((conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0);
    const int drawWidth = renderDirectToFinal ? finalW : conf.captureWidth;
    const int drawHeight = renderDirectToFinal ? finalH : conf.captureHeight;
    MT_SetViewportCached(padding, padding, drawWidth, drawHeight, stateCache);
    const int instanceCount = MT_UploadSourceRectInstances(sourceRectGpuCache, conf, gameW, gameH, stateCache);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instanceCount);

    MT_SetBlendEnabledCached(false, stateCache);

    // Uses async PBO readback: previous frame's result is harvested (non-blocking), then a
    if (useRawOutput) {
        if (writeToBack) {
            inst->hasFrameContentBack = true;
        } else {
            inst->hasFrameContent = true;
        }
    }

    if (renderDirectToFinal || MT_CanCompositeDynamicBorderOnScreen(conf, useRawOutput, writeToBack)) { return true; }

    // This produces screen-ready content so render thread just needs to blit.
    // Use the mirror-thread-local FBO (framebuffer objects may not be shared across contexts).
    if (captureFinalFbo != 0 && finalTexture != 0) {
        PROFILE_SCOPE_CAT("Apply Border Shader", "Mirror Thread");

        if (MT_ShouldUseSeparableDynamicBorder(conf, useRawOutput)) {
            return MT_RenderSeparableDynamicBorder(inst, conf, captureTexture, captureTempFbo, captureFinalFbo,
                                                  captureTempTextureId, finalTexture, finalW, finalH, stateCache);
        }

        if (useRawOutput) {
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);

            MT_BindTextureCached(captureTexture, stateCache);
            MT_UseProgramCached(mt_backgroundProgram, stateCache);
            if (!stateCache || !stateCache->backgroundOpacityValid || stateCache->backgroundOpacity != 1.0f) {
                glUniform1f(mt_backgroundShaderLocs.opacity, 1.0f);
                if (stateCache) {
                    stateCache->backgroundOpacity = 1.0f;
                    stateCache->backgroundOpacityValid = true;
                }
            }
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else if (conf.borderType == MirrorBorderType::Static) {
            // Static border will be rendered later in render_thread.cpp on top of the mirror
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);

            MT_BindTextureCached(captureTexture, stateCache);
            MT_UseProgramCached(mt_backgroundProgram, stateCache);
            if (!stateCache || !stateCache->backgroundOpacityValid || stateCache->backgroundOpacity != 1.0f) {
                glUniform1f(mt_backgroundShaderLocs.opacity, 1.0f);
                if (stateCache) {
                    stateCache->backgroundOpacity = 1.0f;
                    stateCache->backgroundOpacityValid = true;
                }
            }
            glDrawArrays(GL_TRIANGLES, 0, 6);
        } else {
            MT_BindFramebufferCached(captureFinalFbo, stateCache);
            MT_SetViewportCached(0, 0, finalW, finalH, stateCache);
            MT_SetClearColorCached(0.0f, 0.0f, 0.0f, 0.0f, stateCache);
            glClear(GL_COLOR_BUFFER_BIT);

            MT_BindTextureCached(captureTexture, stateCache);
            if (useColorPassthrough) {
                MT_UseProgramCached(mt_renderPassthroughProgram, stateCache);
                if (!stateCache || !stateCache->renderPassthroughBorderWidthValid ||
                    stateCache->renderPassthroughBorderWidth != conf.dynamicBorderThickness) {
                    glUniform1i(mt_renderPassthroughShaderLocs.borderWidth, conf.dynamicBorderThickness);
                    if (stateCache) {
                        stateCache->renderPassthroughBorderWidth = conf.dynamicBorderThickness;
                        stateCache->renderPassthroughBorderWidthValid = true;
                    }
                }
                if (!stateCache || !stateCache->renderPassthroughBorderColorValid ||
                    !MT_ColorEquals(stateCache->renderPassthroughBorderColor, conf.borderColor)) {
                    glUniform4f(mt_renderPassthroughShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g,
                                conf.borderColor.b, conf.borderColor.a);
                    if (stateCache) {
                        stateCache->renderPassthroughBorderColor = conf.borderColor;
                        stateCache->renderPassthroughBorderColorValid = true;
                    }
                }
                const float screenPixelX = 1.0f / finalW;
                const float screenPixelY = 1.0f / finalH;
                if (!stateCache || !stateCache->renderPassthroughScreenPixelValid ||
                    stateCache->renderPassthroughScreenPixelX != screenPixelX ||
                    stateCache->renderPassthroughScreenPixelY != screenPixelY) {
                    glUniform2f(mt_renderPassthroughShaderLocs.screenPixel, screenPixelX, screenPixelY);
                    if (stateCache) {
                        stateCache->renderPassthroughScreenPixelX = screenPixelX;
                        stateCache->renderPassthroughScreenPixelY = screenPixelY;
                        stateCache->renderPassthroughScreenPixelValid = true;
                    }
                }
            } else {
                MT_UseProgramCached(mt_renderProgram, stateCache);
                if (!stateCache || !stateCache->renderBorderWidthValid || stateCache->renderBorderWidth != conf.dynamicBorderThickness) {
                    glUniform1i(mt_renderShaderLocs.borderWidth, conf.dynamicBorderThickness);
                    if (stateCache) {
                        stateCache->renderBorderWidth = conf.dynamicBorderThickness;
                        stateCache->renderBorderWidthValid = true;
                    }
                }
                if (!stateCache || !stateCache->renderOutputColorValid || !MT_ColorEquals(stateCache->renderOutputColor, conf.outputColor)) {
                    glUniform4f(mt_renderShaderLocs.outputColor, conf.outputColor.r, conf.outputColor.g, conf.outputColor.b,
                                conf.outputColor.a);
                    if (stateCache) {
                        stateCache->renderOutputColor = conf.outputColor;
                        stateCache->renderOutputColorValid = true;
                    }
                }
                if (!stateCache || !stateCache->renderBorderColorValid || !MT_ColorEquals(stateCache->renderBorderColor, conf.borderColor)) {
                    glUniform4f(mt_renderShaderLocs.borderColor, conf.borderColor.r, conf.borderColor.g, conf.borderColor.b,
                                conf.borderColor.a);
                    if (stateCache) {
                        stateCache->renderBorderColor = conf.borderColor;
                        stateCache->renderBorderColorValid = true;
                    }
                }
                const float screenPixelX = 1.0f / finalW;
                const float screenPixelY = 1.0f / finalH;
                if (!stateCache || !stateCache->renderScreenPixelValid || stateCache->renderScreenPixelX != screenPixelX ||
                    stateCache->renderScreenPixelY != screenPixelY) {
                    glUniform2f(mt_renderShaderLocs.screenPixel, screenPixelX, screenPixelY);
                    if (stateCache) {
                        stateCache->renderScreenPixelX = screenPixelX;
                        stateCache->renderScreenPixelY = screenPixelY;
                        stateCache->renderScreenPixelValid = true;
                    }
                }
            }

            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // NOTE: Static border is rendered in render_thread.cpp after mirror compositing
    }

    return true;
}

// Mirror-thread local FBOs.
// Framebuffer objects are not reliably shared between WGL contexts across all drivers.
struct MT_MirrorFbos {
    GLuint backFbo = 0;
    GLuint tempBackFbo = 0;
    GLuint finalBackFbo = 0;
    GLuint lastBackTex = 0;
    GLuint lastTempTex = 0;
    GLuint lastFinalBackTex = 0;

    // Async PBO for content detection (replaces synchronous glReadPixels)
    // Frame N+1: read back results from PBO (non-blocking) before starting new readback
    GLuint contentDetectionPBO = 0;
    int contentPBOWidth = 0;
    int contentPBOHeight = 0;
    bool contentReadbackPending = false;
    GLsync contentReadbackFence = nullptr; // Fence for the async readback

    GLuint contentDownsampleFbo = 0;
    GLuint contentDownsampleTex = 0;
    int contentDownW = 0;
    int contentDownH = 0;
};

static void MT_ReleaseContentDetectionResources(MT_MirrorFbos& fb) {
    if (fb.contentReadbackFence && glIsSync(fb.contentReadbackFence)) { glDeleteSync(fb.contentReadbackFence); }
    fb.contentReadbackFence = nullptr;
    fb.contentReadbackPending = false;

    if (fb.contentDetectionPBO) {
        glDeleteBuffers(1, &fb.contentDetectionPBO);
        fb.contentDetectionPBO = 0;
    }
    fb.contentPBOWidth = 0;
    fb.contentPBOHeight = 0;

    if (fb.contentDownsampleFbo) {
        glDeleteFramebuffers(1, &fb.contentDownsampleFbo);
        fb.contentDownsampleFbo = 0;
    }
    if (fb.contentDownsampleTex) {
        glDeleteTextures(1, &fb.contentDownsampleTex);
        fb.contentDownsampleTex = 0;
    }
    fb.contentDownW = 0;
    fb.contentDownH = 0;
}

static void MT_DeleteMirrorFbos(MT_MirrorFbos& fb) {
    if (fb.backFbo) { glDeleteFramebuffers(1, &fb.backFbo); }
    if (fb.tempBackFbo) { glDeleteFramebuffers(1, &fb.tempBackFbo); }
    if (fb.finalBackFbo) { glDeleteFramebuffers(1, &fb.finalBackFbo); }
    fb.backFbo = 0;
    fb.tempBackFbo = 0;
    fb.finalBackFbo = 0;
    fb.lastBackTex = 0;
    fb.lastTempTex = 0;
    fb.lastFinalBackTex = 0;

    MT_ReleaseContentDetectionResources(fb);
}

static bool MT_MirrorNeedsContentDetection(const ThreadedMirrorConfig& conf, bool useRawOutput) {
    return !useRawOutput && conf.borderType == MirrorBorderType::Static && conf.staticBorderThickness > 0;
}

static bool MT_HarvestContentReadback(MT_MirrorFbos& fb, bool& outHasContent) {
    if (!fb.contentReadbackPending || !fb.contentReadbackFence) { return false; }

    GLenum fenceStatus = glClientWaitSync(fb.contentReadbackFence, 0, 0);
    if (fenceStatus != GL_ALREADY_SIGNALED && fenceStatus != GL_CONDITION_SATISFIED) { return false; }

    bool hasContent = false;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, fb.contentDetectionPBO);
    const unsigned char* mapped = static_cast<const unsigned char*>(
        glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, fb.contentPBOWidth * fb.contentPBOHeight * 4, GL_MAP_READ_BIT));
    if (mapped) {
        const int w = fb.contentPBOWidth;
        const int h = fb.contentPBOHeight;
        for (int y = 0; y < h && !hasContent; ++y) {
            const unsigned char* row = mapped + (static_cast<size_t>(y) * w * 4);
            for (int x = 0; x < w; ++x) {
                if (row[(static_cast<size_t>(x) * 4) + 3] > 0) {
                    hasContent = true;
                    break;
                }
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (glIsSync(fb.contentReadbackFence)) { glDeleteSync(fb.contentReadbackFence); }
    fb.contentReadbackFence = nullptr;
    fb.contentReadbackPending = false;

    outHasContent = hasContent;
    return true;
}

static void MT_QueueContentReadback(MT_MirrorFbos& fb, GLuint sourceFbo, int sourceW, int sourceH) {
    if (sourceFbo == 0 || sourceW <= 0 || sourceH <= 0) {
        MT_ReleaseContentDetectionResources(fb);
        return;
    }

    constexpr int kDetectMax = 64;
    const int detW = (std::min)(sourceW, kDetectMax);
    const int detH = (std::min)(sourceH, kDetectMax);
    if (detW <= 0 || detH <= 0) {
        MT_ReleaseContentDetectionResources(fb);
        return;
    }

    if ((fb.contentDownsampleFbo == 0) || (fb.contentDownsampleTex == 0) || (fb.contentDownW != detW) || (fb.contentDownH != detH)) {
        if (fb.contentDownsampleFbo == 0) { glGenFramebuffers(1, &fb.contentDownsampleFbo); }
        if (fb.contentDownsampleTex == 0) { glGenTextures(1, &fb.contentDownsampleTex); }
        BindTextureDirect(GL_TEXTURE_2D, fb.contentDownsampleTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, detW, detH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        BindTextureDirect(GL_TEXTURE_2D, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, fb.contentDownsampleFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.contentDownsampleTex, 0);
        fb.contentDownW = detW;
        fb.contentDownH = detH;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (fb.contentDetectionPBO == 0 || fb.contentPBOWidth != detW || fb.contentPBOHeight != detH) {
        if (fb.contentDetectionPBO == 0) { glGenBuffers(1, &fb.contentDetectionPBO); }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, fb.contentDetectionPBO);
        glBufferData(GL_PIXEL_PACK_BUFFER, detW * detH * 4, nullptr, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        fb.contentPBOWidth = detW;
        fb.contentPBOHeight = detH;
    }

    if (fb.contentReadbackFence && glIsSync(fb.contentReadbackFence)) { glDeleteSync(fb.contentReadbackFence); }
    fb.contentReadbackFence = nullptr;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb.contentDownsampleFbo);
    glBlitFramebuffer(0, 0, sourceW, sourceH, 0, 0, detW, detH, GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb.contentDownsampleFbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, fb.contentDetectionPBO);
    glReadPixels(0, 0, detW, detH, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    fb.contentReadbackFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    fb.contentReadbackPending = (fb.contentReadbackFence != nullptr);
}

static std::unordered_map<std::string, MT_MirrorFbos> g_sameThreadMirrorFbos;
static std::unordered_map<std::string, MT_SourceRectGpuCacheEntry> g_sameThreadSourceRectGpuCaches;
static GLuint g_sameThreadCaptureVAO = 0;
static GLuint g_sameThreadCaptureVBO = 0;

static void EnsureSameThreadMirrorCaptureResources() {
    if (g_sameThreadCaptureVAO != 0 && g_sameThreadCaptureVBO != 0) { return; }

    glGenVertexArrays(1, &g_sameThreadCaptureVAO);
    glGenBuffers(1, &g_sameThreadCaptureVBO);
    glBindVertexArray(g_sameThreadCaptureVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_sameThreadCaptureVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);

    static const float verts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

static std::string MT_NormalizeCaptureAnchor(const std::string& relativeTo) {
    if (relativeTo.length() > 8 && relativeTo.substr(relativeTo.length() - 8) == "Viewport") {
        return relativeTo.substr(0, relativeTo.length() - 8);
    }
    if (relativeTo.length() > 6 && relativeTo.substr(relativeTo.length() - 6) == "Screen") {
        return relativeTo.substr(0, relativeTo.length() - 6);
    }
    return relativeTo;
}

static std::vector<MirrorCaptureConfig> MT_NormalizeCaptureInputs(const std::vector<MirrorCaptureConfig>& inputRegions) {
    std::vector<MirrorCaptureConfig> normalized = inputRegions;
    for (auto& region : normalized) {
        region.relativeTo = MT_NormalizeCaptureAnchor(region.relativeTo);
    }
    return normalized;
}

static void MT_CopyNormalizedCaptureInputs(const std::vector<MirrorCaptureConfig>& inputRegions,
                                           std::vector<MirrorCaptureConfig>& outRegions) {
    outRegions.resize(inputRegions.size());
    for (size_t i = 0; i < inputRegions.size(); ++i) {
        outRegions[i] = inputRegions[i];
        outRegions[i].relativeTo = MT_NormalizeCaptureAnchor(inputRegions[i].relativeTo);
    }
}

static void MT_GetRelativeCoordsNormalized(const std::string& anchor, int relX, int relY, int w, int h, int containerW,
                                           int containerH, int& outX, int& outY) {
    char firstChar = anchor.empty() ? '\0' : anchor[0];

    if (firstChar == 't') {
        outY = relY;
        outX = (anchor == "topLeft") ? relX : containerW - w - relX;
    } else if (firstChar == 'c') {
        outX = (containerW - w) / 2 + relX;
        outY = (containerH - h) / 2 + relY;
    } else if (firstChar == 'p') {
        const int pieYTop = 220;
        const int pieXLeft = 92;
        const int pieXRight = 36;
        int baseX = (anchor == "pieLeft") ? containerW - pieXLeft : containerW - pieXRight;
        outX = baseX + relX;
        outY = containerH - pieYTop + relY;
    } else {
        outY = containerH - h - relY;
        outX = (anchor == "bottomRight") ? containerW - w - relX : relX;
    }
}

static void PopulateThreadedMirrorConfig(ThreadedMirrorConfig& conf, const MirrorConfig& mirror) {
    conf.name = mirror.name;
    conf.captureWidth = mirror.captureWidth;
    conf.captureHeight = mirror.captureHeight;
    conf.borderType = mirror.border.type;
    conf.dynamicBorderThickness = mirror.border.dynamicThickness;
    conf.staticBorderShape = mirror.border.staticShape;
    conf.staticBorderColor = mirror.border.staticColor;
    conf.staticBorderThickness = mirror.border.staticThickness;
    conf.staticBorderRadius = mirror.border.staticRadius;
    conf.staticBorderOffsetX = mirror.border.staticOffsetX;
    conf.staticBorderOffsetY = mirror.border.staticOffsetY;
    conf.staticBorderWidth = mirror.border.staticWidth;
    conf.staticBorderHeight = mirror.border.staticHeight;
    conf.fps = mirror.fps;
    conf.rawOutput = mirror.rawOutput;
    conf.colorPassthrough = mirror.colorPassthrough;
    conf.targetColors = mirror.colors.targetColors;
    conf.outputColor = mirror.colors.output;
    conf.borderColor = mirror.colors.border;
    conf.colorSensitivity = mirror.colorSensitivity;
    MT_CopyNormalizedCaptureInputs(mirror.input, conf.input);
    conf.outputScale = mirror.output.scale;
    conf.outputSeparateScale = mirror.output.separateScale;
    conf.outputScaleX = mirror.output.scaleX;
    conf.outputScaleY = mirror.output.scaleY;
    conf.outputX = mirror.output.x;
    conf.outputY = mirror.output.y;
    conf.outputRelativeTo = mirror.output.relativeTo;
    conf.sourceRectLayoutHash = MT_ComputeSourceRectLayoutHash(conf);
}

void BuildThreadedMirrorConfigs(const std::vector<MirrorConfig>& activeMirrors, std::vector<ThreadedMirrorConfig>& outConfigs) {
    outConfigs.resize(activeMirrors.size());

    for (size_t i = 0; i < activeMirrors.size(); ++i) {
        PopulateThreadedMirrorConfig(outConfigs[i], activeMirrors[i]);
    }
}

static void MirrorCaptureThreadFunc(void* unused) {
    _set_se_translator(SEHTranslator);

    try {
        Log("Mirror Capture Thread: Starting thread loop...");

        // Context should already be created and shared by StartMirrorCaptureThread on main thread
        if (!g_mirrorCaptureDC || !g_mirrorCaptureContext) {
            Log("Mirror Capture Thread: Missing pre-created context or DC");
            g_mirrorCaptureRunning.store(false);
            return;
        }

        // Make context current on this thread
        if (!wglMakeCurrent(g_mirrorCaptureDC, g_mirrorCaptureContext)) {
            Log("Mirror Capture Thread: Failed to make context current (error " + std::to_string(GetLastError()) + ")");
            g_mirrorCaptureRunning.store(false);
            return;
        }

        // Initialize GLEW on this thread's context
        if (glewInit() != GLEW_OK) {
            Log("Mirror Capture Thread: GLEW init failed");
            wglMakeCurrent(NULL, NULL);
            g_mirrorCaptureRunning.store(false);
            return;
        }

        if (!MT_InitializeShaders()) {
            Log("Mirror Capture Thread: Failed to initialize shaders");
            wglMakeCurrent(NULL, NULL);
            g_mirrorCaptureRunning.store(false);
            return;
        }

        MT_LogSharedContextHealthOnce();

        Log("Mirror Capture Thread: Thread loop running");

        GLuint captureVAO = 0, captureVBO = 0;
        glGenVertexArrays(1, &captureVAO);
        glGenBuffers(1, &captureVBO);
        glBindVertexArray(captureVAO);
        glBindBuffer(GL_ARRAY_BUFFER, captureVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(2);
        glVertexAttribDivisor(2, 1);

        static const float verts[] = { -1, -1, 0, 0, 1, -1, 1, 0, 1, 1, 1, 1, -1, -1, 0, 0, 1, 1, 1, 1, -1, 1, 0, 1 };
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

        // The render thread already blitted the game texture to g_copyTextures via GPU-to-GPU copy
        GLuint validTexture = 0;
        int validW = 0, validH = 0;
        bool hasValidTexture = false;


        std::unordered_map<std::string, MT_MirrorFbos> mt_fbos;
        std::unordered_map<std::string, MT_SourceRectGpuCacheEntry> sourceRectGpuCaches;

        uint64_t cachedConfigVersion = 0;
        std::vector<ThreadedMirrorConfig> configsCache;
        std::vector<std::chrono::steady_clock::time_point> lastCaptureTimes;

        GLuint debugSampleFbo = 0;
        auto debugSamplePixel = [&](const ThreadedMirrorConfig& conf, GLuint srcTex, int gameW, int gameH) {
            auto snap = GetConfigSnapshot();
            if (!snap || !snap->debug.logTextureOps) return;
            if (srcTex == 0 || gameW <= 0 || gameH <= 0) return;
            if (conf.input.empty()) return;

            // Rate limit: once every ~2 seconds at 60fps (per thread, not per mirror)
            static int s_sampleCounter = 0;
            if ((++s_sampleCounter % 120) != 0) return;

            if (debugSampleFbo == 0) { glGenFramebuffers(1, &debugSampleFbo); }

            const auto& r = conf.input[0];
            int capX = 0, capY = 0;
            MT_GetRelativeCoordsNormalized(r.relativeTo, r.x, r.y, conf.captureWidth, conf.captureHeight, gameW, gameH, capX, capY);
            int capY_gl = gameH - capY - conf.captureHeight;
            int sampleX = capX + conf.captureWidth / 2;
            int sampleY = capY_gl + conf.captureHeight / 2;
            if (sampleX < 0) sampleX = 0;
            if (sampleY < 0) sampleY = 0;
            if (sampleX >= gameW) sampleX = gameW - 1;
            if (sampleY >= gameH) sampleY = gameH - 1;

            GLint prevReadFbo = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, debugSampleFbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
            GLenum st = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) {
                LogCategory("texture_ops",
                            "MirrorDebugSample: READ FBO incomplete for mirror '" + conf.name + "' (status " + std::to_string(st) +
                                ") tex=" + std::to_string(srcTex));
                glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);
                return;
            }

            unsigned char px[4] = { 0, 0, 0, 0 };
            glReadPixels(sampleX, sampleY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFbo);

            int tR = -1, tG = -1, tB = -1;
            if (!conf.targetColors.empty()) {
                tR = (int)std::round(conf.targetColors[0].r * 255.0f);
                tG = (int)std::round(conf.targetColors[0].g * 255.0f);
                tB = (int)std::round(conf.targetColors[0].b * 255.0f);
            }

            MirrorGammaMode gm = GetGlobalMirrorGammaMode();
            LogCategory("texture_ops",
                        "MirrorDebugSample: '" + conf.name + "' sample(" + std::to_string(sampleX) + "," + std::to_string(sampleY) +
                            ") rgba=" + std::to_string((int)px[0]) + "," + std::to_string((int)px[1]) + "," +
                            std::to_string((int)px[2]) + "," + std::to_string((int)px[3]) +
                            " target0=" + std::to_string(tR) + "," + std::to_string(tG) + "," + std::to_string(tB) +
                            " sens=" + std::to_string(conf.colorSensitivity) + " gammaMode=" + std::to_string((int)gm));
        };

        while (!g_mirrorCaptureShouldStop.load()) {
            PROFILE_SCOPE_CAT("Mirror Capture Thread Frame", "Mirror Thread");

            auto now = std::chrono::steady_clock::now();

            // === PHASE 1: Check for new frame captures from render thread ===
            FrameCaptureNotification notif = {};
            bool hasNotification = false;
            {
                PROFILE_SCOPE_CAT("Check Queue", "Mirror Thread");
                // Lock-free pop from ring buffer
                hasNotification = CaptureQueuePop(notif);

                // If the producer is faster than this thread, keep only the newest frame.
                // This reduces fence waits + mirror work when the game runs > mirror FPS.
                if (hasNotification) {
                    FrameCaptureNotification newer = {};
                    while (CaptureQueuePop(newer)) {
                        if (notif.fence && glIsSync(notif.fence)) { glDeleteSync(notif.fence); }
                        notif = newer;
                    }
                }
            }

            if (!hasNotification) {
                const bool hasConfigs = (g_activeMirrorCaptureCount.load(std::memory_order_acquire) > 0);
                const auto waitTime = (!hasValidTexture && !hasConfigs) ? std::chrono::milliseconds(100) : std::chrono::milliseconds(16);
                std::unique_lock<std::mutex> lk(g_captureSignalMutex);
                g_captureSignalCV.wait_for(lk, waitTime, [] {
                    if (g_mirrorCaptureShouldStop.load()) return true;
                    return g_captureQueueTail.load(std::memory_order_relaxed) != g_captureQueueHead.load(std::memory_order_acquire);
                });
                continue;
            }

            if (hasNotification) {
                PROFILE_SCOPE_CAT("Process Frame Capture", "Mirror Thread");

                // Wait for the async blit to complete (fence created by SubmitFrameCapture)
                GLenum waitResult;
                {
                    PROFILE_SCOPE_CAT("Waiting for GPU Blit", "Mirror Thread");
                    if (!notif.fence || !glIsSync(notif.fence)) {
                        // Invalid fence (can happen across context recreation). Skip this notification.
                        waitResult = GL_WAIT_FAILED;
                    } else {
                    // Wait in short slices so the thread remains responsive to stop requests.
                    // Flush once (first iteration) to ensure the fence becomes visible.
                    GLbitfield flags = GL_SYNC_FLUSH_COMMANDS_BIT;
                    do {
                        waitResult = glClientWaitSync(notif.fence, flags, 5'000'000ULL);
                        flags = 0;
                        if (g_mirrorCaptureShouldStop.load(std::memory_order_relaxed)) { break; }
                    } while (waitResult == GL_TIMEOUT_EXPIRED);
                    }
                    if (notif.fence && glIsSync(notif.fence)) { glDeleteSync(notif.fence); }
                }

                if (waitResult == GL_WAIT_FAILED) {
                    Log("Mirror Capture Thread: Fence wait failed");
                } else {
                    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);

                    // Use the texture index from the notification (fixes race condition where
                    int readIndex = notif.textureIndex;
                    if (readIndex >= 0 && readIndex < 2) {
                        validTexture = g_copyTextures[readIndex];
                        validW = notif.width;
                        validH = notif.height;
                        hasValidTexture = true;

                        static int s_diagCounter = 0;
                        if ((++s_diagCounter % 300) == 0) {
                            GLboolean isTex = glIsTexture(validTexture);
                            GLint tw = 0, th = 0;
                            BindTextureDirect(GL_TEXTURE_2D, validTexture);
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
                            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
                            BindTextureDirect(GL_TEXTURE_2D, 0);
                            LogCategory("texture_ops",
                                        "Mirror Capture Thread: Using copy texture idx=" + std::to_string(readIndex) +
                                            " id=" + std::to_string(validTexture) + " glIsTexture=" + std::to_string((int)isTex) +
                                            " size=" + std::to_string(tw) + "x" + std::to_string(th));
                        }

                        // This must happen HERE, immediately after fence signals,
                        g_readyFrameIndex.store(readIndex, std::memory_order_release);
                        g_readyFrameWidth.store(notif.width, std::memory_order_release);
                        g_readyFrameHeight.store(notif.height, std::memory_order_release);
                    }
                }
            }

            if (!hasValidTexture) { continue; }

            int gameW = validW;
            int gameH = validH;

            {
                PROFILE_SCOPE_CAT("Get Mirror Configs", "Mirror Thread");
                uint64_t v = g_threadedMirrorConfigsVersion.load(std::memory_order_acquire);
                if (v != cachedConfigVersion) {
                    // Copy only when configs change (under mutex), then do any GL cleanup without holding the mutex.
                    std::vector<ThreadedMirrorConfig> newCache;
                    {
                        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
                        newCache = g_threadedMirrorConfigs;
                    }

                    configsCache = std::move(newCache);
                    cachedConfigVersion = v;
                    lastCaptureTimes.assign(configsCache.size(), std::chrono::steady_clock::time_point{});

                    if (!mt_fbos.empty()) {
                        for (auto it = mt_fbos.begin(); it != mt_fbos.end();) {
                            bool stillExists = false;
                            for (const auto& c : configsCache) {
                                if (c.name == it->first) {
                                    stillExists = true;
                                    break;
                                }
                            }
                            if (!stillExists) {
                                MT_DeleteMirrorFbos(it->second);
                                auto sourceRectIt = sourceRectGpuCaches.find(it->first);
                                if (sourceRectIt != sourceRectGpuCaches.end()) {
                                    MT_DeleteSourceRectGpuCacheEntry(sourceRectIt->second);
                                    sourceRectGpuCaches.erase(sourceRectIt);
                                }
                                it = mt_fbos.erase(it);
                                continue;
                            }
                            ++it;
                        }
                    }
                }
            }

            if (configsCache.empty()) { continue; }

            MirrorGammaMode gammaMode = GetGlobalMirrorGammaMode();

            std::vector<MirrorInstance*> readyToPublish;
            readyToPublish.reserve(configsCache.size());
            for (size_t confIndex = 0; confIndex < configsCache.size(); confIndex++) {
                auto& conf = configsCache[confIndex];
                PROFILE_SCOPE_CAT("Process Mirror", "Mirror Thread");
                if (conf.fps > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCaptureTimes[confIndex]).count();
                    if (elapsed < (1000 / conf.fps)) { continue; }
                }

                // Get mirror instance (unique lock - capture thread writes to instance)
                MirrorInstance* inst = nullptr;
                GLuint localBackFbo = 0;
                GLuint localTempBackFbo = 0;
                GLuint localFinalBackFbo = 0;
                {
                    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
                    auto it = g_mirrorInstances.find(conf.name);
                    if (it == g_mirrorInstances.end()) continue;
                    inst = &it->second;

                    // === FBO RESIZE: Handle FBO resize in capture thread (moved from main thread) ===
                    int borderPadding = (conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0;
                    int requiredFboW = conf.captureWidth + 2 * borderPadding;
                    int requiredFboH = conf.captureHeight + 2 * borderPadding;

                    if (inst->fbo_w != requiredFboW || inst->fbo_h != requiredFboH) {
                        inst->fbo_w = requiredFboW;
                        inst->fbo_h = requiredFboH;
                        inst->forceUpdateFrames = 3;

                        BindTextureDirect(GL_TEXTURE_2D, inst->fboTexture);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, inst->fbo_w, inst->fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        BindTextureDirect(GL_TEXTURE_2D, inst->fboTextureBack);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, inst->fbo_w, inst->fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                        BindTextureDirect(GL_TEXTURE_2D, 0);
                    }

                    float finalScaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
                    float finalScaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
                    int requiredFinalW = static_cast<int>(inst->fbo_w * finalScaleX);
                    int requiredFinalH = static_cast<int>(inst->fbo_h * finalScaleY);

                    if (inst->final_w_back != requiredFinalW || inst->final_h_back != requiredFinalH) {

                        BindTextureDirect(GL_TEXTURE_2D, inst->finalTextureBack);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, requiredFinalW, requiredFinalH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        BindTextureDirect(GL_TEXTURE_2D, 0);

                        inst->final_w_back = requiredFinalW;
                        inst->final_h_back = requiredFinalH;

                        inst->cachedRenderStateBack.isValid = false;
                    }

                    // Ensure mirror-thread-local FBOs exist and are attached to the current back textures.
                    // NOTE: We must NOT rely on inst->fboBack / inst->finalFboBack being usable in this context.
                    MT_MirrorFbos& fb = mt_fbos[conf.name];
                    if (fb.backFbo == 0) { glGenFramebuffers(1, &fb.backFbo); }
                    if (fb.tempBackFbo == 0) { glGenFramebuffers(1, &fb.tempBackFbo); }
                    if (fb.finalBackFbo == 0) { glGenFramebuffers(1, &fb.finalBackFbo); }

                    if (fb.lastBackTex != inst->fboTextureBack) {
                        glBindFramebuffer(GL_FRAMEBUFFER, fb.backFbo);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->fboTextureBack, 0);
                        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                        if (st != GL_FRAMEBUFFER_COMPLETE) {
                            Log("Mirror Capture Thread: backFbo incomplete for '" + conf.name + "' (status " + std::to_string(st) + ")");
                        }
                        fb.lastBackTex = inst->fboTextureBack;
                    }

                    if (fb.lastFinalBackTex != inst->finalTextureBack) {
                        glBindFramebuffer(GL_FRAMEBUFFER, fb.finalBackFbo);
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->finalTextureBack, 0);
                        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                        if (st != GL_FRAMEBUFFER_COMPLETE) {
                            Log("Mirror Capture Thread: finalBackFbo incomplete for '" + conf.name + "' (status " + std::to_string(st) + ")");
                        }
                        fb.lastFinalBackTex = inst->finalTextureBack;
                    }

                    localBackFbo = fb.backFbo;
                    localTempBackFbo = fb.tempBackFbo;
                    localFinalBackFbo = fb.finalBackFbo;
                }

                if (!inst || !inst->fboTextureBack || !inst->finalTextureBack || localBackFbo == 0 || localTempBackFbo == 0 ||
                    localFinalBackFbo == 0)
                    continue;

                if (inst->captureReady.load(std::memory_order_acquire)) continue;

                // Do NOT overwrite it here from conf.rawOutput - that causes race condition where

                MT_MirrorFbos& fb = mt_fbos[conf.name];
                const bool needsContentDetection =
                    MT_MirrorNeedsContentDetection(conf, inst->desiredRawOutput.load(std::memory_order_acquire));
                if (needsContentDetection) {
                    bool hasContent = false;
                    if (MT_HarvestContentReadback(fb, hasContent)) {
                        inst->hasFrameContentBack = hasContent;
                    }
                } else {
                    MT_ReleaseContentDetectionResources(fb);
                    inst->hasFrameContentBack = true;
                }

                debugSamplePixel(conf, validTexture, gameW, gameH);

                RenderMirrorToBuffer(inst, conf, validTexture, captureVAO, captureVBO, sourceRectGpuCaches[conf.name], localBackFbo,
                                     localTempBackFbo, &fb.lastTempTex, localFinalBackFbo, gammaMode, gameW, gameH, true);

                if (needsContentDetection) {
                    MT_QueueContentReadback(fb, localBackFbo, inst->fbo_w, inst->fbo_h);
                }

                // Pre-compute render cache for the render thread
                // Read current screen geometry from atomics
                int screenW = g_captureScreenW.load(std::memory_order_acquire);
                int screenH = g_captureScreenH.load(std::memory_order_acquire);
                int finalX = g_captureFinalX.load(std::memory_order_acquire);
                int finalY = g_captureFinalY.load(std::memory_order_acquire);
                int finalW = g_captureFinalW.load(std::memory_order_acquire);
                int finalH = g_captureFinalH.load(std::memory_order_acquire);

                if (screenW > 0 && screenH > 0) {
                    ComputeMirrorRenderCache(inst, conf, gameW, gameH, screenW, screenH, finalX, finalY, finalW, finalH, true);
                }

                inst->capturedAsRawOutputBack = inst->desiredRawOutput.load(std::memory_order_acquire);

                // Create GPU fence for cross-context synchronization
                // This fence will be swapped along with the texture and waited on by the render thread
                if (inst->gpuFenceBack && glIsSync(inst->gpuFenceBack)) { glDeleteSync(inst->gpuFenceBack); }
                inst->gpuFenceBack = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

                // This avoids redundant flushes and prevents the render thread from observing
                // a fence that hasn't been flushed to the driver yet.
                readyToPublish.push_back(inst);
                lastCaptureTimes[confIndex] = now;
            }

            // Note: OBS capture is done synchronously in CaptureToObsFBO (dllmain.cpp)
            // which includes animations and overlays applied by the game thread

            // Submit all queued GPU work and make fences visible to other contexts.
            if (!readyToPublish.empty()) {
                glFlush();
                for (MirrorInstance* inst : readyToPublish) {
                    inst->captureReady.store(true, std::memory_order_release);
                }
            }

        }

        if (captureVAO) glDeleteVertexArrays(1, &captureVAO);
        if (captureVBO) glDeleteBuffers(1, &captureVBO);
        for (auto& kv : sourceRectGpuCaches) {
            MT_DeleteSourceRectGpuCacheEntry(kv.second);
        }
        sourceRectGpuCaches.clear();

        // Cleanup local shader programs (created on this thread's context)
        MT_CleanupShaders();

        if (debugSampleFbo) { glDeleteFramebuffers(1, &debugSampleFbo); }

        // Cleanup mirror-thread local FBOs and PBOs
        for (auto& kv : mt_fbos) {
            MT_DeleteMirrorFbos(kv.second);
        }
        mt_fbos.clear();

        wglMakeCurrent(NULL, NULL);
        if (g_mirrorCaptureContext) {
            if (!g_mirrorContextIsShared) { wglDeleteContext(g_mirrorCaptureContext); }
            g_mirrorCaptureContext = NULL;
        }

        g_mirrorCaptureRunning.store(false);
        Log("Mirror Capture Thread: Stopped");
    } catch (const SE_Exception& e) {
        LogException("MirrorCaptureThreadFunc (SEH)", e.getCode(), e.getInfo());
        g_mirrorCaptureRunning.store(false);
    } catch (const std::exception& e) {
        LogException("MirrorCaptureThreadFunc", e);
        g_mirrorCaptureRunning.store(false);
    } catch (...) {
        Log("EXCEPTION in MirrorCaptureThreadFunc: Unknown exception");
        g_mirrorCaptureRunning.store(false);
    }
}

// Start the mirror capture thread (call from main thread after GPU init)
// MUST be called from main thread where game context is current
void StartMirrorCaptureThread(void* gameGLContext) {
    if (g_sameThreadMirrorPipelineActive.load(std::memory_order_acquire)) {
        if (g_mirrorCaptureRunning.load(std::memory_order_acquire) || g_mirrorCaptureThread.joinable()) {
            StopMirrorCaptureThread();
        }
        Log("Mirror Capture Thread: Start skipped while same-thread render pipeline is enabled");
        return;
    }

    // If thread is already running, don't start another
    if (g_mirrorCaptureThread.joinable()) {
        if (g_mirrorCaptureRunning.load()) {
            // Thread object exists and is still running
            Log("Mirror Capture Thread: Already running");
            return;
        } else {
            // Thread object exists but finished - join it before starting new one
            Log("Mirror Capture Thread: Joining finished thread...");
            g_mirrorCaptureThread.join();

            // If the previous thread exited early (exception), it may not have cleaned up.
            if (!g_mirrorContextIsShared && g_mirrorCaptureContext) {
                wglDeleteContext(g_mirrorCaptureContext);
                g_mirrorCaptureContext = NULL;
            }
            if (!g_mirrorContextIsShared) {
                if (g_mirrorOwnedDCHwnd && g_mirrorCaptureDC) {
                    ReleaseDC(g_mirrorOwnedDCHwnd, g_mirrorCaptureDC);
                }
                g_mirrorOwnedDCHwnd = NULL;

                if (g_mirrorFallbackDummyHwnd && g_mirrorFallbackDummyDC) {
                    ReleaseDC(g_mirrorFallbackDummyHwnd, g_mirrorFallbackDummyDC);
                    g_mirrorFallbackDummyDC = NULL;
                }
                if (g_mirrorFallbackDummyHwnd) {
                    DestroyWindow(g_mirrorFallbackDummyHwnd);
                    g_mirrorFallbackDummyHwnd = NULL;
                }
                g_mirrorCaptureDC = NULL;
            }
        }
    }

    HGLRC sharedContext = GetSharedMirrorContext();
    HDC sharedDC = GetSharedMirrorContextDC();

    if (sharedContext && sharedDC) {
        // Use the pre-shared context (GPU sharing enabled for all threads)
        g_mirrorCaptureContext = sharedContext;
        g_mirrorCaptureDC = sharedDC;
        g_mirrorContextIsShared = true;
        Log("Mirror Capture Thread: Using pre-shared context (GPU texture sharing enabled)");
    } else {
        g_mirrorContextIsShared = false;

        HDC gameHdc = wglGetCurrentDC();
        HWND gameHwndForDC = NULL;
        if (!gameHdc) {
            HWND hwnd = g_minecraftHwnd.load();
            if (hwnd) {
                gameHdc = GetDC(hwnd);
                gameHwndForDC = hwnd;
            }
        }

        if (!gameHdc) {
            Log("Mirror Capture Thread: No DC available");
            return;
        }

        if (MT_CreateFallbackDummyWindowWithMatchingPixelFormat(gameHdc, L"mirror", g_mirrorFallbackDummyHwnd, g_mirrorFallbackDummyDC) &&
            g_mirrorFallbackDummyDC) {
            g_mirrorCaptureDC = g_mirrorFallbackDummyDC;
            // If we called GetDC(hwnd) only to query the pixel format, release it now.
            if (gameHwndForDC) {
                ReleaseDC(gameHwndForDC, gameHdc);
                gameHwndForDC = NULL;
            }
            g_mirrorOwnedDCHwnd = NULL;
        } else {
            // Fall back to using the game HDC (less stable on some drivers).
            g_mirrorCaptureDC = gameHdc;
            g_mirrorOwnedDCHwnd = gameHwndForDC; // Release on StopMirrorCaptureThread if non-null
        }

        // Create the capture context on main thread
        g_mirrorCaptureContext = wglCreateContext(g_mirrorCaptureDC);
        if (!g_mirrorCaptureContext) {
            Log("Mirror Capture Thread: Failed to create GL context (error " + std::to_string(GetLastError()) + ")");
            if (g_mirrorOwnedDCHwnd && g_mirrorCaptureDC) {
                ReleaseDC(g_mirrorOwnedDCHwnd, g_mirrorCaptureDC);
                g_mirrorOwnedDCHwnd = NULL;
                g_mirrorCaptureDC = NULL;
            }
            return;
        }

        // Share OpenGL objects with game context - MUST happen on main thread while game context is current
        HDC prevDC = wglGetCurrentDC();
        HGLRC prevRC = wglGetCurrentContext();
        if (prevRC) { wglMakeCurrent(NULL, NULL); }

        if (!wglShareLists((HGLRC)gameGLContext, g_mirrorCaptureContext)) {
            DWORD err1 = GetLastError();
            if (!wglShareLists(g_mirrorCaptureContext, (HGLRC)gameGLContext)) {
                DWORD err2 = GetLastError();
                Log("Mirror Capture Thread: wglShareLists failed (errors " + std::to_string(err1) + ", " + std::to_string(err2) + ")");
                wglDeleteContext(g_mirrorCaptureContext);
                g_mirrorCaptureContext = NULL;
                if (prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }
                return;
            }
        }

        if (prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }

        Log("Mirror Capture Thread: Context created and shared on main thread (fallback mode)");
    }

    int screenW = GetCachedWindowWidth();
    int screenH = GetCachedWindowHeight();
    if (g_copyTextures[0] == 0) {
        InitCaptureTexture(screenW, screenH);
    }

    g_mirrorCaptureShouldStop.store(false);
    g_mirrorCaptureRunning.store(true); // Mark as running BEFORE starting thread
    g_mirrorCaptureThread = std::thread(MirrorCaptureThreadFunc, gameGLContext);
    LogCategory("init", "Mirror Capture Thread: Started");
}

// Stop the mirror capture thread
void StopMirrorCaptureThread() {
    if (!g_mirrorCaptureRunning.load() && !g_mirrorCaptureThread.joinable()) { return; }

    Log("Mirror Capture Thread: Stopping...");
    g_mirrorCaptureShouldStop.store(true);

    if (g_mirrorCaptureThread.joinable()) { g_mirrorCaptureThread.join(); }

    Log("Mirror Capture Thread: Joined");

    // If the mirror thread crashed, it may not have reached its normal cleanup path.
    if (!g_mirrorContextIsShared && g_mirrorCaptureContext) {
        wglDeleteContext(g_mirrorCaptureContext);
        g_mirrorCaptureContext = NULL;
    }

    // Destroy fallback dummy window/DC on the main thread after join.
    if (!g_mirrorContextIsShared) {
        if (g_mirrorOwnedDCHwnd && g_mirrorCaptureDC) {
            ReleaseDC(g_mirrorOwnedDCHwnd, g_mirrorCaptureDC);
        }
        g_mirrorOwnedDCHwnd = NULL;

        if (g_mirrorFallbackDummyHwnd && g_mirrorFallbackDummyDC) {
            ReleaseDC(g_mirrorFallbackDummyHwnd, g_mirrorFallbackDummyDC);
            g_mirrorFallbackDummyDC = NULL;
        }
        if (g_mirrorFallbackDummyHwnd) {
            DestroyWindow(g_mirrorFallbackDummyHwnd);
            g_mirrorFallbackDummyHwnd = NULL;
        }

        g_mirrorCaptureDC = NULL;
    }
}

static void SwapMirrorInstanceBuffers(MirrorInstance& inst, const std::chrono::steady_clock::time_point& updateTime) {
    std::swap(inst.fbo, inst.fboBack);
    std::swap(inst.fboTexture, inst.fboTextureBack);
    std::swap(inst.capturedAsRawOutput, inst.capturedAsRawOutputBack);
    std::swap(inst.cachedRenderState, inst.cachedRenderStateBack);
    std::swap(inst.finalFbo, inst.finalFboBack);
    std::swap(inst.finalTexture, inst.finalTextureBack);
    std::swap(inst.final_w, inst.final_w_back);
    std::swap(inst.final_h, inst.final_h_back);
    std::swap(inst.hasFrameContent, inst.hasFrameContentBack);
    std::swap(inst.gpuFence, inst.gpuFenceBack);

    inst.hasValidContent = true;
    inst.captureReady.store(false, std::memory_order_release);
    inst.lastUpdateTime = updateTime;
}

// Call this from main render thread each frame
// GPU fence synchronization ensures capture thread's work completes before render reads
void SwapMirrorBuffers() {
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex); // Write lock - swapping buffers
    auto now = std::chrono::steady_clock::now();

    for (auto& [name, inst] : g_mirrorInstances) {
        if (inst.captureReady.load(std::memory_order_acquire)) {
            SwapMirrorInstanceBuffers(inst, now);
        }
    }
}

bool RenderMirrorCapturesOnCurrentThread(const std::vector<ThreadedMirrorConfig>& activeMirrorConfigs, GLuint sourceTexture, int gameW,
                                         int gameH, int screenW, int screenH, int finalX, int finalY, int finalW, int finalH) {
    if (activeMirrorConfigs.empty() || sourceTexture == 0 || gameW <= 0 || gameH <= 0) { return false; }

    if (mt_filterProgram == 0 || mt_renderProgram == 0 || mt_backgroundProgram == 0) {
        if (!MT_InitializeShaders()) { return false; }
    }
    EnsureSameThreadMirrorCaptureResources();

    MirrorGammaMode gammaMode = GetGlobalMirrorGammaMode();
    bool renderedAny = false;
    MT_RenderToBufferStateCache stateCache{};

    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g_sameThreadCaptureVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_sameThreadCaptureVBO);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);

    for (const auto& conf : activeMirrorConfigs) {
        if (conf.input.empty() || conf.captureWidth <= 0 || conf.captureHeight <= 0) { continue; }

        auto it = g_mirrorInstances.find(conf.name);
        if (it == g_mirrorInstances.end()) { continue; }

        MirrorInstance* inst = &it->second;
        if (conf.fps > 0 && inst->forceUpdateFrames <= 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - inst->lastUpdateTime).count();
            if (elapsed < (1000 / conf.fps)) { continue; }
        }

        inst->desiredRawOutput.store(conf.rawOutput, std::memory_order_relaxed);

        int borderPadding = (conf.borderType == MirrorBorderType::Dynamic) ? conf.dynamicBorderThickness : 0;
        int requiredFboW = conf.captureWidth + 2 * borderPadding;
        int requiredFboH = conf.captureHeight + 2 * borderPadding;

        if (inst->fbo_w != requiredFboW || inst->fbo_h != requiredFboH) {
            inst->fbo_w = requiredFboW;
            inst->fbo_h = requiredFboH;
            inst->forceUpdateFrames = 3;
            inst->cachedRenderState.isValid = false;

            BindTextureDirect(GL_TEXTURE_2D, inst->fboTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, inst->fbo_w, inst->fbo_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            BindTextureDirect(GL_TEXTURE_2D, 0);
        }

        float finalScaleX = conf.outputSeparateScale ? conf.outputScaleX : conf.outputScale;
        float finalScaleY = conf.outputSeparateScale ? conf.outputScaleY : conf.outputScale;
        int requiredFinalW = static_cast<int>(inst->fbo_w * finalScaleX);
        int requiredFinalH = static_cast<int>(inst->fbo_h * finalScaleY);
        if (inst->final_w != requiredFinalW || inst->final_h != requiredFinalH) {
            BindTextureDirect(GL_TEXTURE_2D, inst->finalTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, requiredFinalW, requiredFinalH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            BindTextureDirect(GL_TEXTURE_2D, 0);

            inst->final_w = requiredFinalW;
            inst->final_h = requiredFinalH;
            inst->final_w_back = requiredFinalW;
            inst->final_h_back = requiredFinalH;
            inst->cachedRenderState.isValid = false;
            inst->cachedRenderStateBack.isValid = false;
        }

        MT_MirrorFbos& fb = g_sameThreadMirrorFbos[conf.name];
        if (fb.backFbo == 0) { glGenFramebuffers(1, &fb.backFbo); }
        if (fb.tempBackFbo == 0) { glGenFramebuffers(1, &fb.tempBackFbo); }
        if (fb.finalBackFbo == 0) { glGenFramebuffers(1, &fb.finalBackFbo); }

        const bool needsContentDetection = MT_MirrorNeedsContentDetection(conf, conf.rawOutput);
        if (needsContentDetection) {
            bool hasContent = false;
            if (MT_HarvestContentReadback(fb, hasContent)) {
                inst->hasFrameContent = hasContent;
            }
        } else {
            MT_ReleaseContentDetectionResources(fb);
            inst->hasFrameContent = true;
        }

        if (fb.lastBackTex != inst->fboTexture) {
            glBindFramebuffer(GL_FRAMEBUFFER, fb.backFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->fboTexture, 0);
            fb.lastBackTex = inst->fboTexture;
        }
        if (fb.lastFinalBackTex != inst->finalTexture) {
            glBindFramebuffer(GL_FRAMEBUFFER, fb.finalBackFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inst->finalTexture, 0);
            fb.lastFinalBackTex = inst->finalTexture;
        }

        RenderMirrorToBuffer(inst, conf, sourceTexture, g_sameThreadCaptureVAO, g_sameThreadCaptureVBO,
                             g_sameThreadSourceRectGpuCaches[conf.name], fb.backFbo, fb.tempBackFbo, &fb.lastTempTex,
                             fb.finalBackFbo, gammaMode, gameW, gameH, false, &stateCache, true);

        if (needsContentDetection) {
            MT_QueueContentReadback(fb, fb.backFbo, inst->fbo_w, inst->fbo_h);
        }
        ComputeMirrorRenderCache(inst, conf, gameW, gameH, screenW, screenH, finalX, finalY, finalW, finalH, false);
        inst->capturedAsRawOutput = conf.rawOutput;
        if (inst->gpuFence && glIsSync(inst->gpuFence)) { glDeleteSync(inst->gpuFence); }
        inst->gpuFence = nullptr;
        inst->hasValidContent = true;
        inst->captureReady.store(false, std::memory_order_release);
        inst->lastUpdateTime = now;
        if (inst->forceUpdateFrames > 0) { inst->forceUpdateFrames--; }
        renderedAny = true;
    }

    return renderedAny;
}

// Update capture configs from main thread (call when active mirrors change)
void UpdateMirrorCaptureConfigs(const std::vector<MirrorConfig>& activeMirrors) {
    std::vector<ThreadedMirrorConfig> configs;
    BuildThreadedMirrorConfigs(activeMirrors, configs);

    // Compute summaries from the local vector (avoid reading g_threadedMirrorConfigs without its mutex).
    const int mirrorCount = static_cast<int>(configs.size());
    int maxFps = 0;
    bool unlimited = false;
    for (const auto& c : configs) {
        if (c.fps <= 0) {
            unlimited = true;
            break;
        }
        maxFps = (std::max)(maxFps, c.fps);
    }

    // Clear captureReady for all mirrors to allow capture thread to start fresh
    // (captureReady would stay true if main thread never consumed the capture)
    {
        std::unique_lock<std::shared_mutex> clearLock(g_mirrorInstancesMutex);
        for (auto& [name, inst] : g_mirrorInstances) {
            inst.captureReady.store(false, std::memory_order_release);
            inst.cachedRenderState.isValid = false;
            inst.cachedRenderStateBack.isValid = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        g_threadedMirrorConfigs = std::move(configs);
        g_threadedMirrorConfigsVersion.fetch_add(1, std::memory_order_release);
    }

    g_activeMirrorCaptureCount.store(mirrorCount, std::memory_order_release);

    g_activeMirrorCaptureMaxFps.store(unlimited ? 0 : maxFps, std::memory_order_release);

    // Wake the mirror thread (it may be waiting with a long timeout when configs are empty).
    g_captureSignalCV.notify_one();
}

void UpdateMirrorFPS(const std::string& mirrorName, int fps) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.fps = fps;
            break;
        }
    }

    g_threadedMirrorConfigsVersion.fetch_add(1, std::memory_order_release);

    int maxFps = 0;
    bool unlimited = false;
    for (const auto& c : g_threadedMirrorConfigs) {
        if (c.fps <= 0) {
            unlimited = true;
            break;
        }
        maxFps = (std::max)(maxFps, c.fps);
    }
    g_activeMirrorCaptureMaxFps.store(unlimited ? 0 : maxFps, std::memory_order_release);

    g_captureSignalCV.notify_one();
}

void UpdateMirrorOutputPosition(const std::string& mirrorName, int x, int y, float scale, bool separateScale, float scaleX, float scaleY,
                                const std::string& relativeTo) {
    // Update the threaded config
    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        for (auto& conf : g_threadedMirrorConfigs) {
            if (conf.name == mirrorName) {
                conf.outputX = x;
                conf.outputY = y;
                conf.outputScale = scale;
                conf.outputSeparateScale = separateScale;
                conf.outputScaleX = scaleX;
                conf.outputScaleY = scaleY;
                conf.outputRelativeTo = relativeTo;
                break;
            }
        }

        g_threadedMirrorConfigsVersion.fetch_add(1, std::memory_order_release);
    }

    g_captureSignalCV.notify_one();

    // This ensures the render thread recalculates positions immediately
    {
        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
        auto it = g_mirrorInstances.find(mirrorName);
        if (it != g_mirrorInstances.end()) {
            // Front cache: render thread will recalculate immediately
            // Back cache: capture thread will recompute on next capture
            it->second.cachedRenderState.isValid = false;
            it->second.cachedRenderStateBack.isValid = false;
        }
    }
}

void UpdateMirrorGroupOutputPosition(const std::vector<std::string>& mirrorIds, int x, int y, float scale, bool separateScale, float scaleX,
                                     float scaleY, const std::string& relativeTo) {
    // Update the threaded config for all mirrors in the group
    // NOTE: We intentionally do NOT update scale here. The mirror thread should always use
    {
        std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
        for (auto& conf : g_threadedMirrorConfigs) {
            if (std::find(mirrorIds.begin(), mirrorIds.end(), conf.name) != mirrorIds.end()) {
                conf.outputX = x;
                conf.outputY = y;
                conf.outputRelativeTo = relativeTo;
            }
        }

        g_threadedMirrorConfigsVersion.fetch_add(1, std::memory_order_release);
    }

    g_captureSignalCV.notify_one();

    {
        std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
        for (const auto& mirrorName : mirrorIds) {
            auto it = g_mirrorInstances.find(mirrorName);
            if (it != g_mirrorInstances.end()) {
                it->second.cachedRenderState.isValid = false;
                it->second.cachedRenderStateBack.isValid = false;
            }
        }
    }
}

void UpdateMirrorInputRegions(const std::string& mirrorName, const std::vector<MirrorCaptureConfig>& inputRegions) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.input = MT_NormalizeCaptureInputs(inputRegions);
            conf.sourceRectLayoutHash = MT_ComputeSourceRectLayoutHash(conf);
            break;
        }
    }

    g_threadedMirrorConfigsVersion.fetch_add(1, std::memory_order_release);
    g_captureSignalCV.notify_one();
}

void UpdateMirrorCaptureSettings(const std::string& mirrorName, int captureWidth, int captureHeight, const MirrorBorderConfig& border,
                                 const MirrorColors& colors, float colorSensitivity, bool rawOutput, bool colorPassthrough) {
    std::lock_guard<std::mutex> lock(g_threadedMirrorConfigMutex);
    for (auto& conf : g_threadedMirrorConfigs) {
        if (conf.name == mirrorName) {
            conf.captureWidth = captureWidth;
            conf.captureHeight = captureHeight;

            conf.borderType = border.type;
            conf.dynamicBorderThickness = border.dynamicThickness;
            conf.staticBorderShape = border.staticShape;
            conf.staticBorderColor = border.staticColor;
            conf.staticBorderThickness = border.staticThickness;
            conf.staticBorderRadius = border.staticRadius;
            conf.staticBorderOffsetX = border.staticOffsetX;
            conf.staticBorderOffsetY = border.staticOffsetY;
            conf.staticBorderWidth = border.staticWidth;
            conf.staticBorderHeight = border.staticHeight;

            conf.targetColors = colors.targetColors;
            conf.outputColor = colors.output;
            conf.borderColor = colors.border;
            conf.colorSensitivity = colorSensitivity;
            conf.rawOutput = rawOutput;
            conf.colorPassthrough = colorPassthrough;
            conf.sourceRectLayoutHash = MT_ComputeSourceRectLayoutHash(conf);
            break;
        }
    }

    g_threadedMirrorConfigsVersion.fetch_add(1, std::memory_order_release);
    g_captureSignalCV.notify_one();
}

void InvalidateMirrorTextureCaches(const std::vector<std::string>& mirrorNames) {
    if (mirrorNames.empty()) return;

    std::unique_lock<std::shared_mutex> lock(g_mirrorInstancesMutex);
    for (const auto& mirrorName : mirrorNames) {
        auto it = g_mirrorInstances.find(mirrorName);
        if (it == g_mirrorInstances.end()) continue;

        MirrorInstance& inst = it->second;
        inst.captureReady.store(false, std::memory_order_release);
        inst.hasValidContent = false;
        inst.hasFrameContent = false;
        inst.hasFrameContentBack = false;
        inst.capturedAsRawOutput = false;
        inst.capturedAsRawOutputBack = false;
        inst.cachedRenderState.isValid = false;
        inst.cachedRenderStateBack.isValid = false;

        // Force full reallocation/rebuild on next activation so stale mirror/static-border visuals cannot survive.
        inst.fbo_w = 0;
        inst.fbo_h = 0;
        inst.final_w = 0;
        inst.final_h = 0;
        inst.final_w_back = 0;
        inst.final_h_back = 0;

        inst.forceUpdateFrames = 3;
    }

    if (wglGetCurrentContext()) {
        for (const auto& mirrorName : mirrorNames) {
            auto sameThreadFboIt = g_sameThreadMirrorFbos.find(mirrorName);
            if (sameThreadFboIt != g_sameThreadMirrorFbos.end()) {
                MT_DeleteMirrorFbos(sameThreadFboIt->second);
                g_sameThreadMirrorFbos.erase(sameThreadFboIt);
            }

            auto sameThreadCacheIt = g_sameThreadSourceRectGpuCaches.find(mirrorName);
            if (sameThreadCacheIt != g_sameThreadSourceRectGpuCaches.end()) {
                MT_DeleteSourceRectGpuCacheEntry(sameThreadCacheIt->second);
                g_sameThreadSourceRectGpuCaches.erase(sameThreadCacheIt);
            }

            auto instIt = g_mirrorInstances.find(mirrorName);
            if (instIt != g_mirrorInstances.end() && instIt->second.tempCaptureTexture != 0) {
                glDeleteTextures(1, &instIt->second.tempCaptureTexture);
                instIt->second.tempCaptureTexture = 0;
                instIt->second.tempCaptureTextureW = 0;
                instIt->second.tempCaptureTextureH = 0;
            }
        }
    }
}



