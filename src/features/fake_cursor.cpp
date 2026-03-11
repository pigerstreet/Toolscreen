#include "fake_cursor.h"
#include "gui/gui.h"
#include "common/utils.h"
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <vector>
#include <windows.h>

void Log(const std::string& msg);

static std::wstring ResolveCwdPath(const std::wstring& relPath) {
    std::filesystem::path cwdPath = std::filesystem::current_path();
    std::filesystem::path fullPath = cwdPath / relPath;
    return fullPath.wstring();
}

namespace CursorTextures {

static void DestroyCursorOrIcon(HCURSOR handle, UINT loadType) {
    if (!handle) return;
    if (loadType == IMAGE_ICON) {
        DestroyIcon(reinterpret_cast<HICON>(handle));
    } else {
        DestroyCursor(handle);
    }
}

struct CursorDef {
    std::string name;
    std::wstring path;
    UINT loadType;
};

static const std::vector<CursorDef> SYSTEM_CURSORS = { { "Arrow", L"C:/Windows/Cursors/aero_arrow.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, small)", L"C:/Windows/Cursors/cross_i.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, medium)", L"C:/Windows/Cursors/cross_im.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, large)", L"C:/Windows/Cursors/cross_il.cur", IMAGE_CURSOR },
                                                       { "Cross (Inverted, no outline)", L"C:/Windows/Cursors/cross_l.cur", IMAGE_CURSOR },
                                                       { "Cross (Small)", L"C:/Windows/Cursors/cross_r.cur", IMAGE_CURSOR },
                                                       { "Cross (Medium)", L"C:/Windows/Cursors/cross_rm.cur", IMAGE_CURSOR },
                                                       { "Cross (Large)", L"C:/Windows/Cursors/cross_rl.cur", IMAGE_CURSOR } };

static std::vector<CursorDef> AVAILABLE_CURSORS;
static bool g_cursorDefsInitialized = false;

void InitializeCursorDefinitions() {
    if (g_cursorDefsInitialized) return;

    LogCategory("cursor_textures", "[CursorTextures] InitializeCursorDefinitions starting...");

    AVAILABLE_CURSORS = SYSTEM_CURSORS;
    LogCategory("cursor_textures", "[CursorTextures] Loaded " + std::to_string(SYSTEM_CURSORS.size()) + " system cursor definitions");

    int validSystemCursors = 0;
    for (const auto& cursor : SYSTEM_CURSORS) {
        if (std::filesystem::exists(cursor.path)) {
            validSystemCursors++;
        } else {
            LogCategory("cursor_textures", "[CursorTextures] WARNING: System cursor not found: " + WideToUtf8(cursor.path));
        }
    }
    LogCategory("cursor_textures", "[CursorTextures] Verified " + std::to_string(validSystemCursors) + "/" +
                                       std::to_string(SYSTEM_CURSORS.size()) + " system cursors exist on disk");

    try {
        std::wstring toolscreenPath = GetToolscreenPath();
        if (toolscreenPath.empty()) {
            LogCategory("cursor_textures", "[CursorTextures] ERROR: Failed to get toolscreen path - custom cursors will not be available");
            g_cursorDefsInitialized = true;
            return;
        }

        std::filesystem::path cursorsPath = std::filesystem::path(toolscreenPath) / "cursors";
        LogCategory("cursor_textures", "[CursorTextures] Scanning for custom cursors at: " + cursorsPath.string());

        if (!std::filesystem::exists(cursorsPath)) {
            LogCategory("cursor_textures", "[CursorTextures] Custom cursors folder does not exist: " + cursorsPath.string());
            LogCategory("cursor_textures", "[CursorTextures] To add custom cursors, create this folder and add .cur or .ico files");
        } else if (!std::filesystem::is_directory(cursorsPath)) {
            LogCategory("cursor_textures", "[CursorTextures] ERROR: Cursors path exists but is not a directory: " + cursorsPath.string());
        } else {
            int customCursorsFound = 0;
            int filesSkipped = 0;
            for (const auto& entry : std::filesystem::directory_iterator(cursorsPath)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (ext == ".cur" || ext == ".ico") {
                        std::string filename = entry.path().filename().stem().string();
                        std::wstring filepath = entry.path().wstring();

                        UINT loadType = (ext == ".ico") ? IMAGE_ICON : IMAGE_CURSOR;

                        AVAILABLE_CURSORS.push_back({ filename, filepath, loadType });
                        LogCategory("cursor_textures", "[CursorTextures] Found custom cursor: " + filename + " (" + ext + ")");
                        customCursorsFound++;
                    } else {
                        filesSkipped++;
                    }
                }
            }
            LogCategory("cursor_textures", "[CursorTextures] Found " + std::to_string(customCursorsFound) + " custom cursor(s), skipped " +
                                               std::to_string(filesSkipped) + " non-cursor file(s)");
        }
    } catch (const std::filesystem::filesystem_error& e) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Filesystem error scanning cursors folder: " + std::string(e.what()));
        LogCategory("cursor_textures", "[CursorTextures] Error code: " + std::to_string(e.code().value()) + " - " + e.code().message());
    } catch (const std::exception& e) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Exception scanning cursors folder: " + std::string(e.what()));
    } catch (...) { LogCategory("cursor_textures", "[CursorTextures] ERROR: Unknown exception scanning cursors folder"); }

    LogCategory("cursor_textures", "[CursorTextures] InitializeCursorDefinitions complete. Total cursors available: " +
                                       std::to_string(AVAILABLE_CURSORS.size()));
    g_cursorDefsInitialized = true;
}

