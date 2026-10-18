# Split/Second PadFix: wireless Xbox controllers for Split/Second (PC)

Split/Second (Black Rock Studio, 2010) only understands one kind of controller properly: a **wired Xbox 360 pad**.
Connect a modern Xbox controller over Bluetooth (Xbox One, Series X|S, Elite Series 2) and the game either reads it
wrong or reads it fine and ignores every press. PadFix is a small drop-in DLL that makes any Microsoft controller
look exactly like a wired Xbox 360 pad to the game, so it works wirelessly with the game's own Xbox controls.

- No game files are modified. Delete the two DLLs and the game is back to stock.
- Keyboard and mouse are untouched.
- Works alongside [SplitSecond-HFR](https://github.com/happyc0der/SplitSecond-HFR) (60 FPS) and
  [SplitSecond-ButtonNames](https://github.com/happyc0der/SplitSecond-ButtonNames) (Xbox button names in prompts).

## Status

Version 1.0.0. Tested with an Xbox Elite Series 2 over Bluetooth LE on Windows 11, in menus and while driving, on the
community DLC build of the game. The Steam release (1.0.0.1) has the same controller code (the same product table and
checks described below), and PadFix doesn't patch the game at all, so it should behave the same there; reports welcome.

## Install

1. Download `SplitSecond-PadFix-v1.0.0.zip` from [Releases](../../releases).
2. Copy `dinput8.dll`, `xinput1_3.dll` and `padfix.ini` into the game folder
   (Steam → right-click Split/Second → Manage → Browse local files), next to `SplitSecond.exe`.
3. Connect the controller (Bluetooth is fine), then start the game.

If the controller worked before as an unknown device, the game may have saved an empty layout for it: open
**Options → Controls** and reset the controller to its defaults once.

**Uninstall:** delete `dinput8.dll`, `xinput1_3.dll`, `padfix.ini` and `padfix.log` from the game folder.

## Configuration (`padfix.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Translate` | 1 | Build the controller's reading from XInput in the wired Xbox 360 layout. 0 = pass it through untouched. |
| `PretendWired360` | 1 | Introduce the controller to the game as a wired Xbox 360 pad (`045E:028E`), the only way the game gives it the Xbox default controls. |
| `SwapTriggers` | 0 | Swap LT and RT on the shared trigger axis, in case accelerate and brake come out reversed. |
| `Verbose` | 1 | Log the game's controller calls and the first 60 input changes to `padfix.log`. |

## Why the controller doesn't work

Three things stack up:

1. **The game reads controllers through DirectInput, not XInput.** Its only XInput import is `XInputSetState`
   (rumble). Every button press and stick movement is read with `IDirectInputDevice8::GetDeviceState` into a
   `DIJOYSTATE2`.
2. **The DirectInput layout of a Bluetooth Xbox controller is not the Xbox 360 one.** A wired 360 pad (the `xusb`
   driver) reports X/Y = left stick, Z = both triggers on one axis, Rx/Ry = right stick, buttons 0-9 = A B X Y LB RB
   Back Start LS RS and POV 0 = D-pad. A Bluetooth Xbox controller is a HID device with its own layout (separate
   triggers, the right stick on other axes, a different button order), so the game's Xbox mapping lands on the wrong
   inputs.
3. **The game picks default controls by exact product ID.** For every controller it compares DirectInput's
   `guidProduct` with a hard-coded table: `045E:028E` (wired Xbox 360) and `045E:02A1` (Xbox 360 wireless receiver)
   get the Xbox control map. Thirteen Logitech, Saitek, Thrustmaster and other wheels and pads get their own maps.
   Anything else gets a generic type with **no default bindings**. An Elite Series 2 over Bluetooth is `045E:0B22`, so even with
   the right layout, every press was read and then mapped to nothing.

The game also runs Microsoft's well-known `IsXInputDevice` WMI check (device IDs containing `IG_`), but it only
stores the result in a per-device flag that nothing reads, so it doesn't matter here.

## How PadFix fixes it

`dinput8.dll` is a proxy for the system DirectInput (Windows loads DLLs from the game folder first). It forwards
everything to the real `dinput8.dll` in the system folder, except for game controllers made by Microsoft (VID `045E`):

- **Identity:** in `EnumDevices` and `GetDeviceInfo` the controller is reported as
  `Controller (XBOX 360 For Windows)` with product GUID `{028E045E-0000-0000-0000-504944564944}`, so the game assigns
  it the Xbox 360 control map.
- **Input:** `GetDeviceState` (and `GetDeviceData`, if a game uses buffered input) is answered from
  `XInputGetState` (xinput1_4), written in the exact wired-360 DirectInput layout. Values honour the ranges, dead
  zones and saturation the game sets per axis. Axes, POVs and buttons a 360 pad doesn't have read 0 or centred, as
  they would on the real thing.
- **Everything else** (acquire, cooperative level, effects, properties) still goes to the real device, so focus
  handling and identity checks behave normally.

`xinput1_3.dll` exports the same functions and ordinals as the DirectX-redist `xinput1_3` and forwards them to the
system's `xinput1_4`, so the game's rumble calls reach the modern XInput stack. It also counts the calls for the log.

## Logs and troubleshooting

`padfix.log` (next to the DLLs, readable while the game runs) records each session: the settings, the devices the
game enumerates, the data format and properties it sets, and with `Verbose=1` the first 60 input changes with the
values the game was given, for example:

```
[di]   enum: "Bluetooth LE XINPUT compatible input device" type 00010215 product 0B22045E -> shown to the game as a wired Xbox 360 pad (045E:028E)
[di] Bluetooth LE XINPUT compatible input device SetDataFormat: 272 bytes, 164 objects, flags 00000001 (real hr 00000000)
[pad] buttons 1000 LT   0 RT   0 L(  -299,   228) R( -1297,   429) -> game sees X   -299 Y   -228 Z      0 POV -1
```

- **No `enum:` line for the controller:** Windows doesn't see it as a game controller. Check it in `joy.cpl`.
- **`XInput reports no pad connected`:** the controller went to sleep or disconnected. Press the Xbox button.
- **Presses show in the log but do nothing in game:** reset the controller's controls in Options → Controls.
- **Accelerate and brake reversed:** set `SwapTriggers=1`.

## Building

Requirements: Visual Studio 2022 or 2026 (or the Build Tools) with the C++ workload and a Windows 10/11 SDK, plus
CMake 3.25 or newer. The game is 32-bit, so build for Win32 with a Visual Studio generator:

```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

(Use `-G "Visual Studio 18 2026"` for Visual Studio 2026.)

This produces `build/Release/dinput8.dll`, `xinput1_3.dll` and `padtest.exe`. `padtest.exe <path to dinput8.dll>
[seconds]` loads a proxy the same way the game does and prints what a game would read (identity, axes, POVs,
buttons), so you can check a build without starting the game.

## License

GNU General Public License v3.0, see [LICENSE](LICENSE). Not affiliated with Disney Interactive, Black Rock Studio or
Microsoft. Split/Second and Xbox are trademarks of their respective owners.
