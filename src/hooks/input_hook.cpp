#include "input_hook.h"

#include "features/fake_cursor.h"
#include "features/virtual_camera.h"
#include "gui/gui.h"
#include "gui/imgui_cache.h"
#include "runtime/logic_thread.h"
#include "common/profiler.h"
#include "render/render.h"
#include "common/utils.h"
#include "version.h"
#include "features/window_overlay.h"

#include "imgui_impl_win32.h"

#include "gui/imgui_input_queue.h"

#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <windowsx.h>

typedef UINT(WINAPI* GETRAWINPUTDATAPROC)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);

extern std::atomic<bool> g_showGui;
extern std::atomic<bool> g_guiNeedsRecenter;
extern std::atomic<bool> g_wasCursorVisible;
extern std::atomic<bool> g_isShuttingDown;
extern std::atomic<HWND> g_subclassedHwnd;
extern WNDPROC g_originalWndProc;
extern std::atomic<bool> g_configLoadFailed;
extern std::atomic<int> g_wmMouseMoveCount;
extern GameVersion g_gameVersion;
extern Config g_config;
extern HMODULE g_hModule;

extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;
extern std::string g_currentlyEditingMirror;

extern std::atomic<bool> g_imageDragMode;
extern std::atomic<bool> g_windowOverlayDragMode;
extern std::atomic<HCURSOR> g_specialCursorHandle;
// g_glInitialized is declared in render.h as atomic bool
extern std::atomic<bool> g_gameWindowActive;

extern std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::mutex g_hotkeyTimestampsMutex;
extern std::unordered_set<DWORD> g_hotkeyMainKeys;
extern std::mutex g_hotkeyMainKeysMutex;
extern std::atomic<bool> g_tempSensitivityActiveAtomic;
extern std::atomic<float> g_tempSensitivityXAtomic;
extern std::atomic<float> g_tempSensitivityYAtomic;
extern std::set<std::string> g_triggerOnReleasePending;
extern std::set<std::string> g_triggerOnReleaseInvalidated;
extern std::mutex g_triggerOnReleaseMutex;
extern GETRAWINPUTDATAPROC oGetRawInputData;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
static bool s_forcedShowCursor = false;
static size_t s_bestMatchKeyCount = 0;
static std::unordered_map<DWORD, size_t> s_bestMatchKeyCountByMainVk;
static std::atomic<int> s_cachedSystemCursorVisible{ -1 };
static std::atomic<ULONGLONG> s_cachedSystemCursorVisibilityTick{ 0 };
static HHOOK s_lowLevelKeyboardHook = NULL;
static std::mutex s_lowLevelKeyboardHookMutex;
static std::atomic<bool> s_deferredFocusRegainWmSizePending{ false };
static constexpr UINT_PTR kToolscreenKeyRepeatTimerId = 0x54535250;

struct MouseMovementThrottleState {
    std::mutex mutex;
    std::chrono::steady_clock::time_point lastWindowMoveForwardTime{};
    std::chrono::steady_clock::time_point lastRawInputForwardTime{};
    long long pendingRawDeltaX = 0;
    long long pendingRawDeltaY = 0;
    HRAWINPUT pendingRawInjectionHandle = NULL;
    LONG pendingRawInjectionX = 0;
    LONG pendingRawInjectionY = 0;
};

static MouseMovementThrottleState s_mouseMovementThrottleState;

struct LowLevelSuppressedKeyState {
    DWORD rawVk = 0;
    UINT scanCodeWithFlags = 0;
    bool isSystemKey = false;
};

struct LocalKeyRepeatState {
    DWORD rawVk = 0;
    WPARAM keyWParam = 0;
    UINT keyMsg = 0;
    UINT scanCodeWithFlags = 0;
    uint64_t pressSequence = 0;
    std::chrono::steady_clock::time_point initialPressTime{};
    std::chrono::steady_clock::time_point nextRepeatTime{};
    bool hasCharacterMessage = false;
    UINT charMsg = 0;
    WPARAM charWParam = 0;
    UINT charScanCodeWithFlags = 0;
};

static bool IsModifierVk(DWORD vk);
static LPARAM BuildKeyboardMessageLParam(UINT scanCodeWithFlags, bool isKeyDown, bool isSystemKey, UINT repeatCount,
                                         bool previousKeyState, bool transitionState);
static std::chrono::steady_clock::time_point ResolveNextRepeatTimeFromPress(const std::chrono::steady_clock::time_point& initialPressTime);

static LONG ClampAccumulatedRawMouseDelta(long long value) {
    if (value > static_cast<long long>(LONG_MAX)) return LONG_MAX;
    if (value < static_cast<long long>(LONG_MIN)) return LONG_MIN;
    return static_cast<LONG>(value);
}

static int ResolveConfiguredMouseMovementPollingRate() {
    return std::clamp(g_config.mouseMovementPollingRate, 0, ConfigDefaults::CONFIG_MOUSE_MOVEMENT_POLLING_RATE_MAX);
}

void ResetMouseMovementThrottleState() {
    std::lock_guard<std::mutex> lock(s_mouseMovementThrottleState.mutex);
    s_mouseMovementThrottleState.lastWindowMoveForwardTime = std::chrono::steady_clock::time_point{};
    s_mouseMovementThrottleState.lastRawInputForwardTime = std::chrono::steady_clock::time_point{};
    s_mouseMovementThrottleState.pendingRawDeltaX = 0;
    s_mouseMovementThrottleState.pendingRawDeltaY = 0;
    s_mouseMovementThrottleState.pendingRawInjectionHandle = NULL;
    s_mouseMovementThrottleState.pendingRawInjectionX = 0;
    s_mouseMovementThrottleState.pendingRawInjectionY = 0;
}

bool ConsumePendingRawMouseMovementThrottleInjection(HRAWINPUT rawInputHandle, LONG& pendingX, LONG& pendingY) {
    pendingX = 0;
    pendingY = 0;

    std::lock_guard<std::mutex> lock(s_mouseMovementThrottleState.mutex);
    if (s_mouseMovementThrottleState.pendingRawInjectionHandle != rawInputHandle) {
        return false;
    }

    pendingX = s_mouseMovementThrottleState.pendingRawInjectionX;
    pendingY = s_mouseMovementThrottleState.pendingRawInjectionY;
    s_mouseMovementThrottleState.pendingRawInjectionHandle = NULL;
    s_mouseMovementThrottleState.pendingRawInjectionX = 0;
    s_mouseMovementThrottleState.pendingRawInjectionY = 0;
    return pendingX != 0 || pendingY != 0;
}

static bool TryReadRawMouseInput(HRAWINPUT rawInputHandle, RAWINPUT& outRawInput) {
    if (!rawInputHandle || !oGetRawInputData) return false;

    UINT rawSize = sizeof(outRawInput);
    const UINT readResult = oGetRawInputData(rawInputHandle, RID_INPUT, &outRawInput, &rawSize, sizeof(RAWINPUTHEADER));
    if (readResult == static_cast<UINT>(-1) || rawSize < sizeof(RAWINPUTHEADER)) {
        return false;
    }

    return outRawInput.header.dwType == RIM_TYPEMOUSE;
}

static void PreparePendingRawMouseMovementThrottleInjection(HRAWINPUT rawInputHandle) {
    if (!rawInputHandle) return;

    const LONG pendingX = ClampAccumulatedRawMouseDelta(s_mouseMovementThrottleState.pendingRawDeltaX);
    const LONG pendingY = ClampAccumulatedRawMouseDelta(s_mouseMovementThrottleState.pendingRawDeltaY);
    if (pendingX == 0 && pendingY == 0) return;

    s_mouseMovementThrottleState.pendingRawInjectionHandle = rawInputHandle;
    s_mouseMovementThrottleState.pendingRawInjectionX = pendingX;
    s_mouseMovementThrottleState.pendingRawInjectionY = pendingY;
    s_mouseMovementThrottleState.pendingRawDeltaX = 0;
    s_mouseMovementThrottleState.pendingRawDeltaY = 0;
}

static InputHandlerResult HandleMouseMovementRateLimit(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_MOUSEMOVE && uMsg != WM_INPUT) {
        return { false, 0 };
    }

    const int rateLimit = ResolveConfiguredMouseMovementPollingRate();
    if (rateLimit <= 0 || g_showGui.load(std::memory_order_acquire) || g_isShuttingDown.load(std::memory_order_acquire)) {
        ResetMouseMovementThrottleState();
        return { false, 0 };
    }

    using Clock = std::chrono::steady_clock;
    const auto minInterval = std::chrono::duration<double>(1.0 / static_cast<double>(rateLimit));
    const Clock::time_point now = Clock::now();

    if (uMsg == WM_MOUSEMOVE) {
        std::lock_guard<std::mutex> lock(s_mouseMovementThrottleState.mutex);
        const bool hasRecentForward =
            s_mouseMovementThrottleState.lastWindowMoveForwardTime.time_since_epoch() != Clock::duration::zero();
        if (hasRecentForward && (now - s_mouseMovementThrottleState.lastWindowMoveForwardTime) < minInterval) {
            return { true, 0 };
        }

        s_mouseMovementThrottleState.lastWindowMoveForwardTime = now;
        return { false, 0 };
    }

    RAWINPUT rawInput{};
    if (!TryReadRawMouseInput(reinterpret_cast<HRAWINPUT>(lParam), rawInput)) {
        return { false, 0 };
    }

    const RAWMOUSE& rawMouse = rawInput.data.mouse;
    const bool isRelativeMove = (rawMouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0;
    const bool hasMovement = rawMouse.lLastX != 0 || rawMouse.lLastY != 0;
    const bool hasButtonOrWheelState = rawMouse.usButtonFlags != 0;
    const bool isPureMoveEvent = isRelativeMove && hasMovement && !hasButtonOrWheelState;

    std::lock_guard<std::mutex> lock(s_mouseMovementThrottleState.mutex);
    const bool hasRecentForward = s_mouseMovementThrottleState.lastRawInputForwardTime.time_since_epoch() != Clock::duration::zero();

    if (isPureMoveEvent && hasRecentForward && (now - s_mouseMovementThrottleState.lastRawInputForwardTime) < minInterval) {
        s_mouseMovementThrottleState.pendingRawDeltaX += static_cast<long long>(rawMouse.lLastX);
        s_mouseMovementThrottleState.pendingRawDeltaY += static_cast<long long>(rawMouse.lLastY);
        return { true, DefWindowProc(hWnd, uMsg, wParam, lParam) };
    }

    PreparePendingRawMouseMovementThrottleInjection(reinterpret_cast<HRAWINPUT>(lParam));
    if (hasMovement || s_mouseMovementThrottleState.pendingRawInjectionHandle != NULL) {
        s_mouseMovementThrottleState.lastRawInputForwardTime = now;
    }

    return { false, 0 };
}

static std::unordered_map<DWORD, LowLevelSuppressedKeyState> s_lowLevelSuppressedKeys;
static std::mutex s_lowLevelSuppressedKeysMutex;
static std::unordered_map<uint64_t, LocalKeyRepeatState> s_localKeyRepeatStates;
static std::optional<uint64_t> s_localKeyRepeatActiveId;
static uint64_t s_localKeyRepeatPressSequence = 0;

static UINT GetScanCodeWithExtendedFlagFromLParam(LPARAM lParam) {
    UINT scanCodeWithFlags = static_cast<UINT>((lParam >> 16) & 0xFF);
    if ((lParam & (1LL << 24)) != 0) {
        scanCodeWithFlags |= 0xE000;
    }
    return scanCodeWithFlags;
}

static uint64_t MakeLocalKeyRepeatId(DWORD rawVk, UINT scanCodeWithFlags) {
    return (static_cast<uint64_t>(rawVk) << 32) | static_cast<uint64_t>(scanCodeWithFlags);
}

static LocalKeyRepeatState* FindActiveLocalKeyRepeatState() {
    if (!s_localKeyRepeatActiveId.has_value()) return nullptr;

    auto it = s_localKeyRepeatStates.find(*s_localKeyRepeatActiveId);
    if (it == s_localKeyRepeatStates.end()) {
        s_localKeyRepeatActiveId.reset();
        return nullptr;
    }

    return &it->second;
}

static void ResetLocalKeyRepeatSchedule(LocalKeyRepeatState& state) {
    state.initialPressTime = std::chrono::steady_clock::now();
    if (state.hasCharacterMessage) {
        state.nextRepeatTime = ResolveNextRepeatTimeFromPress(state.initialPressTime);
    }
}

static void SetActiveLocalKeyRepeatState(uint64_t repeatId, bool restartDelay) {
    auto it = s_localKeyRepeatStates.find(repeatId);
    if (it == s_localKeyRepeatStates.end()) {
        s_localKeyRepeatActiveId.reset();
        return;
    }

    s_localKeyRepeatActiveId = repeatId;
    if (restartDelay) {
        ResetLocalKeyRepeatSchedule(it->second);
    }
}

static void ActivateMostRecentHeldLocalKeyRepeatState() {
    uint64_t bestRepeatId = 0;
    uint64_t bestSequence = 0;
    bool found = false;

    for (const auto& [repeatId, state] : s_localKeyRepeatStates) {
        if (!found || state.pressSequence > bestSequence) {
            bestRepeatId = repeatId;
            bestSequence = state.pressSequence;
            found = true;
        }
    }

    if (!found) {
        s_localKeyRepeatActiveId.reset();
        return;
    }

    SetActiveLocalKeyRepeatState(bestRepeatId, true);
}

static bool IsCharacterRepeatMessage(UINT uMsg) {
    switch (uMsg) {
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        return true;
    default:
        return false;
    }
}

static UINT ResolveSystemKeyboardStartDelayMs() {
    UINT keyboardDelay = 0;
    if (!SystemParametersInfo(SPI_GETKEYBOARDDELAY, 0, &keyboardDelay, 0)) {
        return 500;
    }

    if (keyboardDelay > 3) keyboardDelay = 3;
    return (keyboardDelay + 1) * 250;
}

static UINT ResolveSystemKeyboardRepeatIntervalMs() {
    UINT keyboardSpeed = 31;
    if (!SystemParametersInfo(SPI_GETKEYBOARDSPEED, 0, &keyboardSpeed, 0)) {
        return 33;
    }

    if (keyboardSpeed > 31) keyboardSpeed = 31;

    const double charsPerSecond = 2.5 + (27.5 * (static_cast<double>(keyboardSpeed) / 31.0));
    const UINT intervalMs = static_cast<UINT>(std::lround(1000.0 / charsPerSecond));
    return intervalMs == 0 ? 1 : intervalMs;
}

static UINT ResolveConfiguredKeyRepeatStartDelayMs() {
    int configuredDelay = g_config.keyRepeatStartDelay;
    if (configuredDelay >= 0) {
        if (configuredDelay < 50) configuredDelay = 50;
        if (configuredDelay > 500) configuredDelay = 500;
        return static_cast<UINT>(configuredDelay);
    }
    return ResolveSystemKeyboardStartDelayMs();
}

