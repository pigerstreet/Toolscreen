#include "hook_chain.h"

#include "MinHook.h"
#include "common/utils.h"

#include <DbgHelp.h>
#include <Psapi.h>
#include <winver.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "Version.lib")


extern Config g_config;

typedef BOOL(WINAPI* WGLSWAPBUFFERS)(HDC);
extern WGLSWAPBUFFERS owglSwapBuffers;
extern WGLSWAPBUFFERS g_owglSwapBuffersThirdParty;
extern std::atomic<void*> g_wglSwapBuffersThirdPartyHookTarget;
extern BOOL WINAPI hkwglSwapBuffers(HDC hDc);
extern BOOL WINAPI hkwglSwapBuffers_ThirdParty(HDC hDc);

// glViewport (export + driver + third-party)
typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
extern GLVIEWPORTPROC oglViewport;
extern GLVIEWPORTPROC g_oglViewportDriver;
extern GLVIEWPORTPROC g_oglViewportThirdParty;
extern std::atomic<void*> g_glViewportDriverHookTarget;
extern std::atomic<void*> g_glViewportThirdPartyHookTarget;
extern std::atomic<int> g_glViewportHookCount;
extern void WINAPI hkglViewport(GLint x, GLint y, GLsizei width, GLsizei height);
extern void WINAPI hkglViewport_Driver(GLint x, GLint y, GLsizei width, GLsizei height);
extern void WINAPI hkglViewport_ThirdParty(GLint x, GLint y, GLsizei width, GLsizei height);

typedef BOOL(WINAPI* SETCURSORPOSPROC)(int, int);
extern SETCURSORPOSPROC oSetCursorPos;
extern SETCURSORPOSPROC g_oSetCursorPosThirdParty;
extern std::atomic<void*> g_setCursorPosThirdPartyHookTarget;
extern BOOL WINAPI hkSetCursorPos(int X, int Y);
extern BOOL WINAPI hkSetCursorPos_ThirdParty(int X, int Y);

typedef BOOL(WINAPI* CLIPCURSORPROC)(const RECT*);
extern CLIPCURSORPROC oClipCursor;
extern CLIPCURSORPROC g_oClipCursorThirdParty;
extern std::atomic<void*> g_clipCursorThirdPartyHookTarget;
extern BOOL WINAPI hkClipCursor(const RECT* lpRect);
extern BOOL WINAPI hkClipCursor_ThirdParty(const RECT* lpRect);

typedef HCURSOR(WINAPI* SETCURSORPROC)(HCURSOR);
extern SETCURSORPROC oSetCursor;
extern SETCURSORPROC g_oSetCursorThirdParty;
extern std::atomic<void*> g_setCursorThirdPartyHookTarget;
extern HCURSOR WINAPI hkSetCursor(HCURSOR hCursor);
extern HCURSOR WINAPI hkSetCursor_ThirdParty(HCURSOR hCursor);

typedef UINT(WINAPI* GETRAWINPUTDATAPROC)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
extern GETRAWINPUTDATAPROC oGetRawInputData;
extern GETRAWINPUTDATAPROC g_oGetRawInputDataThirdParty;
extern std::atomic<void*> g_getRawInputDataThirdPartyHookTarget;
extern UINT WINAPI hkGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
extern UINT WINAPI hkGetRawInputData_ThirdParty(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);

typedef void (*GLFWSETINPUTMODE)(void* window, int mode, int value);
extern GLFWSETINPUTMODE oglfwSetInputMode;
extern GLFWSETINPUTMODE g_oglfwSetInputModeThirdParty;
extern std::atomic<void*> g_glfwSetInputModeThirdPartyHookTarget;
extern void hkglfwSetInputMode(void* window, int mode, int value);
extern void hkglfwSetInputMode_ThirdParty(void* window, int mode, int value);


namespace {

struct HookChainOwnerInfo {
    std::wstring path;
    std::wstring name;
    std::wstring company;
    std::wstring product;
    std::wstring description;
    uintptr_t base = 0;
    size_t size = 0;
};

static std::string PtrToHex(const void* p) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::uppercase << (uintptr_t)p;
    return ss.str();
}

