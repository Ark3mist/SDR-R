#pragma once

#include <imgui.h>

namespace osx {

enum class ThemeId { SdrppDark = 0, DeepBlue, Light, Count };

// Colours used by the custom-drawn widgets (spectrum, frequency display, ...).
struct Palette {
    ImU32 plotBg, grid, gridText;
    ImU32 trace, traceFill, peakHold;
    ImU32 vfoCenter, cursor;
    ImU32 digit, digitDim, digitHover;
    ImU32 icon, accent;
};

const char* themeName(ThemeId id);

// Rebuilds the whole ImGui style for `id` at the given total scale
// (monitor DPI scale x user UI scale).
void applyTheme(ThemeId id, float scale);

const Palette& palette();

}  // namespace osx