static UINT ResolveConfiguredKeyRepeatIntervalMs() {
    int configuredDelay = g_config.keyRepeatDelay;
    if (configuredDelay >= 0) {
        if (configuredDelay < 0) configuredDelay = 0;
        if (configuredDelay > 500) configuredDelay = 500;
        return static_cast<UINT>(configuredDelay);
    }
    return ResolveSystemKeyboardRepeatIntervalMs();
}

static std::chrono::steady_clock::time_point ResolveNextRepeatTimeFromPress(const std::chrono::steady_clock::time_point& initialPressTime) {
    using Clock = std::chrono::steady_clock;

    const Clock::time_point now = Clock::now();
    const auto startDelay = std::chrono::milliseconds(ResolveConfiguredKeyRepeatStartDelayMs());
    const UINT repeatIntervalMs = ResolveConfiguredKeyRepeatIntervalMs();

    Clock::time_point nextRepeatTime = initialPressTime + startDelay;
    if (nextRepeatTime > now) {
        return nextRepeatTime;
    }

    if (repeatIntervalMs == 0) {
        return now;
    }

    const auto repeatInterval = std::chrono::milliseconds(repeatIntervalMs);

    const auto elapsed = now - nextRepeatTime;
    const auto intervalCount = elapsed / repeatInterval;
    nextRepeatTime += repeatInterval * (intervalCount + 1);
    return nextRepeatTime;
}

static void UpdateLocalKeyRepeatTimer(HWND hWnd) {
    if (!hWnd) return;

    using Clock = std::chrono::steady_clock;

    const LocalKeyRepeatState* activeState = FindActiveLocalKeyRepeatState();
    if (!activeState || !activeState->hasCharacterMessage) {
        KillTimer(hWnd, kToolscreenKeyRepeatTimerId);
        return;
    }

    const Clock::time_point now = Clock::now();
    UINT dueMs = 1;
    if (activeState->nextRepeatTime > now) {
        const auto waitDuration = std::chrono::duration_cast<std::chrono::milliseconds>(activeState->nextRepeatTime - now);
        dueMs = static_cast<UINT>(waitDuration.count());
        if (dueMs == 0) dueMs = 1;
    }

    SetTimer(hWnd, kToolscreenKeyRepeatTimerId, dueMs, NULL);
}

static void ClearLocalKeyRepeatStates(HWND hWnd) {
    s_localKeyRepeatStates.clear();
    s_localKeyRepeatActiveId.reset();
    s_localKeyRepeatPressSequence = 0;
    if (hWnd) {
        KillTimer(hWnd, kToolscreenKeyRepeatTimerId);
    }
}

static void RefreshLocalKeyRepeatSchedule(HWND hWnd) {
    LocalKeyRepeatState* activeState = FindActiveLocalKeyRepeatState();
    if (activeState && activeState->hasCharacterMessage) {
        activeState->nextRepeatTime = ResolveNextRepeatTimeFromPress(activeState->initialPressTime);
    }

    UpdateLocalKeyRepeatTimer(hWnd);
}

static LocalKeyRepeatState* FindLocalKeyRepeatState(DWORD rawVk, UINT scanCodeWithFlags) {
    const uint64_t repeatId = MakeLocalKeyRepeatId(rawVk, scanCodeWithFlags);
    auto it = s_localKeyRepeatStates.find(repeatId);
    if (it == s_localKeyRepeatStates.end()) return nullptr;
    return &it->second;
}

static LocalKeyRepeatState* FindLocalKeyRepeatStateForCharMessage(LPARAM lParam) {
    const UINT scanCodeWithFlags = GetScanCodeWithExtendedFlagFromLParam(lParam);
    for (auto& [repeatId, state] : s_localKeyRepeatStates) {
        (void)repeatId;
        if (state.scanCodeWithFlags == scanCodeWithFlags) {
            return &state;
        }
    }
    return nullptr;
}

static void TrackInitialLocalKeyRepeatKeyDown(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_KEYDOWN && uMsg != WM_SYSKEYDOWN) return;

    const DWORD rawVk = static_cast<DWORD>(wParam);
    if (rawVk == 0 || IsModifierVk(rawVk)) return;

    const UINT scanCodeWithFlags = GetScanCodeWithExtendedFlagFromLParam(lParam);
    if (FindLocalKeyRepeatState(rawVk, scanCodeWithFlags) != nullptr) {
        return;
    }

    LocalKeyRepeatState state{};
    state.rawVk = rawVk;
    state.keyWParam = wParam;
    state.keyMsg = uMsg;
    state.scanCodeWithFlags = scanCodeWithFlags;
    state.pressSequence = ++s_localKeyRepeatPressSequence;
    state.initialPressTime = std::chrono::steady_clock::now();

    const uint64_t repeatId = MakeLocalKeyRepeatId(rawVk, state.scanCodeWithFlags);
    s_localKeyRepeatStates[repeatId] = state;
    SetActiveLocalKeyRepeatState(repeatId, false);
    UpdateLocalKeyRepeatTimer(hWnd);
}

static void ReleaseLocalKeyRepeatKey(HWND hWnd, WPARAM wParam, LPARAM lParam) {
    const DWORD rawVk = static_cast<DWORD>(wParam);
    const UINT scanCodeWithFlags = GetScanCodeWithExtendedFlagFromLParam(lParam);
    const uint64_t repeatId = MakeLocalKeyRepeatId(rawVk, scanCodeWithFlags);
    const bool wasActive = s_localKeyRepeatActiveId.has_value() && *s_localKeyRepeatActiveId == repeatId;

    s_localKeyRepeatStates.erase(repeatId);
    if (wasActive) {
        if (g_config.keyRepeatResumePreviousHeldKey) {
            ActivateMostRecentHeldLocalKeyRepeatState();
        } else {
            s_localKeyRepeatActiveId.reset();
        }
    }
    UpdateLocalKeyRepeatTimer(hWnd);
}

static void TrackInitialLocalCharacterMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!IsCharacterRepeatMessage(uMsg)) return;

    LocalKeyRepeatState* state = FindLocalKeyRepeatStateForCharMessage(lParam);
    if (!state) return;
    if (state->hasCharacterMessage) return;

    state->hasCharacterMessage = true;
    state->charMsg = uMsg;
    state->charWParam = wParam;
    state->charScanCodeWithFlags = GetScanCodeWithExtendedFlagFromLParam(lParam);
    if (s_localKeyRepeatActiveId.has_value() && *s_localKeyRepeatActiveId == MakeLocalKeyRepeatId(state->rawVk, state->scanCodeWithFlags)) {
        state->nextRepeatTime = ResolveNextRepeatTimeFromPress(state->initialPressTime);
    }
    UpdateLocalKeyRepeatTimer(hWnd);
}

static bool ShouldSuppressOsAutoRepeatKeyMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_KEYDOWN && uMsg != WM_SYSKEYDOWN) return false;

    const DWORD rawVk = static_cast<DWORD>(wParam);
    const UINT scanCodeWithFlags = GetScanCodeWithExtendedFlagFromLParam(lParam);
    const LocalKeyRepeatState* state = FindLocalKeyRepeatState(rawVk, scanCodeWithFlags);
    if (!state) return false;

    const bool hasRepeatBit = (lParam & (1LL << 30)) != 0;
    return hasRepeatBit || state->hasCharacterMessage;
}

static bool ShouldSuppressOsAutoRepeatCharMessage(UINT uMsg, LPARAM lParam) {
    if (!IsCharacterRepeatMessage(uMsg)) return false;

    const LocalKeyRepeatState* state = FindLocalKeyRepeatStateForCharMessage(lParam);
    if (!state || !state->hasCharacterMessage) return false;

    const bool hasRepeatBit = (lParam & (1LL << 30)) != 0;
    return hasRepeatBit || state->hasCharacterMessage;
}

static LRESULT ForwardLocalRepeatKeyMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    InputHandlerResult result = HandleImGuiInput(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayKeyboard(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiInputBlocking(uMsg);
    if (result.consumed) return result.result;

    result = HandleKeyRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (g_originalWndProc) { return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam); }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static LRESULT ForwardLocalRepeatCharMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    HandleCharLogging(uMsg, wParam, lParam);

    InputHandlerResult result = HandleImGuiInput(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiInputBlocking(uMsg);
    if (result.consumed) return result.result;

    if (uMsg == WM_CHAR) {
        result = HandleCharRebinding(hWnd, uMsg, wParam, lParam);
        if (result.consumed) return result.result;
    }

    if (g_originalWndProc) { return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam); }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static InputHandlerResult HandleLocalKeyRepeatMessages(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (ShouldSuppressOsAutoRepeatKeyMessage(uMsg, wParam, lParam)) {
            return { true, 0 };
        }
        TrackInitialLocalKeyRepeatKeyDown(hWnd, uMsg, wParam, lParam);
        return { false, 0 };

    case WM_KEYUP:
    case WM_SYSKEYUP:
        ReleaseLocalKeyRepeatKey(hWnd, wParam, lParam);
        return { false, 0 };

    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
        if (ShouldSuppressOsAutoRepeatCharMessage(uMsg, lParam)) {
            return { true, 0 };
        }
        TrackInitialLocalCharacterMessage(hWnd, uMsg, wParam, lParam);
        return { false, 0 };

    case WM_TIMER:
        if (wParam != kToolscreenKeyRepeatTimerId) return { false, 0 };
        break;

    case WM_TOOLSCREEN_REFRESH_KEY_REPEAT:
        RefreshLocalKeyRepeatSchedule(hWnd);
        return { true, 0 };

    default:
        return { false, 0 };
    }

    using Clock = std::chrono::steady_clock;

    struct PendingRepeatMessage {
        UINT keyMsg = 0;
        WPARAM keyWParam = 0;
        LPARAM keyLParam = 0;
        UINT charMsg = 0;
        WPARAM charWParam = 0;
        LPARAM charLParam = 0;
    };

    std::vector<PendingRepeatMessage> pendingRepeats;

    const Clock::time_point now = Clock::now();
    LocalKeyRepeatState* activeState = FindActiveLocalKeyRepeatState();
    if (activeState && activeState->hasCharacterMessage && activeState->nextRepeatTime <= now) {
        const bool isSystemKey = (activeState->keyMsg == WM_SYSKEYDOWN);
        PendingRepeatMessage pending{};
        pending.keyMsg = activeState->keyMsg;
        pending.keyWParam = activeState->keyWParam;
        pending.keyLParam = BuildKeyboardMessageLParam(activeState->scanCodeWithFlags, true, isSystemKey, 1, true, false);

        const bool isSystemChar = (activeState->charMsg == WM_SYSCHAR || activeState->charMsg == WM_SYSDEADCHAR);
        pending.charMsg = activeState->charMsg;
        pending.charWParam = activeState->charWParam;
        pending.charLParam = BuildKeyboardMessageLParam(activeState->charScanCodeWithFlags, true, isSystemChar, 1, true, false);
        pendingRepeats.push_back(pending);

        const UINT repeatIntervalMs = ResolveConfiguredKeyRepeatIntervalMs();
        if (repeatIntervalMs == 0) {
            activeState->nextRepeatTime = Clock::now();
        } else {
            activeState->nextRepeatTime += std::chrono::milliseconds(repeatIntervalMs);
            while (activeState->nextRepeatTime <= now) {
                activeState->nextRepeatTime += std::chrono::milliseconds(repeatIntervalMs);
            }
        }
    }

    UpdateLocalKeyRepeatTimer(hWnd);

    for (const PendingRepeatMessage& pending : pendingRepeats) {
        (void)ForwardLocalRepeatKeyMessage(hWnd, pending.keyMsg, pending.keyWParam, pending.keyLParam);
        (void)ForwardLocalRepeatCharMessage(hWnd, pending.charMsg, pending.charWParam, pending.charLParam);
    }

    return { true, 0 };
}

static bool QuerySystemCursorVisibleCached() {
    constexpr ULONGLONG kCursorVisibilityRefreshMs = 50;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastTick = s_cachedSystemCursorVisibilityTick.load(std::memory_order_relaxed);
    const int cachedVisible = s_cachedSystemCursorVisible.load(std::memory_order_relaxed);
    if (cachedVisible != -1 && (now - lastTick) < kCursorVisibilityRefreshMs) {
        return cachedVisible != 0;
    }

    CURSORINFO ci{ sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) {
        return cachedVisible > 0;
    }

    const bool isVisible = (ci.flags & CURSOR_SHOWING) != 0;
    s_cachedSystemCursorVisible.store(isVisible ? 1 : 0, std::memory_order_relaxed);
    s_cachedSystemCursorVisibilityTick.store(now, std::memory_order_relaxed);
    return isVisible;
}

static void EnsureSystemCursorVisible() {
    if (g_gameVersion < GameVersion(1, 13, 0)) { return; }

    if (QuerySystemCursorVisibleCached()) { return; }
    ShowCursor(TRUE);
    s_cachedSystemCursorVisible.store(1, std::memory_order_relaxed);
    s_cachedSystemCursorVisibilityTick.store(GetTickCount64(), std::memory_order_relaxed);
}

static void EnsureSystemCursorHidden() {
    if (g_gameVersion < GameVersion(1, 13, 0)) { return; }

    if (!QuerySystemCursorVisibleCached()) { return; }
    ShowCursor(FALSE);
    s_cachedSystemCursorVisible.store(0, std::memory_order_relaxed);
    s_cachedSystemCursorVisibilityTick.store(GetTickCount64(), std::memory_order_relaxed);
}

static DWORD NormalizeModifierVkFromKeyMessage(DWORD rawVk, LPARAM lParam) {
    DWORD vk = rawVk;

    const UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
    const bool isExtended = (lParam & (1LL << 24)) != 0;

    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        if (scanCode != 0) {
            DWORD mapped = static_cast<DWORD>(::MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX));
            if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) {
                vk = mapped;
            }
        }
        return vk;
    }

    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL) {
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
        return isExtended ? VK_RMENU : VK_LMENU;
    }

    return vk;
}