static bool IsReadableCodePtr(const void* addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY ||
           prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY;
}

static bool IsAbsoluteJumpStub(const uint8_t* bytes) {
    if (!bytes) return false;
    return (bytes[0] == 0xEB) || (bytes[0] == 0xE9) || (bytes[0] == 0xFF && bytes[1] == 0x25) ||
           (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) ||
           (bytes[0] == 0x49 && bytes[1] == 0xBB && bytes[10] == 0x41 && bytes[11] == 0xFF && bytes[12] == 0xE3);
}

static std::wstring ToLowerWide(std::wstring value) {
    for (wchar_t& ch : value) ch = static_cast<wchar_t>(towlower(ch));
    return value;
}

static bool ContainsAnySubstring(const std::wstring& haystack, std::initializer_list<const wchar_t*> needles) {
    for (const wchar_t* needle : needles) {
        if (needle && haystack.find(needle) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

static std::wstring GetFileVersionStringValue(const std::wstring& filePath, const wchar_t* key) {
    if (filePath.empty() || !key) return L"";

    DWORD handle = 0;
    DWORD sz = GetFileVersionInfoSizeW(filePath.c_str(), &handle);
    if (sz == 0) return L"";

    std::vector<uint8_t> data(sz);
    if (!GetFileVersionInfoW(filePath.c_str(), 0, sz, data.data())) return L"";

    struct LANGANDCODEPAGE {
        WORD wLanguage;
        WORD wCodePage;
    };

    LANGANDCODEPAGE* lpTranslate = nullptr;
    UINT cbTranslate = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) && lpTranslate &&
        cbTranslate >= sizeof(LANGANDCODEPAGE)) {
        wchar_t subBlock[256];
        swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage, key);
        LPVOID buf = nullptr;
        UINT bufLen = 0;
        if (VerQueryValueW(data.data(), subBlock, &buf, &bufLen) && buf && bufLen > 0) {
            return std::wstring(reinterpret_cast<const wchar_t*>(buf));
        }
    }

    {
        wchar_t subBlock[256];
        swprintf_s(subBlock, L"\\StringFileInfo\\040904B0\\%s", key);
        LPVOID buf = nullptr;
        UINT bufLen = 0;
        if (VerQueryValueW(data.data(), subBlock, &buf, &bufLen) && buf && bufLen > 0) {
            return std::wstring(reinterpret_cast<const wchar_t*>(buf));
        }
    }
    return L"";
}

static bool GetOwnerInfoForAddress(const void* addr, HookChainOwnerInfo& out) {
    out = HookChainOwnerInfo{};
    if (!addr) return false;

    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &hMod) ||
        !hMod) {
        return false;
    }

    WCHAR pathBuf[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameW(hMod, pathBuf, (DWORD)std::size(pathBuf));
    if (n > 0) {
        out.path.assign(pathBuf, pathBuf + n);
        try {
            out.name = std::filesystem::path(out.path).filename().wstring();
        } catch (...) {
            out.name = out.path;
        }
        out.company = GetFileVersionStringValue(out.path, L"CompanyName");
        out.product = GetFileVersionStringValue(out.path, L"ProductName");
        out.description = GetFileVersionStringValue(out.path, L"FileDescription");
    }

    MODULEINFO mi{};
    if (GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi))) {
        out.base = (uintptr_t)mi.lpBaseOfDll;
        out.size = (size_t)mi.SizeOfImage;
    } else {
        out.base = (uintptr_t)hMod;
    }

    return true;
}

