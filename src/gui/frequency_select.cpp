#include "gui/frequency_select.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <imgui.h>

#include "gui/theme.h"

namespace osx {

bool FrequencySelect(const char* id, double* freqHz, float digitSize) {
    constexpr int kDigits = 10;
    constexpr int64_t kMax = 9'999'999'999ll;
    const Palette& pal = palette();

    ImGui::PushFont(nullptr, digitSize);
    const float fs = ImGui::GetFontSize();
    float digitW = 0.0f;
    for (char ch = '0'; ch <= '9'; ++ch) {
        const char s[2] = {ch, 0};
        digitW = std::max(digitW, ImGui::CalcTextSize(s).x);
    }
    const float cellW = std::floor(digitW + fs * 0.12f);
    const float sepW = std::floor(ImGui::CalcTextSize(".").x + fs * 0.06f);
    const float totalW = cellW * kDigits + sepW * 3;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {totalW, fs},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    int64_t value = std::clamp<int64_t>(std::llround(*freqHz), 0, kMax);
    const int64_t original = value;

    int64_t place = 1'000'000'000ll;
    bool leading = true;
    float x = p0.x;
    for (int i = 0; i < kDigits; ++i, place /= 10) {
        const int digit = int((value / place) % 10);
        if (digit != 0 || i == kDigits - 1) leading = false;

        const ImVec2 a{x, p0.y}, b{x + cellW, p0.y + fs};
        if (hovered && mouse.x >= a.x && mouse.x < b.x) {
            const bool upper = mouse.y < p0.y + fs * 0.5f;
            const float mid = p0.y + fs * 0.5f;
            dl->AddRectFilled(upper ? a : ImVec2{a.x, mid}, upper ? ImVec2{b.x, mid} : b,
                              pal.digitHover, 2.0f);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) value += upper ? place : -place;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) value -= value % place;
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) value += int64_t(std::lround(wheel)) * place;
        }

        const char s[2] = {char('0' + digit), 0};
        const float tw = ImGui::CalcTextSize(s).x;
        dl->AddText({x + (cellW - tw) * 0.5f, p0.y}, leading ? pal.digitDim : pal.digit, s);
        x += cellW;

        if (i == 0 || i == 3 || i == 6) {
            dl->AddText({x + fs * 0.03f, p0.y}, leading ? pal.digitDim : pal.digit, ".");
            x += sepW;
        }
    }
    ImGui::PopFont();

    value = std::clamp<int64_t>(value, 0, kMax);
    if (value != original) {
        *freqHz = double(value);
        return true;
    }
    return false;
}

}  // namespace osx
