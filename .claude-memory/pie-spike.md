# Pie Chart Spike Detector

## What It Does
Automatically detects silverfish spawner spikes in the F3 pie chart (portal room detection in strongholds). Compares orange (block entities) vs green (unspecified) pixel ratios and triggers visual + audio alerts when the ratio matches a configured target.

## Status
- **Implemented, built, committed, and pushed** (2026-03-12)
- **Tested in-game** — spike detection works but visual flash was not appearing (fixed)
- **Needs re-testing** — user should verify the visual flash now works after the bug fix

## Files Modified (15 files)

### Config Layer
| File | Change |
|------|--------|
| `src/config/config_defaults.h` | 9 `PIE_SPIKE_*` constants (lines ~168-176) |
| `src/gui/gui.h` | `PieSpikeConfig` struct before `Config`, `pieSpike` member in `Config`, 3 atomic globals (`g_pieSpikeAlertActive`, `g_pieSpikeLastOrangeRatio`, `g_pieSpikeLastAlertTimeMs`) |
| `src/gui/gui_runtime.cpp` | Definitions of the 3 atomic globals |
| `src/config/config_toml.h` | `PieSpikeConfig` forward decl + `PieSpikeConfigToToml`/`FromToml` declarations |
| `src/config/config_toml.cpp` | Serialization impl (before `ConfigToToml`), wired into `ConfigToToml`/`ConfigFromToml` under key `"pieSpike"` |
| `src/config/default.toml` | `[pieSpike]` section appended at end of file |

### Mirror Thread (GPU Analysis)
| File | Change |
|------|--------|
| `src/render/mirror_thread.h` | `PieSpikeAnalysisResult` struct + `g_pieSpikeResults[2]` double-buffer + `g_pieSpikeResultIndex` atomic |
| `src/render/mirror_thread.cpp` | `g_pieSpikeResults`/`g_pieSpikeResultIndex` globals defined; `MT_PieSpikeGpuResources` struct; `MT_AnalyzePieSpikeChart()` function (async PBO readback, pixel classification by Euclidean distance to orange/green references); called after mirror processing in main loop; GPU resource cleanup on thread exit |

### Logic Thread (Detection)
| File | Change |
|------|--------|
| `src/runtime/logic_thread.cpp` | `#include <mmsystem.h>`; `CheckPieSpikeDetection()` function (reads double-buffer, compares ratio ± tolerance, cooldown check, `MessageBeep`/`PlaySoundW`); called from `LogicThreadFunc` |

### Render Thread (Visual Alert)
| File | Change |
|------|--------|
| `src/render/render_thread.h` | `showPieSpikeAlert` + `pieSpikeAlertTimeMs` fields in `FrameRenderRequest` |
| `src/render/render.cpp` | Populates alert fields from atomics alongside other request fields |
| `src/render/render_thread.cpp` | Renders 4 translucent orange (#E96D4D) screen-edge quads with 800ms fade; inserted after welcome toast, before fence creation |

### GUI Tab
| File | Change |
|------|--------|
| `src/gui/tabs/tab_pie_spike.inl` | **New file** — ImGui tab with enable checkbox, ratio/tolerance/threshold sliders, color pickers for orange/green references, timing spinners, alert checkboxes, sound path input, live status (current ratio + detection indicator) |
| `src/gui/gui.cpp` | `#include "tabs/tab_pie_spike.inl"` before `tab_misc.inl` |

### Build System
| File | Change |
|------|--------|
| `CMakeLists.txt` | Added `winmm` to `target_link_libraries` |

## Thread Safety Model

| Data | Writer | Reader | Mechanism |
|------|--------|--------|-----------|
| `PieSpikeConfig` | GUI thread | Mirror/Logic | Config snapshot (`shared_ptr<const Config>`) |
| `PieSpikeAnalysisResult` | Mirror thread | Logic thread | Double-buffered array + atomic index |
| `g_pieSpikeAlertActive` | Logic thread | Render thread | `atomic<bool>` |
| `g_pieSpikeLastOrangeRatio` | Logic thread | GUI thread | `atomic<float>` |
| `g_pieSpikeLastAlertTimeMs` | Logic thread | Render thread | `atomic<int64_t>` |

## Config Defaults
```
enabled = false
orangeRatioTarget = 0.15
tolerance = 0.03
sampleRateMs = 200
cooldownMs = 3000
visualAlert = true
soundAlert = true
captureSize = 160
soundPath = ''
orangeReference = [233, 109, 77]
greenReference = [69, 204, 101]
colorThreshold = 0.15
```

## How It Works
1. **Mirror thread** captures a `captureSize x captureSize` region around the pie chart center (using existing `PIE_X_LEFT=92, PIE_Y_TOP=220` constants)
2. Async PBO readback classifies pixels by Euclidean distance to orange/green reference colors
3. Computes `orangeRatio = orangePixels / (orangePixels + greenPixels)`, requires minimum 10 colored pixels
4. **Logic thread** reads latest result, compares against `target ± tolerance`, applies cooldown
5. On detection: sets `g_pieSpikeAlertActive`, plays sound (`MessageBeep` or custom `.wav`)
6. **Render thread** draws fading orange screen-edge flash over 800ms

## Bugs Fixed During Implementation
- PBO resize bug: `res.texSize` was updated before PBO size check, making PBO resize dead code. Fixed by capturing `sizeChanged` bool before either allocation block.
- Used `BindTextureDirect` instead of raw `glBindTexture` to bypass hook chains (consistent with rest of mirror_thread.cpp).
- **Visual flash not rendering** (fixed 2026-03-12): `g_pieSpikeAlertActive` was cleared to `false` on the next logic tick (16ms) when the ratio no longer matched, killing the flash before the render thread could draw it. Fix: removed the `g_pieSpikeAlertActive.store(false)` in `CheckPieSpikeDetection()` when spike not detected, and removed the `showPieSpikeAlert` boolean gate in `render_thread.cpp` — the time-based fade (`pieSpikeAlertTimeMs` + 800ms) already handles the flash lifecycle.

## Known Limitations / Future Work
- Tab label is hardcoded `"Pie Spike"` — not using `trc()` i18n system. Add translation key when localizing.
- Pie chart center position is hardcoded from `utils.cpp` constants — may need adjustment for different GUI scales.
- No per-mode enable/disable — detection runs globally when enabled.

## Testing Checklist
1. Build with `build.bat` — verified no compile/link errors ✓
2. Launch Minecraft with Toolscreen injected
3. Open settings (Ctrl+I), navigate to Pie Spike tab
4. Enable spike detection, set orange ratio target
5. Enter stronghold, open F3 pie chart, rotate toward portal room
6. Verify live status shows changing orange ratio values
7. Verify visual flash + sound when ratio matches target ± tolerance
8. Verify cooldown prevents alert spam
9. Test custom .wav sound path