static bool IsExplicitlyAllowedThirdPartyHookOwner(const HookChainOwnerInfo& ownerInfo) {
    const std::wstring ownerNameLower = ToLowerWide(ownerInfo.name);
    const std::wstring ownerCompanyLower = ToLowerWide(ownerInfo.company);
    const std::wstring ownerProductLower = ToLowerWide(ownerInfo.product);
    const std::wstring ownerDescriptionLower = ToLowerWide(ownerInfo.description);
    const std::wstring ownerPathLower = ToLowerWide(ownerInfo.path);

    const bool isToolscreen = ownerNameLower == L"toolscreen.dll" || ContainsAnySubstring(ownerPathLower, { L"\\toolscreen.dll" });

    const bool isObs = ContainsAnySubstring(ownerPathLower, { L"graphics-hook", L"obs-studio" }) ||
                       ContainsAnySubstring(ownerCompanyLower, { L"obs project", L"open broadcaster software" }) ||
                       ContainsAnySubstring(ownerProductLower, { L"obs studio", L"obs-studio", L"open broadcaster software" }) ||
                       ContainsAnySubstring(ownerDescriptionLower,
                                            { L"graphics hook", L"graphics-hook", L"obs studio", L"obs-studio", L"open broadcaster software" });

    const bool isDiscord = ContainsAnySubstring(ownerNameLower, { L"discord" }) ||
                           ContainsAnySubstring(ownerPathLower, { L"discord" }) ||
                           ContainsAnySubstring(ownerCompanyLower, { L"discord" }) ||
                           ContainsAnySubstring(ownerProductLower, { L"discord" }) ||
                           ContainsAnySubstring(ownerDescriptionLower, { L"discord" });

    return isToolscreen || isObs || isDiscord;
}

static bool IsAddressInSet(const void* addr, std::initializer_list<const void*> addrs) {
    for (const void* candidate : addrs) {
        if (candidate && addr == candidate) {
            return true;
        }
    }
    return false;
}

static bool TryResolveJumpTarget(void* cur, void*& next) {
    next = nullptr;
    if (!cur || !IsReadableCodePtr(cur)) return false;

    const uint8_t* b = reinterpret_cast<const uint8_t*>(cur);

    if (b[0] == 0xEB) {
        int8_t rel = *reinterpret_cast<const int8_t*>(b + 1);
        next = const_cast<uint8_t*>(b + 2 + rel);
    } else if (b[0] == 0xE9) {
        int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
        next = const_cast<uint8_t*>(b + 5 + rel);
    } else if (b[0] == 0xFF && b[1] == 0x25) {
        int32_t disp = *reinterpret_cast<const int32_t*>(b + 2);
        const uint8_t* ripNext = b + 6;
        const uint8_t* slot = ripNext + disp;
        if (!IsReadableCodePtr(slot)) return false;
        next = *reinterpret_cast<void* const*>(slot);
    } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {
        next = *reinterpret_cast<void* const*>(b + 2);
    } else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {
        next = *reinterpret_cast<void* const*>(b + 2);
    }

    return next != nullptr;
}

static void* ResolveFirstAllowedThirdPartyHookTarget(void* start, bool allowDirectStart, std::initializer_list<const void*> excludedTargets) {
    if (!start) return nullptr;

    void* cur = start;
    for (int depth = 0; depth < 16; depth++) {
        if (!cur) return nullptr;

        if (allowDirectStart || depth > 0) {
            if (!IsAddressInSet(cur, excludedTargets) && HookChain::IsAllowedThirdPartyHookAddress(cur)) {
                return cur;
            }
        }

        if (!IsReadableCodePtr(cur)) return nullptr;
        if (!IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(cur))) return nullptr;

        void* next = nullptr;
        if (!TryResolveJumpTarget(cur, next)) return nullptr;

        if (IsAddressInSet(next, excludedTargets)) {
            return nullptr;
        }

        cur = next;
    }

    return nullptr;
}

static void LogSkippedDisallowedHookTarget(const char* apiName, void* startAddress, void* skippedTarget) {
    if (!apiName || !startAddress || !skippedTarget) return;

    HookChainOwnerInfo ownerInfo{};
    if (!GetOwnerInfoForAddress(skippedTarget, ownerInfo)) return;
    if (IsExplicitlyAllowedThirdPartyHookOwner(ownerInfo)) return;

    LogCategory("hookchain",
                std::string("[") + apiName + "] ignoring non-allowlisted hook target start=" +
                    HookChain::DescribeAddressWithOwner(startAddress) + " target=" + HookChain::DescribeAddressWithOwner(skippedTarget));
}

