#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

#define PADFIX_VERSION "1.0.0"

namespace padfix {

// padfix.log next to the DLL (shared by both proxies; each line is tagged).
void Log(const char* fmt, ...);
std::wstring DllDir();
HMODULE LoadSystemDll(const wchar_t* name); // from the (WOW64) system directory, never the game folder

// Settings from padfix.ini next to the DLL.
struct Config {
    bool translate = true;    // build the Xbox pad's DirectInput state from XInput (360 layout)
    bool swapTriggers = false; // Z axis: LT -> max, RT -> min (false) or the other way round
    bool spoof360 = true;     // introduce Microsoft pads as a wired Xbox 360 pad (the game's default-controls lookup)
    bool verbose = true;      // log calls in detail during the first seconds
};
extern Config g_cfg;
void LoadConfigOnce();

// XInput snapshot of the first connected pad (xinput1_4).
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
Pad ReadPad();

} // namespace padfix