// Global cursor list and mutex
std::vector<CursorData> g_cursorList;
std::mutex g_cursorListMutex;

static const std::vector<int> STANDARD_SIZES = {
    16, 20, 24, 28, 32, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320
};

static bool LoadSingleCursor(const std::wstring& path, UINT loadType, int size, CursorData& outData) {
    if (path.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: LoadSingleCursor called with empty path");
        return false;
    }
    if (size <= 0 || size > 512) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: LoadSingleCursor called with invalid size: " + std::to_string(size));
        return false;
    }

    std::wstring resolvedPath = path;
    try {
        if (!std::filesystem::path(path).is_absolute()) { resolvedPath = ResolveCwdPath(path); }
    } catch (const std::exception& e) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Failed to resolve path: " + std::string(e.what()));
        return false;
    }

    std::string pathStr = WideToUtf8(resolvedPath);
    LogCategory("cursor_textures", "[CursorTextures] Loading cursor: " + pathStr + " at size " + std::to_string(size) +
                                       " (type: " + (loadType == IMAGE_ICON ? "ICON" : "CURSOR") + ")");

    if (!std::filesystem::exists(resolvedPath)) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Cursor file does not exist: " + pathStr);
        return false;
    }

    outData.filePath = path;
    outData.size = size;
    outData.loadType = loadType;

    HCURSOR hCursor = (HCURSOR)LoadImageW(NULL, resolvedPath.c_str(), loadType, size, size, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!hCursor) {
        DWORD err = GetLastError();
        std::string errMsg;
        switch (err) {
        case ERROR_FILE_NOT_FOUND:
            errMsg = "File not found";
            break;
        case ERROR_PATH_NOT_FOUND:
            errMsg = "Path not found";
            break;
        case ERROR_ACCESS_DENIED:
            errMsg = "Access denied";
            break;
        case ERROR_INVALID_PARAMETER:
            errMsg = "Invalid parameter";
            break;
        case ERROR_NOT_ENOUGH_MEMORY:
            errMsg = "Not enough memory";
            break;
        case ERROR_RESOURCE_TYPE_NOT_FOUND:
            errMsg = "Resource type not found (invalid cursor/icon format?)";
            break;
        default:
            errMsg = "Unknown error";
            break;
        }
        LogCategory("cursor_textures",
                    "[CursorTextures] ERROR: LoadImageW failed for '" + pathStr + "' - Error " + std::to_string(err) + ": " + errMsg);
        return false;
    }

    // (Without this, Windows may pick the nearest embedded variant.)
    if (size > 0) {
        HANDLE scaled = CopyImage(reinterpret_cast<HANDLE>(hCursor), loadType, size, size, 0);
        if (scaled) {
            HCURSOR hScaled = reinterpret_cast<HCURSOR>(scaled);
            if (hScaled != hCursor) {
                DestroyCursorOrIcon(hCursor, loadType);
                hCursor = hScaled;
            }
        } else {
            LogCategory("cursor_textures", "[CursorTextures] WARNING: CopyImage failed to force size to " + std::to_string(size) +
                                               "px for " + pathStr + " (err=" + std::to_string(GetLastError()) + ")");
        }
    }

    outData.hCursor = hCursor;

    ICONINFOEXW iconInfoEx = { 0 };
    iconInfoEx.cbSize = sizeof(ICONINFOEXW);
    bool hasIconInfoEx = GetIconInfoExW(hCursor, &iconInfoEx);

    if (!hasIconInfoEx) {
        DWORD err = GetLastError();
        LogCategory("cursor_textures", "[CursorTextures] ERROR: GetIconInfoExW failed with error " + std::to_string(err));
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    BITMAP bmp;
    bool isMonochrome = (iconInfoEx.hbmColor == NULL);
    LogCategory("cursor_textures", "[CursorTextures] Cursor type: " + std::string(isMonochrome ? "monochrome" : "color"));

    if (isMonochrome) {
        if (!iconInfoEx.hbmMask) {
            LogCategory("cursor_textures", "[CursorTextures] ERROR: Monochrome cursor has no mask bitmap");
            DestroyCursorOrIcon(hCursor, loadType);
            outData.hCursor = nullptr;
            return false;
        }
        if (!GetObject(iconInfoEx.hbmMask, sizeof(BITMAP), &bmp)) {
            DWORD err = GetLastError();
            LogCategory("cursor_textures", "[CursorTextures] ERROR: GetObject for mask bitmap failed with error " + std::to_string(err));
            DeleteObject(iconInfoEx.hbmMask);
            DestroyCursorOrIcon(hCursor, loadType);
            outData.hCursor = nullptr;
            return false;
        }
    } else {
        if (!GetObject(iconInfoEx.hbmColor, sizeof(BITMAP), &bmp)) {
            DWORD err = GetLastError();
            LogCategory("cursor_textures", "[CursorTextures] ERROR: GetObject for color bitmap failed with error " + std::to_string(err));
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
            DestroyCursorOrIcon(hCursor, loadType);
            outData.hCursor = nullptr;
            return false;
        }
    }

    int width = bmp.bmWidth;
    int height = isMonochrome ? bmp.bmHeight / 2 : bmp.bmHeight;

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        LogCategory("cursor_textures",
                    "[CursorTextures] ERROR: Invalid bitmap dimensions: " + std::to_string(width) + "x" + std::to_string(height));
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    LogCategory("cursor_textures", "[CursorTextures] Bitmap size: " + std::to_string(width) + "x" + std::to_string(height) +
                                       ", hotspot: (" + std::to_string(iconInfoEx.xHotspot) + ", " + std::to_string(iconInfoEx.yHotspot) +
                                       ")");

    outData.bitmapWidth = width;
    outData.bitmapHeight = height;
    outData.hotspotX = iconInfoEx.xHotspot;
    outData.hotspotY = iconInfoEx.yHotspot;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        DWORD err = GetLastError();
        LogCategory("cursor_textures", "[CursorTextures] ERROR: GetDC(NULL) failed with error " + std::to_string(err));
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        DWORD err = GetLastError();
        LogCategory("cursor_textures", "[CursorTextures] ERROR: CreateCompatibleDC failed with error " + std::to_string(err));
        ReleaseDC(NULL, hdcScreen);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        DestroyCursorOrIcon(hCursor, loadType);
        outData.hCursor = nullptr;
        return false;
    }

    std::vector<unsigned char> pixels(width * height * 4);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (isMonochrome) {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmMask);

        std::vector<unsigned char> maskData(width * bmp.bmHeight * 4);
        BITMAPINFO maskBmi = bmi;
        maskBmi.bmiHeader.biHeight = -bmp.bmHeight;
        GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, bmp.bmHeight, maskData.data(), &maskBmi, DIB_RGB_COLORS);

        std::vector<unsigned char> invertPixels(width * height * 4, 0);
        bool hasInverted = false;

        // Windows cursor mask logic:
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 4;
                int andIdx = (y * width + x) * 4;
                int xorIdx = ((y + height) * width + x) * 4;

                unsigned char andValue = maskData[andIdx];
                unsigned char xorValue = maskData[xorIdx];

                // Windows monochrome cursor mask logic (complete specification):

                bool andBit = (andValue > 128);
                bool xorBit = (xorValue > 128);

                if (andBit && !xorBit) {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 0;
                } else if (!andBit && !xorBit) {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 255;
                } else if (andBit && xorBit) {
                    pixels[idx + 0] = 0;
                    pixels[idx + 1] = 0;
                    pixels[idx + 2] = 0;
                    pixels[idx + 3] = 0;

                    invertPixels[idx + 0] = 255;
                    invertPixels[idx + 1] = 255;
                    invertPixels[idx + 2] = 255;
                    invertPixels[idx + 3] = 255;
                    hasInverted = true;
                } else {
                    pixels[idx + 0] = 255;
                    pixels[idx + 1] = 255;
                    pixels[idx + 2] = 255;
                    pixels[idx + 3] = 255;
                }
            }
        }

        outData.hasInvertedPixels = hasInverted;

        if (hasInverted) {
            while (glGetError() != GL_NO_ERROR) {}

            glGenTextures(1, &outData.invertMaskTexture);
            if (outData.invertMaskTexture == 0) {
                LogCategory("cursor_textures", "[CursorTextures] WARNING: Failed to create invert mask texture - glGenTextures returned 0");
                outData.hasInvertedPixels = false;
            } else {
                BindTextureDirect(GL_TEXTURE_2D, outData.invertMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, invertPixels.data());

                GLenum glErr = glGetError();
                if (glErr != GL_NO_ERROR) {
                    LogCategory("cursor_textures",
                                "[CursorTextures] WARNING: OpenGL error creating invert mask texture: " + std::to_string(glErr));
                    glDeleteTextures(1, &outData.invertMaskTexture);
                    outData.invertMaskTexture = 0;
                    outData.hasInvertedPixels = false;
                } else {
                    LogCategory("cursor_textures",
                                "[CursorTextures] Created invert mask texture ID " + std::to_string(outData.invertMaskTexture));
                }
                BindTextureDirect(GL_TEXTURE_2D, 0);
            }
        }

        SelectObject(hdcMem, hbmOld);
    } else {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmColor);
        GetDIBits(hdcMem, iconInfoEx.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        if (bmp.bmBitsPixel == 32) {
            for (int i = 0; i < width * height; ++i) {
                if (pixels[i * 4 + 3] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        if (!hasAlpha && iconInfoEx.hbmMask) {
            std::vector<unsigned char> maskPixels(width * height * 4);
            SelectObject(hdcMem, iconInfoEx.hbmMask);

            BITMAPINFO maskBmi = { 0 };
            maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            maskBmi.bmiHeader.biWidth = width;
            maskBmi.bmiHeader.biHeight = -height;
            maskBmi.bmiHeader.biPlanes = 1;
            maskBmi.bmiHeader.biBitCount = 32;
            maskBmi.bmiHeader.biCompression = BI_RGB;

            GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);

            for (int i = 0; i < width * height; ++i) {
                unsigned char maskValue = maskPixels[i * 4];
                pixels[i * 4 + 3] = 255 - maskValue;
            }
        } else if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255; }
        }

        SelectObject(hdcMem, hbmOld);
    }
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
    if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);

    while (glGetError() != GL_NO_ERROR) {}

    glGenTextures(1, &outData.texture);
    if (outData.texture == 0) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: glGenTextures returned 0 - OpenGL context may not be valid");
        DestroyCursorOrIcon(outData.hCursor, outData.loadType);
        outData.hCursor = nullptr;
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, outData.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::string errStr;
        switch (err) {
        case GL_INVALID_ENUM:
            errStr = "GL_INVALID_ENUM";
            break;
        case GL_INVALID_VALUE:
            errStr = "GL_INVALID_VALUE";
            break;
        case GL_INVALID_OPERATION:
            errStr = "GL_INVALID_OPERATION";
            break;
        case GL_OUT_OF_MEMORY:
            errStr = "GL_OUT_OF_MEMORY";
            break;
        default:
            errStr = "Unknown (" + std::to_string(err) + ")";
            break;
        }
        LogCategory("cursor_textures", "[CursorTextures] ERROR: OpenGL error during texture creation: " + errStr);
        glDeleteTextures(1, &outData.texture);
        outData.texture = 0;
        if (outData.invertMaskTexture) {
            glDeleteTextures(1, &outData.invertMaskTexture);
            outData.invertMaskTexture = 0;
        }
        DestroyCursorOrIcon(outData.hCursor, outData.loadType);
        outData.hCursor = nullptr;
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, 0);

    LogCategory("cursor_textures", "[CursorTextures] Successfully created texture ID " + std::to_string(outData.texture) + " (" +
                                       std::to_string(width) + "x" + std::to_string(height) + ") for " + WideToUtf8(path));
    return true;
}