static void* ResolveAbsoluteJumpTarget(void* p) {
    if (!p) return nullptr;

    void* cur = p;
    for (int depth = 0; depth < 8; depth++) {
        void* next = nullptr;

        if (!TryResolveJumpTarget(cur, next)) return nullptr;
        if (!IsReadableCodePtr(next)) return next;
        if (!IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(next))) return next;

        cur = next;
    }

    return nullptr;
}

static void* TraceAbsoluteJumpTarget(void* p, std::vector<std::string>& outTraceLines) {
    outTraceLines.clear();
    if (!p) return nullptr;

    void* cur = p;
    for (int depth = 0; depth < 16; depth++) {
        if (!IsReadableCodePtr(cur)) {
            outTraceLines.push_back("depth=" + std::to_string(depth) + " unreadable @" + PtrToHex(cur));
            return nullptr;
        }

        const uint8_t* b = reinterpret_cast<const uint8_t*>(cur);
        void* next = nullptr;
        const char* kind = nullptr;

        if (b[0] == 0xEB) {
            int8_t rel = *reinterpret_cast<const int8_t*>(b + 1);
            next = const_cast<uint8_t*>(b + 2 + rel);
            kind = "jmp rel8";
        } else if (b[0] == 0xE9) {
            int32_t rel = *reinterpret_cast<const int32_t*>(b + 1);
            next = const_cast<uint8_t*>(b + 5 + rel);
            kind = "jmp rel32";
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            int32_t disp = *reinterpret_cast<const int32_t*>(b + 2);
            const uint8_t* ripNext = b + 6;
            const uint8_t* slot = ripNext + disp;
            if (!IsReadableCodePtr(slot)) {
                outTraceLines.push_back(std::string("depth=") + std::to_string(depth) + " rip-slot unreadable @" + PtrToHex(slot));
                return nullptr;
            }
            next = *reinterpret_cast<void* const*>(slot);
            kind = "jmp [rip+disp32]";
        } else if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) {
            next = *reinterpret_cast<void* const*>(b + 2);
            kind = "mov rax, imm64; jmp rax";
        } else if (b[0] == 0x49 && b[1] == 0xBB && b[10] == 0x41 && b[11] == 0xFF && b[12] == 0xE3) {
            next = *reinterpret_cast<void* const*>(b + 2);
            kind = "mov r11, imm64; jmp r11";
        }

        if (!next || !kind) {
            outTraceLines.push_back(std::string("depth=") + std::to_string(depth) + " no-jump-pattern @" + HookChain::DescribeAddressWithOwner(cur));
            return nullptr;
        }

        outTraceLines.push_back(std::string("depth=") + std::to_string(depth) + " " + kind + " " + HookChain::DescribeAddressWithOwner(cur) +
                                " -> " + HookChain::DescribeAddressWithOwner(next));

        if (!IsReadableCodePtr(next)) return next;
        if (!IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(next))) return next;
        cur = next;
    }

    outTraceLines.push_back("max-depth reached starting @" + HookChain::DescribeAddressWithOwner(p));
    return nullptr;
}

static void LogHookChainDetails(const char* apiName, void* startAddress, void* resolvedHookTarget, const char* reason) {
    if (!apiName) apiName = "(unknown api)";
    if (!reason) reason = "(unspecified)";

    const char* mode = "LatestHook";

    LogCategory("hookchain",
                std::string("[") + apiName + "] chain-detect reason=" + reason + " nextTarget=" + mode + " start=" +
                    HookChain::DescribeAddressWithOwner(startAddress) + " hookTarget=" + HookChain::DescribeAddressWithOwner(resolvedHookTarget));

    std::vector<std::string> trace;
    (void)TraceAbsoluteJumpTarget(startAddress, trace);
    for (const auto& line : trace) {
        LogCategory("hookchain", std::string("[") + apiName + "] " + line);
    }
}

static void LogIatHookChainDetails(const char* apiName, HMODULE importingModule, void* thunkTarget, void* expectedExport) {
    if (!apiName) apiName = "(unknown api)";
    std::string importerDesc = importingModule ? HookChain::DescribeAddressWithOwner(importingModule) : std::string("(null)");
    LogCategory("hookchain",
                std::string("[") + apiName + "] IAT chain-detect importingModule=" + importerDesc + " iatTarget=" +
                    HookChain::DescribeAddressWithOwner(thunkTarget) + " expectedExport=" + HookChain::DescribeAddressWithOwner(expectedExport));
}

