# OpenSDR-X

Cross-platform C++20 software-defined-radio receiver modelled on the SDR++ layout:
GLFW + OpenGL 3.3 + Dear ImGui front end, FFTW3 display FFT, lock-free threaded I/Q pipeline.

## Status

| Phase | Scope | State |
|-------|-------|-------|
| 1 | Window, theme, toolbar, FFT + GPU waterfall, simulated source, VFO interaction | **done** |
| 2 | librtlsdr / rtl_tcp sources, multi-VFO polyphase DDC, demodulators, RtAudio | pending |
| 3 | Noise blanker / NR, recorder, frequency manager, scanner | pending |
| 4 | Network sinks, rigctld server | pending |

Phase 1 produces no audio. The volume slider and mute button hold state only until the Phase 2 audio sink exists.

## Build

Linux (Debian/Ubuntu):

```bash
sudo apt install build-essential cmake ninja-build pkg-config libglfw3-dev libglew-dev libfftw3-dev libgl1-mesa-dev
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
./build/opensdrx --autostart
```

Dear ImGui (docking branch, pinned tag) is fetched by CMake. GLFW falls back to FetchContent when no system package is found.
macOS and Windows code paths exist (forward-compat GL context, MSVC flags) but have **not** been built or tested yet.

## Threads (Phase 1)

| Thread | Owner | Work |
|--------|-------|------|
| I/O | `SimSource` | Generates interleaved uint8 I/Q in real time, writes to an SPSC lock-free ring |
| DSP | `DspEngine` | uint8 -> `std::complex<float>`, IQ invert, DC-spike removal, feeds the spectrum tap ring |
| FFT worker | `SpectrumAnalyzer` | Sliding history, window, `fftwf_plan_dft_1d`, publishes dBFS lines |
| UI | `main` / `App` | 60 FPS ImGui frame, `glTexSubImage2D` ring-texture waterfall, shader colormap/scroll/zoom |

The audio-sink thread arrives with Phase 2.

## Controls

| Action | Input |
|--------|-------|
| Tune VFO | Click or drag in the FFT / waterfall; scroll wheel = one snap step |
| Filter bandwidth | Drag either passband edge |
| Retune hardware centre | Drag the frequency scale |
| Zoom / pan | Ctrl + wheel (about the cursor), middle-drag, or the Zoom strip |
| dB range | Drag the dB scale, or the Max / Min strip |
| Pane split | Drag the bar between FFT and waterfall |
| Frequency digits | Click upper/lower half = +/-, wheel = +/-, right-click = zero lower digits |
| Play / stop | Toolbar button or Space |

## Diagnostics

`--frames N --screenshot out.ppm` renders N frames, prints frame timing and saves the final frame; usable headless under `xvfb-run`.
