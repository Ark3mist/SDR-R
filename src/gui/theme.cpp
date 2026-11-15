#include "gui/theme.h"

namespace osx {

namespace {
Palette g_palette{};

ImVec4 rgb(unsigned hex, float a = 1.0f) {
    return {float((hex >> 16) & 0xFF) / 255.0f, float((hex >> 8) & 0xFF) / 255.0f,
            float(hex & 0xFF) / 255.0f, a};
}
ImU32 u32(unsigned hex, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(rgb(hex, a)); }

struct ThemeDef {
    unsigned windowBg, childBg, frameBg, frameHover, frameActive;
    unsigned button, buttonHover, buttonActive;
    unsigned header, headerHover, headerActive;
    unsigned accent, text, textDim, border, plotBg, grid, gridText, trace;
};

// SDR++ "Dark": neutral dark greys with a blue accent.
constexpr ThemeDef kDark = {0x1C1C1E, 0x1C1C1E, 0x333336, 0x3D3D42, 0x47474D,
                            0x333336, 0x3F5F85, 0x4A7CB8,
                            0x2B4460, 0x3A6291, 0x4A7CB8,
                            0x4A8FDB, 0xF2F2F2, 0x8A8A8E, 0x48484C, 0x0A0A0C, 0x32343A, 0xB8B8BC, 0xD6E8FF};
constexpr ThemeDef kBlue = {0x0F1622, 0x0F1622, 0x1C2940, 0x26385A, 0x2F4672,
                            0x1C2940, 0x2B5C9E, 0x3774C8,
                            0x1E3A66, 0x2B5C9E, 0x3774C8,
                            0x4FA3FF, 0xE8EEF7, 0x7F8CA3, 0x2A3A57, 0x05080E, 0x1E2A40, 0xA9B6CC, 0x8FD0FF};
constexpr ThemeDef kLight = {0xECECEF, 0xECECEF, 0xFFFFFF, 0xE2ECF8, 0xCFE0F5,
                             0xD9D9DE, 0xB7D2F2, 0x8DB9EC,
                             0xC7DBF3, 0xB0CDEF, 0x8DB9EC,
                             0x1F6FD0, 0x151518, 0x6A6A70, 0xB4B4BA, 0xFAFAFC, 0xD0D2D8, 0x404046, 0x1F4E9C};

const ThemeDef& def(ThemeId id) {
    switch (id) {
        case ThemeId::DeepBlue: return kBlue;
        case ThemeId::Light: return kLight;
        default: return kDark;
    }
}
}  // namespace

const char* themeName(ThemeId id) {
    switch (id) {
        case ThemeId::SdrppDark: return "Dark (SDR++)";
        case ThemeId::DeepBlue: return "Deep Blue";
        case ThemeId::Light: return "Light";
        default: return "?";
    }
}

const Palette& palette() { return g_palette; }

void applyTheme(ThemeId id, float scale) {
    const ThemeDef& t = def(id);
    ImGuiStyle style;  // start from defaults so repeated calls do not compound scaling

    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = {8.0f, 8.0f};
    style.FramePadding = {6.0f, 4.0f};
    style.ItemSpacing = {8.0f, 6.0f};
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = rgb(t.text);
    c[ImGuiCol_TextDisabled] = rgb(t.textDim);
    c[ImGuiCol_WindowBg] = rgb(t.windowBg);
    c[ImGuiCol_ChildBg] = rgb(t.childBg);
    c[ImGuiCol_PopupBg] = rgb(t.windowBg, 0.98f);
    c[ImGuiCol_Border] = rgb(t.border);
    c[ImGuiCol_FrameBg] = rgb(t.frameBg);
    c[ImGuiCol_FrameBgHovered] = rgb(t.frameHover);
    c[ImGuiCol_FrameBgActive] = rgb(t.frameActive);
    c[ImGuiCol_TitleBg] = rgb(t.windowBg);
    c[ImGuiCol_TitleBgActive] = rgb(t.header);
    c[ImGuiCol_MenuBarBg] = rgb(t.windowBg);
    c[ImGuiCol_ScrollbarBg] = rgb(t.windowBg);
    c[ImGuiCol_ScrollbarGrab] = rgb(t.frameHover);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(t.frameActive);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(t.accent);
    c[ImGuiCol_CheckMark] = rgb(t.accent);
    c[ImGuiCol_SliderGrab] = rgb(t.accent);
    c[ImGuiCol_SliderGrabActive] = rgb(t.buttonActive);
    c[ImGuiCol_Button] = rgb(t.button);
    c[ImGuiCol_ButtonHovered] = rgb(t.buttonHover);
    c[ImGuiCol_ButtonActive] = rgb(t.buttonActive);
    c[ImGuiCol_Header] = rgb(t.header);
    c[ImGuiCol_HeaderHovered] = rgb(t.headerHover);
    c[ImGuiCol_HeaderActive] = rgb(t.headerActive);
    c[ImGuiCol_Separator] = rgb(t.border);
    c[ImGuiCol_SeparatorHovered] = rgb(t.accent, 0.7f);
    c[ImGuiCol_SeparatorActive] = rgb(t.accent);
    c[ImGuiCol_ResizeGrip] = rgb(t.accent, 0.25f);
    c[ImGuiCol_ResizeGripHovered] = rgb(t.accent, 0.6f);
    c[ImGuiCol_ResizeGripActive] = rgb(t.accent);
    c[ImGuiCol_Tab] = rgb(t.header);
    c[ImGuiCol_TabHovered] = rgb(t.headerHover);
    c[ImGuiCol_PlotLines] = rgb(t.trace);
    c[ImGuiCol_PlotHistogram] = rgb(t.accent);
    c[ImGuiCol_TextSelectedBg] = rgb(t.accent, 0.35f);
    c[ImGuiCol_NavHighlight] = rgb(t.accent);
    c[ImGuiCol_ModalWindowDimBg] = {0.0f, 0.0f, 0.0f, 0.5f};

    style.ScaleAllSizes(scale);
    style.FontSizeBase = 16.0f;
    style.FontScaleDpi = scale;
    ImGui::GetStyle() = style;

    g_palette.plotBg = u32(t.plotBg);
    g_palette.grid = u32(t.grid);
    g_palette.gridText = u32(t.gridText);
    g_palette.trace = u32(t.trace);
    g_palette.traceFill = u32(t.accent, 0.22f);
    g_palette.peakHold = u32(0xE8C547, 0.9f);
    g_palette.vfoCenter = u32(0xFF3B30);
    g_palette.cursor = u32(t.text, 0.25f);
    g_palette.digit = u32(t.text);
    g_palette.digitDim = u32(t.textDim, 0.55f);
    g_palette.digitHover = u32(t.accent, 0.55f);
    g_palette.icon = u32(t.text);
    g_palette.accent = u32(t.accent);
}

}  // namespace osx
