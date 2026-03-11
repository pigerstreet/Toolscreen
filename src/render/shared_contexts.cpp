#include "shared_contexts.h"
#include "common/utils.h"

#include <GL/glew.h>
#include <GL/wglew.h>

std::atomic<HGLRC> g_sharedRenderContext{ nullptr };
std::atomic<HGLRC> g_sharedMirrorContext{ nullptr };
std::atomic<HDC> g_sharedRenderContextDC{ nullptr };
std::atomic<HDC> g_sharedMirrorContextDC{ nullptr };
std::atomic<HDC> g_sharedContextDC{ nullptr };
std::atomic<bool> g_sharedContextsReady{ false };

// Use dedicated hidden dummy windows/DCs for worker contexts.
// Each worker context must have its own drawable (HDC) if it will be current on a different thread.
static HWND g_sharedDummyRenderHwnd = NULL;
static HDC g_sharedDummyRenderDC = NULL;
static HWND g_sharedDummyMirrorHwnd = NULL;
static HDC g_sharedDummyMirrorDC = NULL;

static bool CreateSharedDummyWindowWithMatchingPixelFormat(HDC gameHdc, const wchar_t* windowNameTag, HWND& outHwnd, HDC& outDc) {
    if (outHwnd && outDc) { return true; }
    if (!gameHdc) { return false; }

    int gamePf = GetPixelFormat(gameHdc);
    if (gamePf == 0) {
        Log("SharedContexts: GetPixelFormat(gameHdc) returned 0");
        return false;
    }

    PIXELFORMATDESCRIPTOR gamePfd = {};
    gamePfd.nSize = sizeof(gamePfd);
    gamePfd.nVersion = 1;
    if (DescribePixelFormat(gameHdc, gamePf, sizeof(gamePfd), &gamePfd) == 0) {
        Log("SharedContexts: DescribePixelFormat(gameHdc) failed");
        return false;
    }

    static ATOM s_atom = 0;
    if (!s_atom) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"ToolscreenSharedGLDummy";
        s_atom = RegisterClassExW(&wc);
        if (!s_atom) {
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                Log("SharedContexts: RegisterClassExW failed (error " + std::to_string(err) + ")");
                return false;
            }
        }
    }

    std::wstring wndName = L"ToolscreenSharedGLDummy_";
    wndName += (windowNameTag ? windowNameTag : L"ctx");

    outHwnd = CreateWindowExW(0, L"ToolscreenSharedGLDummy", wndName.c_str(), WS_OVERLAPPED, 0, 0, 1, 1, NULL, NULL,
                              GetModuleHandleW(NULL), NULL);
    if (!outHwnd) {
        Log("SharedContexts: CreateWindowExW(dummy) failed (error " + std::to_string(GetLastError()) + ")");
        return false;
    }

    outDc = GetDC(outHwnd);
    if (!outDc) {
        Log("SharedContexts: GetDC(dummy) failed");
        DestroyWindow(outHwnd);
        outHwnd = NULL;
        return false;
    }

    // For WGL sharing stability, the pixel formats must match. If we cannot set the same
    // pixel format index, do NOT use the dummy window/DC.
    if (!SetPixelFormat(outDc, gamePf, &gamePfd)) {
        Log("SharedContexts: Failed to SetPixelFormat(dummy, gamePf=" + std::to_string(gamePf) + ") (error " +
            std::to_string(GetLastError()) + ")");
        ReleaseDC(outHwnd, outDc);
        DestroyWindow(outHwnd);
        outDc = NULL;
        outHwnd = NULL;
        return false;
    }

    return true;
}

struct ScopedWglMakeCurrent {
    HDC prevDC = NULL;
    HGLRC prevRC = NULL;
    bool changed = false;
    ScopedWglMakeCurrent(HDC dc, HGLRC rc) {
        prevRC = wglGetCurrentContext();
        prevDC = wglGetCurrentDC();
        if (dc && rc) {
            if (prevDC != dc || prevRC != rc) {
                if (wglMakeCurrent(dc, rc)) { changed = true; }
            }
        }
    }
    ~ScopedWglMakeCurrent() {
        if (changed) {
            wglMakeCurrent(prevDC, prevRC);
        }
    }
};

