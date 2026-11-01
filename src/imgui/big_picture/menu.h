// SPDX-FileCopyrightText: Copyright 2026 ForkStation
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <imgui.h>

/// Menus in the look of a console's settings: a slate backdrop with a soft beam of light, a title
/// at the top left and a list of rows (icon, label, value) with hairline separators; the chosen
/// row is outlined. The icons are drawn with lines, no artwork.
namespace BigPictureMode::Menu {

enum class Icon { None, Play, Update, Info, Close, Screen, Folder, Gear, Check, Fork, Clock, Chip };

inline void Waves(ImDrawList* dl, ImVec2 o, ImVec2 size, float s, float strength);

inline void Backdrop(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    dl->AddRectFilledMultiColor(min, max, IM_COL32(46, 48, 56, 255), IM_COL32(30, 32, 39, 255),
                                IM_COL32(15, 16, 20, 255), IM_COL32(20, 21, 26, 255));
    const float w = max.x - min.x, h = max.y - min.y;
    const ImVec2 light{min.x + w * 0.30f, min.y - h * 0.10f};
    for (int i = 0; i < 80; ++i) { // many faint discs: a smooth glow, no rings
        dl->AddCircleFilled(light, h * (0.08f + 0.0105f * i), IM_COL32(255, 238, 215, 2), 96);
    }
    dl->AddRectFilledMultiColor({min.x, min.y + h * 0.7f}, max, IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90));
    Waves(dl, min, {w, h}, std::clamp(w / 1500.0f, 0.6f, 2.6f), 0.55f);
}

/// Waves of light: ribbons of thin lines that undulate slowly across the screen, over whatever is
/// behind them. `strength` scales their brightness (0..1).
inline void Waves(ImDrawList* dl, ImVec2 o, ImVec2 size, float s, float strength) {
    const float time = static_cast<float>(ImGui::GetTime());
    struct Ribbon {
        float base, amp, freq, speed, phase;
        ImU32 rgb;
    };
    static constexpr Ribbon ribbons[] = {
        {0.62f, 0.07f, 1.6f, 0.22f, 0.0f, IM_COL32(120, 160, 255, 0)},
        {0.68f, 0.05f, 2.3f, -0.16f, 1.7f, IM_COL32(170, 130, 255, 0)},
        {0.56f, 0.06f, 1.2f, 0.12f, 3.1f, IM_COL32(255, 210, 160, 0)},
    };
    constexpr int lines = 22;
    constexpr int points = 96;
    for (const auto& r : ribbons) {
        for (int l = 0; l < lines; ++l) {
            const float k = static_cast<float>(l) / (lines - 1);
            const float spread = (k - 0.5f) * 0.09f;
            // Brightest in the middle of the ribbon.
            const float a = (1.0f - std::abs(k - 0.5f) * 2.0f) * 26.0f * strength + 4.0f * strength;
            const ImU32 col = (r.rgb & 0x00FFFFFF) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
            ImVec2 prev{};
            for (int p = 0; p <= points; ++p) {
                const float u = static_cast<float>(p) / points;
                const float wave =
                    std::sin(u * r.freq * 6.2832f + time * r.speed * 6.2832f + r.phase + k * 0.9f) *
                        r.amp +
                    std::sin(u * 9.0f + time * 0.4f + r.phase) * r.amp * 0.18f;
                const ImVec2 pt{
                    o.x + u * size.x,
                    o.y + size.y * (r.base + wave +
                                    spread * (0.6f + 0.4f * std::sin(u * 3.0f + time * 0.2f)))};
                if (p > 0) {
                    dl->AddLine(prev, pt, col, 1.3f * s);
                }
                prev = pt;
            }
        }
    }
}

