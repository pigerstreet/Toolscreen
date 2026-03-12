# Toolscreen Architecture Details

## Mode System
- `ModeConfig` struct in gui.h: id, width/height, expressions, mirrors, images, transitions
- Expression-based sizing: `roundEven(screenHeight * 0.95)`, evaluated on logic thread
- Transitions: Bounce, ease-in/out with configurable duration
- Sensitivity overrides per mode
- Mode switch flow: GUI → g_pendingModeSwitch → logic thread → render thread animates

## Render Pipeline
- Shader programs: g_filterProgram (mirror color), g_renderProgram (borders), g_backgroundProgram, g_gradientProgram, g_imageRenderProgram
- Mirror capture: read game texture → color filter → off-screen texture → GPU fence sync → render to viewport
- EyeZoom: snapshot texture → clone horizontally → stretch → grid overlay → fade transitions

## Config Snapshot Pattern
```
GUI writes g_config (mutable draft)
→ PublishConfigSnapshot() creates shared_ptr<const Config>
→ Render/logic/input threads call GetConfigSnapshot() (lock-free)
→ g_configSnapshotVersion tracks changes
```

## Double-Buffer Pattern
```cpp
extern std::string g_modeIdBuffers[2];
extern std::atomic<int> g_currentModeIdIndex;
// Writer updates inactive buffer, then swaps index atomically
// Reader reads active buffer without locking
```

## Input Handler Chain (sequential in SubclassedWndProc)
1. HandleShutdownCheck
2. HandleWindowValidation
3. HandleConfigLoadFailure
4. HandleSetCursor
5. HandleGuiToggle
6. HandleHotkeys
7. HandleMouseCoordinateTranslation
8. HandleGuiInputBlocking
9. HandleImGuiInput
10. HandleKeyRebinding
11. HandleCharRebinding

## Hotkey Flow
Key press → input_hook → match hotkey from g_hotkeyMainKeys → check conditions → debounce → post g_pendingModeSwitch → logic thread → mode transition

## Mirror Config Example (from default.toml)
```toml
[[mirror]]
name = 'Mapless'
captureWidth = 23
captureHeight = 7
input = [ { relativeTo = 'topLeftViewport', x = 14, y = 38 } ]
output = { relativeTo = 'centerViewport', x = 362, y = 169, scale = 8.0 }
colors = { targetColors = [ [ 221, 221, 221 ] ], output = [ 255, 255, 255 ] }
```