static DWORD NormalizeModifierVkFromConfig(DWORD vk, UINT scanCodeWithFlags = 0) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    switch (vk) {
    case VK_SHIFT:
        if (scanLow == 0x36) return VK_RSHIFT;
        return VK_LSHIFT;
    case VK_CONTROL:
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return isExtended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

static bool MatchesRebindSourceKey(DWORD incomingVk, DWORD incomingRawVk, DWORD fromKey) {
    if (fromKey == 0) return false;
    if (incomingVk == fromKey) return true;

    if (fromKey == VK_CONTROL) {
        return incomingVk == VK_LCONTROL || incomingVk == VK_RCONTROL || incomingRawVk == VK_CONTROL;
    }
    if (fromKey == VK_SHIFT) {
        return incomingVk == VK_LSHIFT || incomingVk == VK_RSHIFT || incomingRawVk == VK_SHIFT;
    }
    if (fromKey == VK_MENU) {
        return incomingVk == VK_LMENU || incomingVk == VK_RMENU || incomingRawVk == VK_MENU;
    }

    if (incomingRawVk == VK_CONTROL && incomingVk == VK_CONTROL && (fromKey == VK_LCONTROL || fromKey == VK_RCONTROL)) return true;
    if (incomingRawVk == VK_SHIFT && incomingVk == VK_SHIFT && (fromKey == VK_LSHIFT || fromKey == VK_RSHIFT)) return true;
    if (incomingRawVk == VK_MENU && incomingVk == VK_MENU && (fromKey == VK_LMENU || fromKey == VK_RMENU)) return true;

    return false;
}

static bool IsModifierVk(DWORD vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

static bool IsAltVk(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

static bool IsShiftVk(DWORD vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT;
}

static bool IsShiftCurrentlyDown() {
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
}

static bool IsShiftDownForIncomingEvent(DWORD incomingVk, DWORD incomingRawVk, bool isKeyDown) {
    if (IsShiftVk(incomingVk) || incomingRawVk == VK_SHIFT) {
        return isKeyDown;
    }
    return IsShiftCurrentlyDown();
}

static bool HasShiftLayerOutputVk(const KeyRebind& rebind) {
    return rebind.shiftLayerEnabled && rebind.shiftLayerOutputVK != 0;
}

static bool HasShiftLayerOutputUnicode(const KeyRebind& rebind) {
    return rebind.shiftLayerEnabled && rebind.shiftLayerOutputUnicode != 0;
}

static bool HasShiftLayerOutputOverride(const KeyRebind& rebind) {
    return HasShiftLayerOutputVk(rebind) || HasShiftLayerOutputUnicode(rebind);
}

static bool IsShiftLayerActiveForRebind(const KeyRebind& rebind, DWORD incomingVk, DWORD incomingRawVk, bool isKeyDown) {
    return HasShiftLayerOutputOverride(rebind) && IsShiftDownForIncomingEvent(incomingVk, incomingRawVk, isKeyDown);
}

static DWORD ResolveEffectiveCustomOutputVk(const KeyRebind& rebind, bool shiftLayerActive) {
    if (shiftLayerActive && HasShiftLayerOutputVk(rebind)) {
        return rebind.shiftLayerOutputVK;
    }
    if (rebind.useCustomOutput && rebind.customOutputVK != 0) {
        return rebind.customOutputVK;
    }
    return 0;
}

static bool IsModifierScanCode(UINT scanCodeWithFlags) {
    const UINT scanLow = (scanCodeWithFlags & 0xFF);
    if (scanLow == 0) return false;

    static thread_local std::unordered_map<UINT, DWORD> s_scanCodeToModifierVk;

    DWORD mappedVk = 0;
    auto it = s_scanCodeToModifierVk.find(scanCodeWithFlags);
    if (it != s_scanCodeToModifierVk.end()) {
        mappedVk = it->second;
    } else {
        mappedVk = static_cast<DWORD>(::MapVirtualKeyW(scanCodeWithFlags, MAPVK_VSC_TO_VK_EX));
        if (mappedVk == 0 && (scanCodeWithFlags & 0xFF00) != 0) {
            mappedVk = static_cast<DWORD>(::MapVirtualKeyW(scanLow, MAPVK_VSC_TO_VK_EX));
        }
        if (mappedVk != 0) {
            mappedVk = NormalizeModifierVkFromConfig(mappedVk, scanCodeWithFlags);
        }
        s_scanCodeToModifierVk.emplace(scanCodeWithFlags, mappedVk);
    }
    if (mappedVk == 0) return false;

    return IsModifierVk(mappedVk);
}

static bool TryGetClientSize(HWND hWnd, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hWnd) { return false; }

    RECT clientRect{};
    if (!GetClientRect(hWnd, &clientRect)) { return false; }

    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (clientW <= 0 || clientH <= 0) { return false; }

    outW = clientW;
    outH = clientH;
    return true;
}

static void ResendCurrentModeWmSize(HWND hWnd, const char* source) {
    if (!hWnd || !IsWindow(hWnd)) { return; }

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return; }

    const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const ModeConfig* mode = GetModeFromSnapshot(*cfgSnap, currentModeId);
    if (!mode || mode->width <= 0 || mode->height <= 0) { return; }

    RequestWindowClientResize(hWnd, mode->width, mode->height, source);
}

static bool IsFocusGainMessage(UINT uMsg) {
    return uMsg == WM_ACTIVATE || uMsg == WM_ACTIVATEAPP || uMsg == WM_SETFOCUS;
}

static void QueueDeferredFocusRegainWmSize(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) { return; }

    if (!PostMessage(hWnd, WM_TOOLSCREEN_APPLY_FOCUS_REGAIN_SIZE, 0, 0)) {
        s_deferredFocusRegainWmSizePending.store(false, std::memory_order_relaxed);
        Log("[WINDOW] Failed to post deferred focus-regain WM_SIZE. Error=" + std::to_string(GetLastError()));
    }
}

static void SyncWindowMetricsFromMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    constexpr int kInactiveTransientClientMin = 32;
    constexpr int kFullscreenTolPx = 1;
    bool shouldInvalidateScreenMetrics = false;
    bool shouldRequestRecalc = false;
    bool shouldInvalidateImGui = false;
    bool shouldRecenterGui = false;
    bool shouldResetGameTexture = false;
    bool sizeMayHaveChanged = false;

    switch (uMsg) {
    case WM_MOVE:
    case WM_MOVING:
        shouldInvalidateScreenMetrics = true;
        shouldRequestRecalc = true;
        break;

    case WM_SIZE:
        shouldInvalidateScreenMetrics = true;
        shouldInvalidateImGui = true;
        shouldRequestRecalc = true;
        sizeMayHaveChanged = (wParam != SIZE_MINIMIZED);
        break;

    case WM_SIZING:
        // During live-resize, client metrics are often transient.
        // Mark dirty only; WM_SIZE/WM_WINDOWPOSCHANGED will commit stable size.
        shouldInvalidateScreenMetrics = true;
        // No-op: resize tracking removed
        break;

    case WM_WINDOWPOSCHANGED: {
        if (lParam == 0) { break; }

        const WINDOWPOS* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
        const bool sizeChanged = (pos->flags & SWP_NOSIZE) == 0;
        const bool moveChanged = (pos->flags & SWP_NOMOVE) == 0;
        if (!sizeChanged && !moveChanged) { break; }

        if (sizeChanged) {
            shouldInvalidateScreenMetrics = true;
            shouldInvalidateImGui = true;
            shouldRequestRecalc = true;
            shouldResetGameTexture = true;
            sizeMayHaveChanged = true;
        } else {
            shouldInvalidateScreenMetrics = true;
            shouldRequestRecalc = true;
        }
        break;
    }

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        shouldInvalidateScreenMetrics = true;
        shouldInvalidateImGui = true;
        shouldRequestRecalc = true;
        sizeMayHaveChanged = true;
        break;

    default:
        return;
    }

    bool clientSizeChanged = false;
    if (sizeMayHaveChanged) {
        const int prevW = GetCachedWindowWidth();
        const int prevH = GetCachedWindowHeight();

        int clientW = 0;
        int clientH = 0;
        if (TryGetClientSize(hWnd, clientW, clientH)) {
            const bool windowActive = g_gameWindowActive.load(std::memory_order_relaxed);
            const bool isInactiveTinySize = !windowActive && (clientW < kInactiveTransientClientMin || clientH < kInactiveTransientClientMin);
            if (!isInactiveTinySize) {
                RECT monitorRect{};
                const bool haveMonitorRect = GetMonitorRectForWindow(hWnd, monitorRect);
                const int monitorW = haveMonitorRect ? (monitorRect.right - monitorRect.left) : 0;
                const int monitorH = haveMonitorRect ? (monitorRect.bottom - monitorRect.top) : 0;
                const bool previousSizeFilledMonitor = haveMonitorRect && prevW >= (monitorW - kFullscreenTolPx) && prevH >= (monitorH - kFullscreenTolPx);
                const bool currentSizeIsWindowed = haveMonitorRect && (clientW < (monitorW - kFullscreenTolPx) || clientH < (monitorH - kFullscreenTolPx));
                if (previousSizeFilledMonitor && currentSizeIsWindowed) {
                    if (CenterWindowedRestoreOnCurrentMonitor(hWnd, "input_hook:fullscreen_exit_restore")) { return; }
                }

                clientSizeChanged = (clientW != prevW) || (clientH != prevH);
                UpdateCachedWindowMetricsFromSize(clientW, clientH);
            }
        }

        if (clientSizeChanged) {
            // Record pending resize for debounced virtual camera update
            int vcClientW = 0, vcClientH = 0;
            if (TryGetClientSize(hWnd, vcClientW, vcClientH)) {
                OnGameWindowResized(static_cast<uint32_t>(vcClientW), static_cast<uint32_t>(vcClientH));
            }
        } else {
            // No-op: resize tracking removed
        }
    }

    if (shouldInvalidateScreenMetrics) { InvalidateCachedScreenMetrics(); }
    if (shouldRequestRecalc) { RequestScreenMetricsRecalculation(); }
    if (shouldInvalidateImGui) { InvalidateImGuiCache(); }
    if (shouldResetGameTexture && clientSizeChanged) { InvalidateTrackedGameTextureId(false); }
    if (clientSizeChanged) { shouldRecenterGui = true; }

    if (clientSizeChanged) {
        ResendCurrentModeWmSize(hWnd, "input_hook:external_resize");
    }
    if (shouldRecenterGui) { g_guiNeedsRecenter = true; }
}

InputHandlerResult HandleMouseMoveViewportOffset(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    if (uMsg != WM_MOUSEMOVE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleMouseMoveViewportOffset");
    // Legacy compatibility path intentionally disabled.
    // Mouse translation is handled centrally in HandleMouseCoordinateTranslationPhase.
    (void)hWnd;
    (void)uMsg;
    (void)wParam;
    (void)lParam;
    return { false, 0 };
}

InputHandlerResult HandleShutdownCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleShutdownCheck");

    if (g_isShuttingDown.load() && g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

InputHandlerResult HandleWindowValidation(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleWindowValidation");

    if (g_subclassedHwnd.load() != hWnd) {
        Log("WARNING: SubclassedWndProc called for unexpected window " + std::to_string(reinterpret_cast<uintptr_t>(hWnd)) + " (expected " +
            std::to_string(reinterpret_cast<uintptr_t>(g_subclassedHwnd.load())) + ")");
        if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
        return { true, DefWindowProc(hWnd, uMsg, wParam, lParam) };
    }
    return { false, 0 };
}

static UINT GetToolscreenIsInstalledMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_IsInstalled");
    return s_msg;
}

static UINT GetToolscreenGetVersionMessageId() {
    static const UINT s_msg = RegisterWindowMessageA("Toolscreen_GetVersion");
    return s_msg;
}

static LRESULT EncodeToolscreenVersionNumber() {
    // 0x00MMmmpp (major/minor/patch in 8-bit fields)
    return static_cast<LRESULT>(((TOOLSCREEN_VERSION_MAJOR & 0xFF) << 16) |
                                ((TOOLSCREEN_VERSION_MINOR & 0xFF) << 8) |
                                (TOOLSCREEN_VERSION_PATCH & 0xFF));
}

InputHandlerResult HandleToolscreenQueryMessages(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const UINT isInstalledMsg = GetToolscreenIsInstalledMessageId();
    const UINT getVersionMsg = GetToolscreenGetVersionMessageId();
    const UINT borderlessToggleMsg = GetToolscreenBorderlessToggleMessageId();
    if (uMsg != isInstalledMsg && uMsg != getVersionMsg && uMsg != borderlessToggleMsg) { return { false, 0 }; }
    PROFILE_SCOPE("HandleToolscreenQueryMessages");
    (void)hWnd;
    (void)wParam;
    (void)lParam;

    if (uMsg == borderlessToggleMsg) {
        ToggleBorderlessWindowedFullscreen(hWnd);
        return { true, 1 };
    }

    if (uMsg == isInstalledMsg) {
        return { true, 1 };
    }

    if (uMsg == getVersionMsg) {
        return { true, EncodeToolscreenVersionNumber() };
    }

    return { false, 0 };
}

InputHandlerResult HandleNonFullscreenCheck(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleNonFullscreenCheck");

    // Windowed mode is supported; do not bypass the hook pipeline.
    return { false, 0 };
}

void HandleCharLogging(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_CHAR) { return; }

    auto cfgSnap = GetConfigSnapshot();
    if (cfgSnap && cfgSnap->debug.showHotkeyDebug) {
        Log("WM_CHAR: " + std::to_string(wParam) + " " + std::to_string(lParam));
    }
}

InputHandlerResult HandleAltF4(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_SYSKEYDOWN) { return { false, 0 }; }
    PROFILE_SCOPE("HandleAltF4");

    if (wParam == VK_F4) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    return { false, 0 };
}

InputHandlerResult HandleConfigLoadFailure(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleConfigLoadFailure");

    if (!g_configLoadFailed.load()) { return { false, 0 }; }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) { return { true, true }; }

    switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_INPUT:
        return { true, 1 };
    default:
        break;
    }
    return { false, 0 };
}

InputHandlerResult HandleSetCursor(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& gameState) {
    if (uMsg != WM_SETCURSOR) { return { false, 0 }; }
    PROFILE_SCOPE("HandleSetCursor");

    if (g_showGui.load() && s_forcedShowCursor && g_gameVersion >= GameVersion(1, 13, 0)) {
        EnsureSystemCursorVisible();
        static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
        SetCursor(s_arrowCursor);
        return { true, true };
    }

    if (!IsCursorVisible() && !g_showGui.load()) {
        SetCursor(NULL);
        return { true, true };
    }

    const CursorTextures::CursorData* cursorData = CursorTextures::GetSelectedCursor(gameState, 64);
    if (cursorData && cursorData->hCursor) {
        SetCursor(cursorData->hCursor);
        return { true, true };
    }
    return { false, 0 };
}

InputHandlerResult HandleDestroy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_DESTROY) { return { false, 0 }; }
    PROFILE_SCOPE("HandleDestroy");

    ClearLocalKeyRepeatStates(hWnd);
    ReleaseActiveLowLevelRebindKeys(hWnd);

    extern GameVersion g_gameVersion;
    if (g_gameVersion >= GameVersion(1, 13, 0)) { g_isShuttingDown = true; }
    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
}

InputHandlerResult HandleImGuiInput(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("HandleImGuiInput");

    if (!g_showGui.load()) { return { false, 0 }; }

    // Never call ImGui from this thread.
    // Instead, enqueue the message for the render thread (which owns the ImGui context).
    ImGuiInputQueue_EnqueueWin32Message(hWnd, uMsg, wParam, lParam);
    return { false, 0 };
}