inline void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float r, ImU32 col) {
    const float t = std::max(1.5f, r * 0.16f);
    switch (icon) {
    case Icon::Play:
        dl->AddTriangleFilled({c.x - r * 0.55f, c.y - r * 0.75f},
                              {c.x - r * 0.55f, c.y + r * 0.75f}, {c.x + r * 0.8f, c.y}, col);
        break;
    case Icon::Update:
        dl->PathArcTo(c, r * 0.78f, 0.6f, 5.6f, 24);
        dl->PathStroke(col, 0, t);
        dl->AddTriangleFilled({c.x + r * 0.95f, c.y - r * 0.05f},
                              {c.x + r * 0.35f, c.y - r * 0.15f},
                              {c.x + r * 0.78f, c.y + r * 0.55f}, col);
        break;
    case Icon::Info:
        dl->AddCircle(c, r * 0.85f, col, 32, t);
        dl->AddLine({c.x, c.y - r * 0.05f}, {c.x, c.y + r * 0.45f}, col, t * 1.1f);
        dl->AddCircleFilled({c.x, c.y - r * 0.38f}, t * 0.75f, col);
        break;
    case Icon::Close:
        dl->AddLine({c.x - r * 0.6f, c.y - r * 0.6f}, {c.x + r * 0.6f, c.y + r * 0.6f}, col,
                    t * 1.2f);
        dl->AddLine({c.x + r * 0.6f, c.y - r * 0.6f}, {c.x - r * 0.6f, c.y + r * 0.6f}, col,
                    t * 1.2f);
        break;
    case Icon::Screen:
        dl->AddRect({c.x - r * 0.9f, c.y - r * 0.6f}, {c.x + r * 0.9f, c.y + r * 0.45f}, col,
                    r * 0.12f, 0, t);
        dl->AddLine({c.x - r * 0.4f, c.y + r * 0.75f}, {c.x + r * 0.4f, c.y + r * 0.75f}, col, t);
        break;
    case Icon::Folder:
        dl->AddRectFilled({c.x - r * 0.9f, c.y - r * 0.55f}, {c.x - r * 0.1f, c.y - r * 0.25f}, col,
                          r * 0.1f);
        dl->AddRectFilled({c.x - r * 0.9f, c.y - r * 0.35f}, {c.x + r * 0.9f, c.y + r * 0.65f}, col,
                          r * 0.14f);
        break;
    case Icon::Gear:
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159265f / 4.0f;
            dl->AddLine({c.x + std::cos(a) * r * 0.55f, c.y + std::sin(a) * r * 0.55f},
                        {c.x + std::cos(a) * r * 0.9f, c.y + std::sin(a) * r * 0.9f}, col,
                        t * 1.6f);
        }
        dl->AddCircle(c, r * 0.55f, col, 24, t * 1.3f);
        break;
    case Icon::Check:
        dl->AddLine({c.x - r * 0.65f, c.y}, {c.x - r * 0.15f, c.y + r * 0.5f}, col, t * 1.3f);
        dl->AddLine({c.x - r * 0.15f, c.y + r * 0.5f}, {c.x + r * 0.7f, c.y - r * 0.5f}, col,
                    t * 1.3f);
        break;
    case Icon::Fork:
        dl->AddLine({c.x, c.y + r * 0.7f}, {c.x, c.y - r * 0.7f}, col, t);
        dl->AddBezierCubic({c.x, c.y + r * 0.1f}, {c.x, c.y - r * 0.3f},
                           {c.x - r * 0.7f, c.y - r * 0.25f}, {c.x - r * 0.7f, c.y - r * 0.7f}, col,
                           t);
        dl->AddBezierCubic({c.x, c.y + r * 0.1f}, {c.x, c.y - r * 0.3f},
                           {c.x + r * 0.7f, c.y - r * 0.25f}, {c.x + r * 0.7f, c.y - r * 0.7f}, col,
                           t);
        for (const float dx : {-0.7f, 0.0f, 0.7f}) {
            dl->AddCircleFilled({c.x + r * dx, c.y - r * 0.75f}, t * 1.3f, col);
        }
        dl->AddCircleFilled({c.x, c.y + r * 0.75f}, t * 1.5f, col);
        break;
    case Icon::Clock:
        dl->AddCircle(c, r * 0.85f, col, 32, t);
        dl->AddLine(c, {c.x, c.y - r * 0.5f}, col, t);
        dl->AddLine(c, {c.x + r * 0.4f, c.y + r * 0.1f}, col, t);
        break;
    case Icon::Chip:
        dl->AddRect({c.x - r * 0.55f, c.y - r * 0.55f}, {c.x + r * 0.55f, c.y + r * 0.55f}, col,
                    r * 0.1f, 0, t);
        for (const float d : {-0.25f, 0.25f}) {
            dl->AddLine({c.x + r * d, c.y - r * 0.85f}, {c.x + r * d, c.y - r * 0.55f}, col, t);
            dl->AddLine({c.x + r * d, c.y + r * 0.55f}, {c.x + r * d, c.y + r * 0.85f}, col, t);
            dl->AddLine({c.x - r * 0.85f, c.y + r * d}, {c.x - r * 0.55f, c.y + r * d}, col, t);
            dl->AddLine({c.x + r * 0.55f, c.y + r * d}, {c.x + r * 0.85f, c.y + r * d}, col, t);
        }
        break;
    case Icon::None:
        break;
    }
}

