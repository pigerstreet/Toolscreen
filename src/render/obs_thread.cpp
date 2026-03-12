#include "obs_thread.h"
#include "common/profiler.h"
#include "mirror_thread.h"
#include "render_thread.h"
#include "common/utils.h"
#include <mutex>
#include <thread>
#include <vector>

#include "MinHook.h"

std::atomic<bool> g_obsOverrideEnabled{ false };
std::atomic<GLuint> g_obsOverrideTexture{ 0 };
std::atomic<int> g_obsOverrideWidth{ 0 };
std::atomic<int> g_obsOverrideHeight{ 0 };

std::atomic<bool> g_obsPre113Windowed{ false };
std::atomic<int> g_obsPre113OffsetX{ 0 };
std::atomic<int> g_obsPre113OffsetY{ 0 };
std::atomic<int> g_obsPre113ContentW{ 0 };
std::atomic<int> g_obsPre113ContentH{ 0 };

static std::atomic<bool> g_obsHookInitialized{ false };
static std::atomic<bool> g_obsHookActive{ false };

typedef void(APIENTRY* PFN_glBlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                                              GLint dstY1, GLbitfield mask, GLenum filter);
static PFN_glBlitFramebuffer Real_glBlitFramebuffer = nullptr;

static GLuint g_obsRedirectFBO = 0;
static std::mutex g_obsHookMutex;

static GLuint g_obsCaptureFBO = 0;
static GLuint g_obsCaptureTexture = 0;
static int g_obsCaptureWidth = 0;
static int g_obsCaptureHeight = 0;
static GLuint g_obsRedirectAttachedTexture = 0;

static GLuint SelectObsRedirectTexture(bool allowDedicatedObsTexture, GLsync& outFence, bool& outNeedsFenceWait) {
    outFence = nullptr;
    outNeedsFenceWait = false;

    if (allowDedicatedObsTexture) {
        GLuint obsTexture = GetCompletedObsTexture();
        if (obsTexture != 0) {
            outFence = GetCompletedObsFence();
            outNeedsFenceWait = true;
            return obsTexture;
        }
    }

    GLuint overrideTexture = g_obsOverrideTexture.load(std::memory_order_acquire);
    if (overrideTexture != 0) { return overrideTexture; }

    return 0;
}

static void APIENTRY Hook_glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                                            GLint dstY1, GLbitfield mask, GLenum filter) {
    if (g_obsOverrideEnabled.load(std::memory_order_acquire)) {
        GLint readFBO = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);

        if (readFBO == 0) {
            const bool sameThreadRenderPipeline = g_sameThreadMirrorPipelineActive.load(std::memory_order_acquire);
            const bool allowDedicatedObsTexture = !sameThreadRenderPipeline || g_renderThreadRunning.load(std::memory_order_acquire);
            GLsync obsFence = nullptr;
            bool needsFenceWait = false;
            GLuint obsTexture = SelectObsRedirectTexture(allowDedicatedObsTexture, obsFence, needsFenceWait);

            if (obsTexture != 0) {
                PROFILE_SCOPE_CAT("OBS Capture Redirect", "OBS Hook");

                if (needsFenceWait && obsFence && glIsSync(obsFence)) { glWaitSync(obsFence, 0, GL_TIMEOUT_IGNORED); }

                if (needsFenceWait) { glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT); }

                if (g_obsRedirectFBO == 0) {
                    glGenFramebuffers(1, &g_obsRedirectFBO);
                    g_obsRedirectAttachedTexture = 0;
                }

                glBindFramebuffer(GL_READ_FRAMEBUFFER, g_obsRedirectFBO);
                if (g_obsRedirectAttachedTexture != obsTexture) {
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, obsTexture, 0);

                    GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
                    if (status != GL_FRAMEBUFFER_COMPLETE) {
                        static GLenum lastLoggedStatus = GL_FRAMEBUFFER_COMPLETE;
                        if (status != lastLoggedStatus) {
                            Log("[OBS Hook] WARNING: Redirect FBO incomplete! Status: " + std::to_string(status) +
                                ", Texture: " + std::to_string(obsTexture));
                            lastLoggedStatus = status;
                        }
                        g_obsRedirectAttachedTexture = 0;
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                        Real_glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
                        return;
                    }

                    g_obsRedirectAttachedTexture = obsTexture;
                }

                GLint blitSrcX0 = srcX0, blitSrcY0 = srcY0, blitSrcX1 = srcX1, blitSrcY1 = srcY1;
                if (g_obsPre113Windowed.load(std::memory_order_acquire)) {
                    int offsetX = g_obsPre113OffsetX.load(std::memory_order_acquire);
                    int offsetY = g_obsPre113OffsetY.load(std::memory_order_acquire);
                    blitSrcX0 = srcX0 + offsetX;
                    blitSrcY0 = srcY0 + offsetY;
                    blitSrcX1 = srcX1 + offsetX;
                    blitSrcY1 = srcY1 + offsetY;
                }
                Real_glBlitFramebuffer(blitSrcX0, blitSrcY0, blitSrcX1, blitSrcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);

                glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                return;
            }
        }
    }

    Real_glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void CaptureBackbufferForObs(int width, int height) {
    PROFILE_SCOPE_CAT("Capture Backbuffer for OBS", "OBS");

    if (g_obsCaptureFBO == 0 || width != g_obsCaptureWidth || height != g_obsCaptureHeight) {
        if (g_obsCaptureTexture != 0) { glDeleteTextures(1, &g_obsCaptureTexture); }
        if (g_obsCaptureFBO == 0) { glGenFramebuffers(1, &g_obsCaptureFBO); }

        glGenTextures(1, &g_obsCaptureTexture);
        BindTextureDirect(GL_TEXTURE_2D, g_obsCaptureTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, g_obsCaptureFBO);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_obsCaptureTexture, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        BindTextureDirect(GL_TEXTURE_2D, 0);

        g_obsCaptureWidth = width;
        g_obsCaptureHeight = height;
    }

    GLint prevReadFBO = 0, prevDrawFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_obsCaptureFBO);

    if (Real_glBlitFramebuffer) {
        Real_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);

    SetObsOverrideTexture(g_obsCaptureTexture, width, height);
}

