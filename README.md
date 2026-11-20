# TapBPM

Ultra-lightweight, zero-dependency single-executable Windows BPM tap utility built for DJs to verify track tempo on the fly alongside performance software (Rekordbox, Serato, Traktor, VirtualDJ, Ableton).

## Features

- **Single Portable `.exe`**: Statically linked native Win32/GDI+ x64 binary requiring no installer, runtime, or DLL downloads.
- **DJ-Safe Input**: Uses window-focused keyboard input (no global hooks), so pressing Spacebar never interferes with other running performance apps. Ignores Windows key auto-repeat and rapid-click double-click swallowing.
- **High-Precision OLS Tempo Engine**: Fits up to the last 16 taps (4 bars of 4/4) via Ordinary Least Squares linear regression against `QueryPerformanceCounter` timestamps, displaying BPM, half-time (`½×`), double-time (`2×`), RMS tap jitter (`± ms`), and a lock confidence bar.
- **Smart Auto-Hold**: Pauses > 2.5 s after 2+ taps hold the measured BPM cleanly (`HELD`); tapping again seamlessly starts a fresh count. Single accidental taps time out back to `READY`.
- **Zero Idle CPU**: Animations and hold timers shut off automatically at rest.
- **Compact Dark UI & Always-on-Top**: Floats cleanly above DJ decks with Per-Monitor DPI Awareness v2 and native dark titlebar.

## Controls

| Action | Keyboard | Mouse |
| :--- | :--- | :--- |
| **Tap Beat** | `Space` | Click main BPM card |
| **Reset** | `R`, `Esc`, or `Backspace` | Click `RST` |
| **Toggle Always-on-Top** | `T` | Click `TOP` |
| **Copy BPM** (`%.1f`) | `C` or `Ctrl+C` | Click `COPY` |

## Building

Requires Visual Studio Build Tools (MSVC x64) and Windows 10/11 SDK:

```bat
build.bat test
```

Outputs `TapBPM.exe` in the repository root and runs `build\test_bpm_engine.exe`.
