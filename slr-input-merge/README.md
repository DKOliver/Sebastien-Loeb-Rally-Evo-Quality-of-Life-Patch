# SLR EVO wheel + pedals merge (DirectInput proxy) - DRAFT

Lets Sebastien Loeb Rally EVO use a wheel and a separate set of pedals (e.g. Thrustmaster
TS-PC + Fanatec ClubSport V3 over USB) as one device, with force feedback going to the wheel.

**Status: untested draft.** It was syntax-checked against hand-written stand-in headers, never
compiled with the real Windows SDK and never run with real hardware. Expect to fix a compile
error or two and to tune the axis mapping.

## Files

| File | Purpose |
|---|---|
| `dinput8.cpp` | the proxy DLL source |
| `dinput8.def` | exports `DirectInput8Create` (forwarded to our implementation) |
| `dinput8_merge.ini` | settings: which devices, axis mapping, optional ID spoof |

## Build (64-bit, for SLRX64.exe)

Visual Studio Build Tools, in an **x64 Native Tools Command Prompt**:

```
cl /nologo /LD /O2 /EHsc /MT dinput8.cpp /link /DEF:dinput8.def /OUT:dinput8.dll user32.lib
```

MinGW-w64:

```
x86_64-w64-mingw32-g++ -O2 -shared -static -static-libgcc -static-libstdc++ -o dinput8.dll dinput8.cpp dinput8.def -luser32
```

If you launch the 32-bit `SLR.exe` instead, build the same source as 32-bit (x86 prompt / i686 MinGW).
The engine is already loaded by whichever exe you start, so match the bitness of that exe.

## Install / uninstall

1. Copy `dinput8.dll` and `dinput8_merge.ini` next to `SLRX64.exe` in the game folder.
2. Start the game once, then quit.
3. Open `dinput8_merge.log` (same folder). Uninstall = delete `dinput8.dll`.

Do not touch DATA.MIX for this. Use it **offline**; I have not checked what the online mode does
with a replaced DLL.

## First run: read the log

- `dinput8 merge proxy starting` means the game loaded the proxy. If the log file never appears,
  the game isn't loading it (wrong bitness, wrong folder, or DLL failed to load).
- The `device:` lines list every controller the game can see, with product name and vid:pid.
  If `MERGE ACTIVE` is missing, change `WheelMatch` / `PedalsMatch` in the .ini to words that
  appear in those product names.

## Mapping the pedals

With `Debug=1` the log prints one `state` line per second, e.g.

```
state  wheel[ X=32768 Y=0 Z=0 ... ]  pedals[ X=0 Y=65535 Z=0 ... ]
```

Press one pedal at a time and see which `pedals[...]` axis moves. Then set the lines in `[Map]`:
`MapN=PedalAxis,GameAxis,Invert`. The default `GameAxis` values (throttle=Rz, brake=Y,
clutch=Slider0) match the T300RS layout in the game's data and the edited
`WHEELGAMESCRIPT_DEFAULT.TML` I gave you earlier. The default `PedalAxis` values are guesses.

If a pedal works backwards, flip its `Invert`. If the wheel's own axes also move when you press
pedals, that's fine: mapped axes are overwritten with the pedal values.

## Which profile does the game use?

- `[Spoof] Enabled=0` (default): the game sees a wheel it doesn't know, so it should use its
  default wheel script. That script binds throttle/brake to buttons unless you edit it
  (use the edited `WHEELGAMESCRIPT_DEFAULT.TML`, which means repacking DATA.MIX).
- `[Spoof] Enabled=1`: the wheel is presented with the vendor/product ID and name in the .ini.
  If the game keys its profile off those, it should load the T300RS script with no DATA.MIX
  edit. The IDs in the .ini are from memory: check them against the real device first.
  This is the experimental part.

## Virtual buttons (pause etc.)

The `[Buttons]` section of the .ini makes the game see a game button as pressed when a keyboard
key or another wheel button is down. Example: `Button1=Key:ESC,7` makes ESC act as the game's
button 7 (pause). With `Debug=1` the log prints `wheel button N DOWN` for each wheel button
you press, which tells you which number to use for `Wheel:N`. The in-game label stays "BUTT 7";
only the behaviour changes.

## Known limits and things most likely to need work

- Only `IDirectInput8W` is wrapped (that's what the engine references in code). Other interfaces
  are passed through unmerged and logged.
- Buffered input (`GetDeviceData`) is not merged; only `GetDeviceState`. The log warns if the
  game uses it.
- Pedal ranges are fixed at 0..65535 and scaled to whatever range the game sets on the wheel.
- If the pedals fail to open, the game just gets the plain wheel (log says why).
- Force feedback calls are passed to the wheel untouched; whether the TS-PC gets good FFB when
  the game doesn't recognise it is unknown until tested.

## If it doesn't work

Send the contents of `dinput8_merge.log` and the exact compiler errors, if any. Those two things
tell us almost everything.