void LoadCursorTextures() {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    InitializeCursorDefinitions();

    LogCategory("cursor_textures", "[CursorTextures] LoadCursorTextures called - loading initial cursors at default size (64px)");

    int totalLoaded = 0;
    const int defaultSize = 64;

    for (const auto& cursorDef : AVAILABLE_CURSORS) {
        CursorData cursorData;
        if (LoadSingleCursor(cursorDef.path, cursorDef.loadType, defaultSize, cursorData)) {
            g_cursorList.push_back(cursorData);
            LogCategory("cursor_textures",
                        "[CursorTextures] Loaded " + WideToUtf8(cursorDef.path) + " at size " + std::to_string(defaultSize));
            totalLoaded++;
        } else {
            LogCategory("cursor_textures",
                        "[CursorTextures] Failed to load " + WideToUtf8(cursorDef.path) + " at size " + std::to_string(defaultSize));
        }
    }

    LogCategory("cursor_textures", "[CursorTextures] Finished loading " + std::to_string(totalLoaded) + " default cursor variants");
}

// NOTE: Caller must NOT hold g_cursorListMutex when calling this function
const CursorData* LoadOrFindCursor(const std::wstring& path, UINT loadType, int size) {
    std::string pathStr = WideToUtf8(path);

    if (path.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: LoadOrFindCursor called with empty path");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == path && cursor.size == size) {
                return &cursor;
            }
        }
    }

    LogCategory("cursor_textures", "[CursorTextures] Loading cursor on-demand: " + pathStr + " at size " + std::to_string(size));
    CursorData newCursorData;
    if (LoadSingleCursor(path, loadType, size, newCursorData)) {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        g_cursorList.push_back(newCursorData);
        LogCategory("cursor_textures",
                    "[CursorTextures] Successfully loaded on-demand cursor. Total loaded: " + std::to_string(g_cursorList.size()));
        return &g_cursorList.back();
    } else {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: Failed to load cursor on-demand: " + pathStr);
        return nullptr;
    }
}

