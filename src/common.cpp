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
    g_cfg.reversePads = GetPrivateProfileIntW(L"PadFix", L"ReversePadOrder", 0, ini.c_str()) != 0;
    g_cfg.xboxNames = GetPrivateProfileIntW(L"PadFix", L"XboxButtonNames", 1, ini.c_str()) != 0;
    Log("[padfix] version " PADFIX_VERSION);
    Log("[cfg] Translate=%d PretendWired360=%d SwapTriggers=%d ReversePadOrder=%d XboxButtonNames=%d Verbose=%d", g_cfg.translate,
        g_cfg.spoof360, g_cfg.swapTriggers, g_cfg.reversePads, g_cfg.xboxNames, g_cfg.verbose);
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

// XInputGetState on an empty slot is slow (it searches for a device), so which slots hold a pad is cached: an empty
// slot is probed again at most once a second; a connected one is re-read whenever its own device reads it and at
// least once a second otherwise, so a pad that disconnects drops out of the order.
struct Slot {
    bool connected = false;
    DWORD lastProbe = 0;
    XState last{};
};
std::mutex g_slotLock;
Slot g_slots[4];

bool Probe(GetState_t get, int i, DWORD now) {
    Slot& s = g_slots[i];
    s.lastProbe = now;
    s.connected = get(i, &s.last) == ERROR_SUCCESS;
    return s.connected;
}

} // namespace

Pad ReadPad(int nth, int* slot) {
    Pad p;
    if (slot) *slot = -1;
    auto get = ResolveGetState();
    if (!get || nth < 0) return p;
    std::lock_guard<std::mutex> lock(g_slotLock);
    DWORD now = GetTickCount();
    int seen = 0;
    for (int k = 0; k < 4; ++k) {
        int i = g_cfg.reversePads ? 3 - k : k;
        Slot& s = g_slots[i];
        if (!s.connected && now - s.lastProbe < 1000) continue; // known empty, checked recently
        bool mine = seen == nth;
        // pads before ours only need to be known connected; ours is read now
        if (!s.connected || mine || now - s.lastProbe >= 1000) Probe(get, i, now);
        if (!s.connected) continue;
        if (mine) {
            if (slot) *slot = i;
            p.connected = true;
            p.buttons = s.last.Gamepad.wButtons;
            p.lt = s.last.Gamepad.bLeftTrigger;
            p.rt = s.last.Gamepad.bRightTrigger;
            p.lx = s.last.Gamepad.sThumbLX;
            p.ly = s.last.Gamepad.sThumbLY;
            p.rx = s.last.Gamepad.sThumbRX;
            p.ry = s.last.Gamepad.sThumbRY;
            return p;
        }
        ++seen;
    }
    return p;
}

} // namespace padfix