void SetObsOverrideTexture(GLuint texture, int width, int height) {
    g_obsOverrideTexture.store(texture, std::memory_order_release);
    g_obsOverrideWidth.store(width, std::memory_order_release);
    g_obsOverrideHeight.store(height, std::memory_order_release);
    g_obsOverrideEnabled.store(true, std::memory_order_release);
}

void ClearObsOverride() { g_obsOverrideEnabled.store(false, std::memory_order_release); }

void EnableObsOverride() {
    if (g_obsHookActive.load(std::memory_order_acquire)) { g_obsOverrideEnabled.store(true, std::memory_order_release); }
}

GLuint GetObsCaptureTexture() { return g_obsCaptureTexture; }

int GetObsCaptureWidth() { return g_obsCaptureWidth; }

int GetObsCaptureHeight() { return g_obsCaptureHeight; }

bool IsObsHookDetected() {
    return GetModuleHandleA("graphics-hook64.dll") != NULL;
}

void StartObsHookThread() {
    if (g_obsHookInitialized.load()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_obsHookMutex);
    if (g_obsHookInitialized.load()) {
        return; // Double-check after lock
    }

    Log("OBS Hook: Initializing...");

    HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
    if (!opengl32) {
        Log("OBS Hook: Failed to find opengl32.dll");
        return;
    }

    typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress wglGetProcAddressPtr = (PFN_wglGetProcAddress)GetProcAddress(opengl32, "wglGetProcAddress");
    if (!wglGetProcAddressPtr) {
        Log("OBS Hook: Failed to get wglGetProcAddress");
        return;
    }

    void* blitAddr = (void*)wglGetProcAddressPtr("glBlitFramebuffer");
    if (!blitAddr) {
        Log("OBS Hook: Failed to get glBlitFramebuffer address");
        return;
    }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        Log("OBS Hook: Failed to initialize MinHook");
        return;
    }

    MH_STATUS status = MH_CreateHook(blitAddr, (void*)Hook_glBlitFramebuffer, (void**)&Real_glBlitFramebuffer);
    if (status != MH_OK) {
        Log("OBS Hook: Failed to create hook (status " + std::to_string(status) + ")");
        return;
    }

    status = MH_EnableHook(blitAddr);
    if (status != MH_OK) {
        Log("OBS Hook: Failed to enable hook (status " + std::to_string(status) + ")");
        MH_RemoveHook(blitAddr);
        return;
    }

    g_obsHookActive.store(true);
    g_obsHookInitialized.store(true);

    // Enable the OBS override so the hook redirects captures to our render thread texture
    g_obsOverrideEnabled.store(true, std::memory_order_release);

    Log("OBS Hook: Successfully hooked glBlitFramebuffer");
}

void StopObsHookThread() {
    if (!g_obsHookInitialized.load()) { return; }

    std::lock_guard<std::mutex> lock(g_obsHookMutex);

    g_obsOverrideEnabled.store(false, std::memory_order_release);

    if (g_obsHookActive.load()) {
        HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
        if (opengl32) {
            typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
            PFN_wglGetProcAddress wglGetProcAddressPtr = (PFN_wglGetProcAddress)GetProcAddress(opengl32, "wglGetProcAddress");
            if (wglGetProcAddressPtr) {
                void* blitAddr = (void*)wglGetProcAddressPtr("glBlitFramebuffer");
                if (blitAddr) {
                    MH_DisableHook(blitAddr);
                    MH_RemoveHook(blitAddr);
                }
            }
        }
        g_obsHookActive.store(false);
    }

    if (g_obsRedirectFBO != 0) {
        glDeleteFramebuffers(1, &g_obsRedirectFBO);
        g_obsRedirectFBO = 0;
        g_obsRedirectAttachedTexture = 0;
    }

    g_obsHookInitialized.store(false);
    Log("OBS Hook: Stopped");
}