const CursorData* FindCursor(const std::wstring& path, int size) {
    if (path.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] ERROR: FindCursor called with empty path");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.filePath == path && cursor.size == size) { return &cursor; }
        }
    }

    UINT loadType = IMAGE_CURSOR;
    try {
        std::filesystem::path fsPath(path);
        std::string ext = fsPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".ico") {
            loadType = IMAGE_ICON;
        } else if (ext != ".cur" && ext != ".ani") {
            LogCategory("cursor_textures", "[CursorTextures] WARNING: Unexpected cursor file extension: " + ext + ", treating as cursor");
        }
    } catch (const std::exception& e) {
        LogCategory("cursor_textures",
                    "[CursorTextures] WARNING: Failed to parse path extension: " + std::string(e.what()) + ", defaulting to IMAGE_CURSOR");
    }

    return LoadOrFindCursor(path, loadType, size);
}

const CursorData* FindCursorByHandle(HCURSOR hCursor) {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    for (const auto& cursor : g_cursorList) {
        if (cursor.hCursor == hCursor) { return &cursor; }
    }
    return nullptr;
}

// Does NOT take ownership of hCursor - caller keeps it
static bool CreateTextureFromHandle(HCURSOR hCursor, CursorData& outData) {
    if (!hCursor) { return false; }

    ICONINFOEXW iconInfoEx = { 0 };
    iconInfoEx.cbSize = sizeof(ICONINFOEXW);
    if (!GetIconInfoExW(hCursor, &iconInfoEx)) { return false; }

    BITMAP bmp;
    bool isMonochrome = (iconInfoEx.hbmColor == NULL);

    if (isMonochrome) {
        if (!iconInfoEx.hbmMask || !GetObject(iconInfoEx.hbmMask, sizeof(BITMAP), &bmp)) {
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            return false;
        }
    } else {
        if (!GetObject(iconInfoEx.hbmColor, sizeof(BITMAP), &bmp)) {
            if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
            if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
            return false;
        }
    }

    int width = bmp.bmWidth;
    int height = isMonochrome ? bmp.bmHeight / 2 : bmp.bmHeight;

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    outData.hCursor = hCursor;
    outData.filePath = L"<system>";
    outData.size = 0;
    outData.loadType = IMAGE_CURSOR;
    outData.bitmapWidth = width;
    outData.bitmapHeight = height;
    outData.hotspotX = iconInfoEx.xHotspot;
    outData.hotspotY = iconInfoEx.yHotspot;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) {
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(NULL, hdcScreen);
        if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);
        if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
        return false;
    }

    std::vector<unsigned char> pixels(width * height * 4);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (isMonochrome) {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmMask);
        std::vector<unsigned char> maskData(width * bmp.bmHeight * 4);
        BITMAPINFO maskBmi = bmi;
        maskBmi.bmiHeader.biHeight = -bmp.bmHeight;
        GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, bmp.bmHeight, maskData.data(), &maskBmi, DIB_RGB_COLORS);

        std::vector<unsigned char> invertPixels(width * height * 4, 0);
        bool hasInverted = false;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int idx = (y * width + x) * 4;
                int andIdx = (y * width + x) * 4;
                int xorIdx = ((y + height) * width + x) * 4;
                unsigned char andValue = maskData[andIdx];
                unsigned char xorValue = maskData[xorIdx];
                bool andBit = (andValue > 128);
                bool xorBit = (xorValue > 128);

                if (andBit && !xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 0;
                } else if (!andBit && !xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = 0;
                    pixels[idx + 3] = 255;
                } else if (andBit && xorBit) {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 0;
                    invertPixels[idx + 0] = invertPixels[idx + 1] = invertPixels[idx + 2] = invertPixels[idx + 3] = 255;
                    hasInverted = true;
                } else {
                    pixels[idx + 0] = pixels[idx + 1] = pixels[idx + 2] = pixels[idx + 3] = 255;
                }
            }
        }

        outData.hasInvertedPixels = hasInverted;
        if (hasInverted) {
            while (glGetError() != GL_NO_ERROR) {}
            glGenTextures(1, &outData.invertMaskTexture);
            if (outData.invertMaskTexture != 0) {
                BindTextureDirect(GL_TEXTURE_2D, outData.invertMaskTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, invertPixels.data());
                BindTextureDirect(GL_TEXTURE_2D, 0);
            }
        }
        SelectObject(hdcMem, hbmOld);
    } else {
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, iconInfoEx.hbmColor);
        GetDIBits(hdcMem, iconInfoEx.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS);

        bool hasAlpha = false;
        if (bmp.bmBitsPixel == 32) {
            for (int i = 0; i < width * height; ++i) {
                if (pixels[i * 4 + 3] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }

        if (!hasAlpha && iconInfoEx.hbmMask) {
            std::vector<unsigned char> maskPixels(width * height * 4);
            SelectObject(hdcMem, iconInfoEx.hbmMask);
            BITMAPINFO maskBmi = { 0 };
            maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            maskBmi.bmiHeader.biWidth = width;
            maskBmi.bmiHeader.biHeight = -height;
            maskBmi.bmiHeader.biPlanes = 1;
            maskBmi.bmiHeader.biBitCount = 32;
            maskBmi.bmiHeader.biCompression = BI_RGB;
            GetDIBits(hdcMem, iconInfoEx.hbmMask, 0, height, maskPixels.data(), &maskBmi, DIB_RGB_COLORS);
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255 - maskPixels[i * 4]; }
        } else if (!hasAlpha) {
            for (int i = 0; i < width * height; ++i) { pixels[i * 4 + 3] = 255; }
        }
        SelectObject(hdcMem, hbmOld);
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (iconInfoEx.hbmColor) DeleteObject(iconInfoEx.hbmColor);
    if (iconInfoEx.hbmMask) DeleteObject(iconInfoEx.hbmMask);

    while (glGetError() != GL_NO_ERROR) {}
    glGenTextures(1, &outData.texture);
    if (outData.texture == 0) { return false; }

    BindTextureDirect(GL_TEXTURE_2D, outData.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, pixels.data());

    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &outData.texture);
        outData.texture = 0;
        if (outData.invertMaskTexture) {
            glDeleteTextures(1, &outData.invertMaskTexture);
            outData.invertMaskTexture = 0;
        }
        return false;
    }

    BindTextureDirect(GL_TEXTURE_2D, 0);
    return true;
}