InputHandlerResult HandleGuiToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleGuiToggle");

    DWORD vkCode = 0;
    bool isEscape = false;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        isEscape = (wParam == VK_ESCAPE);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!isEscape && !CheckHotkeyMatch(g_config.guiHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    if (g_showGui.load(std::memory_order_acquire) && !isEscape) {
        switch (uMsg) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            return { false, 0 };
        default:
            break;
        }
    }

    bool allow_toggle = true;
    if (isEscape && !g_showGui.load()) { allow_toggle = false; }

    if (!allow_toggle) { return { false, 0 }; }

    // Lock-free debouncing using atomic timestamp
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = g_lastGuiToggleTimeMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 200) {
        return { true, 1 };
    }
    g_lastGuiToggleTimeMs.store(nowMs, std::memory_order_relaxed);

    if (!g_glInitialized.load(std::memory_order_acquire)) {
        Log("GUI toggle ignored - OpenGL not initialized yet");
        return { true, 1 };
    }

    bool is_closing = g_showGui.load();

    if (isEscape && g_imguiAnyItemActive.load(std::memory_order_acquire)) { is_closing = false; }
    if (isEscape && IsHotkeyBindingActive()) { is_closing = false; }
    if (isEscape && IsRebindBindingActive()) { is_closing = false; }

    if (is_closing) {
        g_showGui = false;
        InvalidateImGuiCache();
        if (s_forcedShowCursor) {
            EnsureSystemCursorHidden();
            s_forcedShowCursor = false;
        }

        // Flush any queued ImGui input and release any mouse capture we may have taken.
        ImGuiInputQueue_Clear();
        ImGuiInputQueue_ResetMouseCapture(hWnd);

        if (!g_wasCursorVisible.load()) {
            RECT clipRect{};
            if (GetWindowClientRectInScreen(hWnd, clipRect)) {
                ClipCursor(&clipRect);
            } else {
                ClipCursor(NULL);
            }
            SetCursor(NULL);

            if (g_gameVersion < GameVersion(1, 13, 0)) {
                HCURSOR airCursor = g_specialCursorHandle.load();
                if (airCursor) SetCursor(airCursor);
            }
        }
        g_currentlyEditingMirror = "";

        g_imageDragMode.store(false);
        g_windowOverlayDragMode.store(false);

        extern std::string s_hoveredImageName;
        extern std::string s_draggedImageName;
        extern bool s_isDragging;
        s_hoveredImageName = "";
        s_draggedImageName = "";
        s_isDragging = false;

        extern std::string s_hoveredWindowOverlayName;
        extern std::string s_draggedWindowOverlayName;
        extern bool s_isWindowOverlayDragging;
        s_hoveredWindowOverlayName = "";
        s_draggedWindowOverlayName = "";
        s_isWindowOverlayDragging = false;
    } else if (!isEscape) {
        g_showGui = true;
        InvalidateImGuiCache();
        const bool wasCursorVisible = IsCursorVisible();
        g_wasCursorVisible = wasCursorVisible;
        g_guiNeedsRecenter = true;
        ClipCursor(NULL);
        if (!wasCursorVisible && g_gameVersion >= GameVersion(1, 13, 0)) {
            s_forcedShowCursor = true;
            EnsureSystemCursorVisible();
            static HCURSOR s_arrowCursor = LoadCursorW(NULL, IDC_ARROW);
            SetCursor(s_arrowCursor);
        }

        g_configurePromptDismissedThisSession.store(true, std::memory_order_release);

        if (!g_toolscreenPath.empty()) {
            std::wstring flagPath = g_toolscreenPath + L"\\has_opened";
            HANDLE hFile = CreateFileW(flagPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); }
        }
    }
    return { true, 1 };
}

InputHandlerResult HandleBorderlessToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleBorderlessToggle");

    if (g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    if (g_config.borderlessHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.borderlessHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    ToggleBorderlessWindowedFullscreen(hWnd);
    return { true, 1 };
}

InputHandlerResult HandleImageOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleImageOverlaysToggle");

    if (g_config.imageOverlaysHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.imageOverlaysHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_imageOverlaysVisible.load(std::memory_order_acquire);
    g_imageOverlaysVisible.store(newVisible, std::memory_order_release);

    return { true, 1 };
}

InputHandlerResult HandleWindowOverlaysToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleWindowOverlaysToggle");

    if (g_config.windowOverlaysHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.windowOverlaysHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    bool newVisible = !g_windowOverlaysVisible.load(std::memory_order_acquire);
    g_windowOverlaysVisible.store(newVisible, std::memory_order_release);

    if (!newVisible) {
        UnfocusWindowOverlay();
    }

    return { true, 1 };
}

InputHandlerResult HandleKeyRebindsToggle(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleKeyRebindsToggle");

    if (g_config.keyRebinds.toggleHotkey.empty()) { return { false, 0 }; }

    if (IsHotkeyBindingActive() || IsRebindBindingActive()) { return { false, 0 }; }

    DWORD vkCode = 0;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        vkCode = static_cast<DWORD>(wParam);
        vkCode = NormalizeModifierVkFromKeyMessage(vkCode, lParam);
        break;
    }
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    }
    default:
        return { false, 0 };
    }

    if (!CheckHotkeyMatch(g_config.keyRebinds.toggleHotkey, vkCode, {}, false, s_bestMatchKeyCount)) { return { false, 0 }; }

    static std::atomic<int64_t> s_lastToggleMs{ 0 };
    auto now = std::chrono::steady_clock::now();
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    int64_t lastMs = s_lastToggleMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs < 250) { return { true, 1 }; }
    s_lastToggleMs.store(nowMs, std::memory_order_relaxed);

    if (g_config.keyRebinds.enabled) {
        ReleaseActiveLowLevelRebindKeys(hWnd);
    }

    g_config.keyRebinds.enabled = !g_config.keyRebinds.enabled;
    g_configIsDirty = true;

    {
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        RebuildHotkeyMainKeys_Internal();
    }

    PublishConfigSnapshot();

    (void)hWnd;
    return { true, 1 };
}

InputHandlerResult HandleWindowOverlayKeyboard(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_KEYDOWN && uMsg != WM_KEYUP && uMsg != WM_SYSKEYDOWN && uMsg != WM_SYSKEYUP) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowOverlayKeyboard");

    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return { false, 0 }; }

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (!isOverlayInteractionActive) { return { false, 0 }; }

    // Never query ImGui from this thread. Use state published by render thread.
    bool imguiWantsKeyboard = g_showGui.load() && g_imguiWantCaptureKeyboard.load(std::memory_order_acquire);

    if (!imguiWantsKeyboard) {
        if (ForwardKeyboardToWindowOverlay(uMsg, wParam, lParam)) { return { true, 1 }; }
    }
    return { false, 0 };
}

InputHandlerResult HandleWindowOverlayMouse(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg < WM_MOUSEFIRST || uMsg > WM_MOUSELAST) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWindowOverlayMouse");

    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return { false, 0 }; }

    int mouseX, mouseY;

    // WM_MOUSEWHEEL and WM_MOUSEHWHEEL use SCREEN coordinates in lParam, not client coordinates
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        if (ScreenToClient(hWnd, &cursorPos)) {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        } else {
            mouseX = cursorPos.x;
            mouseY = cursorPos.y;
        }
    } else {
        mouseX = GET_X_LPARAM(lParam);
        mouseY = GET_Y_LPARAM(lParam);
    }

    const int screenW = GetCachedWindowWidth();
    const int screenH = GetCachedWindowHeight();

    bool isOverlayInteractionActive = IsWindowOverlayFocused();

    if (isOverlayInteractionActive) {
        if (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN) {
            std::string focusedName = GetFocusedWindowOverlayName();
            std::string overlayAtPoint = GetWindowOverlayAtPoint(mouseX, mouseY, screenW, screenH);

            if (overlayAtPoint.empty() || overlayAtPoint != focusedName) {
                UnfocusWindowOverlay();
                if (!overlayAtPoint.empty()) {
                    FocusWindowOverlay(overlayAtPoint);
                    ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
                    return { true, 1 };
                }
            } else {
                ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
                return { true, 1 };
            }
        } else {
            ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
            return { true, 1 };
        }
    } else if ((uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN || uMsg == WM_MBUTTONDOWN)) {
        const bool cursorVisible = IsCursorVisible();
        if (!(g_showGui.load() || cursorVisible)) {
            return { false, 0 };
        }
        std::string overlayAtPoint = GetWindowOverlayAtPoint(mouseX, mouseY, screenW, screenH);
        if (!overlayAtPoint.empty()) {
            FocusWindowOverlay(overlayAtPoint);
            ForwardMouseToWindowOverlay(uMsg, mouseX, mouseY, wParam, screenW, screenH);
            return { true, 1 };
        }
    }
    return { false, 0 };
}

InputHandlerResult HandleGuiInputBlocking(UINT uMsg) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_CHAR:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_INPUT:
        break;
    default:
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleGuiInputBlocking");

    if (!g_showGui.load()) { return { false, 0 }; }

    return { true, 1 };
}

void RestoreWindowsMouseSpeed();
void ApplyWindowsMouseSpeed();
void RestoreKeyRepeatSettings();
void ApplyKeyRepeatSettings();

InputHandlerResult HandleActivate(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    bool becameInactive = false;
    bool becameActive = false;
    const char* focusSource = nullptr;

    switch (uMsg) {
    case WM_ACTIVATE:
        becameInactive = (LOWORD(wParam) == WA_INACTIVE);
        becameActive = !becameInactive;
        focusSource = becameActive ? "input_hook:focus_regain_activate" : "input_hook:focus_loss_activate";
        break;

    case WM_ACTIVATEAPP:
        becameActive = (wParam != FALSE);
        becameInactive = !becameActive;
        focusSource = becameActive ? "input_hook:focus_regain_activateapp" : "input_hook:focus_loss_activateapp";
        break;

    case WM_SETFOCUS:
        becameActive = true;
        focusSource = "input_hook:focus_regain_setfocus";
        break;

    case WM_KILLFOCUS:
        becameInactive = true;
        focusSource = "input_hook:focus_loss_killfocus";
        break;

    default:
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleActivate");
    (void)lParam;

    if (becameInactive) {
        ImGuiInputQueue_EnqueueFocus(false);

        ClearLocalKeyRepeatStates(hWnd);
        ReleaseActiveLowLevelRebindKeys(hWnd);

        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) {
            Log(std::string("[WINDOW] Window became inactive via ") + focusSource + ".");
        }
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(false);
        g_gameWindowActive.store(false);

        RestoreWindowsMouseSpeed();
        RestoreKeyRepeatSettings();
    } else {
        ImGuiInputQueue_EnqueueFocus(true);

        if (auto cs = GetConfigSnapshot(); cs && cs->debug.showHotkeyDebug) {
            Log(std::string("[WINDOW] Window became active via ") + focusSource + ".");
        }
        extern std::atomic<bool> g_isGameFocused;
        g_isGameFocused.store(true);
        g_gameWindowActive.store(true);

        ApplyWindowsMouseSpeed();
        ApplyKeyRepeatSettings();

        int clientW = 0;
        int clientH = 0;
        if (TryGetClientSize(hWnd, clientW, clientH)) {
            UpdateCachedWindowMetricsFromSize(clientW, clientH);
        }

        RequestScreenMetricsRecalculation();
        InvalidateImGuiCache();
        g_guiNeedsRecenter = true;
        s_deferredFocusRegainWmSizePending.store(true, std::memory_order_relaxed);
    }
    return { false, 0 };
}

InputHandlerResult HandleWmSizeModeDimensions(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId) {
    if (uMsg != WM_SIZE) { return { false, 0 }; }
    PROFILE_SCOPE("HandleWmSizeModeDimensions");

    if (wParam == SIZE_MINIMIZED) { return { false, 0 }; }

    const int msgW = LOWORD(lParam);
    const int msgH = HIWORD(lParam);
    if (msgW <= 0 || msgH <= 0) { return { false, 0 }; }

    auto cfgSnap = GetConfigSnapshot();
    const ModeConfig* mode = cfgSnap ? GetModeFromSnapshot(*cfgSnap, currentModeId) : nullptr;
    if (!mode || mode->width <= 0 || mode->height <= 0) { return { false, 0 }; }

    if (EqualsIgnoreCase(mode->id, "Fullscreen")) {
        // Fullscreen custom sizing is an internal render size with a live stretch
        // rect over the current client area. Let WM_SIZE keep the real client size.
        return { false, 0 };
    }

    // IMPORTANT: use already-recalculated mode dimensions as authoritative target.
    // Re-applying relative/expression math against WM_SIZE repeatedly causes compounding
    // shrink (e.g. 98.4% of 900 -> 885, then 98.4% of 885 -> 870).
    int targetW = mode->width;
    int targetH = mode->height;

    if (targetW <= 0 || targetH <= 0 || (msgW == targetW && msgH == targetH)) { return { false, 0 }; }

    const LPARAM adjustedSize = MAKELPARAM(targetW, targetH);
    InvalidateTrackedGameTextureId(false);
    LRESULT forwarded = CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, adjustedSize);

    return { true, forwarded };
}