static void* FindIatImportedFunctionTarget(HMODULE importingModule, const char* importedDllNameLower, const char* funcName) {
    if (!importingModule || !importedDllNameLower || !funcName) return nullptr;

    uint8_t* base = reinterpret_cast<uint8_t*>(importingModule);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!impDir.VirtualAddress || !impDir.Size) return nullptr;

    auto* desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + impDir.VirtualAddress);
    for (; desc->Name != 0; desc++) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (!dllName) continue;

        std::string dllLower(dllName);
        for (char& c : dllLower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (dllLower != importedDllNameLower) continue;

        auto* oft = reinterpret_cast<PIMAGE_THUNK_DATA>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* ft = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);
        for (; oft->u1.AddressOfData != 0; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + oft->u1.AddressOfData);
            if (!ibn || !ibn->Name) continue;
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) == 0) {
#if defined(_WIN64)
                return reinterpret_cast<void*>(ft->u1.Function);
#else
                return reinterpret_cast<void*>(ft->u1.Function);
#endif
            }
        }
    }
    return nullptr;
}

static void RefreshThirdPartyViewportHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) return;

    void* exportViewport = reinterpret_cast<void*>(GetProcAddress(hOpenGL32, "glViewport"));
    if (!exportViewport) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportViewport);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("glViewport", exportViewport, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportViewport,
        false,
        { reinterpret_cast<void*>(&hkglViewport), reinterpret_cast<void*>(&hkglViewport_Driver), reinterpret_cast<void*>(&hkglViewport_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkglViewport) || jumpTarget == reinterpret_cast<void*>(&hkglViewport_Driver) ||
        jumpTarget == reinterpret_cast<void*>(&hkglViewport_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_glViewportDriverHookTarget.load(std::memory_order_acquire) ||
        jumpTarget == g_glViewportThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (jumpTarget == g_glViewportThirdPartyHookTarget.load(std::memory_order_acquire)) return;

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkglViewport_ThirdParty),
                                          reinterpret_cast<void**>(&g_oglViewportThirdParty), "glViewport (third-party chain)")) {
        g_glViewportThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        g_glViewportHookCount.fetch_add(1);
        LogHookChainDetails("glViewport", exportViewport, jumpTarget, "export detour (prolog)");
        Log("Chained glViewport through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
        Log("Total glViewport hook count: " + std::to_string(g_glViewportHookCount.load()));
    }
}

static void RefreshThirdPartyViewportHookChainFromDriverTarget() {
    if (g_config.disableHookChaining) return;

    void* driverTarget = g_glViewportDriverHookTarget.load(std::memory_order_acquire);
    if (!driverTarget) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(driverTarget);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("glViewport", driverTarget, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        driverTarget,
        false,
        { reinterpret_cast<void*>(&hkglViewport), reinterpret_cast<void*>(&hkglViewport_Driver), reinterpret_cast<void*>(&hkglViewport_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkglViewport) || jumpTarget == reinterpret_cast<void*>(&hkglViewport_Driver) ||
        jumpTarget == reinterpret_cast<void*>(&hkglViewport_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_glViewportThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkglViewport_ThirdParty),
                                          reinterpret_cast<void**>(&g_oglViewportThirdParty), "glViewport (driver third-party chain)")) {
        g_glViewportThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        g_glViewportHookCount.fetch_add(1);
        LogHookChainDetails("glViewport", driverTarget, jumpTarget, "driver detour (after driver hook)");
        Log("Chained glViewport (driver target) through third-party detour at " + HookChain::DescribeAddressWithOwner(jumpTarget));
        Log("Total glViewport hook count: " + std::to_string(g_glViewportHookCount.load()));
    }
}

