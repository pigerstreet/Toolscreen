#include "mode_dimensions.h"

#include "gui/gui.h"
#include "runtime/logic_thread.h"

#include <cmath>

void RecalculateModeDimensions() {
    int screenW = GetCachedWindowWidth();
    int screenH = GetCachedWindowHeight();
    if (screenW < 1) screenW = 1;
    if (screenH < 1) screenH = 1;

    for (auto& mode : g_config.modes) {
        if (mode.id == "Fullscreen") {
            if (mode.width < 1) mode.width = screenW;
            if (mode.height < 1) mode.height = screenH;

            mode.stretch.enabled = true;
            mode.stretch.x = 0;
            mode.stretch.y = 0;
            mode.stretch.width = screenW;
            mode.stretch.height = screenH;
        }

        if (mode.id == "Preemptive") {
            mode.useRelativeSize = false;
            mode.relativeWidth = -1.0f;
            mode.relativeHeight = -1.0f;
        }

        const bool widthIsRelative = mode.id != "Preemptive" && mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
        const bool heightIsRelative = mode.id != "Preemptive" && mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

        if (widthIsRelative) {
            int newWidth = static_cast<int>(std::lround(mode.relativeWidth * static_cast<float>(screenW)));
            if (newWidth < 1) newWidth = 1;
            mode.width = newWidth;
        }
        if (heightIsRelative) {
            int newHeight = static_cast<int>(std::lround(mode.relativeHeight * static_cast<float>(screenH)));
            if (newHeight < 1) newHeight = 1;
            mode.height = newHeight;
        }

        if (mode.id == "Thin" && mode.width < 330) { mode.width = 330; }
    }

    ModeConfig* eyezoomMode = nullptr;
    ModeConfig* preemptiveMode = nullptr;
    for (auto& mode : g_config.modes) {
        if (!eyezoomMode && mode.id == "EyeZoom") { eyezoomMode = &mode; }
        if (!preemptiveMode && mode.id == "Preemptive") { preemptiveMode = &mode; }
    }
    if (eyezoomMode && preemptiveMode) {
        preemptiveMode->width = eyezoomMode->width;
        preemptiveMode->height = eyezoomMode->height;
        preemptiveMode->manualWidth = (eyezoomMode->manualWidth > 0) ? eyezoomMode->manualWidth : eyezoomMode->width;
        preemptiveMode->manualHeight = (eyezoomMode->manualHeight > 0) ? eyezoomMode->manualHeight : eyezoomMode->height;
        preemptiveMode->useRelativeSize = false;
        preemptiveMode->relativeWidth = -1.0f;
        preemptiveMode->relativeHeight = -1.0f;
    }
}