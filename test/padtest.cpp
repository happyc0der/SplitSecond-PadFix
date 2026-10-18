// Offline checks for a PadFix build, without starting the game.
//   padtest <dinput8.dll> [seconds] [devices]   read the first Xbox pad the way Split/Second does; with devices > 1
//                                               open it that many times (as split-screen does: the 2nd reads pad 2)
//   padtest <dinput8.dll> --names               plant a copy of the game's input-name table in this process and
//                                               check that the proxy renames it to Xbox names
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static GUID g_pad{};
static BOOL CALLBACK Enum(LPCDIDEVICEINSTANCEA d, LPVOID) {
    g_pad = d->guidInstance;
    printf("pad: %s (product %08lX)\n", d->tszProductName, d->guidProduct.Data1);
    return DIENUM_STOP;
}

static int Names(Create_t create) {
    // a slice of the real table: NUL-separated strings, unrelated ones in between
    const char* strs[] = {"PREMIUM VEHICLE", "Button 1", "Button 10", "Pov Button 1", "Axis X Neg", "Axis X Pos", "Axis Y Neg",
                          "Axis Y Pos", "Axis Z Pos", "Axis Z Neg", "Right Axis X Neg", "Right Axis X Pos", "Right Axis Y Neg",
                          "Right Axis Y Pos", "Right Axis Z Pos", "Right Axis Z Neg", "Button 2", "Button 3", "Button 4",
                          "Button 5", "Button 6", "Button 7", "Button 8", "Button 9", "Button 11", "RMenu", "RShift",
                          "PRESS *ACTION_PP_1* TO TRIGGER LEVEL 1 POWER PLAYS"};
    std::string blob(1, '\0');
    for (const char* s : strs) { blob += s; blob += '\0'; }
    char* table = static_cast<char*>(VirtualAlloc(nullptr, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    memcpy(table + 4096, blob.data(), blob.size());
    IDirectInput8A* di = nullptr;
    create(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8A, (void**)&di, nullptr); // starts the name watcher
    Sleep(3000);
    int changed = 0, kept = 0;
    for (const char* p = table + 4097; *p || p[1]; p += strlen(p) + 1) {
        if (!*p) { ++p; continue; }
        printf("  %s\n", p);
    }
    const char* expectNew[] = {"A", "RS CLICK", "D-PAD", "LS LEFT", "LT", "RT", "RS DOWN", "RAlt"};
    const char* expectOld[] = {"Right Axis Z Pos", "Button 11", "RShift", "PREMIUM VEHICLE"};
    for (const char* e : expectNew) for (const char* p = table + 4096; p < table + 4096 + blob.size(); ++p) if (!p[-1] && !strcmp(p, e)) { ++changed; break; }
    for (const char* e : expectOld) for (const char* p = table + 4096; p < table + 4096 + blob.size(); ++p) if (!p[-1] && !strcmp(p, e)) { ++kept; break; }
    printf("renamed %d of 8 sampled, left alone %d of 4 that must stay\n", changed, kept);
    if (di) di->Release();
    return changed == 8 && kept == 4 ? 0 : 1;
}

static DWORD g_mainThread;
static LONG WINAPI Crash(EXCEPTION_POINTERS* e) {
    void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    ULONG_PTR info1 = e->ExceptionRecord->NumberParameters > 1 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    printf("CRASH %08lX at %p (%s+%IX) %s thread, data address %IX\n", e->ExceptionRecord->ExceptionCode, at, name,
           (ULONG_PTR)at - (ULONG_PTR)m, GetCurrentThreadId() == g_mainThread ? "main" : "other", info1);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t** argv) {
    g_mainThread = GetCurrentThreadId();
    SetUnhandledExceptionFilter(Crash);
    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) { printf("load failed %lu\n", GetLastError()); return 1; }
    auto create = (Create_t)GetProcAddress(h, "DirectInput8Create");
    if (argc > 2 && !wcscmp(argv[2], L"--names")) return Names(create);
    IDirectInput8A* di = nullptr;
    printf("create %08lX\n", create(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8A, (void**)&di, nullptr));
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, Enum, nullptr, DIEDFL_ATTACHEDONLY);
    int secs = argc > 2 ? _wtoi(argv[2]) : 1, n = argc > 3 ? _wtoi(argv[3]) : 1;
    std::vector<IDirectInputDevice8A*> devs;
    for (int k = 0; k < n; ++k) {
        IDirectInputDevice8A* dev = nullptr;
        if (FAILED(di->CreateDevice(g_pad, &dev, nullptr))) { printf("no pad\n"); return 1; }
        dev->SetDataFormat(&c_dfDIJoystick2);
        for (DWORD ofs = 0; ofs <= 20; ofs += 4) {
            DIPROPRANGE r{{sizeof(r), sizeof(DIPROPHEADER), ofs, DIPH_BYOFFSET}, -32767, 32767};
            dev->SetProperty(DIPROP_RANGE, &r.diph);
        }
        dev->SetCooperativeLevel(GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        dev->Acquire();
        devs.push_back(dev);
    }
    for (int i = 0; i < secs * 4; ++i) {
        for (size_t k = 0; k < devs.size(); ++k) {
            DIJOYSTATE2 s{};
            devs[k]->Poll();
            HRESULT hr = devs[k]->GetDeviceState(sizeof(s), &s);
            int pressed = 0;
            for (int b = 0; b < 31; ++b) if (s.rgbButtons[b]) pressed |= 1 << b;
            printf("dev %zu hr %08lX X %6ld Y %6ld Z %6ld Rx %6ld Ry %6ld Rz %ld sl %ld/%ld POV %ld,%ld btn %08X vX %ld\n", k + 1, hr, s.lX, s.lY,
                   s.lZ, s.lRx, s.lRy, s.lRz, s.rglSlider[0], s.rglSlider[1], (long)s.rgdwPOV[0], (long)s.rgdwPOV[1], pressed, s.lVX);
        }
        Sleep(250);
    }
    for (auto* d : devs) { d->Unacquire(); d->Release(); }
    di->Release();
}