InputHandlerResult HandleHotkeys(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, const std::string& currentModeId,
                                 const std::string& gameState) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleHotkeys");

    DWORD rawVkCode = 0;
    DWORD vkCode = 0;
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_XBUTTONDOWN) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONUP) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDown = false;
    } else if (uMsg == WM_LBUTTONDOWN) {
        rawVkCode = VK_LBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_LBUTTONUP) {
        rawVkCode = VK_LBUTTON;
        isKeyDown = false;
    } else if (uMsg == WM_RBUTTONDOWN) {
        rawVkCode = VK_RBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_RBUTTONUP) {
        rawVkCode = VK_RBUTTON;
        isKeyDown = false;
    } else if (uMsg == WM_MBUTTONDOWN) {
        rawVkCode = VK_MBUTTON;
        isKeyDown = true;
    } else if (uMsg == WM_MBUTTONUP) {
        rawVkCode = VK_MBUTTON;
        isKeyDown = false;
    } else {
        return { false, 0 };
    }

    const bool isAutoRepeatKeyDown =
        (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && isKeyDown && ((lParam & (static_cast<LPARAM>(1) << 30)) != 0);

    // This mirrors imgui_impl_win32 behavior and enables reliable hotkeys + key rebinding.
    vkCode = rawVkCode;
    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        vkCode = NormalizeModifierVkFromKeyMessage(rawVkCode, lParam);
        if (vkCode == 0) vkCode = rawVkCode;
    }

    // Even if resolution-change features are unsupported, we must not short-circuit the input pipeline.
    if (!IsResolutionChangeSupported(g_gameVersion)) { return { false, 0 }; }

    bool isHotkeyMainKey = false;
    {
        std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
        isHotkeyMainKey = (g_hotkeyMainKeys.find(rawVkCode) != g_hotkeyMainKeys.end()) ||
                          (g_hotkeyMainKeys.find(vkCode) != g_hotkeyMainKeys.end());
    }

    if (!isHotkeyMainKey) {
        // This key is not a hotkey main key, but it might invalidate pending trigger-on-release hotkeys
        if (isKeyDown) {
            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
            // Any key press (that's not a hotkey) invalidates ALL pending trigger-on-release hotkeys
            for (const auto& pendingHotkeyId : g_triggerOnReleasePending) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
        }
        // Do not return "consumed" here.
        return { false, 0 };
    }

    // Use config snapshot for thread-safe hotkey iteration
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) }; }
    const Config& cfg = *cfgSnap;

    DWORD rebindTargetVk = 0;
    if (cfg.keyRebinds.enabled && cfg.keyRebinds.resolveRebindTargetsForHotkeys) {
        for (const auto& rebind : cfg.keyRebinds.rebinds) {
            if (rebind.enabled && rebind.fromKey != 0 && rebind.toKey != 0 &&
                (vkCode == rebind.fromKey || rawVkCode == rebind.fromKey)) {
                const bool shiftLayerActive = IsShiftLayerActiveForRebind(rebind, vkCode, rawVkCode, isKeyDown);
                const DWORD effectiveCustomOutputVk = ResolveEffectiveCustomOutputVk(rebind, shiftLayerActive);
                rebindTargetVk = (effectiveCustomOutputVk != 0) ? effectiveCustomOutputVk : rebind.toKey;
                break;
            }
        }
    }

    bool s_enableHotkeyDebug = cfg.debug.showHotkeyDebug;

    if (s_enableHotkeyDebug) {
        Log("[Hotkey] Key/button pressed: " + std::to_string(vkCode) + " (raw=" + std::to_string(rawVkCode) + ") in mode: " +
            currentModeId);
    }
    if (s_enableHotkeyDebug) {
        Log("[Hotkey] Current game state: " + gameState);
        Log("[Hotkey] Evaluating " + std::to_string(cfg.hotkeys.size()) + " configured hotkeys");
    }

    for (size_t hotkeyIdx = 0; hotkeyIdx < cfg.hotkeys.size(); ++hotkeyIdx) {
        const auto& hotkey = cfg.hotkeys[hotkeyIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking: " + GetKeyComboString(hotkey.keys) + " (main: " + hotkey.mainMode + ", sec: " + hotkey.secondaryMode +
                ")");
        }

        bool conditionsMet = hotkey.conditions.gameState.empty() ||
                             std::find(hotkey.conditions.gameState.begin(), hotkey.conditions.gameState.end(), gameState) !=
                                 hotkey.conditions.gameState.end();

        std::string currentSecMode;
        bool wouldExitToFullscreen = false;
        if (hotkey.allowExitToFullscreenRegardlessOfGameState) {
            currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
            wouldExitToFullscreen = !currentSecMode.empty() && EqualsIgnoreCase(currentModeId, currentSecMode);
        }

        if (!conditionsMet) {
            bool allowBypass = (hotkey.allowExitToFullscreenRegardlessOfGameState && wouldExitToFullscreen) ||
                               (hotkey.triggerOnHold && !isKeyDown); // Hold release must always revert
            if (!allowBypass) {
                if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP: Game state conditions not met"); }
                continue;
            }
            if (s_enableHotkeyDebug) {
                Log("[Hotkey] BYPASS: Allowing exit even though game state conditions are not met");
            }
        }

        // Hold-mode helper: activate target mode on press, revert to default on release
        auto handleHoldMode = [&](const std::string& targetMode, const std::string& hotkeyId, bool blockKey) -> InputHandlerResult {
            if (isKeyDown) {
                if (isAutoRepeatKeyDown) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] HOLD DOWN repeat ignored: " + hotkeyId); }
                    if (blockKey) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }

                auto now = std::chrono::steady_clock::now();
                bool debounced = false;
                {
                    std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                    auto it = g_hotkeyTimestamps.find(hotkeyId);
                    if (it != g_hotkeyTimestamps.end() &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                        debounced = true;
                    } else {
                        g_hotkeyTimestamps[hotkeyId] = now;
                    }
                }
                if (!debounced && !targetMode.empty()) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] HOLD DOWN: " + hotkeyId + " -> " + targetMode); }
                    SwitchToMode(targetMode, "hotkey (hold)");
                }
            } else {
                if (s_enableHotkeyDebug) { Log("[Hotkey] HOLD RELEASE: " + hotkeyId + " -> " + cfg.defaultMode); }
                SwitchToMode(cfg.defaultMode, "hotkey (hold release)");
            }
            if (blockKey) return { true, 0 };
            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
        };

        for (const auto& alt : hotkey.altSecondaryModes) {
            bool skipExclusions = hotkey.triggerOnRelease || (hotkey.triggerOnHold && !isKeyDown);
            bool matched = CheckHotkeyMatch(alt.keys, vkCode, hotkey.conditions.exclusions, skipExclusions, s_bestMatchKeyCount);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(alt.keys, rebindTargetVk, hotkey.conditions.exclusions, skipExclusions, s_bestMatchKeyCount);
            if (matched || matchedViaRebind) {
                bool blockKey = hotkey.blockKeyFromGame || matchedViaRebind;
                std::string hotkeyId = GetKeyComboString(alt.keys);

                if (hotkey.triggerOnHold) { return handleHoldMode(alt.mode, hotkeyId, blockKey); }

                // Handle trigger-on-release invalidation tracking
                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    } else {
                        // Key released - check if invalidated
                        bool wasInvalidated = false;
                        {
                            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                            wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                            g_triggerOnReleasePending.erase(hotkeyId);
                            g_triggerOnReleaseInvalidated.erase(hotkeyId);
                        }

                        if (wasInvalidated) {
                            if (s_enableHotkeyDebug) { Log("[Hotkey] Alt trigger-on-release hotkey invalidated: " + hotkeyId); }
                            if (blockKey) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    bool debounced = false;
                    {
                        std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                        auto it = g_hotkeyTimestamps.find(hotkeyId);
                        if (it != g_hotkeyTimestamps.end() &&
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                            debounced = true;
                        } else {
                            g_hotkeyTimestamps[hotkeyId] = now;
                        }
                    }
                    if (debounced) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Alt hotkey matched but debounced: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    std::string currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    std::string newSecMode = (currentSecMode == alt.mode) ? hotkey.secondaryMode : alt.mode;
                    SetHotkeySecondaryMode(hotkeyIdx, newSecMode);

                    if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ ALT HOTKEY TRIGGERED: " + hotkeyId + " -> " + newSecMode); }

                    if (!newSecMode.empty()) { SwitchToMode(newSecMode, "alt hotkey"); }
                }
                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }

        {
            bool skipExclusions = hotkey.triggerOnRelease || (hotkey.triggerOnHold && !isKeyDown);
            bool matched = CheckHotkeyMatch(hotkey.keys, vkCode, hotkey.conditions.exclusions, skipExclusions, s_bestMatchKeyCount);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(hotkey.keys, rebindTargetVk, hotkey.conditions.exclusions, skipExclusions, s_bestMatchKeyCount);
            if (matched || matchedViaRebind) {
                bool blockKey = hotkey.blockKeyFromGame || matchedViaRebind;
                std::string hotkeyId = GetKeyComboString(hotkey.keys);

                if (hotkey.triggerOnHold) {
                    if (currentSecMode.empty()) { currentSecMode = GetHotkeySecondaryMode(hotkeyIdx); }
                    return handleHoldMode(currentSecMode, hotkeyId, blockKey);
                }

                if (hotkey.triggerOnRelease) {
                    if (isKeyDown) {
                        std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                        // Invalidate all other pending trigger-on-release hotkeys
                        for (const auto& pendingHotkeyId : g_triggerOnReleasePending) {
                            if (pendingHotkeyId != hotkeyId) { g_triggerOnReleaseInvalidated.insert(pendingHotkeyId); }
                        }
                        g_triggerOnReleasePending.insert(hotkeyId);
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Trigger-on-release hotkey pressed, added to pending: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    } else {
                        // Key released - check if invalidated
                        bool wasInvalidated = false;
                        {
                            std::lock_guard<std::mutex> lock(g_triggerOnReleaseMutex);
                            wasInvalidated = g_triggerOnReleaseInvalidated.count(hotkeyId) > 0;
                            g_triggerOnReleasePending.erase(hotkeyId);
                            g_triggerOnReleaseInvalidated.erase(hotkeyId);
                        }

                        if (wasInvalidated) {
                            if (s_enableHotkeyDebug) {
                                Log("[Hotkey] Trigger-on-release hotkey invalidated (another key was pressed): " + hotkeyId);
                            }
                            if (blockKey) return { true, 0 };
                            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                        }
                    }
                }

                // Check if this hotkey should trigger based on triggerOnRelease setting
                // When triggerOnRelease is true, only fire on key UP; when false (default), only fire on key DOWN
                if (hotkey.triggerOnRelease != isKeyDown) {
                    auto now = std::chrono::steady_clock::now();
                    bool debounced = false;
                    {
                        std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                        auto it = g_hotkeyTimestamps.find(hotkeyId);
                        if (it != g_hotkeyTimestamps.end() &&
                            std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < hotkey.debounce) {
                            debounced = true;
                        } else {
                            g_hotkeyTimestamps[hotkeyId] = now;
                        }
                    }
                    if (debounced) {
                        if (s_enableHotkeyDebug) { Log("[Hotkey] Main hotkey matched but debounced: " + hotkeyId); }
                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    // Lock-free read of current mode ID from double-buffer
                    std::string current = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
                    std::string targetMode;

                    if (currentSecMode.empty()) {
                        currentSecMode = GetHotkeySecondaryMode(hotkeyIdx);
                    }

                    if (EqualsIgnoreCase(current, currentSecMode)) {
                        targetMode = cfg.defaultMode;
                    } else {
                        targetMode = currentSecMode;
                    }

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ MAIN HOTKEY TRIGGERED: " + hotkeyId + " (current: " + current + " -> target: " + targetMode + ")");
                    }

                    if (!targetMode.empty()) { SwitchToMode(targetMode, "main hotkey"); }
                }
                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
    }

    for (size_t sensIdx = 0; sensIdx < cfg.sensitivityHotkeys.size(); ++sensIdx) {
        const auto& sensHotkey = cfg.sensitivityHotkeys[sensIdx];
        if (s_enableHotkeyDebug) {
            Log("[Hotkey] Checking sensitivity hotkey: " + GetKeyComboString(sensHotkey.keys) +
                " -> sens=" + std::to_string(sensHotkey.sensitivity));
        }

        bool conditionsMet = sensHotkey.conditions.gameState.empty() ||
                             std::find(sensHotkey.conditions.gameState.begin(), sensHotkey.conditions.gameState.end(), gameState) !=
                                 sensHotkey.conditions.gameState.end();
        if (!conditionsMet) {
            if (s_enableHotkeyDebug) { Log("[Hotkey] SKIP sensitivity: Game state conditions not met"); }
            continue;
        }

        // Sensitivity hotkeys only trigger on key down (no triggerOnRelease support)
        if (!isKeyDown) { continue; }

        {
            bool matched = CheckHotkeyMatch(sensHotkey.keys, vkCode, sensHotkey.conditions.exclusions, false, s_bestMatchKeyCount);
            bool matchedViaRebind = !matched && rebindTargetVk && CheckHotkeyMatch(sensHotkey.keys, rebindTargetVk, sensHotkey.conditions.exclusions, false, s_bestMatchKeyCount);
            if (matched || matchedViaRebind) {
                bool blockKey = matchedViaRebind;
                std::string hotkeyId = "sens_" + GetKeyComboString(sensHotkey.keys);

                auto now = std::chrono::steady_clock::now();
                bool debounced = false;
                {
                    std::lock_guard<std::mutex> tsLock(g_hotkeyTimestampsMutex);
                    auto it = g_hotkeyTimestamps.find(hotkeyId);
                    if (it != g_hotkeyTimestamps.end() &&
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() < sensHotkey.debounce) {
                        debounced = true;
                    } else {
                        g_hotkeyTimestamps[hotkeyId] = now;
                    }
                }
                if (debounced) {
                    if (s_enableHotkeyDebug) { Log("[Hotkey] Sensitivity hotkey matched but debounced: " + hotkeyId); }
                    if (blockKey) return { true, 0 };
                    return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                }

                if (sensHotkey.toggle) {
                    extern TempSensitivityOverride g_tempSensitivityOverride;
                    extern std::mutex g_tempSensitivityMutex;
                    std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);

                    if (g_tempSensitivityOverride.active && g_tempSensitivityOverride.activeSensHotkeyIndex == static_cast<int>(sensIdx)) {
                        g_tempSensitivityOverride.active = false;
                        g_tempSensitivityOverride.sensitivityX = 1.0f;
                        g_tempSensitivityOverride.sensitivityY = 1.0f;
                        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
                        g_tempSensitivityXAtomic.store(1.0f, std::memory_order_release);
                        g_tempSensitivityYAtomic.store(1.0f, std::memory_order_release);
                        g_tempSensitivityActiveAtomic.store(false, std::memory_order_release);

                        if (s_enableHotkeyDebug) { Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED OFF: " + hotkeyId); }

                        if (blockKey) return { true, 0 };
                        return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
                    }

                    g_tempSensitivityOverride.active = true;
                    if (sensHotkey.separateXY) {
                        g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivityX;
                        g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivityY;
                    } else {
                        g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivity;
                        g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivity;
                    }
                    g_tempSensitivityOverride.activeSensHotkeyIndex = static_cast<int>(sensIdx);
                    g_tempSensitivityXAtomic.store(g_tempSensitivityOverride.sensitivityX, std::memory_order_release);
                    g_tempSensitivityYAtomic.store(g_tempSensitivityOverride.sensitivityY, std::memory_order_release);
                    g_tempSensitivityActiveAtomic.store(true, std::memory_order_release);

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TOGGLED ON: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                    }
                } else {
                    {
                        extern TempSensitivityOverride g_tempSensitivityOverride;
                        extern std::mutex g_tempSensitivityMutex;
                        std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
                        g_tempSensitivityOverride.active = true;
                        if (sensHotkey.separateXY) {
                            g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivityX;
                            g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivityY;
                        } else {
                            g_tempSensitivityOverride.sensitivityX = sensHotkey.sensitivity;
                            g_tempSensitivityOverride.sensitivityY = sensHotkey.sensitivity;
                        }
                        g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
                        g_tempSensitivityXAtomic.store(g_tempSensitivityOverride.sensitivityX, std::memory_order_release);
                        g_tempSensitivityYAtomic.store(g_tempSensitivityOverride.sensitivityY, std::memory_order_release);
                        g_tempSensitivityActiveAtomic.store(true, std::memory_order_release);
                    }

                    if (s_enableHotkeyDebug) {
                        Log("[Hotkey] ✓✓✓ SENSITIVITY HOTKEY TRIGGERED: " + hotkeyId + " -> sens=" + std::to_string(sensHotkey.sensitivity));
                    }
                }

                if (blockKey) return { true, 0 };
                return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam) };
            }
        }
    }

    return { false, 0 };
}

