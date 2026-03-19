#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "gui/gui.h"
#include <GL/glew.h>
#include <string>
#include <vector>

struct BrowserOverlayTextureFrame {
    GLuint textureId = 0;
    int textureWidth = 0;
    int textureHeight = 0;
};

void StartBrowserOverlayThread();
void StopBrowserOverlayThread();
void CleanupBrowserOverlayCache();
void RemoveBrowserOverlayFromCache(const std::string& overlayId);
void RequestBrowserOverlayRefresh(const std::string& overlayId);
const BrowserOverlayConfig* FindBrowserOverlayConfig(const std::string& overlayId);
const BrowserOverlayConfig* FindBrowserOverlayConfigIn(const std::string& overlayId, const Config& config);
bool PrepareBrowserOverlayTexture(const BrowserOverlayConfig& config, BrowserOverlayTextureFrame& outFrame);
