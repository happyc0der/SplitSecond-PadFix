// Loads the deployed proxy dinput8.dll from the game folder and reads the Xbox pad like Split/Second does.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdio>
using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static GUID g_pad{};
static BOOL CALLBACK Enum(LPCDIDEVICEINSTANCEA d, LPVOID) { g_pad = d->guidInstance; printf("pad: %s\n", d->tszProductName); return DIENUM_STOP; }
int wmain(int argc, wchar_t** argv) {
    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) { printf("load failed %lu\n", GetLastError()); return 1; }
    auto create = (Create_t)GetProcAddress(h, "DirectInput8Create");
    IDirectInput8A* di = nullptr;
    printf("create %08lX\n", create(GetModuleHandleW(nullptr), 0x0800, IID_IDirectInput8A, (void**)&di, nullptr));
    di->EnumDevices(DI8DEVCLASS_GAMECTRL, Enum, nullptr, DIEDFL_ATTACHEDONLY);
    IDirectInputDevice8A* dev = nullptr;
    if (FAILED(di->CreateDevice(g_pad, &dev, nullptr))) { printf("no pad\n"); return 1; }
    dev->SetDataFormat(&c_dfDIJoystick2);
    for (DWORD ofs = 0; ofs <= 20; ofs += 4) {
        DIPROPRANGE r{{sizeof(r), sizeof(DIPROPHEADER), ofs, DIPH_BYOFFSET}, -32767, 32767};
        dev->SetProperty(DIPROP_RANGE, &r.diph);
    }
    dev->SetCooperativeLevel(GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    dev->Acquire();
    int secs = argc > 2 ? _wtoi(argv[2]) : 1;
    for (int i = 0; i < secs * 4; ++i) {
        DIJOYSTATE2 s{};
        dev->Poll();
        HRESULT hr = dev->GetDeviceState(sizeof(s), &s);
        int pressed = 0; for (int b = 0; b < 128; ++b) if (s.rgbButtons[b]) pressed |= (b < 31 ? 1 << b : 0);
        printf("hr %08lX X %6ld Y %6ld Z %6ld Rx %6ld Ry %6ld Rz %ld sl %ld/%ld POV %ld,%ld,%ld,%ld btn %08X vX %ld fX %ld\n", hr, s.lX, s.lY, s.lZ, s.lRx, s.lRy, s.lRz,
               s.rglSlider[0], s.rglSlider[1], (long)s.rgdwPOV[0], (long)s.rgdwPOV[1], (long)s.rgdwPOV[2], (long)s.rgdwPOV[3], pressed, s.lVX, s.lFX);
        Sleep(250);
    }
    dev->Unacquire(); dev->Release(); di->Release();
}
