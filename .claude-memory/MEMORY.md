# Toolscreen Project Memory

## Project Overview
- **Toolscreen** — screen mirroring & overlay tool for Minecraft Java Edition speedrunning
- Windows-only C++20 DLL injected into Minecraft via EasyInjectBundled (jar/exe installer)
- Built with CMake, uses MSVC, MinHook, GLEW, ImGui (v1.92.6), nlohmann_json
- Repo: `C:/Users/FSOS/Documents/Programs/toolscreen/Toolscreen/`
- Author: jojoe77777 (GitHub)
- Legal for speedrun.com and MCSR Ranked

## Architecture
- See [architecture.md](architecture.md) for detailed breakdown
- See [preemptive-nav.md](preemptive-nav.md) for Minecraft preemptive navigation guide

## Key Directories
- `src/bootstrap/` — DLL entry point (dllmain.cpp)
- `src/common/` — utils, i18n, expression parser, profiler, version
- `src/config/` — TOML config system (config_defaults.h, config_toml.cpp/h, default.toml)
- `src/features/` — fake_cursor, virtual_camera, window_overlay
- `src/gui/` — ImGui-based GUI (gui.h, gui_internal.h, tabs/*.inl)
- `src/hooks/` — hook_chain (MinHook detours), input_hook (WndProc subclass)
- `src/render/` — OpenGL rendering, mirror capture, OBS integration
- `src/runtime/` — logic_thread (mode transitions, animations)
- `src/third_party/` — pl_mpeg.h, stb_image.h

## Build
- `build.bat` builds everything; output in `out/build/bin/Release/`
- Produces Toolscreen.dll + jar/exe installers

## Threading Model
- Main/GUI thread: ImGui rendering, config editing
- Render thread: OpenGL presentation (wglSwapBuffers hook)
- Logic thread: mode transitions, screen metrics, animation
- Mirror capture thread: screenshot + color filtering
- Window overlay capture thread
- OBS virtual camera thread
- Lock-free double-buffering for cross-thread data (atomic snapshots)

## Key Features
- **Modes** — viewport presets (Fullscreen, Thin, Wide, EyeZoom, custom) with expression-based sizing
- **Mirrors** — screen region capture with color filtering (e.g., mapless boat)
- **EyeZoom** — vertical pixel slice magnification for ender eye readings (24x clone, 810px stretch)
- **Image overlays** — display images over game (e.g., Ninjabrain Bot)
- **Window overlays** — capture other windows into game (triple-buffered)
- **Hotkeys** — mode switching with conditions, debounce, hold/release triggers
- **Key rebinding** — remap keys with custom input behavior
- **Virtual camera** — output EyeZoom to OBS (RGBA→NV12, 60fps)
- **Fake cursor** — custom cursor per game state

## Config System
- TOML format (default.toml as template)
- Runtime Config struct modified by GUI
- Thread-safe via atomic config snapshots (PublishConfigSnapshot/GetConfigSnapshot)
- Ctrl+I opens settings in fullscreen

## Hook Chain
- Three-level: export hook → driver hook → third-party hook
- Hooks: wglSwapBuffers, glViewport, SetCursorPos, ClipCursor, GetRawInputData, glfwSetInputMode
- Input: WndProc subclass with sequential handler chain

## Changes Made
- **Pie Spike Detector** — see [pie-spike.md](pie-spike.md) for full details

## Build Environment
- Requires VS 2022 v143 toolset (user has VS 2025/v18 with v143 component installed)
- CMake only available via Developer Command Prompt, not in default PATH
- Build: open Developer Command Prompt for VS → `cd C:\Users\FSOS\Documents\Programs\toolscreen\Toolscreen` → `build.bat release`
- **From bash/Claude Code**: VsDevCmd.bat doesn't propagate env vars to bash. Use cmake directly:
  ```
  export PATH="/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:$PATH"
  cd "C:\Users\FSOS\Documents\Programs\toolscreen\Toolscreen"
  cmake --preset vs2022-x64
  cmake --build --preset release --target Toolscreen
  ```
- VS is at `C:\Program Files\Microsoft Visual Studio\18\` (VS 2025/2026)

## Git Config
- Git identity set locally (not global): name=`pigerstreet`, email=`jj123jonathan@gmail.com`
- GitHub repo: `https://github.com/pigerstreet/Toolscreen.git`
- `.gitignore` uses whitelist pattern (`*` then `!` exceptions)
- `out/build/bin/Release/*` is whitelisted for build artifacts
- Toolscreen.pdb is ~55MB (GitHub warns but accepts)