static void RefreshThirdPartyWglSwapBuffersHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) return;

    void* exportSwap = reinterpret_cast<void*>(GetProcAddress(hOpenGL32, "wglSwapBuffers"));
    if (!exportSwap) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportSwap);
    const bool sawDisallowedOuterHook = observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget);
    if (sawDisallowedOuterHook) {
        LogSkippedDisallowedHookTarget("wglSwapBuffers", exportSwap, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportSwap,
        false,
        { reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty) });
    if (!jumpTarget) {
        jumpTarget = observedTarget;
    }
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty)) {
        return;
    }

    if (jumpTarget == reinterpret_cast<void*>(&hkwglSwapBuffers) && !sawDisallowedOuterHook) {
        return;
    }

    if (jumpTarget == g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty),
                                          reinterpret_cast<void**>(&g_owglSwapBuffersThirdParty), "wglSwapBuffers (third-party chain)")) {
        g_wglSwapBuffersThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        LogHookChainDetails("wglSwapBuffers", exportSwap, jumpTarget,
                            sawDisallowedOuterHook && jumpTarget == observedTarget ? "export detour (transport fallback)"
                                                                                   : "export detour (prolog)");
        Log("Chained wglSwapBuffers through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartySetCursorPosHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hUser32 = GetModuleHandle(L"user32.dll");
    if (!hUser32) return;

    void* exportFunc = reinterpret_cast<void*>(GetProcAddress(hUser32, "SetCursorPos"));
    if (!exportFunc) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportFunc);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("SetCursorPos", exportFunc, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportFunc,
        false,
        { reinterpret_cast<void*>(&hkSetCursorPos), reinterpret_cast<void*>(&hkSetCursorPos_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkSetCursorPos) || jumpTarget == reinterpret_cast<void*>(&hkSetCursorPos_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_setCursorPosThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkSetCursorPos_ThirdParty),
                                          reinterpret_cast<void**>(&g_oSetCursorPosThirdParty), "SetCursorPos (third-party chain)")) {
        g_setCursorPosThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        LogHookChainDetails("SetCursorPos", exportFunc, jumpTarget, "export detour (prolog)");
        Log("Chained SetCursorPos through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartyClipCursorHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hUser32 = GetModuleHandle(L"user32.dll");
    if (!hUser32) return;

    void* exportFunc = reinterpret_cast<void*>(GetProcAddress(hUser32, "ClipCursor"));
    if (!exportFunc) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportFunc);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("ClipCursor", exportFunc, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportFunc,
        false,
        { reinterpret_cast<void*>(&hkClipCursor), reinterpret_cast<void*>(&hkClipCursor_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkClipCursor) || jumpTarget == reinterpret_cast<void*>(&hkClipCursor_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_clipCursorThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkClipCursor_ThirdParty),
                                          reinterpret_cast<void**>(&g_oClipCursorThirdParty), "ClipCursor (third-party chain)")) {
        g_clipCursorThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        LogHookChainDetails("ClipCursor", exportFunc, jumpTarget, "export detour (prolog)");
        Log("Chained ClipCursor through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartySetCursorHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hUser32 = GetModuleHandle(L"user32.dll");
    if (!hUser32) return;

    void* exportFunc = reinterpret_cast<void*>(GetProcAddress(hUser32, "SetCursor"));
    if (!exportFunc) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportFunc);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("SetCursor", exportFunc, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportFunc,
        false,
        { reinterpret_cast<void*>(&hkSetCursor), reinterpret_cast<void*>(&hkSetCursor_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkSetCursor) || jumpTarget == reinterpret_cast<void*>(&hkSetCursor_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_setCursorThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkSetCursor_ThirdParty),
                                          reinterpret_cast<void**>(&g_oSetCursorThirdParty), "SetCursor (third-party chain)")) {
        g_setCursorThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        LogHookChainDetails("SetCursor", exportFunc, jumpTarget, "export detour (prolog)");
        Log("Chained SetCursor through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartyGetRawInputDataHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hUser32 = GetModuleHandle(L"user32.dll");
    if (!hUser32) return;

    void* exportFunc = reinterpret_cast<void*>(GetProcAddress(hUser32, "GetRawInputData"));
    if (!exportFunc) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportFunc);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("GetRawInputData", exportFunc, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportFunc,
        false,
        { reinterpret_cast<void*>(&hkGetRawInputData), reinterpret_cast<void*>(&hkGetRawInputData_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkGetRawInputData) || jumpTarget == reinterpret_cast<void*>(&hkGetRawInputData_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_getRawInputDataThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkGetRawInputData_ThirdParty),
                                          reinterpret_cast<void**>(&g_oGetRawInputDataThirdParty), "GetRawInputData (third-party chain)")) {
        g_getRawInputDataThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        LogHookChainDetails("GetRawInputData", exportFunc, jumpTarget, "export detour (prolog)");
        Log("Chained GetRawInputData through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartyGlfwSetInputModeHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE hGlfw = GetModuleHandle(L"glfw.dll");
    if (!hGlfw) return;

    void* exportFunc = reinterpret_cast<void*>(GetProcAddress(hGlfw, "glfwSetInputMode"));
    if (!exportFunc) return;

    void* observedTarget = ResolveAbsoluteJumpTarget(exportFunc);
    if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
        LogSkippedDisallowedHookTarget("glfwSetInputMode", exportFunc, observedTarget);
    }

    void* jumpTarget = ResolveFirstAllowedThirdPartyHookTarget(
        exportFunc,
        false,
        { reinterpret_cast<void*>(&hkglfwSetInputMode), reinterpret_cast<void*>(&hkglfwSetInputMode_ThirdParty) });
    if (!jumpTarget) return;

    if (jumpTarget == reinterpret_cast<void*>(&hkglfwSetInputMode) || jumpTarget == reinterpret_cast<void*>(&hkglfwSetInputMode_ThirdParty)) {
        return;
    }

    if (jumpTarget == g_glfwSetInputModeThirdPartyHookTarget.load(std::memory_order_acquire)) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(jumpTarget, reinterpret_cast<void*>(&hkglfwSetInputMode_ThirdParty),
                                          reinterpret_cast<void**>(&g_oglfwSetInputModeThirdParty), "glfwSetInputMode (third-party chain)")) {
        g_glfwSetInputModeThirdPartyHookTarget.store(jumpTarget, std::memory_order_release);
        LogHookChainDetails("glfwSetInputMode", exportFunc, jumpTarget, "export detour (prolog)");
        Log("Chained glfwSetInputMode through third-party detour target at " + HookChain::DescribeAddressWithOwner(jumpTarget));
    }
}

static void RefreshThirdPartyWglSwapBuffersIatHookChain() {
    if (g_config.disableHookChaining) return;

    HMODULE opengl32 = GetModuleHandle(L"opengl32.dll");
    if (!opengl32) return;

    void* exportSwap = reinterpret_cast<void*>(GetProcAddress(opengl32, "wglSwapBuffers"));
    if (!exportSwap) return;

    HMODULE mods[1024];
    DWORD cbNeeded = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &cbNeeded)) return;

    const DWORD count = cbNeeded / sizeof(HMODULE);
    for (DWORD i = 0; i < count; i++) {
        HMODULE m = mods[i];
        if (!m) continue;

        if (m == opengl32) continue;

        void* thunkTarget = FindIatImportedFunctionTarget(m, "opengl32.dll", "wglSwapBuffers");
        if (!thunkTarget) continue;

        if (!HookChain::IsAllowedThirdPartyHookAddress(thunkTarget)) {
            void* observedTarget = ResolveAbsoluteJumpTarget(thunkTarget);
            if (observedTarget && !HookChain::IsAllowedThirdPartyHookAddress(observedTarget)) {
                LogSkippedDisallowedHookTarget("wglSwapBuffers", thunkTarget, observedTarget);
            } else {
                LogSkippedDisallowedHookTarget("wglSwapBuffers", thunkTarget, thunkTarget);
            }
        }

        void* allowedTarget = ResolveFirstAllowedThirdPartyHookTarget(
            thunkTarget,
            true,
            { reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty) });
        if (allowedTarget) {
            thunkTarget = allowedTarget;
        } else {
            void* observedTarget = ResolveAbsoluteJumpTarget(thunkTarget);
            if (observedTarget) {
                thunkTarget = observedTarget;
            } else if (!HookChain::IsAllowedThirdPartyHookAddress(thunkTarget)) {
                continue;
            }
        }

        if (thunkTarget == exportSwap || thunkTarget == reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty)) {
            continue;
        }

        if (thunkTarget == g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire)) {
            return;
        }

        if (HookChain::TryCreateAndEnableHook(thunkTarget, reinterpret_cast<void*>(&hkwglSwapBuffers_ThirdParty),
                                              reinterpret_cast<void**>(&g_owglSwapBuffersThirdParty),
                                              "wglSwapBuffers (IAT third-party chain)")) {
            g_wglSwapBuffersThirdPartyHookTarget.store(thunkTarget, std::memory_order_release);
            LogIatHookChainDetails("wglSwapBuffers", m, thunkTarget, exportSwap);
            Log("Chained wglSwapBuffers via IAT target at " + HookChain::DescribeAddressWithOwner(thunkTarget));
            return;
        }
    }
}

}