static bool VerifyTextureSharing(HGLRC gameContext, HDC gameDC, HGLRC otherContext, HDC otherDC, const char* tag) {
    if (!gameContext || !otherContext || !gameDC || !otherDC) { return false; }

    {
        ScopedWglMakeCurrent makeGame(gameDC, gameContext);
        if (wglGetCurrentContext() != gameContext) {
            Log(std::string("SharedContexts: VerifyTextureSharing(") + tag + "): failed to make game context current");
            return false;
        }
    }

    GLuint testTex = 0;
    {
        ScopedWglMakeCurrent makeGame(gameDC, gameContext);
        glGenTextures(1, &testTex);
        BindTextureDirect(GL_TEXTURE_2D, testTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        unsigned int pixel = 0xFF00FFFFu;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
        BindTextureDirect(GL_TEXTURE_2D, 0);
    }

    bool ok = false;
    {
        ScopedWglMakeCurrent makeOther(otherDC, otherContext);
        if (wglGetCurrentContext() == otherContext) {
            ok = (glIsTexture(testTex) == GL_TRUE);
        }
    }

    {
        ScopedWglMakeCurrent makeGame(gameDC, gameContext);
        if (testTex) { glDeleteTextures(1, &testTex); }
    }

    if (!ok) {
        Log(std::string("SharedContexts: Texture sharing verification FAILED for ") + tag);
    } else {
        Log(std::string("SharedContexts: Texture sharing verification OK for ") + tag);
    }
    return ok;
}

struct ScopedWglUnbind {
    HDC prevDC = NULL;
    HGLRC prevRC = NULL;
    bool unbound = false;
    ScopedWglUnbind() {
        prevRC = wglGetCurrentContext();
        prevDC = wglGetCurrentDC();
        if (prevRC) {
            if (wglMakeCurrent(NULL, NULL)) { unbound = true; }
        }
    }
    ~ScopedWglUnbind() {
        if (unbound && prevRC && prevDC) { wglMakeCurrent(prevDC, prevRC); }
    }
};

bool InitializeSharedContexts(void* gameGLContext, HDC hdc) {
    if (g_sharedContextsReady.load()) {
        return true;
    }

    if (!gameGLContext || !hdc) {
        Log("SharedContexts: Invalid game context or DC");
        return false;
    }

    HGLRC gameContext = (HGLRC)gameGLContext;
    HDC gameDC = wglGetCurrentDC();
    if (!gameDC) { gameDC = hdc; }

    Log("SharedContexts: Initializing all shared contexts...");

    // Prefer dedicated dummy windows/DCs for worker contexts.
    // across multiple worker threads can be unstable on some drivers.
    HDC renderHdc = hdc;
    HDC mirrorHdc = hdc;

    bool renderDummyOk = CreateSharedDummyWindowWithMatchingPixelFormat(hdc, L"render", g_sharedDummyRenderHwnd, g_sharedDummyRenderDC);
    bool mirrorDummyOk = CreateSharedDummyWindowWithMatchingPixelFormat(hdc, L"mirror", g_sharedDummyMirrorHwnd, g_sharedDummyMirrorDC);

    if (renderDummyOk && g_sharedDummyRenderDC) { renderHdc = g_sharedDummyRenderDC; }
    if (mirrorDummyOk && g_sharedDummyMirrorDC) { mirrorHdc = g_sharedDummyMirrorDC; }

    if (renderHdc != hdc && mirrorHdc != hdc) {
        Log("SharedContexts: Using dedicated dummy DCs for render+mirror worker contexts");
    } else if (renderHdc != hdc || mirrorHdc != hdc) {
        Log("SharedContexts: Using a dummy DC for one worker context (partial) - may be more stable than using the game DC for both");
    } else {
        Log("SharedContexts: WARNING: Using game DC for worker contexts (dummy DC unavailable) - may be less stable");
    }

    // Store DCs for later use by worker threads
    g_sharedRenderContextDC.store(renderHdc);
    g_sharedMirrorContextDC.store(mirrorHdc);
    g_sharedContextDC.store(renderHdc);

    HGLRC renderContext = NULL;
    HGLRC mirrorContext = NULL;

    GLint major = 3, minor = 3;
    GLint profileMask = 0;
    GLint flags = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);
    glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
    while (glGetError() != GL_NO_ERROR) {
    }

    if (wglCreateContextAttribsARB) {
        int attribs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB, major, WGL_CONTEXT_MINOR_VERSION_ARB, minor, WGL_CONTEXT_FLAGS_ARB, flags,
                          WGL_CONTEXT_PROFILE_MASK_ARB, (profileMask != 0) ? profileMask : WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB, 0 };

        renderContext = wglCreateContextAttribsARB(renderHdc, gameContext, attribs);
        mirrorContext = wglCreateContextAttribsARB(mirrorHdc, gameContext, attribs);
        if (renderContext && mirrorContext) {
            Log("SharedContexts: Created shared contexts via wglCreateContextAttribsARB (" + std::to_string(major) + "." +
                std::to_string(minor) + ")");
        } else {
            DWORD err = GetLastError();
            Log("SharedContexts: wglCreateContextAttribsARB failed (error " + std::to_string(err) +
                "), falling back to wglCreateContext + wglShareLists");
            if (renderContext) {
                wglDeleteContext(renderContext);
                renderContext = NULL;
            }
            if (mirrorContext) {
                wglDeleteContext(mirrorContext);
                mirrorContext = NULL;
            }
        }
    }

    if (!renderContext || !mirrorContext) {
        renderContext = wglCreateContext(renderHdc);
        if (!renderContext) {
            Log("SharedContexts: Failed to create render context (error " + std::to_string(GetLastError()) + ")");
            return false;
        }

        mirrorContext = wglCreateContext(mirrorHdc);
        if (!mirrorContext) {
            Log("SharedContexts: Failed to create mirror context (error " + std::to_string(GetLastError()) + ")");
            wglDeleteContext(renderContext);
            return false;
        }

        Log("SharedContexts: Created 2 contexts (legacy), now sharing with game...");

        ScopedWglUnbind unbind;

        SetLastError(0);
        if (!wglShareLists(gameContext, renderContext)) {
            DWORD err = GetLastError();
            if (!wglShareLists(renderContext, gameContext)) {
                Log("SharedContexts: Failed to share render context (error " + std::to_string(err) + ", " + std::to_string(GetLastError()) +
                    ")");
                wglDeleteContext(renderContext);
                wglDeleteContext(mirrorContext);
                return false;
            }
        }
        Log("SharedContexts: Render context shared with game");

        SetLastError(0);
        if (!wglShareLists(gameContext, mirrorContext)) {
            DWORD err = GetLastError();
            if (!wglShareLists(mirrorContext, gameContext)) {
                Log("SharedContexts: Failed to share mirror context (error " + std::to_string(err) + ", " + std::to_string(GetLastError()) +
                    ")");
                wglDeleteContext(renderContext);
                wglDeleteContext(mirrorContext);
                return false;
            }
        }
        Log("SharedContexts: Mirror context shared with game");
    }

    // Some driver/pixel-format combinations can appear to succeed but not share correctly, causing
    if (!VerifyTextureSharing(gameContext, gameDC, renderContext, renderHdc, "render") ||
        !VerifyTextureSharing(gameContext, gameDC, mirrorContext, mirrorHdc, "mirror")) {
        if (renderContext) { wglDeleteContext(renderContext); }
        if (mirrorContext) { wglDeleteContext(mirrorContext); }
        return false;
    }

    g_sharedRenderContext.store(renderContext);
    g_sharedMirrorContext.store(mirrorContext);
    g_sharedContextsReady.store(true);

    Log("SharedContexts: All contexts initialized and shared successfully");
    return true;
}

