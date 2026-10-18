#include "common.h"

#include <cstdarg>
#include <mutex>

namespace padfix {

Config g_cfg;

std::wstring DllDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &self);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

HMODULE LoadSystemDll(const wchar_t* name) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH); // a 32-bit process is redirected to SysWOW64
    std::wstring p = std::wstring(sys) + L"\\" + name;
    return LoadLibraryW(p.c_str());
}

void Log(const char* fmt, ...) {
    static std::mutex m;
    static FILE* f = nullptr;
    std::lock_guard<std::mutex> lock(m);
    if (!f) {
        std::wstring p = DllDir() + L"\\padfix.log";
        f = _wfsopen(p.c_str(), L"a", _SH_DENYNO); // shareable, so it can be read while the game runs
        if (!f) return;
        fprintf(f, "\n==== session start (pid %lu) ====\n", GetCurrentProcessId());
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fflush(f);
}

void LoadConfigOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    std::wstring ini = DllDir() + L"\\padfix.ini";
    g_cfg.translate = GetPrivateProfileIntW(L"PadFix", L"Translate", 1, ini.c_str()) != 0;
    g_cfg.swapTriggers = GetPrivateProfileIntW(L"PadFix", L"SwapTriggers", 0, ini.c_str()) != 0;
    g_cfg.verbose = GetPrivateProfileIntW(L"PadFix", L"Verbose", 1, ini.c_str()) != 0;
    g_cfg.spoof360 = GetPrivateProfileIntW(L"PadFix", L"PretendWired360", 1, ini.c_str()) != 0;
    Log("[padfix] version " PADFIX_VERSION);
    Log("[cfg] Translate=%d SwapTriggers=%d PretendWired360=%d Verbose=%d", g_cfg.translate, g_cfg.swapTriggers, g_cfg.spoof360, g_cfg.verbose);
}

namespace {
struct XGamepad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XGamepad Gamepad; };
using GetState_t = DWORD(WINAPI*)(DWORD, XState*);
GetState_t ResolveGetState() {
    static GetState_t f = [] {
        HMODULE h = LoadSystemDll(L"xinput1_4.dll");
        return h ? reinterpret_cast<GetState_t>(GetProcAddress(h, "XInputGetState")) : nullptr;
    }();
    return f;
}
} // namespace

Pad ReadPad() {
    Pad p;
    auto get = ResolveGetState();
    if (!get) return p;
    for (DWORD i = 0; i < 4; ++i) {
        XState s{};
        if (get(i, &s) == ERROR_SUCCESS) {
            p.connected = true;
            p.buttons = s.Gamepad.wButtons;
            p.lt = s.Gamepad.bLeftTrigger;
            p.rt = s.Gamepad.bRightTrigger;
            p.lx = s.Gamepad.sThumbLX;
            p.ly = s.Gamepad.sThumbLY;
            p.rx = s.Gamepad.sThumbRX;
            p.ry = s.Gamepad.sThumbRY;
            break;
        }
    }
    return p;
}

} // namespace padfix
