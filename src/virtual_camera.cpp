#include "virtual_camera.h"
#include "utils.h"

// Prevent Windows min/max macros from conflicting with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>


std::atomic<bool> g_virtualCameraActive{ false };

static std::mutex g_vcMutex;
static std::string g_vcLastError;
static std::atomic<LONGLONG> g_vcLastCaptureTick{ 0 };

constexpr int kVirtualCameraFixedFps = 60;

#define VIDEO_NAME L"OBSVirtualCamVideo"
#define FRAME_HEADER_SIZE 32
#define ALIGN_SIZE(size, align) size = (((size) + (align - 1)) & (~(align - 1)))

enum queue_state {
    SHARED_QUEUE_STATE_INVALID = 0,
    SHARED_QUEUE_STATE_STARTING = 1,
    SHARED_QUEUE_STATE_READY = 2,
    SHARED_QUEUE_STATE_STOPPING = 3,
};

struct queue_header {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t state;
    uint32_t offsets[3];
    uint32_t type;
    uint32_t cx;
    uint32_t cy;
    uint64_t interval;
    uint32_t reserved[8];
};

struct VirtualCameraState {
    HANDLE handle = nullptr;
    queue_header* header = nullptr;
    uint64_t* ts[3] = { nullptr, nullptr, nullptr };
    uint8_t* frame[3] = { nullptr, nullptr, nullptr };
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t interval = 10000000ULL / kVirtualCameraFixedFps;
    LARGE_INTEGER lastFrameTime = {};
    LARGE_INTEGER perfFreq = {};
    bool active = false;
};

static VirtualCameraState g_vcState;


// Helper to clamp int to byte range (avoids Windows min/max macro conflict)
static inline uint8_t clampToByte(int32_t val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return static_cast<uint8_t>(val);
}

// Single pass: computes Y for every pixel and UV for every 2x2 block simultaneously
static void ConvertRGBAtoNV12(const uint8_t* __restrict rgba, uint8_t* __restrict nv12, uint32_t width, uint32_t height) {
    const uint32_t yPlaneSize = width * height;
    uint8_t* __restrict yPlane = nv12;
    uint8_t* __restrict uvPlane = nv12 + yPlaneSize;
    const uint32_t stride = width * 4;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* __restrict srcRow0 = rgba + (height - 1 - y) * stride;
        const uint8_t* __restrict srcRow1 = rgba + (height - 2 - y) * stride;
        uint8_t* __restrict yRow0 = yPlane + y * width;
        uint8_t* __restrict yRow1 = yPlane + (y + 1) * width;
        uint8_t* __restrict uvRow = uvPlane + (y / 2) * width;

        for (uint32_t x = 0; x < width; x += 2) {
            // Load 2x2 block of RGBA pixels
            const uint8_t* p00 = srcRow0 + x * 4;
            const uint8_t* p10 = srcRow0 + (x + 1) * 4;
            const uint8_t* p01 = srcRow1 + x * 4;
            const uint8_t* p11 = srcRow1 + (x + 1) * 4;

            int32_t y00 = ((66 * p00[0] + 129 * p00[1] + 25 * p00[2] + 128) >> 8) + 16;
            int32_t y10 = ((66 * p10[0] + 129 * p10[1] + 25 * p10[2] + 128) >> 8) + 16;
            int32_t y01 = ((66 * p01[0] + 129 * p01[1] + 25 * p01[2] + 128) >> 8) + 16;
            int32_t y11 = ((66 * p11[0] + 129 * p11[1] + 25 * p11[2] + 128) >> 8) + 16;

            yRow0[x] = clampToByte(y00);
            yRow0[x + 1] = clampToByte(y10);
            yRow1[x] = clampToByte(y01);
            yRow1[x + 1] = clampToByte(y11);

            // Average RGB of 2x2 block for chroma (shift right by 2 = divide by 4)
            int32_t avgR = (p00[0] + p10[0] + p01[0] + p11[0] + 2) >> 2;
            int32_t avgG = (p00[1] + p10[1] + p01[1] + p11[1] + 2) >> 2;
            int32_t avgB = (p00[2] + p10[2] + p01[2] + p11[2] + 2) >> 2;

            int32_t u = ((-38 * avgR - 74 * avgG + 112 * avgB + 128) >> 8) + 128;
            int32_t v = ((112 * avgR - 94 * avgG - 18 * avgB + 128) >> 8) + 128;

            uvRow[x] = clampToByte(u);
            uvRow[x + 1] = clampToByte(v);
        }
    }
}

