#pragma once

#ifndef GLEW_STATIC
#define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace CursorTextures {
struct CursorData {
    HCURSOR hCursor = nullptr;    // Windows cursor handle
    int size = 0;
    std::wstring filePath;
    GLuint texture = 0;
    GLuint invertMaskTexture = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    int bitmapWidth = 32;
    int bitmapHeight = 32;
    bool hasInvertedPixels = false;
    UINT loadType = IMAGE_CURSOR;
};

extern std::vector<CursorData> g_cursorList;
extern std::mutex g_cursorListMutex;

void LoadCursorTextures();

const CursorData* LoadOrFindCursor(const std::wstring& path, UINT loadType, int size);

const CursorData* FindCursor(const std::wstring& path, int size);

const CursorData* FindCursorByHandle(HCURSOR hCursor);

const CursorData* LoadOrFindCursorFromHandle(HCURSOR hCursor);

void Cleanup();

const CursorData* GetSelectedCursor(const std::string& gameState, int size = 64);

bool GetCursorPathByName(const std::string& cursorName, std::wstring& outPath, UINT& outLoadType);

bool IsCursorFileValid(const std::string& cursorName);

void InitializeCursorDefinitions();

std::vector<std::string> GetAvailableCursorNames();
}

void RenderFakeCursor(HWND hwnd, int windowWidth, int windowHeight);