/// One row of the list at (x, y), `w` by `h`. `selected` outlines it (the pad's place); the mouse
/// over it lights it. Pressing it returns true. Rows without an action pass `interactive` false.
inline bool Row(const char* id, Icon icon, const char* label, const char* value, float x, float y,
                float w, float h, bool selected, bool interactive = true) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool pressed = false;
    bool hovered = false;
    if (interactive) {
        ImGui::SetCursorScreenPos({x, y});
        pressed = ImGui::InvisibleButton(id, {w, h});
        hovered = ImGui::IsItemHovered();
        selected = selected || ImGui::IsItemFocused();
    }
    const float s = h / 64.0f;
    const ImVec2 min{x, y}, max{x + w, y + h};
    if (selected || hovered) {
        dl->AddRectFilled(min, max, IM_COL32(255, 255, 255, selected ? 22 : 12), 6.0f * s);
        if (selected) {
            dl->AddRect(min, max, IM_COL32(225, 228, 236, 235), 6.0f * s, 0, 2.2f * s);
        }
    }
    const float text_x = x + 64.0f * s;
    DrawIcon(dl, icon, {x + 32.0f * s, y + h * 0.5f}, 13.0f * s, IM_COL32(240, 242, 246, 255));
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText({text_x, y + (h - ls.y) * 0.5f}, IM_COL32(240, 242, 246, 255), label);
    if (value && *value) {
        const ImVec2 vs = ImGui::CalcTextSize(value);
        dl->AddText({max.x - 24.0f * s - vs.x, y + (h - vs.y) * 0.5f}, IM_COL32(255, 255, 255, 150),
                    value);
    }
    if (!selected) {
        dl->AddLine({text_x, max.y - 0.5f}, {max.x - 14.0f * s, max.y - 0.5f},
                    IM_COL32(255, 255, 255, 30), 1.0f);
    }
    return pressed;
}

/// The pad the player holds, for the buttons the hints show.
enum class PadKind { Keyboard, PlayStation, Xbox };
enum class PadButton { Confirm, Back, Action, Extra, Shoulders, Options };

