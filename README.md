# OpenSDR-X

C++20 software-defined-radio receiver modelled on SDR++. One engine, two front ends:

| Target | Binary | Stack | Purpose |
|--------|--------|-------|---------|
| **M5Stack Cardputer Zero** (default) | `cardsdr` | SDL2 software renderer -> KMS/DRM or raw `/dev/fb0`, evdev/SDL keyboard, ALSA | 320x170, keyboard only |
| Desktop | `opensdrx` | GLFW + OpenGL 3.3 + Dear ImGui | development rig (spectrum only) |

## What exists

| Area | State |
|------|-------|
| Sources: RTL-SDR (async, 15 x <=256 KiB USB blocks), rtl_tcp, IQ WAV player (u8/s16/f32, loop, seek), simulator | built |
| Unplug handling: RTL-SDR lost or absent -> simulator, re-detected every 2 s | built |
| Conditioning: DC blocker (`y = x - x1 + 0.9995 y1`), blind I/Q balance, Invert IQ | built |
| DDC: NCO -> polyphase decimating FIR -> channel filter -> polyphase resampler to 48 kHz | built |
| Demodulators: WFM (stereo PLL + **RDS**), NFM (+ CTCSS), AM, SAM, USB, LSB, DSB, CW, RAW | built |
| Interference: noise blanker, 64-tap NLMS auto-notch, 256-pt spectral NR; `N` presets | built |
| Audio: ALSA (1024-frame periods, XRUN recovery, `default` fallback) or SDL2; log master volume; -6 dBFS limiter; mixer locked at -6 dB | built |
| Bookmarks: JSON, quick-save + rename, recall restores freq/mode/bandwidth, spectrum tags | built |
| Recorder, scanner, network audio sink, rigctl, multi-VFO | **not built** (Phase 3/4) |

Tabs 5 and 6 and the `R` / `L` keys say so on screen.

## Build (Raspberry Pi OS / Debian, aarch64)

```bash
sudo apt install build-essential cmake ninja-build pkg-config git libsdl2-dev libfftw3-dev librtlsdr-dev libasound2-dev nlohmann-json3-dev alsa-utils
```

```bash
cmake -B build
```

```bash
cmake --build build -j4
```

```bash
ctest --test-dir build --output-on-failure
```

```bash
./run_cardsdr.sh
```

aarch64 builds use `-O3 -march=armv8-a+simd -mtune=cortex-a53`; the DSP library adds `-ffast-math` so its FIR loops vectorise to NEON.
macOS: `OPENSDRX_AUDIO` defaults to `sdl` (`brew install sdl2 fftw librtlsdr nlohmann-json`); not yet built or tested on a Mac.
Desktop rig: `-DOPENSDRX_FRONTEND=desktop` (or `both`) plus `libglfw3-dev libglew-dev libgl1-mesa-dev`.

## Keys (no Fn needed)

| Key | Drawer closed | Drawer open |
|-----|---------------|-------------|
| Left/Right, or `,` `/` | tune by the underlined digit | change the selected setting |
| Up/Down | step cursor coarser / finer (10 Hz - 100 kHz) | move the selection |
| `;` | step cursor coarser, wraps | move up |
| `.` | starts frequency entry | move down |
| `0-9` | frequency entry in MHz, Enter tunes | `1`-`7` jump to a tab |
| `[` `]` | - | previous / next tab |
| Tab | open drawer | close drawer |
| Esc or `` ` `` | cancel a box | close drawer |
| `D` | cycle WFM NFM AM SAM USB LSB DSB CW RAW | same |
| `K` or Ctrl+S | save frequency + mode + bandwidth, then rename | same |
| `N` | interference preset: Off, NB, NB+Notch, Full NR | same |
| `+` `=` / `-` `_` | volume +/- 5 % (with OSD) | same |
| `V` then Left/Right | volume | same |
| `B` / Shift+B | widen / narrow filter | same |
| `G` / Shift+G | RF gain up / down | same |
| `S` / Shift+S | squelch up / down (first press arms it) | same |
| `A`, `Z`, `P`, Space | auto-range, zoom 1x/2x/4x, play/stop, mute | same |

Bookmarks tab: Enter recalls (frequency, mode, bandwidth), `E` renames, Backspace/Delete deletes after
confirmation, Left/Right moves it to another category. While any text box is open, no hotkey fires.

Bookmarks live in `~/.config/cardsdr/bookmarks.json` (`$XDG_CONFIG_HOME` honoured), are written atomically,
and a corrupt file is moved aside to `bookmarks.json.bad`. Defaults are generic samples; edit them.

## Display and keyboard back ends

| `--video` | Presentation | Keyboard |
|-----------|--------------|----------|
| `sdl` | SDL window (`SDL_VIDEODRIVER=kmsdrm`, or x11/wayland for testing) | SDL key events |
| `fbdev` | direct RGB565/XRGB8888 writes to `/dev/fb0`, auto-rotates a portrait panel | Linux evdev (grabbed) |
| `auto` | tries `sdl`, falls back to `fbdev` | as above |

## Audio and DAC protection

Three layers keep the ES8389 below 0 dB. `run_cardsdr.sh` sets the mixer to -6 dB with `amixer`.
`cardsdr` locks the card's main playback control at -6 dB itself. The output stage hard-limits every sample
to -6 dBFS. Master volume is logarithmic (100 % = 0 dB, 75 % = -10 dB, 40 dB span) and ramped to avoid clicks.

## Verification (all automated, `ctest`)

Each line is checked against the simulator's RF environment:

* **RDS** decodes PI, PTY, PS and RadioText at 0.25, 1.024, 1.536 and 2.048 MSPS, with no false sync on a station that has no RDS.
* **WFM stereo** separation exceeds 23 dB.
* **CTCSS** decodes at 100.0 Hz.
* **SSB** opposite-sideband rejection exceeds 25 dB.
* **I/Q balance** raises image rejection from 23 to 43 dB.
* **Noise blanker** cuts impulse noise by 11 dB.
* **Auto-notch** removes 60 dB of carrier with 0.2 dB broadband change.
* **NR** removes 10 dB of hiss and keeps a keyed tone.
* **Real-time safety**: zero heap allocations in the DSP loop across all 9 modes with NB, notch and NR on.
* **IQ files** play through the full threaded chain.
* **rtl_tcp** works against a mock server.
* **Keyboard**: focus lock, bookmark save/rename/recall/delete, tab and volume keys.

## Diagnostics

```bash
./build/cardsdr --video none --source sim --frames 200 --screenshot out.bmp --keys "TAB,4,.,.,ENTER"
```
