# OpenSDR-X

C++20 software-defined-radio receiver modelled on SDR++. One engine, two front ends:

| Target | Binary | Stack | Purpose |
|--------|--------|-------|---------|
| **M5Stack Cardputer Zero** (default) | `cardsdr` | SDL2 software renderer -> KMS/DRM window or raw `/dev/fb0`, evdev/SDL keyboard, ALSA | 320x170, keyboard only |
| Desktop | `opensdrx` | GLFW + OpenGL 3.3 + Dear ImGui | development rig (mouse UI, spectrum only) |

## What exists

| Area | State |
|------|-------|
| Sources: simulator, RTL-SDR (librtlsdr), rtl_tcp client | built |
| DDC: NCO -> integer decimating FIR -> channel filter -> arbitrary resampler to 48 kHz | built |
| Demodulators: WFM (stereo pilot PLL), NFM, AM, SAM (PLL), USB, LSB, CW, RAW | built |
| Squelch, AGC (fast/medium/slow + hang), de-emphasis, CTCSS detector | built |
| DC removal, Invert IQ | built |
| ALSA sink, -6 dBFS hard limiter, mixer clamp | built |
| 320x170 keyboard UI, software waterfall, 5 colormaps, 3 themes | built |
| RDS decoder, DCS | **not built** |
| IQ balance, noise blanker, spectral NR, notch / auto-notch | **not built** |
| Recorder, frequency manager / bookmarks, scanner | **not built** |
| UDP/TCP audio sink, rigctl server | **not built** |
| Multi-VFO, IQ file player, SDR++ server client | **not built** |

Drawer tabs 4-6 and the `R` / `L` keys say so on screen rather than pretending.

## Build (on the Cardputer Zero / Raspberry Pi OS, aarch64)

```bash
sudo apt install build-essential cmake ninja-build pkg-config git libsdl2-dev libfftw3-dev librtlsdr-dev libasound2-dev alsa-utils
```

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

```bash
./run_cardsdr.sh
```

On aarch64 the build uses `-O3 -march=armv8-a+simd -mtune=cortex-a53`. The desktop rig is opt-in:
`-DOPENSDRX_FRONTEND=desktop` (or `both`) and additionally needs `libglfw3-dev libglew-dev libgl1-mesa-dev`.

## Display and keyboard back ends

`cardsdr` renders everything into one 320x170 ARGB surface with SDL's software renderer, then presents it one of two ways:

| `--video` | Presentation | Keyboard | Needs |
|-----------|--------------|----------|-------|
| `sdl` | SDL window (`SDL_VIDEODRIVER=kmsdrm`, or x11/wayland for testing) | SDL key events | GBM/EGL on the panel's DRM node for KMSDRM |
| `fbdev` | direct RGB565/XRGB8888 writes to `/dev/fb0` (auto-rotates a portrait panel, `--rotate` overrides) | Linux evdev, grabbed | read access to `/dev/fb0` and `/dev/input/event*` |
| `auto` | tries `sdl`, falls back to `fbdev` | as above | |

SDL2 has no `fbcon` driver (that was SDL 1.2), which is why the framebuffer path is implemented directly.

## Keys

| Key | Drawer closed | Drawer open |
|-----|---------------|-------------|
| Left / Right | tune by the underlined digit | change the selected setting |
| Up / Down | move the tuning digit cursor | move the selection |
| 0-9 `.` | direct frequency entry in MHz, Enter tunes | 1-7 jump to a tab |
| Enter | - | toggle / run / edit text field |
| Tab | open drawer | close drawer |
| Esc or `` ` `` | cancel popup | close drawer |
| D | cycle WFM / NFM / AM / SAM / USB / LSB / CW / RAW | same |
| B / Shift+B | widen / narrow filter | same |
| G / Shift+G | RF gain up / down | same |
| S / Shift+S | squelch threshold up / down (first press arms it at the current level) | same |
| A | auto-scale FFT and waterfall to the noise floor | same |
| Z | zoom 1x / 2x / 4x centred on the VFO | same |
| Space | mute / unmute | same |
| P | play / stop | same |

Drawer tabs: `1 SOURCE`, `2 RADIO`, `3 DSP/NR`, `7 DISPLAY`. Tabs 4-6 are reserved for bookmarks, scanner and rec/net.

## Boot defaults (CM0-safe, all adjustable)

1.024 MSPS, 1024-point FFT, 30 lines/s, 30 FPS frame cap, one VFO, WFM 200 kHz.
RTL-SDR is tried first; if no dongle starts, the simulator runs and the reason is shown.

## Audio and DAC protection

Three layers keep the ES8389 away from 0 dB: `run_cardsdr.sh` sets the mixer to -6 dB with `amixer`,
`cardsdr` clamps the card's main playback control itself, and the output stage hard-limits every sample to -6 dBFS
regardless of volume (unit-tested).

## Diagnostics

```bash
./build/cardsdr --video none --source sim --frames 200 --screenshot out.bmp --keys "1,0,0,.,4,ENTER,d,TAB,2"
```

runs headless, injects keystrokes and saves the final frame.