void CleanupSharedContexts() {
    g_sharedContextsReady.store(false);

    HGLRC render = g_sharedRenderContext.exchange(nullptr);
    HGLRC mirror = g_sharedMirrorContext.exchange(nullptr);

    // Only delete if not already deleted by their respective threads
    // Note: Threads should set these to nullptr when they clean up
    if (render) { wglDeleteContext(render); }
    if (mirror) { wglDeleteContext(mirror); }

    g_sharedRenderContextDC.store(nullptr);
    g_sharedMirrorContextDC.store(nullptr);
    g_sharedContextDC.store(nullptr);

    if (g_sharedDummyRenderHwnd && g_sharedDummyRenderDC) {
        ReleaseDC(g_sharedDummyRenderHwnd, g_sharedDummyRenderDC);
        g_sharedDummyRenderDC = NULL;
    }
    if (g_sharedDummyRenderHwnd) {
        DestroyWindow(g_sharedDummyRenderHwnd);
        g_sharedDummyRenderHwnd = NULL;
    }

    if (g_sharedDummyMirrorHwnd && g_sharedDummyMirrorDC) {
        ReleaseDC(g_sharedDummyMirrorHwnd, g_sharedDummyMirrorDC);
        g_sharedDummyMirrorDC = NULL;
    }
    if (g_sharedDummyMirrorHwnd) {
        DestroyWindow(g_sharedDummyMirrorHwnd);
        g_sharedDummyMirrorHwnd = NULL;
    }
    Log("SharedContexts: Cleaned up");
}

HGLRC GetSharedRenderContext() { return g_sharedRenderContext.load(); }

HGLRC GetSharedMirrorContext() { return g_sharedMirrorContext.load(); }

HDC GetSharedRenderContextDC() { return g_sharedRenderContextDC.load(); }

HDC GetSharedMirrorContextDC() { return g_sharedMirrorContextDC.load(); }

HDC GetSharedContextDC() { return g_sharedContextDC.load(); }

bool AreSharedContextsReady() { return g_sharedContextsReady.load(); }