namespace HookChain {

bool IsAllowedThirdPartyHookAddress(const void* addr) {
    HookChainOwnerInfo ownerInfo{};
    if (!GetOwnerInfoForAddress(addr, ownerInfo)) return false;
    return IsExplicitlyAllowedThirdPartyHookOwner(ownerInfo);
}

    bool HasAllowedThirdPartyHookOnStack(unsigned skipFrames) {
        void* stack[32] = {};
        const USHORT frames = CaptureStackBackTrace(1 + skipFrames, static_cast<DWORD>(std::size(stack)), stack, NULL);
        for (USHORT i = 0; i < frames; ++i) {
            if (IsAllowedThirdPartyHookAddress(stack[i])) {
                return true;
            }
        }
        return false;
    }

bool TryCreateAndEnableHook(void* target, void* detour, void** outOriginal, const char* what) {
    if (!target) return false;

    MH_STATUS st = MH_CreateHook(target, detour, outOriginal);
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        Log(std::string("ERROR: Failed to create ") + (what ? what : "(hook)") + " hook (status " + std::to_string((int)st) + ")");
        return false;
    }

    st = MH_EnableHook(target);
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        if ((int)st == 11) {
            MH_RemoveHook(target);
            Log(std::string("INFO: Skipping ") + (what ? what : "(hook)") +
                " hook because the target is not safely patchable by MinHook (status " + std::to_string((int)st) + ")");
            return false;
        }
        Log(std::string("ERROR: Failed to enable ") + (what ? what : "(hook)") + " hook (status " + std::to_string((int)st) + ")");
        return false;
    }
    return true;
}