const CursorData* LoadOrFindCursorFromHandle(HCURSOR hCursor) {
    if (!hCursor) { return nullptr; }

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.hCursor == hCursor) { return &cursor; }
        }
    }

    CursorData newData;
    if (!CreateTextureFromHandle(hCursor, newData)) { return nullptr; }

    std::lock_guard<std::mutex> lock(g_cursorListMutex);
    g_cursorList.push_back(newData);
    return &g_cursorList.back();
}

const CursorData* GetSelectedCursor(const std::string& gameState, int size) {
    // Get cursor name and size for the current game state from config snapshot (thread-safe)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return nullptr;

    std::string selectedCursorName = "";
    int selectedSize = size;

    if (!cfgSnap->cursors.enabled) {
        return nullptr;
    }

    if (gameState == "title") {
        selectedCursorName = cfgSnap->cursors.title.cursorName;
        selectedSize = cfgSnap->cursors.title.cursorSize;
    } else if (gameState == "wall") {
        selectedCursorName = cfgSnap->cursors.wall.cursorName;
        selectedSize = cfgSnap->cursors.wall.cursorSize;
    } else {
        selectedCursorName = cfgSnap->cursors.ingame.cursorName;
        selectedSize = cfgSnap->cursors.ingame.cursorSize;
    }

    std::wstring cursorPath;
    UINT loadType = IMAGE_CURSOR;
    GetCursorPathByName(selectedCursorName, cursorPath, loadType);

    const CursorData* cursorData = FindCursor(cursorPath, selectedSize);
    if (cursorData) { return cursorData; }

    Log("[GetSelectedCursor] Cursor '" + selectedCursorName + "' not found at size " + std::to_string(selectedSize) + ", trying fallback");

    {
        std::lock_guard<std::mutex> lock(g_cursorListMutex);
        for (const auto& cursor : g_cursorList) {
            if (cursor.size == selectedSize && cursor.texture != 0) {
                Log("[GetSelectedCursor] Fallback: using cursor from " + WideToUtf8(cursor.filePath));
                return &cursor;
            }
        }
        for (const auto& cursor : g_cursorList) {
            if (cursor.texture != 0) {
                Log("[GetSelectedCursor] Fallback: using cursor from " + WideToUtf8(cursor.filePath) + " at size " +
                    std::to_string(cursor.size));
                return &cursor;
            }
        }
    }

    Log("[GetSelectedCursor] No fallback cursor available, rendering nothing");
    return nullptr;
}

