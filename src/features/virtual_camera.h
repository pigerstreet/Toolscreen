#pragma once

#include <cstdint>
#include <atomic>

// This works independently of OBS Studio - the driver just needs to be installed

bool StartVirtualCamera(uint32_t width, uint32_t height);

void StopVirtualCamera();

bool WriteVirtualCameraFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, uint64_t timestamp);

// nv12_data must be width*height*3/2 bytes (NV12 format)
bool WriteVirtualCameraFrameNV12(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint64_t timestamp);

bool WriteVirtualCameraFrameNV12Planes(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t width, uint32_t height,
									   uint64_t timestamp);

bool IsVirtualCameraActive();

// Check if OBS Virtual Camera driver is installed
bool IsVirtualCameraDriverInstalled();

bool IsVirtualCameraInUseByOBS();

bool ShouldCaptureVirtualCameraFrame();

// Single public resize entry point: updates dimensions in-place if capacity allows,
// otherwise recreates the shared memory producer. Thread-safe.
bool EnsureVirtualCameraSize(uint32_t width, uint32_t height);

bool GetVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight);

void GetVirtualCameraMonitorSize(uint32_t& outWidth, uint32_t& outHeight);

bool GetPreferredVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight);

// Call this when the game window is resized. Records pending resize state;
// the actual resize is deferred and debounced via FlushPendingVirtualCameraResize().
void OnGameWindowResized(uint32_t newWidth, uint32_t newHeight);

// Called once per frame (from SyncVirtualCameraRuntimeState) to apply any pending
// debounced resize. Returns true if a resize was actually performed.
bool FlushPendingVirtualCameraResize();

void RequestVirtualCameraRecoveryFrames();

const char* GetVirtualCameraError();

extern std::atomic<bool> g_virtualCameraActive;