std::string DescribeAddressWithOwner(const void* addr) {
    if (!addr) return std::string("(null)");

    HookChainOwnerInfo info;
    if (!GetOwnerInfoForAddress(addr, info)) {
        return PtrToHex(addr) + " (unknown module)";
    }

    std::ostringstream ss;
    ss << PtrToHex(addr);
    if (!info.name.empty()) {
        ss << " (" << WideToUtf8(info.name);
        if (info.base) {
            ss << "+0x" << std::hex << std::uppercase << ((uintptr_t)addr - info.base);
        }
        ss << ")";
    }
    if (!info.description.empty()) ss << " \"" << WideToUtf8(info.description) << "\"";
    if (!info.product.empty()) ss << " product=\"" << WideToUtf8(info.product) << "\"";
    if (!info.company.empty()) ss << " company=\"" << WideToUtf8(info.company) << "\"";
    if (!info.path.empty()) ss << " path=\"" << WideToUtf8(info.path) << "\"";
    return ss.str();
}

void RefreshAllThirdPartyHookChains() {
    RefreshThirdPartyViewportHookChain();
    RefreshThirdPartyViewportHookChainFromDriverTarget();
    RefreshThirdPartyWglSwapBuffersHookChain();
    RefreshThirdPartyWglSwapBuffersIatHookChain();
    RefreshThirdPartySetCursorPosHookChain();
    RefreshThirdPartyClipCursorHookChain();
    RefreshThirdPartySetCursorHookChain();
    RefreshThirdPartyGetRawInputDataHookChain();
    RefreshThirdPartyGlfwSetInputModeHookChain();
}

}