bool GetCursorPathByName(const std::string& cursorName, std::wstring& outPath, UINT& outLoadType) {
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    const CursorDef* selectedDef = nullptr;
    for (const auto& def : AVAILABLE_CURSORS) {
        if (def.name == cursorName) {
            selectedDef = &def;
            break;
        }
    }

    if (selectedDef) {
        outPath = selectedDef->path;
        outLoadType = selectedDef->loadType;
        return true;
    } else {
        LogCategory("cursor_textures", "[CursorTextures] WARNING: Unknown cursor name '" + cursorName + "'");
        LogCategory("cursor_textures", "[CursorTextures] Available cursors: " + std::to_string(AVAILABLE_CURSORS.size()));
        for (const auto& def : AVAILABLE_CURSORS) { LogCategory("cursor_textures", "[CursorTextures]   - " + def.name); }

        if (!AVAILABLE_CURSORS.empty()) {
            outPath = AVAILABLE_CURSORS[0].path;
            outLoadType = AVAILABLE_CURSORS[0].loadType;
            LogCategory("cursor_textures", "[CursorTextures] Using first available cursor as fallback: " + AVAILABLE_CURSORS[0].name);
            return false;
        }

        outPath = L"";
        outLoadType = IMAGE_CURSOR;
        LogCategory("cursor_textures", "[CursorTextures] ERROR: No cursors available for fallback");
        return false;
    }
}

