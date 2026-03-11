#pragma once

#include <atomic>
#include <string>

// Thread runs independently at ~60Hz, handling logic checks that don't require the GL context
// This offloads work from the game's render thread (SwapBuffers hook)

// Pre-computed viewport mode data, updated by logic_thread when mode changes
struct CachedModeViewport {
    int width = 0;
    int height = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;
    bool valid = false;
};

// Double-buffered viewport cache for lock-free access
// Logic thread writes, game thread (hkglViewport) reads
extern CachedModeViewport g_viewportModeCache[2];
extern std::atomic<int> g_viewportModeCacheIndex;

// Update the cached viewport mode data (called by logic_thread when mode changes)
void UpdateCachedViewportMode();

extern std::atomic<bool> g_logicThreadRunning;

// Start the logic thread (call after config is loaded and HWND is known)
void StartLogicThread();

// Stop the logic thread (call before DLL unload)
void StopLogicThread();

// These are updated by the logic thread and read by the render thread
//   extern std::atomic<bool> g_graphicsHookDetected;
//   extern std::atomic<HMODULE> g_graphicsHookModule;


void PollObsGraphicsHook();

void CheckWorldExitReset();

// Apply Windows mouse speed setting if config changed
void CheckWindowsMouseSpeedChange();

void ProcessPendingModeSwitch();

void CheckGameStateReset();

// Safe to call from any thread without locking
int GetCachedWindowWidth();
int GetCachedWindowHeight();

// Updates cached game-window client metrics immediately from a resize event.
// Safe to call from the subclassed window thread.
void UpdateCachedWindowMetricsFromSize(int clientWidth, int clientHeight);

// the game window is currently on. Safe to call from any thread.
void InvalidateCachedScreenMetrics();

// Marks screen metrics as dirty and requests mode-size recalculation on the logic thread.
// Call this for resize/DPI/display events so relative-sized modes are recomputed immediately.
void RequestScreenMetricsRecalculation();


