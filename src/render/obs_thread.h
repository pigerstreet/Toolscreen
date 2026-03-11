#pragma once

#include <GL/glew.h>
#include <atomic>
#include <windows.h>

// This thread hooks into OBS's graphics-hook to redirect capture from backbuffer to our overlay texture

void StartObsHookThread();
void StopObsHookThread();

extern std::atomic<bool> g_obsOverrideEnabled;
extern std::atomic<GLuint> g_obsOverrideTexture;
extern std::atomic<int> g_obsOverrideWidth;
extern std::atomic<int> g_obsOverrideHeight;

extern std::atomic<bool> g_obsPre113Windowed;
extern std::atomic<int> g_obsPre113OffsetX;
extern std::atomic<int> g_obsPre113OffsetY;
extern std::atomic<int> g_obsPre113ContentW;
extern std::atomic<int> g_obsPre113ContentH;

void CaptureBackbufferForObs(int width, int height);

// Set the override texture (called by render_thread after compositing)
void SetObsOverrideTexture(GLuint texture, int width, int height);

void ClearObsOverride();

void EnableObsOverride();

bool IsObsHookDetected();

GLuint GetObsCaptureTexture();
int GetObsCaptureWidth();
int GetObsCaptureHeight();