InputHandlerResult HandleMouseCoordinateTranslationPhase(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM& lParam) {
    // Only translate messages whose lParam is already in CLIENT coordinates.
    // Wheel messages use SCREEN coordinates and must not be transformed here.
    switch (uMsg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        break;
    default:
        return { false, 0 };
    }

    if (!IsCursorVisible() && !g_showGui.load(std::memory_order_acquire)) {
        return { false, 0 };
    }

    PROFILE_SCOPE("HandleMouseCoordinateTranslation");

    // Prefer live viewport geometry for correctness during/after resize.
    // Cached mode viewport may lag by a tick and can desync mouse mapping.
    ModeViewportInfo geo = GetCurrentModeViewport();
    if (!geo.valid) {
        const CachedModeViewport& cachedMode = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];
        if (cachedMode.valid) {
            geo.valid = true;
            geo.width = cachedMode.width;
            geo.height = cachedMode.height;
            geo.stretchEnabled = cachedMode.stretchEnabled;
            geo.stretchX = cachedMode.stretchX;
            geo.stretchY = cachedMode.stretchY;
            geo.stretchWidth = cachedMode.stretchWidth;
            geo.stretchHeight = cachedMode.stretchHeight;
        }
    }
    if (!geo.valid || geo.width <= 0 || geo.height <= 0 || geo.stretchWidth <= 0 || geo.stretchHeight <= 0) { return { false, 0 }; }

    RECT clientRect{};
    if (!GetClientRect(hWnd, &clientRect)) { return { false, 0 }; }
    const int clientW = clientRect.right - clientRect.left;
    const int clientH = clientRect.bottom - clientRect.top;
    if (clientW <= 0 || clientH <= 0) { return { false, 0 }; }

    const int viewportLeft = geo.stretchX;
    const int viewportTop = geo.stretchY;
    const int viewportRight = geo.stretchX + geo.stretchWidth;
    const int viewportBottom = geo.stretchY + geo.stretchHeight;

    const int visibleLeft = (std::max)(0, viewportLeft);
    const int visibleTop = (std::max)(0, viewportTop);
    const int visibleRight = (std::min)(clientW, viewportRight);
    const int visibleBottom = (std::min)(clientH, viewportBottom);

    const int visibleW = visibleRight - visibleLeft;
    const int visibleH = visibleBottom - visibleTop;
    if (visibleW <= 0 || visibleH <= 0) { return { false, 0 }; }

    int mouseX = GET_X_LPARAM(lParam);
    int mouseY = GET_Y_LPARAM(lParam);

    if (mouseX < visibleLeft) mouseX = visibleLeft;
    if (mouseX >= visibleRight) mouseX = visibleRight - 1;
    if (mouseY < visibleTop) mouseY = visibleTop;
    if (mouseY >= visibleBottom) mouseY = visibleBottom - 1;

    const float scaleX = static_cast<float>(geo.width) / static_cast<float>(geo.stretchWidth);
    const float scaleY = static_cast<float>(geo.height) / static_cast<float>(geo.stretchHeight);

    const float srcOffsetX = static_cast<float>(visibleLeft - viewportLeft) * scaleX;
    const float srcOffsetY = static_cast<float>(visibleTop - viewportTop) * scaleY;

    const float localVisibleX = static_cast<float>(mouseX - visibleLeft);
    const float localVisibleY = static_cast<float>(mouseY - visibleTop);

    int newX = static_cast<int>(srcOffsetX + localVisibleX * scaleX);
    int newY = static_cast<int>(srcOffsetY + localVisibleY * scaleY);

    if (newX < 0) newX = 0;
    if (newY < 0) newY = 0;
    if (newX >= geo.width) newX = geo.width - 1;
    if (newY >= geo.height) newY = geo.height - 1;

    lParam = MAKELPARAM(newX, newY);
    return { false, 0 };
}

static UINT GetScanCodeWithExtendedFlag(DWORD vkCode) {
    auto isExtendedVk = [](DWORD vk) {
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_DIVIDE:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    };

    UINT scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC_EX);
    if (scanCodeWithFlags == 0) {
        scanCodeWithFlags = MapVirtualKey(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC);
    }

    if ((scanCodeWithFlags & 0xFF00) == 0 && isExtendedVk(vkCode) && (scanCodeWithFlags & 0xFF) != 0) { scanCodeWithFlags |= 0xE000; }

    return scanCodeWithFlags;
}

static LPARAM BuildKeyboardMessageLParam(UINT scanCodeWithFlags, bool isKeyDown, bool isSystemKey, UINT repeatCount, bool previousKeyState,
                                         bool transitionState) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    LPARAM out = static_cast<LPARAM>(repeatCount == 0 ? 1 : repeatCount);
    out |= (static_cast<LPARAM>(scanLow) << 16);
    if (isExtended) out |= (1LL << 24);
    if (isSystemKey) out |= (1LL << 29);
    if (previousKeyState) out |= (1LL << 30);
    if (transitionState) out |= (1LL << 31);

    if (!isKeyDown) out |= (1LL << 30) | (1LL << 31);

    return out;
}

static UINT ResolveOutputScanCode(DWORD outputVk, UINT configuredScanCodeWithFlags) {
    if (configuredScanCodeWithFlags == 0) { return GetScanCodeWithExtendedFlag(outputVk); }

    if ((configuredScanCodeWithFlags & 0xFF00) == 0) {
        UINT vkScan = GetScanCodeWithExtendedFlag(outputVk);
        if ((vkScan & 0xFF00) != 0 && ((vkScan & 0xFF) == (configuredScanCodeWithFlags & 0xFF))) { return vkScan; }
    }

    return configuredScanCodeWithFlags;
}

static bool IsMouseButtonVk(DWORD vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

static bool RebindCannotType(const KeyRebind& rebind) {
    DWORD triggerVk = rebind.toKey;
    if (triggerVk == 0) triggerVk = rebind.fromKey;
    if (triggerVk == 0) return false;

    UINT triggerScan = (rebind.useCustomOutput && rebind.customOutputScanCode != 0)
        ? static_cast<UINT>(rebind.customOutputScanCode)
        : GetScanCodeWithExtendedFlag(triggerVk);

    if (triggerScan != 0 && (triggerScan & 0xFF00) == 0) {
        UINT vkScan = GetScanCodeWithExtendedFlag(triggerVk);
        if ((vkScan & 0xFF00) != 0 && ((vkScan & 0xFF) == (triggerScan & 0xFF))) { triggerScan = vkScan; }
    }

    if (IsModifierVk(triggerVk) || IsModifierScanCode(triggerScan)) return true;
    if (IsMouseButtonVk(triggerVk)) return true;

    switch (triggerVk) {
    case VK_BACK:
    case VK_CAPITAL:
    case VK_DELETE:
    case VK_HOME:
    case VK_INSERT:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        return true;
    default:
        return false;
    }
}

static bool TryTranslateVkToChar(DWORD vkCode, bool shiftDown, WCHAR& outChar) {
    BYTE keyboardState[256] = {};
    if (shiftDown) keyboardState[VK_SHIFT] = 0x80;

    HKL keyboardLayout = GetKeyboardLayout(0);
    UINT scanCode = GetScanCodeWithExtendedFlag(vkCode) & 0xFF;
    WCHAR utf16Buffer[8] = {};

    int translated = ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, keyboardState, utf16Buffer, 8, 0, keyboardLayout);
    if (translated == 1) {
        outChar = utf16Buffer[0];
        return outChar != 0;
    }

    if (translated < 0) {
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

static bool TryTranslateVkToCharWithKeyboardState(DWORD vkCode, const BYTE keyboardState[256], WCHAR& outChar) {
    HKL keyboardLayout = GetKeyboardLayout(0);
    UINT scanCode = GetScanCodeWithExtendedFlag(vkCode) & 0xFF;

    WCHAR utf16Buffer[8] = {};
    BYTE ksCopy[256] = {};
    memcpy(ksCopy, keyboardState, 256);

    int translated = ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, ksCopy, utf16Buffer, 8, 0, keyboardLayout);
    if (translated == 1) {
        outChar = utf16Buffer[0];
        return outChar != 0;
    }

    if (translated < 0) {
        BYTE emptyState[256] = {};
        WCHAR clearBuffer[8] = {};
        ToUnicodeEx(static_cast<UINT>(vkCode), scanCode, emptyState, clearBuffer, 8, 0, keyboardLayout);
    }

    return false;
}

static bool ResolvePreferredOutputShiftState(const KeyRebind& rebind, bool shiftLayerActive, bool fallbackShifted) {
    if (shiftLayerActive && HasShiftLayerOutputOverride(rebind)) {
        return rebind.shiftLayerOutputShifted;
    }
    if (rebind.baseOutputShifted) {
        return true;
    }
    return fallbackShifted;
}

static void ApplyPreferredOutputShiftState(const KeyRebind& rebind, bool shiftLayerActive, BYTE keyboardState[256]) {
    if (!keyboardState) return;

    // Rebind text output should only follow live Shift state or explicit rebind shift settings,
    // not the toggle state from Caps Lock.
    keyboardState[VK_CAPITAL] = 0;

    bool shouldOverrideShiftState = false;
    BYTE shiftState = 0;
    if (shiftLayerActive && HasShiftLayerOutputVk(rebind)) {
        shouldOverrideShiftState = true;
        shiftState = rebind.shiftLayerOutputShifted ? 0x80 : 0;
    } else if (rebind.baseOutputShifted) {
        shouldOverrideShiftState = true;
        shiftState = 0x80;
    }
    if (!shouldOverrideShiftState) return;

    keyboardState[VK_SHIFT] = shiftState;
    keyboardState[VK_LSHIFT] = shiftState;
    keyboardState[VK_RSHIFT] = shiftState;
}

static bool TryTranslateVkToCharPreferShiftState(DWORD vkCode, bool preferShifted, WCHAR& outChar) {
    if (TryTranslateVkToChar(vkCode, preferShifted, outChar) && outChar != 0) {
        return true;
    }
    if (TryTranslateVkToChar(vkCode, !preferShifted, outChar) && outChar != 0) {
        return true;
    }
    return false;
}

static bool IsValidUnicodeScalar(uint32_t cp) {
    if (cp == 0) return false;
    if (cp > 0x10FFFFu) return false;
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
    return true;
}

static LRESULT SendUnicodeScalarAsCharMessage(HWND hWnd, UINT charMsg, uint32_t cp, LPARAM lParam) {
    if (!IsValidUnicodeScalar(cp)) return 0;

    if (cp <= 0xFFFFu) {
        return CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)(WCHAR)cp, lParam);
    }

    uint32_t v = cp - 0x10000u;
    WCHAR high = (WCHAR)(0xD800u + (v >> 10));
    WCHAR low = (WCHAR)(0xDC00u + (v & 0x3FFu));
    (void)CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)high, lParam);
    return CallWindowProc(g_originalWndProc, hWnd, charMsg, (WPARAM)low, lParam);
}

static constexpr ULONG_PTR kToolscreenInjectedExtraInfo = (ULONG_PTR)0x5453434E;
static constexpr ULONG_PTR kToolscreenMenuMaskExtraInfo = (ULONG_PTR)0x54534D4B;
static constexpr WORD kToolscreenMenuMaskVk = 0xFF;

static bool SendSynthKeyByVirtualKey(WORD virtualKey, bool keyDown, ULONG_PTR extraInfo) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = virtualKey;
    in.ki.wScan = 0;
    in.ki.dwFlags = keyDown ? 0 : KEYEVENTF_KEYUP;
    in.ki.time = 0;
    in.ki.dwExtraInfo = extraInfo;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

static bool SendMenuMaskKeyTap() {
    const bool downSent = SendSynthKeyByVirtualKey(kToolscreenMenuMaskVk, true, kToolscreenMenuMaskExtraInfo);
    const bool upSent = SendSynthKeyByVirtualKey(kToolscreenMenuMaskVk, false, kToolscreenMenuMaskExtraInfo);
    return downSent && upSent;
}

static bool IsMenuActivationModifierVk(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

static bool DoesRebindPreserveMenuModifier(const KeyRebind& rebind, DWORD triggerVk) {
    if (rebind.useCustomOutput) return false;

    switch (rebind.fromKey) {
    case VK_MENU:
        return triggerVk == VK_MENU || triggerVk == VK_LMENU || triggerVk == VK_RMENU;
    case VK_LMENU:
        return triggerVk == VK_LMENU;
    case VK_RMENU:
        return triggerVk == VK_RMENU;
    case VK_LWIN:
        return triggerVk == VK_LWIN;
    case VK_RWIN:
        return triggerVk == VK_RWIN;
    default:
        return false;
    }
}

static bool ShouldMaskMenuModifierForRebind(const KeyRebind& rebind, DWORD incomingVk, DWORD incomingRawVk, bool isKeyDown,
                                            bool isAutoRepeatKeyDown, DWORD triggerVk) {
    if (!isKeyDown || isAutoRepeatKeyDown) return false;
    if (!IsMenuActivationModifierVk(incomingVk) && !IsMenuActivationModifierVk(incomingRawVk) &&
        !IsMenuActivationModifierVk(rebind.fromKey)) {
        return false;
    }

    return !DoesRebindPreserveMenuModifier(rebind, triggerVk);
}

static bool IsDeepSuppressionEligibleSourceVk(DWORD vk) {
    return IsMenuActivationModifierVk(vk);
}

static UINT BuildScanCodeWithFlagsFromLowLevelEvent(const KBDLLHOOKSTRUCT& info) {
    UINT scanCodeWithFlags = static_cast<UINT>(info.scanCode & 0xFF);
    if ((info.flags & LLKHF_EXTENDED) != 0) {
        scanCodeWithFlags |= 0xE000;
    }
    return scanCodeWithFlags;
}

static bool ShouldSuppressLowLevelMenuModifierKey(DWORD rawVk) {
    if (!IsDeepSuppressionEligibleSourceVk(rawVk)) return false;
    if (g_isShuttingDown.load(std::memory_order_acquire)) return false;
    if (!g_gameWindowActive.load(std::memory_order_acquire)) return false;
    if (IsHotkeyBindingActive() || IsRebindBindingActive()) return false;

    const auto cfg = GetConfigSnapshot();
    if (!cfg || !cfg->keyRebinds.enabled) return false;

    for (const auto& rebind : cfg->keyRebinds.rebinds) {
        if (!rebind.enabled || rebind.fromKey == 0 || rebind.toKey == 0) continue;
        if (!IsDeepSuppressionEligibleSourceVk(rebind.fromKey)) continue;
        if (!MatchesRebindSourceKey(rawVk, rawVk, rebind.fromKey)) continue;

        const DWORD triggerVk = NormalizeModifierVkFromConfig(rebind.toKey, (rebind.useCustomOutput ? rebind.customOutputScanCode : 0));
        if (ShouldMaskMenuModifierForRebind(rebind, rawVk, rawVk, true, false, triggerVk)) {
            return true;
        }
    }

    return false;
}

static bool PostSuppressedLowLevelKeyMessage(HWND hWnd, const LowLevelSuppressedKeyState& state, bool isKeyDown, bool previousKeyState) {
    if (!hWnd || state.rawVk == 0) return false;

    const UINT msg = isKeyDown ? (state.isSystemKey ? WM_SYSKEYDOWN : WM_KEYDOWN) : (state.isSystemKey ? WM_SYSKEYUP : WM_KEYUP);
    const LPARAM msgLParam = BuildKeyboardMessageLParam(state.scanCodeWithFlags, isKeyDown, state.isSystemKey, 1, previousKeyState, !isKeyDown);
    return ::PostMessage(hWnd, msg, static_cast<WPARAM>(state.rawVk), msgLParam) != FALSE;
}

static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || lParam == 0) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const KBDLLHOOKSTRUCT* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (!info) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    if (info->dwExtraInfo == kToolscreenInjectedExtraInfo || info->dwExtraInfo == kToolscreenMenuMaskExtraInfo) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isKeyDown && !isKeyUp) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    const DWORD rawVk = static_cast<DWORD>(info->vkCode);
    const HWND targetHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (!targetHwnd) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    LowLevelSuppressedKeyState existingState{};
    bool hasExistingState = false;
    {
        std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
        auto it = s_lowLevelSuppressedKeys.find(rawVk);
        if (it != s_lowLevelSuppressedKeys.end()) {
            existingState = it->second;
            hasExistingState = true;
            if (isKeyUp) {
                s_lowLevelSuppressedKeys.erase(it);
            }
        }
    }

    if (hasExistingState) {
        (void)PostSuppressedLowLevelKeyMessage(targetHwnd, existingState, isKeyDown, true);
        return 1;
    }

    if (!isKeyDown) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    if (!ShouldSuppressLowLevelMenuModifierKey(rawVk)) {
        return CallNextHookEx(s_lowLevelKeyboardHook, code, wParam, lParam);
    }

    LowLevelSuppressedKeyState newState{};
    newState.rawVk = rawVk;
    newState.scanCodeWithFlags = BuildScanCodeWithFlagsFromLowLevelEvent(*info);
    newState.isSystemKey = (rawVk == VK_MENU || rawVk == VK_LMENU || rawVk == VK_RMENU);

    {
        std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
        s_lowLevelSuppressedKeys[rawVk] = newState;
    }

    (void)PostSuppressedLowLevelKeyMessage(targetHwnd, newState, true, false);
    return 1;
}