bool IsCursorFileValid(const std::string& cursorName) {
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    if (cursorName.empty()) {
        LogCategory("cursor_textures", "[CursorTextures] IsCursorFileValid: Empty cursor name provided");
        return false;
    }

    const CursorDef* selectedDef = nullptr;
    for (const auto& def : AVAILABLE_CURSORS) {
        if (def.name == cursorName) {
            selectedDef = &def;
            break;
        }
    }

    if (!selectedDef) {
        LogCategory("cursor_textures", "[CursorTextures] IsCursorFileValid: Cursor '" + cursorName + "' not found in definitions");
        return false;
    }

    std::wstring resolvedPath = selectedDef->path;
    try {
        if (!std::filesystem::path(selectedDef->path).is_absolute()) { resolvedPath = ResolveCwdPath(selectedDef->path); }
    } catch (const std::exception& e) {
        LogCategory("cursor_textures",
                    "[CursorTextures] IsCursorFileValid: Failed to resolve path for '" + cursorName + "': " + std::string(e.what()));
        return false;
    }

    bool exists = std::filesystem::exists(resolvedPath);
    if (!exists) {
        LogCategory("cursor_textures", "[CursorTextures] IsCursorFileValid: Cursor file does not exist: " + WideToUtf8(resolvedPath));
    }
    return exists;
}

void Cleanup() {
    std::lock_guard<std::mutex> lock(g_cursorListMutex);

    LogCategory("cursor_textures",
                "[CursorTextures] Cleanup: Starting cleanup of " + std::to_string(g_cursorList.size()) + " cursor entries");

    int texturesDeleted = 0;
    int invertMasksDeleted = 0;
    int cursorsDestroyed = 0;

    for (auto& cursor : g_cursorList) {
        if (cursor.texture) {
            glDeleteTextures(1, &cursor.texture);
            cursor.texture = 0;
            texturesDeleted++;
        }
        if (cursor.invertMaskTexture) {
            glDeleteTextures(1, &cursor.invertMaskTexture);
            cursor.invertMaskTexture = 0;
            invertMasksDeleted++;
        }
        if (cursor.hCursor) {
            DestroyCursorOrIcon(cursor.hCursor, cursor.loadType);
            cursor.hCursor = nullptr;
            cursorsDestroyed++;
        }
    }

    g_cursorList.clear();
    LogCategory("cursor_textures", "[CursorTextures] Cleanup complete: " + std::to_string(texturesDeleted) + " textures, " +
                                       std::to_string(invertMasksDeleted) + " invert masks, " + std::to_string(cursorsDestroyed) +
                                       " cursor handles");
}

std::vector<std::string> GetAvailableCursorNames() {
    if (!g_cursorDefsInitialized) { InitializeCursorDefinitions(); }

    std::vector<std::string> names;
    for (const auto& cursor : AVAILABLE_CURSORS) { names.push_back(cursor.name); }
    return names;
}

}

static int s_fakeCursorLogCounter = 0;
static const int FAKE_CURSOR_LOG_INTERVAL = 300;