bool IsVirtualCameraDriverInstalled() {
    HKEY hKey;
    LONG result = RegOpenKeyExA(HKEY_CLASSES_ROOT, "CLSID\\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}", 0, KEY_READ, &hKey);

    if (result == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }

    const char* possiblePaths[] = {
        "C:\\Program Files\\obs-studio\\data\\obs-plugins\\win-dshow\\obs-virtualcam-module64.dll",
        "C:\\Program Files (x86)\\obs-studio\\data\\obs-plugins\\win-dshow\\obs-virtualcam-module64.dll",
    };

    for (const auto& path : possiblePaths) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { return true; }
    }

    return false;
}

bool IsVirtualCameraInUseByOBS() {
    if (g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    HANDLE testHandle = OpenFileMappingW(FILE_MAP_READ, FALSE, VIDEO_NAME);
    if (!testHandle) {
        return false;
    }

    queue_header* testHeader = static_cast<queue_header*>(MapViewOfFile(testHandle, FILE_MAP_READ, 0, 0, sizeof(queue_header)));

    bool inUse = false;
    if (testHeader) {
        inUse = (testHeader->state == SHARED_QUEUE_STATE_READY || testHeader->state == SHARED_QUEUE_STATE_STARTING);
        UnmapViewOfFile(testHeader);
    }

    CloseHandle(testHandle);
    return inUse;
}

bool StartVirtualCamera(uint32_t width, uint32_t height) {
    if ((width & 1U) != 0) { width -= 1; }
    if ((height & 1U) != 0) { height -= 1; }
    if (width < 2 || height < 2) {
        g_vcLastError = "Invalid virtual camera dimensions";
        Log("Virtual Camera: " + g_vcLastError);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (g_vcState.active) {
        g_vcLastError = "Virtual camera already active";
        return true;
    }

    if (!IsVirtualCameraDriverInstalled()) {
        g_vcLastError = "OBS Virtual Camera driver not installed";
        Log("Virtual Camera: " + g_vcLastError);
        return false;
    }

    if (IsVirtualCameraInUseByOBS()) {
        g_vcLastError = "Virtual camera is currently in use by OBS";
        Log("Virtual Camera: " + g_vcLastError);
        return false;
    }

    g_vcState.interval = 10000000ULL / kVirtualCameraFixedFps;
    QueryPerformanceFrequency(&g_vcState.perfFreq);
    g_vcState.lastFrameTime.QuadPart = 0;
    g_vcLastCaptureTick.store(0, std::memory_order_release);

    uint32_t frameSize = width * height * 3 / 2;
    uint32_t offset_frame[3];
    uint32_t totalSize;

    totalSize = sizeof(queue_header);
    ALIGN_SIZE(totalSize, 32);

    offset_frame[0] = totalSize;
    totalSize += frameSize + FRAME_HEADER_SIZE;
    ALIGN_SIZE(totalSize, 32);

    offset_frame[1] = totalSize;
    totalSize += frameSize + FRAME_HEADER_SIZE;
    ALIGN_SIZE(totalSize, 32);

    offset_frame[2] = totalSize;
    totalSize += frameSize + FRAME_HEADER_SIZE;
    ALIGN_SIZE(totalSize, 32);

    g_vcState.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, totalSize, VIDEO_NAME);

    if (!g_vcState.handle) {
        g_vcLastError = "Failed to create shared memory (error " + std::to_string(GetLastError()) + ")";
        Log("Virtual Camera: " + g_vcLastError);
        return false;
    }

    g_vcState.header = static_cast<queue_header*>(MapViewOfFile(g_vcState.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));

    if (!g_vcState.header) {
        CloseHandle(g_vcState.handle);
        g_vcState.handle = nullptr;
        g_vcLastError = "Failed to map shared memory";
        Log("Virtual Camera: " + g_vcLastError);
        return false;
    }

    memset(g_vcState.header, 0, sizeof(queue_header));
    g_vcState.header->state = SHARED_QUEUE_STATE_STARTING;
    g_vcState.header->type = 0;
    g_vcState.header->cx = width;
    g_vcState.header->cy = height;
    g_vcState.header->interval = g_vcState.interval;

    for (int i = 0; i < 3; i++) {
        g_vcState.header->offsets[i] = offset_frame[i];
        uint8_t* basePtr = reinterpret_cast<uint8_t*>(g_vcState.header);
        g_vcState.ts[i] = reinterpret_cast<uint64_t*>(basePtr + offset_frame[i]);
        g_vcState.frame[i] = basePtr + offset_frame[i] + FRAME_HEADER_SIZE;
    }

    g_vcState.width = width;
    g_vcState.height = height;
    g_vcState.active = true;
    g_virtualCameraActive.store(true, std::memory_order_release);

    Log("Virtual Camera: Started at " + std::to_string(width) + "x" + std::to_string(height) + " @ 60fps");
    return true;
}

void StopVirtualCamera() {
    g_virtualCameraActive.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (!g_vcState.active) { return; }

    if (g_vcState.header) { g_vcState.header->state = SHARED_QUEUE_STATE_STOPPING; }

    if (g_vcState.header) {
        UnmapViewOfFile(g_vcState.header);
        g_vcState.header = nullptr;
    }

    if (g_vcState.handle) {
        CloseHandle(g_vcState.handle);
        g_vcState.handle = nullptr;
    }

    for (int i = 0; i < 3; i++) {
        g_vcState.ts[i] = nullptr;
        g_vcState.frame[i] = nullptr;
    }

    g_vcState.active = false;
    g_vcState.lastFrameTime.QuadPart = 0;
    g_vcLastCaptureTick.store(0, std::memory_order_release);

    Log("Virtual Camera: Stopped");
}

bool ShouldCaptureVirtualCameraFrame() {
    if (!g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const LONGLONG minTicks = g_vcState.perfFreq.QuadPart / kVirtualCameraFixedFps;
    if (minTicks <= 0) { return true; }

    LONGLONG observed = g_vcLastCaptureTick.load(std::memory_order_relaxed);
    while (true) {
        if (observed != 0 && (now.QuadPart - observed) < minTicks) { return false; }
        if (g_vcLastCaptureTick.compare_exchange_weak(observed, now.QuadPart, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

bool WriteVirtualCameraFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, uint64_t timestamp) {
    if (!g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (!g_vcState.active || !g_vcState.header) { return false; }

    // FPS limiting before any work (lock-free fast path)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_vcState.lastFrameTime.QuadPart != 0) {
        LONGLONG elapsed = now.QuadPart - g_vcState.lastFrameTime.QuadPart;
        LONGLONG minTicks = g_vcState.perfFreq.QuadPart / kVirtualCameraFixedFps;
        if (elapsed < minTicks) {
            return true;
        }
    }

    if (width != g_vcState.width || height != g_vcState.height) { return false; }

    uint32_t writeIdx = g_vcState.header->write_idx + 1;
    uint32_t idx = writeIdx % 3;
    uint8_t* dst = g_vcState.frame[idx];

    ConvertRGBAtoNV12(rgba_data, dst, width, height);

    *g_vcState.ts[idx] = timestamp;

    MemoryBarrier();

    // Update indices atomically
    g_vcState.header->write_idx = writeIdx;
    g_vcState.header->read_idx = writeIdx;
    g_vcState.header->state = SHARED_QUEUE_STATE_READY;

    MemoryBarrier();

    static int frameCount = 0;
    if (frameCount < 3) {
        uint32_t frameSize = width * height * 3 / 2;
        Log("Virtual Camera: Wrote frame " + std::to_string(frameCount) + " at idx " + std::to_string(idx) +
            " ts=" + std::to_string(timestamp) + " size=" + std::to_string(frameSize));
        frameCount++;
    }

    g_vcState.lastFrameTime = now;
    return true;
}

bool WriteVirtualCameraFrameNV12(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint64_t timestamp) {
    if (!g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (!g_vcState.active || !g_vcState.header) { return false; }

    // FPS limiting (lock-free integer comparison)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_vcState.lastFrameTime.QuadPart != 0) {
        LONGLONG elapsed = now.QuadPart - g_vcState.lastFrameTime.QuadPart;
        LONGLONG minTicks = g_vcState.perfFreq.QuadPart / kVirtualCameraFixedFps;
        if (elapsed < minTicks) {
            return true;
        }
    }

    if (width != g_vcState.width || height != g_vcState.height) { return false; }

    uint32_t writeIdx = g_vcState.header->write_idx + 1;
    uint32_t idx = writeIdx % 3;

    uint32_t frameSize = width * height * 3 / 2;
    memcpy(g_vcState.frame[idx], nv12_data, frameSize);

    *g_vcState.ts[idx] = timestamp;

    MemoryBarrier();

    g_vcState.header->write_idx = writeIdx;
    g_vcState.header->read_idx = writeIdx;
    g_vcState.header->state = SHARED_QUEUE_STATE_READY;

    MemoryBarrier();

    g_vcState.lastFrameTime = now;
    return true;
}

bool IsVirtualCameraActive() { return g_virtualCameraActive.load(std::memory_order_acquire); }

const char* GetVirtualCameraError() { return g_vcLastError.c_str(); }


