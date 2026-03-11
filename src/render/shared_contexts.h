#pragma once

#include <Windows.h>
#include <atomic>

// All contexts that need to share textures must be created and shared at the same time,
// before any of them are made current on their respective threads.

// Pre-created shared contexts (created on main thread, used by worker threads)
extern std::atomic<HGLRC> g_sharedRenderContext;    // For render thread
extern std::atomic<HGLRC> g_sharedMirrorContext;    // For mirror capture thread
// Each worker thread must use its own DC/drawable.
// Using the same HDC for two contexts on different threads is undefined on many drivers and
extern std::atomic<HDC> g_sharedRenderContextDC;     // DC for render thread context
extern std::atomic<HDC> g_sharedMirrorContextDC;     // DC for mirror capture thread context
extern std::atomic<HDC> g_sharedContextDC;

extern std::atomic<bool> g_sharedContextsReady;

// Must be called from main thread with a valid GL context current
bool InitializeSharedContexts(void* gameGLContext, HDC hdc);

void CleanupSharedContexts();

HGLRC GetSharedRenderContext();
HGLRC GetSharedMirrorContext();
HDC GetSharedRenderContextDC();
HDC GetSharedMirrorContextDC();
HDC GetSharedContextDC();

bool AreSharedContextsReady();


