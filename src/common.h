#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>

#define PADFIX_VERSION "1.1.0"

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
    bool reversePads = false; // split-screen: hand out XInput pads highest slot first
    bool xboxNames = true;    // rename "Button 1"/"Axis Z Pos"... to Xbox names in the game's text table in memory
};
extern Config g_cfg;
void LoadConfigOnce();

// XInput snapshot (xinput1_4).
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
// The nth connected XInput pad (0 = first), so the game's first Xbox controller reads the first connected pad,
// its second the second, and so on. *slot receives the XInput user index, or -1 when there is no such pad.
Pad ReadPad(int nth, int* slot = nullptr);

// Background thread that renames the controller input names in the game's text table once it is in memory.
void StartNameWatcher();

} // namespace padfix