static void EnsureLowLevelKeyboardHookInstalled() {
    if (s_lowLevelKeyboardHook != NULL) return;
    if (g_isShuttingDown.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lock(s_lowLevelKeyboardHookMutex);
    if (s_lowLevelKeyboardHook != NULL) return;
    if (g_hModule == NULL) return;

    s_lowLevelKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hModule, 0);
    if (s_lowLevelKeyboardHook == NULL) {
        Log("WARNING: Failed to install low-level keyboard hook for deep key rebind suppression.");
        return;
    }

    LogCategory("init", "Installed low-level keyboard hook for deep key rebind suppression.");
}

void ReleaseActiveLowLevelRebindKeys(HWND hWnd) {
    if (!hWnd) return;

    std::vector<LowLevelSuppressedKeyState> activeKeys;
    {
        std::lock_guard<std::mutex> lock(s_lowLevelSuppressedKeysMutex);
        if (s_lowLevelSuppressedKeys.empty()) return;

        activeKeys.reserve(s_lowLevelSuppressedKeys.size());
        for (const auto& [vk, state] : s_lowLevelSuppressedKeys) {
            (void)vk;
            activeKeys.push_back(state);
        }
        s_lowLevelSuppressedKeys.clear();
    }

    for (const auto& state : activeKeys) {
        const UINT msg = state.isSystemKey ? WM_SYSKEYUP : WM_KEYUP;
        const LPARAM msgLParam = BuildKeyboardMessageLParam(state.scanCodeWithFlags, false, state.isSystemKey, 1, true, true);
        (void)HandleKeyRebinding(hWnd, msg, static_cast<WPARAM>(state.rawVk), msgLParam);
    }
}

static bool SendSynthKeyByScanCode(UINT scanCodeWithFlags, bool keyDown) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = (WORD)(scanCodeWithFlags & 0xFF);
    DWORD flags = KEYEVENTF_SCANCODE;
    if ((scanCodeWithFlags & 0xFF00) != 0) flags |= KEYEVENTF_EXTENDEDKEY;
    if (!keyDown) flags |= KEYEVENTF_KEYUP;
    in.ki.dwFlags = flags;
    in.ki.time = 0;
    in.ki.dwExtraInfo = kToolscreenInjectedExtraInfo;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

InputHandlerResult HandleInjectedMenuMaskKey(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleInjectedMenuMaskKey");

    (void)hWnd;
    (void)lParam;

    if (GetMessageExtraInfo() != kToolscreenMenuMaskExtraInfo) { return { false, 0 }; }
    if (static_cast<WORD>(wParam) != kToolscreenMenuMaskVk) { return { false, 0 }; }
    return { true, 0 };
}

InputHandlerResult HandleKeyRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        break;
    default:
        return { false, 0 };
    }
    PROFILE_SCOPE("HandleKeyRebinding");

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        if (GetMessageExtraInfo() == kToolscreenInjectedExtraInfo) {
            return { false, 0 };
        }
    }

    DWORD rawVkCode = 0;
    DWORD vkCode = 0;
    bool isMouseButton = false;
    bool isKeyDown = false;

    if (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = true;
    } else if (uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP) {
        rawVkCode = static_cast<DWORD>(wParam);
        isKeyDown = false;
    } else if (uMsg == WM_XBUTTONDOWN) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_XBUTTONUP) {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        rawVkCode = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_LBUTTONDOWN) {
        rawVkCode = VK_LBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_LBUTTONUP) {
        rawVkCode = VK_LBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_RBUTTONDOWN) {
        rawVkCode = VK_RBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_RBUTTONUP) {
        rawVkCode = VK_RBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else if (uMsg == WM_MBUTTONDOWN) {
        rawVkCode = VK_MBUTTON;
        isMouseButton = true;
        isKeyDown = true;
    } else if (uMsg == WM_MBUTTONUP) {
        rawVkCode = VK_MBUTTON;
        isMouseButton = true;
        isKeyDown = false;
    } else {
        return { false, 0 };
    }

    if (isMouseButton && g_showGui.load(std::memory_order_acquire)) { return { false, 0 }; }

    vkCode = rawVkCode;
    if (!isMouseButton && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYUP)) {
        vkCode = NormalizeModifierVkFromKeyMessage(rawVkCode, lParam);
        if (vkCode == 0) vkCode = rawVkCode;
    }

    const bool isAutoRepeatKeyDown = (!isMouseButton && isKeyDown && (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && ((lParam & (1LL << 30)) != 0));

    // Use config snapshot for thread-safe access to key rebinds
    auto rebindCfg = GetConfigSnapshot();
    if (!rebindCfg || !rebindCfg->keyRebinds.enabled) { return { false, 0 }; }

    for (size_t i = 0; i < rebindCfg->keyRebinds.rebinds.size(); ++i) {
        const auto& rebind = rebindCfg->keyRebinds.rebinds[i];

        if (rebind.enabled && rebind.fromKey != 0 && rebind.toKey != 0 && MatchesRebindSourceKey(vkCode, rawVkCode, rebind.fromKey)) {
            const bool shiftLayerActive = IsShiftLayerActiveForRebind(rebind, vkCode, rawVkCode, isKeyDown);
            auto isNonCharSourceVk = [&](DWORD vk) {
                if (IsModifierVk(vk)) return true;
                if (vk == VK_LWIN || vk == VK_RWIN) return true;
                if (vk >= VK_F1 && vk <= VK_F24) return true;

                switch (vk) {
                case VK_INSERT:
                case VK_DELETE:
                case VK_HOME:
                case VK_END:
                case VK_PRIOR:
                case VK_NEXT:
                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN:
                case VK_CLEAR:
                case VK_ESCAPE:
                case VK_PAUSE:
                case VK_SNAPSHOT:
                case VK_CAPITAL:
                case VK_NUMLOCK:
                case VK_SCROLL:
                case VK_APPS:
                    return true;
                default:
                    return false;
                }
            };

            const DWORD triggerVK =
                NormalizeModifierVkFromConfig(rebind.toKey, (rebind.useCustomOutput ? rebind.customOutputScanCode : 0));

            if (ShouldMaskMenuModifierForRebind(rebind, vkCode, rawVkCode, isKeyDown, isAutoRepeatKeyDown, triggerVK)) {
                (void)SendMenuMaskKeyTap();
            }

            const DWORD defaultTextVK = NormalizeModifierVkFromConfig(rebind.fromKey);
            const DWORD effectiveCustomOutputVk = ResolveEffectiveCustomOutputVk(rebind, shiftLayerActive);
            const DWORD textVK = NormalizeModifierVkFromConfig(
                (effectiveCustomOutputVk != 0) ? effectiveCustomOutputVk : defaultTextVK);
            const bool preferShiftedText = ResolvePreferredOutputShiftState(
                rebind, shiftLayerActive, IsShiftDownForIncomingEvent(vkCode, rawVkCode, isKeyDown));

            UINT outputScanCode = GetScanCodeWithExtendedFlag(triggerVK);
            if (rebind.useCustomOutput && rebind.customOutputScanCode != 0) {
                outputScanCode = ResolveOutputScanCode(triggerVK, rebind.customOutputScanCode);
            }
            const bool outputScanIsModifier = IsModifierScanCode(outputScanCode);

            if (triggerVK == VK_LBUTTON || triggerVK == VK_RBUTTON || triggerVK == VK_MBUTTON || triggerVK == VK_XBUTTON1 ||
                triggerVK == VK_XBUTTON2) {
                UINT newMsg = 0;
                auto buildMouseKeyState = [&](DWORD buttonVk, bool buttonDown) -> WORD {
                    WORD mk = 0;
                    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) mk |= MK_CONTROL;
                    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) mk |= MK_SHIFT;

                    auto setBtn = [&](int vk, WORD mask, bool isThisButton) {
                        bool down = (GetKeyState(vk) & 0x8000) != 0;
                        if (isThisButton) down = buttonDown;
                        if (down) mk |= mask;
                    };

                    setBtn(VK_LBUTTON, MK_LBUTTON, buttonVk == VK_LBUTTON);
                    setBtn(VK_RBUTTON, MK_RBUTTON, buttonVk == VK_RBUTTON);
                    setBtn(VK_MBUTTON, MK_MBUTTON, buttonVk == VK_MBUTTON);
                    setBtn(VK_XBUTTON1, MK_XBUTTON1, buttonVk == VK_XBUTTON1);
                    setBtn(VK_XBUTTON2, MK_XBUTTON2, buttonVk == VK_XBUTTON2);
                    return mk;
                };

                LPARAM mouseLParam = lParam;
                if (!isMouseButton) {
                    POINT pt{};
                    if (GetCursorPos(&pt) && ScreenToClient(hWnd, &pt)) {
                        mouseLParam = MAKELPARAM(pt.x, pt.y);
                    } else {
                        RECT clientRect{};
                        if (GetClientRect(hWnd, &clientRect)) {
                            mouseLParam = MAKELPARAM((clientRect.right - clientRect.left) / 2, (clientRect.bottom - clientRect.top) / 2);
                        }
                    }
                }

                WORD mkState = buildMouseKeyState(triggerVK, isKeyDown);
                WPARAM newWParam = mkState;

                if (triggerVK == VK_LBUTTON) {
                    newMsg = isKeyDown ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                } else if (triggerVK == VK_RBUTTON) {
                    newMsg = isKeyDown ? WM_RBUTTONDOWN : WM_RBUTTONUP;
                } else if (triggerVK == VK_MBUTTON) {
                    newMsg = isKeyDown ? WM_MBUTTONDOWN : WM_MBUTTONUP;
                } else if (triggerVK == VK_XBUTTON1) {
                    newMsg = isKeyDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON1);
                } else if (triggerVK == VK_XBUTTON2) {
                    newMsg = isKeyDown ? WM_XBUTTONDOWN : WM_XBUTTONUP;
                    newWParam = MAKEWPARAM(mkState, XBUTTON2);
                }

                LRESULT mouseResult = CallWindowProc(g_originalWndProc, hWnd, newMsg, newWParam, mouseLParam);

                const bool fromKeyIsNonCharMouse =
                    isMouseButton ||
                    isNonCharSourceVk(rebind.fromKey);

                if (isKeyDown && fromKeyIsNonCharMouse) {
                    const uint32_t configuredUnicodeText =
                        (shiftLayerActive && HasShiftLayerOutputUnicode(rebind))
                            ? (uint32_t)rebind.shiftLayerOutputUnicode
                            : ((!shiftLayerActive && rebind.useCustomOutput && rebind.customOutputUnicode != 0)
                                   ? (uint32_t)rebind.customOutputUnicode
                                   : 0u);

                    const UINT textScanCode = GetScanCodeWithExtendedFlag(textVK);
                    LPARAM charLParam = BuildKeyboardMessageLParam(textScanCode, true, false, 1, false, false);

                    if (configuredUnicodeText != 0) {
                        SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, (WPARAM)(WCHAR)configuredUnicodeText, charLParam);
                    } else {
                        WCHAR outChar = 0;
                        if (textVK == VK_RETURN) {
                            outChar = L'\r';
                        } else if (textVK == VK_TAB) {
                            outChar = L'\t';
                        } else if (textVK == VK_BACK) {
                            outChar = L'\b';
                        } else {
                            BYTE ks[256] = {};
                            if (GetKeyboardState(ks)) {
                                if (rebind.fromKey == VK_SHIFT || rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                                    ks[VK_SHIFT] = 0;
                                    ks[VK_LSHIFT] = 0;
                                    ks[VK_RSHIFT] = 0;
                                } else if (rebind.fromKey == VK_CONTROL || rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                                    ks[VK_CONTROL] = 0;
                                    ks[VK_LCONTROL] = 0;
                                    ks[VK_RCONTROL] = 0;
                                } else if (rebind.fromKey == VK_MENU || rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                                    ks[VK_MENU] = 0;
                                    ks[VK_LMENU] = 0;
                                    ks[VK_RMENU] = 0;
                                }

                                ApplyPreferredOutputShiftState(rebind, shiftLayerActive, ks);

                                (void)TryTranslateVkToCharWithKeyboardState(textVK, ks, outChar);
                            }

                            if (outChar == 0) {
                                (void)TryTranslateVkToCharPreferShiftState(textVK, preferShiftedText, outChar);
                            }
                        }

                        if (outChar != 0) {
                            SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, static_cast<WPARAM>(outChar), charLParam);
                        }
                    }
                }

                return { true, mouseResult };
            }

            const bool isSystemKeyMsg = (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP);
            const bool sourceIsAlt = IsAltVk(vkCode) || IsAltVk(rawVkCode);
            const bool altContextActive = isSystemKeyMsg && !sourceIsAlt;
            const bool outputIsAlt = IsAltVk(triggerVK);
            const bool outputUsesSystemMessage = outputIsAlt;
            const bool outputHasAltContext = altContextActive || outputIsAlt;
            UINT outputMsg =
                isKeyDown ? (outputUsesSystemMessage ? WM_SYSKEYDOWN : WM_KEYDOWN) : (outputUsesSystemMessage ? WM_SYSKEYUP : WM_KEYUP);

            const bool fromKeyIsNonChar =
                isMouseButton ||
                isNonCharSourceVk(rebind.fromKey);

            UINT repeatCount = 1;
            bool previousState = !isKeyDown;
            bool transitionState = !isKeyDown;
            if (!isMouseButton) {
                repeatCount = static_cast<UINT>(lParam & 0xFFFF);
                if (repeatCount == 0) repeatCount = 1;

                previousState = ((lParam & (1LL << 30)) != 0);
                transitionState = ((lParam & (1LL << 31)) != 0);
            }

            auto emitTypedChar = [&](LPARAM charLParam) {
                const uint32_t configuredUnicodeText =
                    (shiftLayerActive && HasShiftLayerOutputUnicode(rebind))
                        ? (uint32_t)rebind.shiftLayerOutputUnicode
                        : ((!shiftLayerActive && rebind.useCustomOutput && rebind.customOutputUnicode != 0)
                               ? (uint32_t)rebind.customOutputUnicode
                               : 0u);

                if (configuredUnicodeText != 0) {
                    if (configuredUnicodeText <= 0xFFFFu) {
                        SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, (WPARAM)(WCHAR)configuredUnicodeText, charLParam);
                    } else {
                        SendUnicodeScalarAsCharMessage(hWnd, WM_CHAR, configuredUnicodeText, charLParam);
                    }
                    return;
                }

                WCHAR outChar = 0;

                if (textVK == VK_RETURN) {
                    outChar = L'\r';
                } else if (textVK == VK_TAB) {
                    outChar = L'\t';
                } else if (textVK == VK_BACK) {
                    outChar = L'\b';
                } else {
                    BYTE ks[256] = {};
                    if (GetKeyboardState(ks)) {
                        if (rebind.fromKey == VK_SHIFT || rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                            ks[VK_SHIFT] = 0;
                            ks[VK_LSHIFT] = 0;
                            ks[VK_RSHIFT] = 0;
                        } else if (rebind.fromKey == VK_CONTROL || rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                            ks[VK_CONTROL] = 0;
                            ks[VK_LCONTROL] = 0;
                            ks[VK_RCONTROL] = 0;
                        } else if (rebind.fromKey == VK_MENU || rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                            ks[VK_MENU] = 0;
                            ks[VK_LMENU] = 0;
                            ks[VK_RMENU] = 0;
                        }

                        ApplyPreferredOutputShiftState(rebind, shiftLayerActive, ks);

                        (void)TryTranslateVkToCharWithKeyboardState(textVK, ks, outChar);
                    }

                    if (outChar == 0) {
                        (void)TryTranslateVkToCharPreferShiftState(textVK, preferShiftedText, outChar);
                    }
                }

                if (outChar != 0) {
                    SendMessage(hWnd, WM_TOOLSCREEN_CHAR_NO_REBIND, static_cast<WPARAM>(outChar), charLParam);
                }
            };

            // If the output is a modifier key, we must synthesize it via SendInput so the OS/game keyboard state
            if (IsModifierVk(triggerVK) || outputScanIsModifier) {
                const bool sourceIsModifier = IsModifierVk(rebind.fromKey) || IsModifierVk(vkCode) || IsModifierVk(rawVkCode);
                if (isAutoRepeatKeyDown && !sourceIsModifier) {
                    return { true, 0 };
                }

                (void)SendSynthKeyByScanCode(outputScanCode, isKeyDown);

                if (isKeyDown && fromKeyIsNonChar && !outputScanIsModifier) {
                    const UINT textScanCode = GetScanCodeWithExtendedFlag(textVK);
                    LPARAM charLParam =
                        BuildKeyboardMessageLParam(textScanCode, true, outputHasAltContext, repeatCount, previousState, transitionState);
                    emitTypedChar(charLParam);
                }

                return { true, 0 };
            }

            // Windows typically sends generic modifier VKs in wParam (VK_SHIFT/VK_CONTROL/VK_MENU)
            const DWORD msgVk = [&]() -> DWORD {
                if (triggerVK == VK_LSHIFT || triggerVK == VK_RSHIFT) return VK_SHIFT;
                if (triggerVK == VK_LCONTROL || triggerVK == VK_RCONTROL) return VK_CONTROL;
                if (triggerVK == VK_LMENU || triggerVK == VK_RMENU) return VK_MENU;
                return triggerVK;
            }();

            LPARAM newLParam =
                BuildKeyboardMessageLParam(outputScanCode, isKeyDown, outputHasAltContext, repeatCount, previousState, transitionState);

            // Do NOT PostMessage keyboard outputs when the source event is a mouse button.
            LRESULT keyResult = CallWindowProc(g_originalWndProc, hWnd, outputMsg, msgVk, newLParam);

            if (isKeyDown && fromKeyIsNonChar) {
                emitTypedChar(newLParam);
            }

            return { true, keyResult };
        }
    }
    return { false, 0 };
}