void RenderFakeCursor(HWND hwnd, int windowWidth, int windowHeight) {
    s_fakeCursorLogCounter++;
    bool shouldLog = (s_fakeCursorLogCounter % FAKE_CURSOR_LOG_INTERVAL == 0);

    CURSORINFO cursorInfo = { 0 };
    cursorInfo.cbSize = sizeof(CURSORINFO);
    if (!GetCursorInfo(&cursorInfo)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] GetCursorInfo failed with error " + std::to_string(err));
        }
        return;
    }
    if (!cursorInfo.hCursor) { return; }
    if (!(cursorInfo.flags & CURSOR_SHOWING)) { return; }

    const CursorTextures::CursorData* cursorData = CursorTextures::FindCursorByHandle(cursorInfo.hCursor);

    if (!cursorData) {
        if (shouldLog) {
            Log("[FakeCursor] Cursor handle 0x" + std::to_string(reinterpret_cast<uintptr_t>(cursorInfo.hCursor)) +
                " not found in loaded cursors (may be a system cursor)");
        }
        return;
    }

    POINT cursorPos;
    if (!GetCursorPos(&cursorPos)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] GetCursorPos failed with error " + std::to_string(err));
        }
        return;
    }

    if (!ScreenToClient(hwnd, &cursorPos)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] ScreenToClient failed with error " + std::to_string(err));
        }
        return;
    }

    RECT gameRect;
    if (!GetClientRect(hwnd, &gameRect)) {
        if (shouldLog) {
            DWORD err = GetLastError();
            Log("[FakeCursor] GetClientRect failed with error " + std::to_string(err));
        }
        return;
    }
    int gameWidth = gameRect.right - gameRect.left;
    int gameHeight = gameRect.bottom - gameRect.top;

    if (gameWidth == 0 || gameHeight == 0) { return; }

    // The bitmap is already at the user's desired cursor size (includes Windows cursor scaling)

    float offset = cursorData->loadType == IMAGE_CURSOR ? 1.5f : 1.0f;

    int systemCursorWidth = cursorData->bitmapWidth;
    int systemCursorHeight = cursorData->bitmapHeight;
    int scaledCursorWidth = (systemCursorWidth * windowWidth) / gameWidth;
    int scaledCursorHeight = (systemCursorHeight * windowHeight) / gameHeight;

    int scaledHotspotX = static_cast<int>((cursorData->hotspotX * scaledCursorWidth * offset) / systemCursorWidth);
    int scaledHotspotY = static_cast<int>((cursorData->hotspotY * scaledCursorHeight * offset) / systemCursorHeight);

    int renderWidth = static_cast<int>(scaledCursorWidth * offset);
    int renderHeight = static_cast<int>(scaledCursorHeight * offset);

    int cursorX = cursorPos.x - scaledHotspotX;
    int cursorY = cursorPos.y - scaledHotspotY;

    auto RenderCursorQuad = [&](int x, int y) {
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex2i(x, y);
        glTexCoord2f(1, 0);
        glVertex2i(x + renderWidth, y);
        glTexCoord2f(1, 1);
        glVertex2i(x + renderWidth, y + renderHeight);
        glTexCoord2f(0, 1);
        glVertex2i(x, y + renderHeight);
        glEnd();
    };

    if (renderWidth > 0 && renderHeight > 0 && renderWidth < 512 && renderHeight < 512) {
        GLboolean oldBlend = glIsEnabled(GL_BLEND);
        GLboolean oldDepth = glIsEnabled(GL_DEPTH_TEST);
        GLboolean oldTexture2D = glIsEnabled(GL_TEXTURE_2D);
        GLboolean oldScissor = glIsEnabled(GL_SCISSOR_TEST);
        GLboolean oldCullFace = glIsEnabled(GL_CULL_FACE);
        GLint oldBlendSrc, oldBlendDst;
        glGetIntegerv(GL_BLEND_SRC, &oldBlendSrc);
        glGetIntegerv(GL_BLEND_DST, &oldBlendDst);

        GLint oldProgram;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glDisable(GL_SCISSOR_TEST);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(0);

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, windowWidth, windowHeight, 0, -1, 1);

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glEnable(GL_TEXTURE_2D);
        BindTextureDirect(GL_TEXTURE_2D, cursorData->texture);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        RenderCursorQuad(cursorX, cursorY);

        if (cursorData->hasInvertedPixels && cursorData->invertMaskTexture != 0) {
            BindTextureDirect(GL_TEXTURE_2D, cursorData->invertMaskTexture);

            glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);

            RenderCursorQuad(cursorX, cursorY);
        }

        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);

        if (!oldTexture2D) glDisable(GL_TEXTURE_2D);
        if (!oldBlend) glDisable(GL_BLEND);
        if (oldDepth) glEnable(GL_DEPTH_TEST);
        if (oldScissor) glEnable(GL_SCISSOR_TEST);
        if (oldCullFace) glEnable(GL_CULL_FACE);
        glBlendFunc(oldBlendSrc, oldBlendDst);
        glUseProgram(oldProgram);

        glFlush();
    }
}



