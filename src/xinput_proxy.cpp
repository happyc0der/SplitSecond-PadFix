// xinput1_3.dll proxy for Split/Second: same ordinals as the DirectX-redist xinput1_3, forwarded to the system's
// xinput1_4 (better Bluetooth support). Logs which functions the game uses.
#include <atomic>
#include <thread>

#include "common.h"

using namespace padfix;

namespace {

HMODULE g_x14 = nullptr, g_x13 = nullptr;
std::atomic<long> g_getCalls[4], g_getOk[4], g_setCalls, g_capsCalls, g_otherCalls;
std::atomic<bool> g_reporter{false};

FARPROC Real(const char* nameOrOrdinal) {
    if (!g_x14) g_x14 = LoadSystemDll(L"xinput1_4.dll");
    if (!g_x13) g_x13 = LoadSystemDll(L"xinput1_3.dll");
    FARPROC p = g_x14 ? GetProcAddress(g_x14, nameOrOrdinal) : nullptr;
    if (!p && g_x13) p = GetProcAddress(g_x13, nameOrOrdinal);
    return p;
}

void StartReporter() {
    bool expected = false;
    if (!g_reporter.compare_exchange_strong(expected, true)) return;
    LoadConfigOnce();
    Log("[xinput] proxy active (forwarding to xinput1_4)");
    std::thread([] {
        for (int i = 0; i < 120; ++i) { // ten minutes of 5 s summaries, then quiet
            Sleep(5000);
            Log("[xinput] GetState calls/ok per index: 0:%ld/%ld 1:%ld/%ld 2:%ld/%ld 3:%ld/%ld | SetState %ld | GetCapabilities %ld | other %ld",
                g_getCalls[0].load(), g_getOk[0].load(), g_getCalls[1].load(), g_getOk[1].load(), g_getCalls[2].load(),
                g_getOk[2].load(), g_getCalls[3].load(), g_getOk[3].load(), g_setCalls.load(), g_capsCalls.load(),
                g_otherCalls.load());
        }
    }).detach();
}

} // namespace

extern "C" {

DWORD WINAPI XInputGetState(DWORD i, void* s) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, void*);
    static F f = reinterpret_cast<F>(Real("XInputGetState"));
    DWORD r = f ? f(i, s) : ERROR_DEVICE_NOT_CONNECTED;
    if (i < 4) { ++g_getCalls[i]; if (r == ERROR_SUCCESS) ++g_getOk[i]; }
    return r;
}

DWORD WINAPI XInputSetState(DWORD i, void* v) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, void*);
    static F f = reinterpret_cast<F>(Real("XInputSetState"));
    ++g_setCalls;
    return f ? f(i, v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD flags, void* c) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, DWORD, void*);
    static F f = reinterpret_cast<F>(Real("XInputGetCapabilities"));
    ++g_capsCalls;
    return f ? f(i, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL enable) {
    StartReporter();
    using F = void(WINAPI*)(BOOL);
    static F f = reinterpret_cast<F>(Real("XInputEnable"));
    ++g_otherCalls;
    Log("[xinput] XInputEnable(%d)", enable);
    if (f) f(enable);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, GUID* render, GUID* capture) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, GUID*, GUID*);
    static F f = g_x13 || (g_x13 = LoadSystemDll(L"xinput1_3.dll")) ? reinterpret_cast<F>(GetProcAddress(g_x13, "XInputGetDSoundAudioDeviceGuids")) : nullptr;
    ++g_otherCalls;
    return f ? f(i, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE type, void* info) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, BYTE, void*);
    static F f = reinterpret_cast<F>(Real("XInputGetBatteryInformation"));
    ++g_otherCalls;
    return f ? f(i, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD reserved, void* k) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, DWORD, void*);
    static F f = reinterpret_cast<F>(Real("XInputGetKeystroke"));
    ++g_otherCalls;
    return f ? f(i, reserved, k) : ERROR_EMPTY;
}

// Undocumented ordinals 100-103 (GetStateEx, WaitForGuideButton, CancelGuideButtonWait, PowerOffController).
DWORD WINAPI XInputOrd100(DWORD i, void* s) {
    StartReporter();
    using F = DWORD(WINAPI*)(DWORD, void*);
    static F f = reinterpret_cast<F>(Real(MAKEINTRESOURCEA(100)));
    DWORD r = f ? f(i, s) : ERROR_DEVICE_NOT_CONNECTED;
    if (i < 4) { ++g_getCalls[i]; if (r == ERROR_SUCCESS) ++g_getOk[i]; }
    return r;
}
DWORD WINAPI XInputOrd101(DWORD i, DWORD flags, void* l) {
    using F = DWORD(WINAPI*)(DWORD, DWORD, void*);
    static F f = reinterpret_cast<F>(Real(MAKEINTRESOURCEA(101)));
    ++g_otherCalls;
    return f ? f(i, flags, l) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputOrd102(DWORD i) {
    using F = DWORD(WINAPI*)(DWORD);
    static F f = reinterpret_cast<F>(Real(MAKEINTRESOURCEA(102)));
    ++g_otherCalls;
    return f ? f(i) : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD WINAPI XInputOrd103(DWORD i) {
    using F = DWORD(WINAPI*)(DWORD);
    static F f = reinterpret_cast<F>(Real(MAKEINTRESOURCEA(103)));
    ++g_otherCalls;
    return f ? f(i) : ERROR_DEVICE_NOT_CONNECTED;
}

} // extern "C"
