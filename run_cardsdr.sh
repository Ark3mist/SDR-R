#!/bin/sh
# OpenSDR-X launcher for the M5Stack Cardputer Zero (320x170 SPI panel,
# TCA8418 keyboard, ES8389 DAC). Runs without X11/Wayland.
#
#   ./run_cardsdr.sh [extra cardsdr options]
#
# Environment overrides:
#   CARDSDR_BIN     path to the cardsdr binary
#   CARDSDR_VIDEO   kmsdrm | fbdev | auto   (default: auto-detect)
#   CARDSDR_AUDIO   ALSA playback device    (default: plughw:0,0)
#   CARDSDR_CARD    ALSA card index for the mixer clamp (default: 0)
#   CARDSDR_FBDEV   framebuffer device      (default: /dev/fb0)
set -u

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${CARDSDR_BIN:-}
if [ -z "$BIN" ]; then
    for c in "$HERE/cardsdr" "$HERE/build/cardsdr" /usr/local/bin/cardsdr; do
        [ -x "$c" ] && BIN=$c && break
    done
fi
[ -n "$BIN" ] || { echo "cardsdr binary not found (set CARDSDR_BIN)" >&2; exit 1; }

AUDIO=${CARDSDR_AUDIO:-plughw:0,0}
CARD=${CARDSDR_CARD:-0}
FBDEV=${CARDSDR_FBDEV:-/dev/fb0}
VIDEO=${CARDSDR_VIDEO:-auto}

# --- ES8389 protection: the DAC clips / drops out at 0 dB, cap playback at -6 dB.
# Control names differ between kernel builds, so try the usual ones; cardsdr
# also clamps the mixer itself and hard-limits its digital output to -6 dBFS.
if command -v amixer >/dev/null 2>&1; then
    for ctl in DAC Master PCM Headphone Speaker; do
        if amixer -c "$CARD" sget "$ctl" >/dev/null 2>&1; then
            amixer -q -c "$CARD" sset "$ctl" -- -6dB unmute 2>/dev/null \
                || amixer -q -c "$CARD" sset "$ctl" -- -6dB 2>/dev/null
            echo "mixer: '$ctl' set to -6 dB on card $CARD"
            break
        fi
    done
fi

# --- Display: prefer DRM/KMS, fall back to the raw framebuffer.
# (SDL2 has no "fbcon" driver; cardsdr writes to /dev/fb0 itself in fbdev mode.)
if [ "$VIDEO" = auto ]; then
    if [ -e /dev/dri/card0 ]; then VIDEO=kmsdrm; else VIDEO=fbdev; fi
fi

# Keep the text console from drawing over the UI, restore it on exit.
restore() {
    [ -w /dev/tty1 ] && printf '\033[?25h' > /dev/tty1 2>/dev/null
    [ -w /sys/class/graphics/fbcon/cursor_blink ] && echo 1 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null
}
trap restore EXIT INT TERM
[ -w /dev/tty1 ] && printf '\033[?25l\033[2J' > /dev/tty1 2>/dev/null
[ -w /sys/class/graphics/fbcon/cursor_blink ] && echo 0 > /sys/class/graphics/fbcon/cursor_blink 2>/dev/null

if [ "$VIDEO" = kmsdrm ]; then
    SDL_VIDEODRIVER=kmsdrm "$BIN" --video sdl --audio "$AUDIO" "$@"
    rc=$?
    # KMSDRM needs GBM/EGL on the panel's DRM node; SPI "tiny" DRM drivers often lack it.
    if [ $rc -ne 0 ] && [ -e "$FBDEV" ]; then
        echo "kmsdrm failed (rc=$rc), retrying on $FBDEV" >&2
        "$BIN" --video fbdev --fbdev "$FBDEV" --audio "$AUDIO" "$@"
        rc=$?
    fi
else
    "$BIN" --video fbdev --fbdev "$FBDEV" --audio "$AUDIO" "$@"
    rc=$?
fi
exit $rc