/// A pad button as the player's pad prints it: PlayStation shapes in their colours, Xbox letters,
/// or a keyboard key without a pad.
inline void DrawPadButton(ImDrawList* dl, PadKind kind, PadButton button, ImVec2 c, float r) {
    if (kind == PadKind::Keyboard) {
        const char* key = button == PadButton::Options   ? "Menú"
                          : button == PadButton::Confirm ? "Enter"
                          : button == PadButton::Back    ? "Esc"
                          : button == PadButton::Action  ? "U"
                          : button == PadButton::Extra   ? "S"
                                                         : "Tab";
        const ImVec2 ts = ImGui::CalcTextSize(key);
        const float w = std::max(r * 2.0f, ts.x + r);
        dl->AddRect({c.x - w * 0.5f, c.y - r}, {c.x + w * 0.5f, c.y + r},
                    IM_COL32(235, 238, 245, 230), r * 0.3f, 0, std::max(1.2f, r * 0.1f));
        dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, IM_COL32(245, 246, 250, 255), key);
        return;
    }
    if (button == PadButton::Options) {
        // Three lines, as the options / menu buttons of both pads print them.
        dl->AddCircleFilled(c, r, IM_COL32(20, 22, 28, 200));
        dl->AddCircle(c, r, IM_COL32(235, 238, 245, 200), 0, std::max(1.2f, r * 0.09f));
        for (const float dy : {-0.32f, 0.0f, 0.32f}) {
            dl->AddLine({c.x - r * 0.42f, c.y + r * dy}, {c.x + r * 0.42f, c.y + r * dy},
                        IM_COL32(235, 238, 245, 255), std::max(1.4f, r * 0.12f));
        }
        return;
    }
    if (button == PadButton::Shoulders) {
        const char* text = kind == PadKind::PlayStation ? "L1  R1" : "LB  RB";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const float w = ts.x + r * 1.2f;
        dl->AddRect({c.x - w * 0.5f, c.y - r * 0.8f}, {c.x + w * 0.5f, c.y + r * 0.8f},
                    IM_COL32(235, 238, 245, 230), r * 0.8f, 0, std::max(1.2f, r * 0.1f));
        dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, IM_COL32(245, 246, 250, 255), text);
        return;
    }
    dl->AddCircleFilled(c, r, IM_COL32(20, 22, 28, 200));
    dl->AddCircle(c, r, IM_COL32(235, 238, 245, 200), 0, std::max(1.2f, r * 0.09f));
    const float t = std::max(1.6f, r * 0.14f);
    if (kind == PadKind::PlayStation) {
        // Cross confirms, circle goes back, square and triangle for the rest (in their colours).
        switch (button) {
        case PadButton::Confirm: {
            const ImU32 col = IM_COL32(124, 178, 232, 255);
            dl->AddLine({c.x - r * 0.42f, c.y - r * 0.42f}, {c.x + r * 0.42f, c.y + r * 0.42f}, col,
                        t);
            dl->AddLine({c.x + r * 0.42f, c.y - r * 0.42f}, {c.x - r * 0.42f, c.y + r * 0.42f}, col,
                        t);
            break;
        }
        case PadButton::Back:
            dl->AddCircle(c, r * 0.45f, IM_COL32(255, 102, 102, 255), 24, t);
            break;
        case PadButton::Action:
            dl->AddRect({c.x - r * 0.4f, c.y - r * 0.4f}, {c.x + r * 0.4f, c.y + r * 0.4f},
                        IM_COL32(255, 105, 248, 255), 0.0f, 0, t);
            break;
        case PadButton::Extra:
            dl->AddTriangle({c.x, c.y - r * 0.48f}, {c.x - r * 0.46f, c.y + r * 0.34f},
                            {c.x + r * 0.46f, c.y + r * 0.34f}, IM_COL32(64, 226, 160, 255), t);
            break;
        default:
            break;
        }
        return;
    }
    const char* letter = button == PadButton::Confirm  ? "A"
                         : button == PadButton::Back   ? "B"
                         : button == PadButton::Action ? "X"
                                                       : "Y";
    const ImU32 col = button == PadButton::Confirm  ? IM_COL32(110, 200, 90, 255)
                      : button == PadButton::Back   ? IM_COL32(235, 80, 70, 255)
                      : button == PadButton::Action ? IM_COL32(80, 150, 240, 255)
                                                    : IM_COL32(245, 200, 60, 255);
    const ImVec2 ts = ImGui::CalcTextSize(letter);
    dl->AddText({c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, col, letter);
}

} // namespace BigPictureMode::Menu