InputHandlerResult HandleCustomKeyNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TOOLSCREEN_KEYDOWN_NO_REBIND && uMsg != WM_TOOLSCREEN_KEYUP_NO_REBIND) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCustomKeyNoRebind");

    const UINT forwardedMsg = (uMsg == WM_TOOLSCREEN_KEYDOWN_NO_REBIND) ? WM_KEYDOWN : WM_KEYUP;

    if (g_showGui.load()) {
        ImGuiInputQueue_EnqueueWin32Message(hWnd, forwardedMsg, wParam, lParam);
        return { true, 1 };
    }

    if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, forwardedMsg, wParam, lParam) }; }
    return { true, DefWindowProc(hWnd, forwardedMsg, wParam, lParam) };
}

InputHandlerResult HandleCustomCharNoRebind(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_TOOLSCREEN_CHAR_NO_REBIND) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCustomCharNoRebind");

    HandleCharLogging(WM_CHAR, wParam, lParam);

    if (g_showGui.load()) {
        ImGuiInputQueue_EnqueueWin32Message(hWnd, WM_CHAR, wParam, lParam);
        return { true, 1 };
    }

    if (g_originalWndProc) { return { true, CallWindowProc(g_originalWndProc, hWnd, WM_CHAR, wParam, lParam) }; }
    return { true, DefWindowProc(hWnd, WM_CHAR, wParam, lParam) };
}

InputHandlerResult HandleCharRebinding(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg != WM_CHAR) { return { false, 0 }; }
    PROFILE_SCOPE("HandleCharRebinding");

    auto charRebindCfg = GetConfigSnapshot();
    if (!charRebindCfg || !charRebindCfg->keyRebinds.enabled) { return { false, 0 }; }

    WCHAR inputChar = static_cast<WCHAR>(wParam);

    for (const auto& rebind : charRebindCfg->keyRebinds.rebinds) {
        if (!rebind.enabled || rebind.fromKey == 0 || rebind.toKey == 0) continue;

        WCHAR fromUnshifted = 0;
        WCHAR fromShifted = 0;
        bool hasFromUnshifted = TryTranslateVkToChar(rebind.fromKey, false, fromUnshifted);
        bool hasFromShifted = TryTranslateVkToChar(rebind.fromKey, true, fromShifted);

        bool matched = false;

        if (hasFromUnshifted && inputChar == fromUnshifted) {
            matched = true;
        } else if (hasFromShifted && inputChar == fromShifted) {
            matched = true;
        }

        if (matched) {
            const bool shiftDown = IsShiftCurrentlyDown();
            const bool shiftLayerActive = HasShiftLayerOutputOverride(rebind) && shiftDown;
            const bool preferShiftedText = ResolvePreferredOutputShiftState(rebind, shiftLayerActive, shiftDown);

            if (shiftLayerActive && HasShiftLayerOutputUnicode(rebind)) {
                LRESULT r = SendUnicodeScalarAsCharMessage(hWnd, uMsg, (uint32_t)rebind.shiftLayerOutputUnicode, lParam);
                return { true, r };
            }

            if (!shiftLayerActive && rebind.useCustomOutput && rebind.customOutputUnicode != 0) {
                LRESULT r = SendUnicodeScalarAsCharMessage(hWnd, uMsg, (uint32_t)rebind.customOutputUnicode, lParam);
                return { true, r };
            }

            DWORD outputVK = ResolveEffectiveCustomOutputVk(rebind, shiftLayerActive);

            if (outputVK == 0) {
                if (RebindCannotType(rebind)) {
                    Log("[REBIND WM_CHAR] Consuming char code " + std::to_string(static_cast<unsigned int>(inputChar)) +
                        " (trigger cannot type)");
                    return { true, 0 };
                }
                return { false, 0 };
            }

            outputVK = NormalizeModifierVkFromConfig(outputVK);

            WCHAR outputChar = 0;
            if (outputVK == VK_RETURN) {
                outputChar = L'\r';
            } else if (outputVK == VK_TAB) {
                outputChar = L'\t';
            } else if (outputVK == VK_BACK) {
                outputChar = L'\b';
            } else {
                (void)TryTranslateVkToCharPreferShiftState(outputVK, preferShiftedText, outputChar);
            }

            if (outputChar == 0) {
                Log("[REBIND WM_CHAR] Consuming char code " + std::to_string(static_cast<unsigned int>(inputChar)) +
                    " (output VK has no WM_CHAR)");
                return { true, 0 };
            }

            Log("[REBIND WM_CHAR] Remapping char code " + std::to_string(static_cast<unsigned int>(inputChar)) + " -> " +
                std::to_string(static_cast<unsigned int>(outputChar)));

            return { true, CallWindowProc(g_originalWndProc, hWnd, uMsg, outputChar, lParam) };
        }
    }
    return { false, 0 };
}

static void ResolveHotkeyPriority(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DWORD vkCode = 0;
    bool isKeyDownMessage = false;
    bool isKeyUpMessage = false;
    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        vkCode = NormalizeModifierVkFromKeyMessage(static_cast<DWORD>(wParam), lParam);
        isKeyDownMessage = true;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        vkCode = NormalizeModifierVkFromKeyMessage(static_cast<DWORD>(wParam), lParam);
        isKeyUpMessage = true;
        break;
    case WM_LBUTTONDOWN:
        vkCode = VK_LBUTTON;
        isKeyDownMessage = true;
        break;
    case WM_RBUTTONDOWN:
        vkCode = VK_RBUTTON;
        isKeyDownMessage = true;
        break;
    case WM_MBUTTONDOWN:
        vkCode = VK_MBUTTON;
        isKeyDownMessage = true;
        break;
    case WM_LBUTTONUP:
        vkCode = VK_LBUTTON;
        isKeyUpMessage = true;
        break;
    case WM_RBUTTONUP:
        vkCode = VK_RBUTTON;
        isKeyUpMessage = true;
        break;
    case WM_MBUTTONUP:
        vkCode = VK_MBUTTON;
        isKeyUpMessage = true;
        break;
    case WM_XBUTTONDOWN:
        vkCode = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyDownMessage = true;
        break;
    case WM_XBUTTONUP:
        vkCode = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isKeyUpMessage = true;
        break;
    default:
        s_bestMatchKeyCount = 0;
        return;
    }

    if (isKeyUpMessage) {
        auto it = s_bestMatchKeyCountByMainVk.find(vkCode);
        s_bestMatchKeyCount = (it != s_bestMatchKeyCountByMainVk.end()) ? it->second : 0;
        if (it != s_bestMatchKeyCountByMainVk.end()) {
            s_bestMatchKeyCountByMainVk.erase(it);
        }
        return;
    }

    if (!isKeyDownMessage) {
        s_bestMatchKeyCount = 0;
        return;
    }

    s_bestMatchKeyCount = 0;

    { // Skip resolution entirely for keys that aren't bound to any hotkey
        std::lock_guard<std::mutex> lock(g_hotkeyMainKeysMutex);
        if (g_hotkeyMainKeys.find(vkCode) == g_hotkeyMainKeys.end()) {
            s_bestMatchKeyCountByMainVk.erase(vkCode);
            return;
        }
    }

    auto check = [&](const std::vector<DWORD>& keys, const std::vector<DWORD>& exclusions = {}) {
        if (!keys.empty() && CheckHotkeyMatch(keys, vkCode, exclusions, false))
            s_bestMatchKeyCount = (std::max)(s_bestMatchKeyCount, keys.size());
    };

    check(g_config.guiHotkey);
    check(g_config.borderlessHotkey);
    check(g_config.imageOverlaysHotkey);
    check(g_config.windowOverlaysHotkey);
    check(g_config.keyRebinds.toggleHotkey);

    for (const auto& hk : g_config.hotkeys) {
        check(hk.keys, hk.conditions.exclusions);
        for (const auto& alt : hk.altSecondaryModes)
            check(alt.keys, hk.conditions.exclusions);
    }

    for (const auto& sh : g_config.sensitivityHotkeys)
        check(sh.keys, sh.conditions.exclusions);

    s_bestMatchKeyCountByMainVk[vkCode] = s_bestMatchKeyCount;
}

LRESULT CALLBACK SubclassedWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    PROFILE_SCOPE("SubclassedWndProc");

    const HWND expectedHwnd = g_subclassedHwnd.load();
    if (expectedHwnd != NULL && hWnd != expectedHwnd) {
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    EnsureLowLevelKeyboardHookInstalled();

    RegisterBindingInputEvent(uMsg, wParam, lParam);

    // Keep all window metrics/cache updates in one place to avoid split-brain resize state.
    SyncWindowMetricsFromMessage(hWnd, uMsg, wParam, lParam);

    InputHandlerResult result;

    result = HandleShutdownCheck(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowValidation(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleToolscreenQueryMessages(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (uMsg == WM_TOOLSCREEN_APPLY_FOCUS_REGAIN_SIZE) {
        s_deferredFocusRegainWmSizePending.store(false, std::memory_order_relaxed);
        ResendCurrentModeWmSize(hWnd, "input_hook:focus_regain_deferred");
        return 0;
    }

    result = HandleInjectedMenuMaskKey(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        ResolveHotkeyPriority(uMsg, wParam, lParam);
        break;
    default:
        s_bestMatchKeyCount = 0;
        break;
    }

    result = HandleBorderlessToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleImageOverlaysToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;
    result = HandleWindowOverlaysToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;
    result = HandleKeyRebindsToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    HandleCharLogging(uMsg, wParam, lParam);

    result = HandleAltF4(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleConfigLoadFailure(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (uMsg == WM_SETCURSOR) {
        const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        result = HandleSetCursor(hWnd, uMsg, wParam, lParam, localGameState);
        if (result.consumed) return result.result;
    }

    result = HandleDestroy(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (g_isShuttingDown.load()) { return CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam); }

    result = HandleLocalKeyRepeatMessages(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleImGuiInput(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiToggle(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayKeyboard(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleWindowOverlayMouse(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleGuiInputBlocking(uMsg);
    if (result.consumed) return result.result;

    result = HandleActivate(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    if (uMsg == WM_SIZE) {
        const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
        result = HandleWmSizeModeDimensions(hWnd, uMsg, wParam, lParam, currentModeId);
        if (result.consumed) return result.result;
    }

    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP: {
        const std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
        const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        result = HandleHotkeys(hWnd, uMsg, wParam, lParam, currentModeId, localGameState);
        if (result.consumed) return result.result;
        break;
    }
    default:
        break;
    }

    result = HandleMouseCoordinateTranslationPhase(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleMouseMovementRateLimit(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCustomKeyNoRebind(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleKeyRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCustomCharNoRebind(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    result = HandleCharRebinding(hWnd, uMsg, wParam, lParam);
    if (result.consumed) return result.result;

    const LRESULT forwarded = CallWindowProc(g_originalWndProc, hWnd, uMsg, wParam, lParam);
    if (IsFocusGainMessage(uMsg) && s_deferredFocusRegainWmSizePending.exchange(false, std::memory_order_relaxed)) {
        QueueDeferredFocusRegainWmSize(hWnd);
    }
    return forwarded;
}


