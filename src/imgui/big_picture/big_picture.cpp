//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "common/external_engines.h"
#include "common/game_edition.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/file_sys/ifile.h"
#include "emulator.h"
#include "imgui/big_picture/big_picture.h"
#include "imgui/big_picture/imgui_impl_sdl3_big_picture.h"
#include "imgui/big_picture/imgui_impl_sdlrenderer3.h"
#include "imgui/big_picture/menu.h"
#include "imgui/big_picture/settings_dialog_imgui.h"
#include "imgui/big_picture/theme.h"
#include "imgui/big_picture/translation.h"
#include "imgui/imgui_std.h"
#include "imgui/renderer/font_data.h"
#include "imgui/renderer/font_stack.h"
#include "sdl_window.h"

namespace BigPictureMode {

constexpr float gameImageSize = 260.f;

bool done = false;
bool showSettings = false;

std::filesystem::path runEbootPath = "";
std::string runSerial = "";
std::vector<IconInfo> gameIcons = {};

float uiScale = 1.0f;
SDL_Renderer* renderer;

/// LaunchUpdate's game: the window opens straight on its fork's update and closes with it.
std::string updateOnlySerial;

namespace {

std::string CleanEmulatorVersion() {
    std::string ver{Common::g_version};
    if (const auto wip = ver.find(" WIP"); wip != std::string::npos) {
        ver.erase(wip);
    }
    return ver;
}

/// A game's own launcher: a juego.ini next to the executable names the game, and the launcher
/// then shows only that game, its main settings and its recommended ones. Lines:
///   title=The Order: 1886
///   serials=CUSA00076,CUSA00035,CUSA00100
struct SingleGame {
    bool active{};
    std::string title;
    std::vector<std::string> serials;
    /// The folder of the executable, where a fondo.jpg or fondo.png can set the background.
    std::filesystem::path folder;
    /// The picture behind the launcher: fondo.jpg/png next to the executable, or else the
    /// game's own art (sce_sys/pic1.png). Nothing is shipped with the launcher.
    SDL_Texture* background{};
    bool background_searched{};
    /// The window height that shows the whole page, measured while drawing it: the window is
    /// resized to it.
    float wanted_height{};
};
SingleGame single_game;

void LoadSingleGame(const char* executable_name) {
    const auto& edition = Common::LoadGameEdition(executable_name);
    single_game.active = edition.active;
    single_game.title = edition.title;
    single_game.serials = edition.serials;
    single_game.folder = edition.folder;
}

/// The window's icon in a game's own launcher: an icono.png next to the executable, or else
/// the game's own icon.
void SetSingleGameIcon(SDL_Window* window, const std::vector<IconInfo>& icons) {
    std::vector<std::filesystem::path> candidates{single_game.folder / "icono.png"};
    if (!icons.empty()) {
        candidates.push_back(icons[0].ebootPath.parent_path() / "sce_sys" / "icon0.png");
    }
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (!std::filesystem::exists(candidate, ec)) {
            continue;
        }
        std::ifstream file{candidate, std::ios::binary};
        std::vector<u8> png{std::istreambuf_iterator<char>(file), {}};
        if (!png.empty()) {
            Frontend::SetWindowIcon(window, png);
            return;
        }
    }
}

/// Finds the background picture once the game is known (its own art is in its folder).
void FindBackground(const std::vector<IconInfo>& icons) {
    if (!single_game.active || single_game.background_searched) {
        return;
    }
    std::vector<std::filesystem::path> candidates;
    for (const char* name : {"fondo.jpg", "fondo.jpeg", "fondo.png"}) {
        candidates.push_back(single_game.folder / name);
    }
    if (!icons.empty()) {
        const auto game_folder = icons[0].ebootPath.parent_path();
        candidates.push_back(game_folder / "sce_sys" / "pic1.png");
        candidates.push_back(game_folder / "sce_sys" / "pic0.png");
    }
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate, ec)) {
            single_game.background = LoadSdlTextureDataFromFile(candidate);
            if (single_game.background) {
                break;
            }
        }
    }
    // Without the game, its art may still come once its folder is chosen.
    single_game.background_searched = single_game.background != nullptr || !icons.empty();
}

/// Draws the game's picture in a rounded rectangle, filling it without stretching, and darkens
/// its left part, where the name and the play button go.
void DrawBanner(ImDrawList* draw_list, ImVec2 min, ImVec2 max, float rounding) {
    draw_list->AddRectFilled(min, max, ImGui::GetColorU32(Theme::Card), rounding);
    if (!single_game.background) {
        return;
    }
    float width{}, height{};
    SDL_GetTextureSize(single_game.background, &width, &height);
    const ImVec2 size{max.x - min.x, max.y - min.y};
    if (width <= 0.0f || height <= 0.0f || size.x <= 0.0f || size.y <= 0.0f) {
        return;
    }
    const float scale = std::max(size.x / width, size.y / height);
    const float u = size.x / (width * scale);
    const float v = size.y / (height * scale);
    // Anchored to the right, where the picture shows through.
    const ImVec2 uv0{1.0f - u, (1.0f - v) * 0.5f};
    const ImVec2 uv1{1.0f, uv0.y + v};
    draw_list->AddImageRounded(ImTextureID(single_game.background), min, max, uv0, uv1,
                               IM_COL32_WHITE, rounding);
    const ImU32 dark =
        ImGui::GetColorU32(ImVec4(Theme::Card.x, Theme::Card.y, Theme::Card.z, 0.92f));
    const ImU32 clear =
        ImGui::GetColorU32(ImVec4(Theme::Card.x, Theme::Card.y, Theme::Card.z, 0.0f));
    const float solid = min.x + rounding * 2.0f;
    draw_list->AddRectFilled(min, {solid, max.y}, dark, rounding, ImDrawFlags_RoundCornersLeft);
    draw_list->AddRectFilledMultiColor({solid, min.y}, {min.x + size.x * 0.66f, max.y}, dark, clear,
                                       clear, dark);
}

/// A game's own launcher: a banner with the game's picture, its name and the play button, and
/// under it two panels, its main settings and how it is set up.
void DrawSingleGame(ImGuiEmuSettings::SettingsWindow& settings, bool show_settings,
                    int& updateFor) {
    const IconInfo& game = gameIcons[0];
    const float s = uiScale;
    const float width = ImGui::GetContentRegionAvail().x;
    // Its contents set the banner's height: the window is sized to the page, not the other way.
    const float banner_height = 230.0f * s;
    const float rounding = 26.0f * s;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    // The banner goes behind its contents and grows to hold them: draw them first.
    ImDrawListSplitter splitter;
    splitter.Split(draw_list, 2);
    splitter.SetCurrentChannel(draw_list, 1);

    // The version, as a small pill, then the name and the actions.
    const float left = min.x + 44.0f * s;
    ImGui::SetCursorScreenPos({left, min.y + 36.0f * s});
    ImGui::BeginGroup();
    {
        const char* badge = "PlayStation 4";
        ImGui::SetWindowFontScale(s * 0.8f);
        const ImVec2 text = ImGui::CalcTextSize(badge);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 pad{10.0f * s, 4.0f * s};
        draw_list->AddRectFilled(at, {at.x + text.x + pad.x * 2.0f, at.y + text.y + pad.y * 2.0f},
                                 ImGui::GetColorU32(Theme::Accent), 100.0f);
        ImGui::SetCursorScreenPos({at.x + pad.x, at.y + pad.y});
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::OnAccent);
        ImGui::TextUnformatted(badge);
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos({at.x, at.y + text.y + pad.y * 2.0f});
        ImGui::SetWindowFontScale(s);
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f * s));
    ImGui::SetWindowFontScale(s * 2.0f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width * 0.5f);
    ImGui::TextUnformatted(game.title.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(s);
    ImGui::TextDisabled("%s", fmt::format("{}   {}", game.serial, game.details[0]).c_str());
    ImGui::Dummy(ImVec2(0.0f, 14.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(40.0f * s, 15.0f * s));
    ImGui::SetWindowFontScale(s * 1.15f);
    const bool play = Theme::AccentButton(Tr("Play"));
    ImGui::SameLine(0.0f, 12.0f * s);
    if (Theme::Button(Tr("Update"))) {
        updateFor = 0;
    }
    ImGui::SetWindowFontScale(s);
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const ImVec2 max{min.x + width,
                     std::max(min.y + banner_height, ImGui::GetItemRectMax().y + 40.0f * s)};
    splitter.SetCurrentChannel(draw_list, 0);
    DrawBanner(draw_list, min, max, rounding);
    splitter.Merge(draw_list);

    // The two panels fill what is left.
    ImGui::SetCursorScreenPos({min.x, max.y + 22.0f * s});
    const float gap = 22.0f * s;
    const float panel_width = (width - gap) * 0.5f;
    const float panel_height = std::max(ImGui::GetContentRegionAvail().y, 200.0f * s);
    const ImGuiChildFlags panel_flags =
        ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened;
    ImGui::BeginChild("MainSettings", ImVec2(panel_width, panel_height), panel_flags);
    ImGui::SetWindowFontScale(s * 1.15f);
    ImGui::TextUnformatted(Tr("Main settings"));
    ImGui::SetWindowFontScale(s);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
    if (show_settings) {
        settings.ResetQuickSettings();
    } else {
        settings.DrawQuickSettings(game.serial, game.title);
    }
    const float settings_height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::EndChild();
    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("GameInfo", ImVec2(panel_width, panel_height), panel_flags);
    ImGui::SetWindowFontScale(s * 1.15f);
    ImGui::TextUnformatted(Tr("This game"));
    ImGui::SetWindowFontScale(s);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * s));
    for (const auto& line : game.details) {
        if (!line.empty()) {
            ImGui::TextDisabled("%s", line.c_str());
        }
    }
    const float info_height = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::EndChild();
    // The panels start here: the window has to reach their longest content, then the bottom bar.
    const float panels_top = max.y + 22.0f * s - ImGui::GetMainViewport()->Pos.y;
    single_game.wanted_height = panels_top + std::max(settings_height, info_height) +
                                ImGui::GetFrameHeightWithSpacing() +
                                ImGui::GetStyle().WindowPadding.y + 16.0f * s;

    if (play) {
        // What was changed and not applied yet goes with the game.
        settings.ApplyQuickSettings();
        done = true;
        Core::FileSys::MntPoints::ignore_game_patches =
            ImGui::IsKeyDown(ImGuiKey::ImGuiKey_LeftCtrl);
        runEbootPath = game.ebootPath;
        runSerial = game.serial;
    }
}

/// In a game's own launcher, keeps only that game.
void KeepSingleGame(std::vector<IconInfo>& icons) {
    if (!single_game.active) {
        return;
    }
    std::erase_if(icons, [](const IconInfo& icon) {
        return std::ranges::find(single_game.serials, icon.serial) == single_game.serials.end();
    });
}

std::filesystem::path UpdateChecker(const std::string sceItem, std::filesystem::path game_folder) {
    std::filesystem::path updatedPath = "";
    std::filesystem::path basePath = game_folder.parent_path();
    std::string fileName;
    std::string item = "sce_sys/" + sceItem;

    if (Core::FileSys::IsZArchiveFile(game_folder)) {
        fileName = Core::FileSys::StripZArchiveExtension(game_folder).filename().string();
    } else {
        fileName = game_folder.filename().string();
    }

    if (std::filesystem::exists(basePath / (fileName + "-UPDATE") / item)) {
        updatedPath = basePath / (fileName + "-UPDATE") / item;
    } else if (Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-UPDATE.zar"), item)
                   .has_value()) {
        updatedPath =
            Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-UPDATE.zar"), item).value();
    } else if (std::filesystem::exists(basePath / (fileName + "-patch") / item)) {
        updatedPath = basePath / (fileName + "-patch") / item;
    } else if (Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-patch.zar"), item)
                   .has_value()) {
        updatedPath =
            Core::FileSys::ResolveGameFilePath(basePath / (fileName + "-patch.zar"), item).value();
    } else if (Core::FileSys::ResolveGameFilePath(game_folder, item).has_value()) {
        updatedPath = Core::FileSys::ResolveGameFilePath(game_folder, item).value();
    }

    return updatedPath;
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
    std::ifstream in{path};
    if (!in) {
        return {};
    }
    // A damaged file only means that there is nothing to show from it.
    return nlohmann::json::parse(in, nullptr, false);
}

/// The settings a game will start with are read from the files, not from the loaded settings:
/// those hold the profile of one game at most.
template <typename T>
T StoredSetting(const nlohmann::json& game, const nlohmann::json& global, const char* group,
                const char* key, T fallback) {
    for (const auto* file : {&game, &global}) {
        if (file->is_object() && file->contains(group) && (*file)[group].contains(key)) {
            const auto& value = (*file)[group][key];
            if (!value.is_null()) {
                return value.get<T>();
            }
        }
    }
    return fallback;
}

const char* ConsoleLanguageName(int id) {
    static constexpr std::array<const char*, 31> names = {
        "Japanese",
        "English (United States)",
        "French (France)",
        "Spanish (Spain)",
        "German",
        "Italian",
        "Dutch",
        "Portuguese (Portugal)",
        "Russian",
        "Korean",
        "Traditional Chinese",
        "Simplified Chinese",
        "Finnish",
        "Swedish",
        "Danish",
        "Norwegian (Bokmaal)",
        "Polish",
        "Portuguese (Brazil)",
        "English (United Kingdom)",
        "Turkish",
        "Spanish (Latin America)",
        "Arabic",
        "French (Canada)",
        "Czech",
        "Hungarian",
        "Greek",
        "Romanian",
        "Thai",
        "Vietnamese",
        "Indonesian",
        "Ukrainian",
    };
    return id >= 0 && id < static_cast<int>(names.size()) ? Tr(names[id]) : "";
}

/// Fills in what each card says under the name of its game.
void FillGameDetails(std::vector<IconInfo>& icons) {
    using namespace Common::FS;
    const auto global = ReadJson(GetUserPath(PathType::UserDir) / "config.json");

    // Full screen shows the game at the size of the display.
    int display_width = 0;
    int display_height = 0;
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        display_width = static_cast<int>(mode->w * mode->pixel_density);
        display_height = static_cast<int>(mode->h * mode->pixel_density);
    }

    // A window is kept inside the usable area of the display, as the emulator does.
    SDL_Rect usable{};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    const std::string own_version = CleanEmulatorVersion();

    for (auto& icon : icons) {
        const auto game = ReadJson(GetUserPath(PathType::CustomConfigs) / (icon.serial + ".json"));
        try {
            const bool full_screen =
                StoredSetting(game, global, "GPU", "full_screen", false) &&
                StoredSetting<std::string>(game, global, "GPU", "full_screen_mode", "Windowed") !=
                    "Windowed";
            const int width = full_screen
                                  ? display_width
                                  : StoredSetting(game, global, "GPU", "window_width", 1280);
            const int height = full_screen
                                   ? display_height
                                   : StoredSetting(game, global, "GPU", "window_height", 720);
            int out_width = width;
            int out_height = height;
            if (!full_screen && usable.w > 0 && usable.h > 0 && width > 0 && height > 0) {
                const float fit =
                    std::min({1.0f, usable.w * 0.9f / width, usable.h * 0.9f / height});
                out_width = static_cast<int>(width * fit);
                out_height = static_cast<int>(height * fit);
            }
            // Line 0 always shows the fork/port and its installed version.
            icon.details[0] = fmt::format("{}ForkStation {}", Tr("Engine: "), own_version);

            // The game renders at a resolution of its own, which only a patch changes. A
            // resolution patch says the resolution in its name.
            using ImGuiEmuSettings::SettingsWindow;
            const std::string patch = StoredSetting(game, global, "GPU", "use_game_patch", true)
                                          ? SettingsWindow::GamePatchName(icon.serial)
                                          : std::string{};
            static const std::regex size{R"((\d{3,4})\s*[xX]\s*(\d{3,4}))"};
            if (std::smatch found; std::regex_search(patch, found, size)) {
                icon.details[1] =
                    fmt::format("{}{} x {}", Tr("Resolution: "), found[1].str(), found[2].str());
            } else {
                // Without a patch a game gives the console the picture it was made for: 1080p
                // on a PS4, and whatever it chooses on a PS4 Pro.
                const bool pro = StoredSetting(game, global, "General", "neo_mode", false);
                icon.details[1] = fmt::format("{}{}", Tr("Resolution: "),
                                              !patch.empty() ? Tr("patched")
                                              : pro          ? Tr("original of the game")
                                                             : "1920 x 1080");
            }
            // The resolution above is the detail the picture has. In full screen it is
            // stretched over the display, whose size says nothing about the picture, so only
            // a window is given a size.
            icon.details[2] =
                full_screen ? std::string{Tr("Full screen")}
                            : fmt::format("{} {} x {}", Tr("Windowed"), out_width, out_height);
            // In full screen the picture is enlarged to the display, named the usual way.
            const bool fsr = StoredSetting(game, global, "GPU", "fsr_enabled", false);
            if (!fsr) {
                icon.details[3] = Tr("Without FSR");
            } else if (full_screen && display_height > 0) {
                const std::string target = display_height == 2160 ? "4K"
                                           : display_height == 4320
                                               ? "8K"
                                               : fmt::format("{}p", display_height);
                icon.details[3] = fmt::format(fmt::runtime(Tr("Enlarged to {} with FSR")), target);
            } else {
                icon.details[3] = Tr("Enlarged with FSR");
            }
            icon.details[4] = Tr(StoredSetting(game, global, "GPU", "frame_generation", false)
                                     ? "Frame generation: on"
                                     : "Frame generation: off");
        } catch (const nlohmann::json::exception&) {
            icon.details[0] = fmt::format("{}ForkStation {}", Tr("Engine: "), own_version);
            icon.details[1].clear();
            icon.details[2].clear();
            icon.details[3].clear();
            icon.details[4].clear();
        }
        // A game that runs on another program shows that program, its installed version, and its
        // settings.
        icon.external = false;
        if (const auto other = Common::DescribeExternalEngine(icon.serial)) {
            icon.external = true;
            icon.details[0] = fmt::format("{}{}", Tr("Engine: "), other->engine);
            for (size_t k = 0; k < other->details.size(); ++k) {
                icon.details[k + 1] = other->details[k];
            }
        }
        icon.details[5] = Tr("Not played yet");
        icon.details[6].clear();
    }

    // One line per game: its serial, the time played as h:mm:ss and when it was last played.
    std::ifstream play_times{GetUserPath(PathType::UserDir) / "play_time.txt"};
    std::string line;
    while (std::getline(play_times, line)) {
        // A line that cannot be read is skipped, the rest of the file still counts.
        std::istringstream fields{line};
        std::string serial;
        std::string played;
        std::time_t last{};
        if (!(fields >> serial >> played >> last)) {
            continue;
        }
        const auto icon = std::ranges::find(icons, serial, &IconInfo::serial);
        int hours = 0;
        int minutes = 0;
        if (icon == icons.end() || std::sscanf(played.c_str(), "%d:%d", &hours, &minutes) != 2) {
            continue;
        }
        icon->details[5] = hours > 0 ? fmt::format("{}{} h {} min", Tr("Played: "), hours, minutes)
                                     : fmt::format("{}{} min", Tr("Played: "), minutes);
        if (const std::tm* when = std::localtime(&last)) {
            char date[32]{};
            std::strftime(date, sizeof(date), Tr("%Y-%m-%d"), when);
            icon->details[6] = fmt::format("{}{}", Tr("Last played: "), date);
        }
    }
}

/// Draws the game grid: one card per game with its cover, its name and its actions. Sets
/// `settingsFor` or `updateFor` to the game whose button was pressed.
void SetGameIcons(std::vector<IconInfo>& gameIcons, int& settingsFor, int& suggestFor,
                  int& updateFor) {
    const float maxAvailableWidth = ImGui::GetContentRegionAvail().x;
    const float pad = 18.0f * uiScale;
    const float gap = 22.0f * uiScale;
    const float cover = gameImageSize * uiScale;
    const float line = ImGui::GetTextLineHeight();
    const float frame = ImGui::GetFrameHeight();
    // Seven smaller lines under the name say how the game is set up and how much it was played.
    constexpr float details_scale = 0.74f;
    const float details_line = line * details_scale + 2.0f * uiScale;
    const float details_height = details_line * 7.0f + 16.0f * uiScale;
    const ImVec2 card{cover + pad * 2.0f, pad + cover + 12.0f * uiScale + line * 2.0f +
                                              details_height + 12.0f * uiScale + frame + pad};
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    float rowWidth = 0.0f;

    // With PS5 games in the list, each console gets its own section with a heading.
    const bool sections = std::ranges::any_of(gameIcons, &IconInfo::ps5);
    for (int i = 0; i < gameIcons.size(); i++) {
        if (sections && (i == 0 || gameIcons[i].ps5 != gameIcons[i - 1].ps5)) {
            if (i > 0) {
                ImGui::Dummy(ImVec2(0.0f, gap * 1.5f));
            }
            ImGui::SetWindowFontScale(uiScale * 1.15f);
            ImGui::TextUnformatted(gameIcons[i].ps5 ? "PlayStation 5" : "PlayStation 4");
            ImGui::SetWindowFontScale(uiScale);
            const ImVec2 under = ImGui::GetCursorScreenPos();
            draw_list->AddLine({under.x, under.y + 2.0f * uiScale},
                               {under.x + maxAvailableWidth, under.y + 2.0f * uiScale},
                               ImGui::GetColorU32(Theme::Chip), 2.0f * uiScale);
            ImGui::Dummy(ImVec2(0.0f, 10.0f * uiScale));
            rowWidth = 0.0f;
        } else if (i > 0) {
            // Use same line if the next card fits, move to the next row if not
            if (rowWidth + gap + card.x <= maxAvailableWidth) {
                ImGui::SameLine(0.0f, gap);
            } else {
                ImGui::Dummy(ImVec2(0.0f, gap - ImGui::GetStyle().ItemSpacing.y));
                rowWidth = 0.0f;
            }
        }
        rowWidth += (rowWidth > 0.0f ? gap : 0.0f) + card.x;

        ImGui::PushID(i);
        ImGui::BeginGroup();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 cover_min{pos.x + pad, pos.y + pad};
        const ImVec2 cover_max{cover_min.x + cover, cover_min.y + cover};

        // The cover starts the game.
        ImGui::SetCursorScreenPos(cover_min);
        // The recommended settings button sits on top of the cover and has to get its clicks.
        ImGui::SetNextItemAllowOverlap();
        bool play = ImGui::InvisibleButton("cover", ImVec2(cover, cover));
        const bool cover_hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (ImGui::IsItemFocused() && !gameIcons[i].focusState) {
            ImGui::SetScrollHereY(0.5f);
        }
        if (ImGui::IsWindowFocused()) {
            gameIcons[i].focusState = ImGui::IsItemFocused();
        }

        draw_list->AddRectFilled(
            pos, {pos.x + card.x, pos.y + card.y},
            ImGui::GetColorU32(cover_hovered ? Theme::CardHovered : Theme::Card), 28.0f * uiScale);
        if (ImTextureID id = gameIcons[i].textureId; id != nullptr) {
            draw_list->AddImageRounded(id, cover_min, cover_max, {0.0f, 0.0f}, {1.0f, 1.0f},
                                       IM_COL32_WHITE, 18.0f * uiScale);
        } else {
            draw_list->AddRectFilled(cover_min, cover_max, ImGui::GetColorU32(Theme::Chip),
                                     16.0f * uiScale);
        }

        // A game with known good settings that has none of its own yet offers them right here.
        using ImGuiEmuSettings::SettingsWindow;
        if (SettingsWindow::HasSuggestedFor(gameIcons[i].serial) &&
            !SettingsWindow::HasOwnSettings(gameIcons[i].serial)) {
            ImGui::SetCursorScreenPos({cover_min.x + 8.0f * uiScale, cover_min.y + 8.0f * uiScale});
            ImGui::SetWindowFontScale(uiScale * 0.72f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(10.0f * uiScale, 5.0f * uiScale));
            if (Theme::AccentButton(Tr("Use recommended settings##suggest"))) {
                suggestFor = i;
            }
            ImGui::PopStyleVar();
            ImGui::SetWindowFontScale(uiScale);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s",
                                  Tr("Settings tested with this game. You can change them later."));
            }
        }

        // Two lines for the name, cut off if it is longer.
        const ImVec2 title_min{cover_min.x, cover_max.y + 12.0f * uiScale};
        ImGui::SetCursorScreenPos(title_min);
        ImGui::PushClipRect(title_min, {cover_max.x, title_min.y + line * 2.0f}, true);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cover);
        ImGui::TextWrapped("%s", gameIcons[i].title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopClipRect();

        const float details_top = title_min.y + line * 2.0f + 8.0f * uiScale;
        ImGui::PushClipRect({cover_min.x, details_top},
                            {cover_max.x, details_top + details_line * 7.0f}, true);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::TextDim);
        for (int row = 0; row < 7; ++row) {
            ImGui::SetWindowFontScale(uiScale * details_scale);
            const float text_w = ImGui::CalcTextSize(gameIcons[i].details[row].c_str()).x;
            if (text_w > cover && text_w > 0.0f) {
                ImGui::SetWindowFontScale(uiScale * details_scale * (cover / text_w));
            }
            ImGui::SetCursorScreenPos({cover_min.x, details_top + details_line * row});
            ImGui::TextUnformatted(gameIcons[i].details[row].c_str());
        }
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(uiScale);
        ImGui::PopClipRect();

        // What the check at start-up found: a badge on the cover, and the Update button lit.
        const auto hint = Common::GetUpdateHint(gameIcons[i].serial);
        const bool must_update = hint && hint->missing;
        const bool should_update = hint && hint->available;
        if (must_update || should_update) {
            const char* badge = must_update ? "Debe instalarse" : "Actualización recomendada";
            ImGui::SetWindowFontScale(uiScale * 0.68f);
            const ImVec2 text = ImGui::CalcTextSize(badge);
            const float bx = 9.0f * uiScale;
            const float by = 4.0f * uiScale;
            const ImVec2 bmax{cover_max.x - 8.0f * uiScale, cover_max.y - 8.0f * uiScale};
            const ImVec2 bmin{bmax.x - text.x - bx * 2.0f, bmax.y - text.y - by * 2.0f};
            draw_list->AddRectFilled(
                bmin, bmax, must_update ? IM_COL32(200, 60, 50, 235) : IM_COL32(230, 150, 20, 235),
                8.0f * uiScale);
            draw_list->AddText({bmin.x + bx, bmin.y + by}, IM_COL32_WHITE, badge);
            ImGui::SetWindowFontScale(uiScale);
            if (ImGui::IsMouseHoveringRect(bmin, bmax)) {
                ImGui::SetTooltip(
                    "%s", must_update
                              ? fmt::format("El motor no está instalado. Pulsa Actualizar para "
                                            "instalar {}.",
                                            hint->latest_version)
                                    .c_str()
                              : fmt::format("Instalada: {}\nNueva: {}\nPulsa Actualizar.",
                                            hint->installed_version, hint->latest_version)
                                    .c_str());
            }
        }
        const auto update_button = [&] {
            return (must_update || should_update) ? Theme::AccentButton(Tr("Update"))
                                                  : Theme::Button(Tr("Update"));
        };

        ImGui::SetCursorScreenPos({cover_min.x, pos.y + card.y - pad - frame});
        if (gameIcons[i].external) {
            // External fork/port: Play + Update from official GitHub releases.
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(14.0f * uiScale, ImGui::GetStyle().FramePadding.y));
            play |= Theme::AccentButton(Tr("Play"));
            ImGui::SameLine(0.0f, 8.0f * uiScale);
            if (update_button()) {
                updateFor = i;
            }
            ImGui::PopStyleVar();
        } else {
            // Internal engine: Play + Settings + Update, sized to fit cleanly inside the card
            // width.
            const float btn_scale = 0.85f;
            ImGui::SetWindowFontScale(uiScale * btn_scale);
            const float text_sum = ImGui::CalcTextSize(Tr("Play")).x +
                                   ImGui::CalcTextSize(Tr("Settings")).x +
                                   ImGui::CalcTextSize(Tr("Update")).x;
            const float btn_gap = 6.0f * uiScale;
            const float pad_x = std::clamp((cover - text_sum - btn_gap * 2.0f) / 6.0f,
                                           6.0f * uiScale, 11.0f * uiScale);
            const float pad_y =
                ImGui::GetStyle().FramePadding.y + (frame - ImGui::GetFrameHeight()) * 0.5f;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(pad_x, std::max(4.0f * uiScale, pad_y)));
            play |= Theme::AccentButton(Tr("Play"));
            ImGui::SameLine(0.0f, btn_gap);
            if (Theme::Button(Tr("Settings"))) {
                settingsFor = i;
            }
            ImGui::SameLine(0.0f, btn_gap);
            if (update_button()) {
                updateFor = i;
            }
            ImGui::PopStyleVar();
            ImGui::SetWindowFontScale(uiScale);
        }

        if (play) {
            done = true;
            Core::FileSys::MntPoints::ignore_game_patches =
                ImGui::IsKeyDown(ImGuiKey::ImGuiKey_LeftCtrl);
            runEbootPath = gameIcons[i].ebootPath;
            runSerial = gameIcons[i].serial;
        }

        // Make the group as large as the card, whatever was drawn inside.
        ImGui::SetCursorScreenPos(pos);
        ImGui::Dummy(card);
        ImGui::EndGroup();
        ImGui::PopID();
    }
}

} // namespace

SDL_Texture* LoadSdlTextureData(std::vector<u8> data) {
    int image_width = 0;
    int image_height = 0;
    int channels = 4;
    unsigned char* image_data = stbi_load_from_memory(
        (const unsigned char*)data.data(), (int)data.size(), &image_width, &image_height, NULL, 4);
    if (image_data == nullptr) {
        LOG_ERROR(ImGui, "Failed to load image: {}", stbi_failure_reason());
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(image_width, image_height, SDL_PIXELFORMAT_RGBA32,
                                                 (void*)image_data, channels * image_width);
    if (surface == nullptr) {
        LOG_ERROR(ImGui, "Unable to create SDL surface: {}", SDL_GetError());
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == nullptr) {
        LOG_ERROR(ImGui, "Unable to create SDL texture: {}", SDL_GetError());
    }

    SDL_DestroySurface(surface);
    stbi_image_free(image_data);

    return texture;
}

SDL_Texture* LoadSdlTextureDataFromFile(std::filesystem::path filePath) {
    std::ifstream file(filePath, std::ios::binary);
    std::vector<u8> data =
        std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return LoadSdlTextureData(data);
}

void GetGameIconInfo(std::vector<IconInfo>& icons) {
    icons.clear();

    for (const auto& installLoc : EmulatorSettings.GetAllGameInstallDirs()) {
        if (installLoc.enabled && std::filesystem::exists(installLoc.path)) {
            for (const auto& entry : std::filesystem::directory_iterator(installLoc.path)) {

                std::string pathstring = entry.path().filename().string();
                if (pathstring.ends_with("-UPDATE") || pathstring.ends_with("-patch") ||
                    (!entry.is_directory() && !Core::FileSys::IsZArchiveFile(entry))) {
                    continue;
                }

                if (Core::FileSys::IsZArchiveFile(entry)) {
                    size_t start = pathstring.length() - 3;
                    for (size_t i = start; i < pathstring.length(); ++i) {
                        pathstring[i] = static_cast<char>(
                            std::tolower(static_cast<unsigned char>(pathstring[i])));
                    }

                    if (pathstring.ends_with("-UPDATE.zar") || pathstring.ends_with("-patch.zar")) {
                        continue;
                    }
                }

                IconInfo icon;
                PSF psf;
                const std::string sfoFileName = "param.sfo";
                std::filesystem::path sfoPath = UpdateChecker(sfoFileName, entry.path());

                if (std::filesystem::exists(sfoPath) && psf.Open(sfoPath)) {
                    if (const auto title = psf.GetString("TITLE"); title.has_value()) {
                        icon.title = *title;
                    }

                    if (const auto title_id = psf.GetString("TITLE_ID"); title_id.has_value()) {
                        icon.serial = *title_id;
                    }

                    // Additional content (DLC) comes with a param.sfo of its own, but it is
                    // not a game to list.
                    if (const auto category = psf.GetString("CATEGORY");
                        category.has_value() && category->starts_with("ac")) {
                        continue;
                    }
                } else if (const auto ps5 = Common::ReadPs5GameInfo(entry.path());
                           ps5 && Common::DescribeExternalEngine(ps5->serial)) {
                    // A PS5 game: listed only when a program in motores runs it.
                    icon.title = ps5->title;
                    icon.serial = ps5->serial;
                    icon.ps5 = true;
                } else {
                    continue;
                }

                const std::string iconFileName = "icon0.png";
                std::filesystem::path iconPath = UpdateChecker(iconFileName, entry.path());

                SDL_Texture* texture = LoadSdlTextureDataFromFile(iconPath);
                icon.textureId = ImTextureID(texture);

                icon.ebootPath = entry.path() / "eboot.bin";
                if (Core::FileSys::IsZArchiveFile(entry.path())) {
                    icon.ebootPath = entry.path();
                }

                icon.focusState = false;
                icons.push_back(icon);
            }
        }
    }

    std::sort(icons.begin(), icons.end(), [](const IconInfo& a, const IconInfo& b) {
        if (a.ps5 != b.ps5) {
            return !a.ps5; // PS4 games first, then the PS5 section
        }
        return a.title < b.title; // Alphabetical order
    });
    KeepSingleGame(icons);
    FindBackground(icons);
    FillGameDetails(icons);
    // Once per session, in the background: which engines have a newer release on GitHub.
    std::vector<std::string> serials;
    for (const auto& icon : icons) {
        serials.push_back(icon.serial);
    }
    Common::StartUpdateChecks(serials);
}

// ---- The home screen: one console's games as a row of tiles over the chosen game's art ----

namespace {

struct Home {
    int tab = -1;                            ///< 0: PS4 games, 1: PS5 games
    std::array<int, 2> selected{};           ///< the chosen tile of each tab
    std::map<std::string, SDL_Texture*> art; ///< by serial (nullptr: the game has none)
    SDL_Texture* shown = nullptr;
    SDL_Texture* previous = nullptr;
    float fade = 1.0f;       ///< 0..1, the new art over the previous one
    float scroll = 0.0f;     ///< the tile row's offset, eased towards the chosen tile
    std::vector<float> grow; ///< per game, 0..1: how far its tile is enlarged
    int zone = 1;            ///< the pad's row: 0 the top bar, 1 the tiles, 2 the actions
    int item = 0;            ///< the pad's place in the top bar or among the actions
    bool menu = false;       ///< the game's options menu is open
    int menu_item = 0;       ///< the pad's place in it
    ImVec2 menu_at{};        ///< where it opens (under the options button)
    int menu_frame = -1;
    // Transitions: 0..1 each, eased in after a change.
    int shown_game = -1;  ///< the game whose details are shown
    int shown_tab = -1;   ///< the tab whose tiles are shown
    float info_in = 1.0f; ///< the details, after another game was chosen
    float row_in = 1.0f;  ///< the tiles, after the tab changed
    float menu_in = 0.0f; ///< the options menu, while it opens     ///< the frame it opened in (the
                          ///< press that opened it is not reused)
};
Home home;

/// The kind of pad connected (a PlayStation pad wins over others), for the button hints.
Menu::PadKind CurrentPadKind() {
    int count = 0;
    Menu::PadKind kind = Menu::PadKind::Keyboard;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            const auto type = SDL_GetGamepadTypeForID(ids[i]);
            if (type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
                type == SDL_GAMEPAD_TYPE_PS5) {
                kind = Menu::PadKind::PlayStation;
                break;
            }
            kind = Menu::PadKind::Xbox;
        }
        SDL_free(ids);
    }
    return kind;
}

/// Hints along the bottom right: each pad button and what it does, right to left.
void DrawPadHints(ImDrawList* dl, const std::vector<std::pair<Menu::PadButton, const char*>>& hints,
                  float right, float y, float s) {
    const auto kind = CurrentPadKind();
    float hx = right;
    for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
        const ImVec2 ls = ImGui::CalcTextSize(it->second);
        hx -= ls.x;
        dl->AddText({hx, y - ls.y * 0.5f}, IM_COL32(255, 255, 255, 210), it->second);
        float r = 13.0f * s;
        float wide = r;
        if (it->first == Menu::PadButton::Shoulders || (kind == Menu::PadKind::Keyboard)) {
            wide = std::max(r, ImGui::CalcTextSize("LB  RB").x * 0.5f + r * 0.6f);
        }
        hx -= wide + 8.0f * s;
        Menu::DrawPadButton(dl, kind, it->first, {hx, y}, r);
        hx -= wide + 28.0f * s;
    }
}

/// The game's key art (sce_sys/pic1.png for PS4 games, pic0.png for PS5 ones), from its folder or
/// its .zar, loaded once.
SDL_Texture* GameArt(const IconInfo& icon) {
    if (const auto it = home.art.find(icon.serial); it != home.art.end()) {
        return it->second;
    }
    auto root = icon.ebootPath;
    if (root.filename() == "eboot.bin" || root.filename() == "EBOOT.BIN") {
        root = root.parent_path();
    }
    SDL_Texture* texture = nullptr;
    for (const char* name : {"sce_sys/pic1.png", "sce_sys/pic0.png"}) {
        if (const auto data = Core::FileSys::ReadGameFile(root, name); data && !data->empty()) {
            texture = LoadSdlTextureData(*data);
            if (texture) {
                break;
            }
        }
    }
    home.art[icon.serial] = texture;
    return texture;
}

/// Draws a texture covering the rectangle (cropped, never stretched).
void DrawCover(ImDrawList* dl, SDL_Texture* texture, ImVec2 min, ImVec2 max, float alpha) {
    float tw = 0.0f, th = 0.0f;
    if (!texture || !SDL_GetTextureSize(texture, &tw, &th) || tw <= 0.0f || th <= 0.0f) {
        return;
    }
    const float w = max.x - min.x, h = max.y - min.y;
    const float scale = std::max(w / tw, h / th);
    const float u = (w / (tw * scale)), v = (h / (th * scale));
    const ImVec2 uv0{(1.0f - u) * 0.5f, (1.0f - v) * 0.5f};
    const ImVec2 uv1{uv0.x + u, uv0.y + v};
    dl->AddImage(ImTextureID(texture), min, max, uv0, uv1,
                 IM_COL32(255, 255, 255, static_cast<int>(alpha * 255.0f)));
}

/// "Label: value" split for the detail cards.
std::pair<std::string, std::string> SplitDetail(const std::string& text) {
    if (const auto colon = text.find(": "); colon != std::string::npos) {
        return {text.substr(0, colon), text.substr(colon + 2)};
    }
    return {{}, text};
}

} // namespace

// ---- Choosing a games folder with the pad: drives and folders as a settings-like list ----

namespace {

struct FolderBrowser {
    bool open = false;
    bool loaded = false;
    std::filesystem::path dir; ///< empty: the list of drives
    std::vector<std::filesystem::path> entries;
    int sel = 0;
    float scroll = 0.0f;
};
FolderBrowser browser;

void OpenFolderBrowser() {
    browser = {};
    browser.open = true;
}

void LoadFolderBrowser() {
    browser.entries.clear();
    browser.loaded = true;
    std::error_code ec;
    if (browser.dir.empty()) {
#ifdef _WIN32
        for (char letter = 'C'; letter <= 'Z'; ++letter) {
            const std::filesystem::path drive = std::string{letter} + ":\\";
            if (std::filesystem::is_directory(drive, ec)) {
                browser.entries.push_back(drive);
            }
        }
#else
        browser.entries.push_back("/");
#endif
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(
             browser.dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        const auto name = Common::FS::PathToUTF8String(entry.path().filename());
        if (!entry.is_directory(ec) || name.empty() || name[0] == '.' || name[0] == '$' ||
            name == "System Volume Information") {
            continue;
        }
        browser.entries.push_back(entry.path());
    }
    std::ranges::sort(browser.entries, [](const auto& a, const auto& b) {
        auto x = Common::FS::PathToUTF8String(a.filename());
        auto y = Common::FS::PathToUTF8String(b.filename());
        std::ranges::transform(x, x.begin(), [](unsigned char c) { return std::tolower(c); });
        std::ranges::transform(y, y.begin(), [](unsigned char c) { return std::tolower(c); });
        return x < y;
    });
}

void ChooseGamesFolder(const std::filesystem::path& folder) {
    EmulatorSettings.AddGameInstallDir(folder);
    EmulatorSettings.Save();
    GetGameIconInfo(gameIcons);
    browser.open = false;
}

} // namespace

/// The folder browser, over the whole window, while it is open.
void DrawFolderBrowser() {
    if (!browser.open) {
        return;
    }
    if (!browser.loaded) {
        LoadFolderBrowser();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float s = std::clamp(size.x / 1500.0f, 0.6f, 2.6f);
    Menu::Backdrop(dl, o, {o.x + size.x, o.y + size.y});
    ImGui::SetWindowFontScale(s * 0.95f);
    dl->AddText({o.x + 64.0f * s, o.y + 48.0f * s}, IM_COL32(245, 245, 248, 255),
                "Elige la carpeta de tus juegos");
    ImGui::SetWindowFontScale(s * 0.42f);
    const std::string where = browser.dir.empty() ? std::string{"Discos de este PC"}
                                                  : Common::FS::PathToUTF8String(browser.dir);
    dl->AddText({o.x + 64.0f * s, o.y + 104.0f * s}, IM_COL32(255, 255, 255, 150), where.c_str());

    // The rows: going up first (inside a folder), then its folders.
    const bool inside = !browser.dir.empty();
    const int count = static_cast<int>(browser.entries.size()) + (inside ? 1 : 0);
    browser.sel = std::clamp(browser.sel, 0, std::max(0, count - 1));
    const float lx = o.x + size.x * 0.155f;
    const float lw = size.x * 0.69f;
    const float rh = 62.0f * s;
    const float top = o.y + 150.0f * s;
    const int visible = std::max(1, static_cast<int>((size.y - 150.0f * s - 120.0f * s) / rh));
    // Keep the chosen row in view.
    const float want = std::clamp(static_cast<float>(browser.sel - visible / 2), 0.0f,
                                  static_cast<float>(std::max(0, count - visible)));
    browser.scroll += (want - browser.scroll) * 0.35f;
    ImGui::SetWindowFontScale(s * 0.5f);
    dl->PushClipRect({o.x, top}, {o.x + size.x, top + visible * rh}, true);
    int clicked = -1;
    for (int i = 0; i < count; ++i) {
        const float y = top + (i - browser.scroll) * rh;
        if (y < top - rh || y > top + visible * rh) {
            continue;
        }
        const bool up_row = inside && i == 0;
        const auto& path = up_row ? browser.dir : browser.entries[i - (inside ? 1 : 0)];
        std::string label =
            up_row ? std::string{"Subir un nivel"}
                   : (browser.dir.empty() ? Common::FS::PathToUTF8String(path)
                                          : Common::FS::PathToUTF8String(path.filename()));
        ImGui::PushID(i);
        if (Menu::Row("##row", up_row ? Menu::Icon::Update : Menu::Icon::Folder, label.c_str(), "",
                      lx, y, lw, rh, i == browser.sel)) {
            clicked = i;
        }
        ImGui::PopID();
    }
    dl->PopClipRect();
    if (count == 0) {
        dl->AddText({lx + 64.0f * s, top + 20.0f * s}, IM_COL32(255, 255, 255, 150),
                    "Esta carpeta no tiene subcarpetas.");
    }

    ImGui::SetWindowFontScale(s * 0.36f);
    DrawPadHints(dl,
                 {{Menu::PadButton::Confirm, "Abrir"},
                  {Menu::PadButton::Action, "Elegir esta carpeta"},
                  {Menu::PadButton::Back, inside ? "Subir" : "Cancelar"}},
                 o.x + size.x - 64.0f * s, o.y + size.y - 46.0f * s, s);
    ImGui::SetWindowFontScale(s);

    // The pad, the keyboard and the mouse.
    const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
        return std::ranges::any_of(keys, [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); });
    };
    if (pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown})) {
        browser.sel = std::min(browser.sel + 1, count - 1);
    }
    if (pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp})) {
        browser.sel = std::max(browser.sel - 1, 0);
    }
    if (ImGui::GetIO().MouseWheel != 0.0f) {
        browser.sel = std::clamp(browser.sel - static_cast<int>(ImGui::GetIO().MouseWheel * 3.0f),
                                 0, std::max(0, count - 1));
    }
    const auto enter = [&](int i) {
        if (inside && i == 0) {
            const auto parent = browser.dir.parent_path();
            browser.dir = parent == browser.dir ? std::filesystem::path{} : parent;
        } else if (i >= 0 && i < count) {
            browser.dir = browser.entries[i - (inside ? 1 : 0)];
        }
        browser.sel = 0;
        browser.scroll = 0.0f;
        browser.loaded = false;
    };
    const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    if (clicked >= 0) {
        browser.sel = clicked;
        enter(clicked);
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
        if (count > 0) {
            enter(browser.sel);
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        // The highlighted folder, or the open one when "up" is highlighted.
        if (inside && browser.sel == 0) {
            ChooseGamesFolder(browser.dir);
        } else if (count > 0) {
            ChooseGamesFolder(browser.entries[browser.sel - (inside ? 1 : 0)]);
        }
    } else if (back) {
        if (inside) {
            enter(0);
        } else {
            browser.open = false;
        }
    }
}

// ---- First start: a guide through the setup, all of it with the pad ----

namespace {

struct Onboarding {
    bool active = false;
    int step = 0;
    float shown = 0.0f; ///< 0..1, the step's fade in
    int added = 0;      ///< folders added from the guide
};
Onboarding onboarding;

std::filesystem::path OnboardingFlag() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) /
           "forkstation_bienvenida_vista.txt";
}

/// RetroBat's ROM folders for PS4 and PS5 games, when this program sits in RetroBat.
std::vector<std::filesystem::path> RetroBatRomFolders() {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    auto dir = std::filesystem::path{Common::FS::GetUserPath(Common::FS::PathType::UserDir)};
    for (int up = 0; up < 6 && !dir.empty() && dir != dir.parent_path();
         ++up, dir = dir.parent_path()) {
        if (std::filesystem::is_directory(dir / "roms", ec)) {
            for (const char* system : {"ps4", "ps5"}) {
                found.push_back(dir / "roms" / system);
            }
            break;
        }
    }
    return found;
}

/// Gold particles drifting slowly over a dark backdrop with a beam of light.
void DrawParticles(ImDrawList* dl, ImVec2 o, ImVec2 size, float s) {
    dl->AddRectFilledMultiColor(o, {o.x + size.x, o.y + size.y}, IM_COL32(34, 30, 30, 255),
                                IM_COL32(14, 15, 22, 255), IM_COL32(6, 8, 14, 255),
                                IM_COL32(16, 16, 22, 255));
    const ImVec2 light{o.x + size.x * 0.18f, o.y - size.y * 0.12f};
    for (int i = 0; i < 90; ++i) { // many faint discs: a smooth glow, no rings
        dl->AddCircleFilled(light, size.y * (0.06f + 0.0105f * i), IM_COL32(255, 236, 210, 2), 96);
    }
    Menu::Waves(dl, o, size, s, 0.8f);
    const float time = static_cast<float>(ImGui::GetTime());
    uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    for (int i = 0; i < 170; ++i) {
        const float fx = next(), fy = next(), speed = 0.004f + next() * 0.012f;
        const float phase = next() * 6.28f, big = next();
        // Most of them in a band across the lower half, as dust in the light.
        const float x = std::fmod(fx + time * speed, 1.0f);
        const float y = 0.35f + fy * 0.6f + std::sin(time * 0.3f + phase) * 0.01f;
        const float twinkle = 0.5f + 0.5f * std::sin(time * (0.6f + big) + phase);
        const float r = (big > 0.93f ? 5.5f : 1.2f + big * 2.2f) * s;
        const int a = static_cast<int>((big > 0.93f ? 70.0f : 120.0f) * twinkle + 30.0f);
        dl->AddCircleFilled({o.x + x * size.x, o.y + y * size.y}, r, IM_COL32(255, 196, 120, a),
                            12);
    }
}

} // namespace

/// The guide's steps; at the end the flag is written and the home screen comes.
void DrawOnboarding() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float s = std::clamp(size.x / 1500.0f, 0.6f, 2.6f);
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.05f);
    const auto kind = CurrentPadKind();
    DrawParticles(dl, o, size, s);
    onboarding.shown = std::min(1.0f, onboarding.shown + dt * 3.0f);
    const int alpha = static_cast<int>(255.0f * onboarding.shown);
    const auto centered = [&](const std::string& text, float y, float scale, ImU32 col) {
        ImGui::SetWindowFontScale(s * scale);
        const float wrap = size.x * 0.62f;
        const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
        ImGui::SetCursorScreenPos({o.x + (size.x - ts.x) * 0.5f, y});
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        return y + ts.y;
    };
    const ImU32 white = IM_COL32(245, 245, 248, alpha);
    const ImU32 dim = IM_COL32(220, 222, 230, alpha * 3 / 4);
    const auto retrobat = RetroBatRomFolders();
    constexpr int steps = 5;
    bool next = false, back = false, action_pressed = false;
    const char* ok_label = "OK";
    const char* action_label = nullptr;

    switch (onboarding.step) {
    case 0: {
        centered("Bienvenido a ForkStation", o.y + size.y * 0.20f, 1.25f, white);
        centered("Tus juegos de PS4 y PS5 en PC, cada uno con el fork o port que mejor lo mueve.",
                 o.y + size.y * 0.31f, 0.5f, dim);
        const ImVec2 c{o.x + size.x * 0.5f, o.y + size.y * 0.52f};
        const float pulse = 1.0f + 0.04f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.5f);
        const float r = 58.0f * s * pulse;
        dl->AddCircle(c, r + 10.0f * s, IM_COL32(255, 255, 255, alpha), 64, 5.0f * s);
        ImGui::SetWindowFontScale(s * 1.1f);
        Menu::DrawPadButton(dl, kind, Menu::PadButton::Confirm, c, r * 0.78f);
        centered(kind == Menu::PadKind::Keyboard
                     ? "Pulsa Enter para empezar."
                     : "Pulsa el botón de aceptar de tu mando para empezar.",
                 o.y + size.y * 0.68f, 0.5f, white);
        ok_label = "Empezar";
        break;
    }
    case 1: {
        float y = centered("1.  Consigue tus juegos", o.y + size.y * 0.14f, 0.95f, white);
        y = centered("Vuelca desde tu consola las copias de los juegos que tienes. Cada juego es "
                     "una carpeta:",
                     y + 26.0f * s, 0.5f, dim);
        y = centered("PS4:  CUSA00000, con eboot.bin y sce_sys/param.sfo", y + 18.0f * s, 0.5f,
                     white);
        y = centered("PS5:  PPSA00000, con eboot.bin y sce_sys/param.json", y + 8.0f * s, 0.5f,
                     white);
        centered("Opcional: empaqueta la carpeta en un .zar (ocupa hasta un 70 % menos). Pon los "
                 "juegos de cada consola juntos en una carpeta, por ejemplo roms/ps4 y roms/ps5.",
                 y + 18.0f * s, 0.5f, dim);
        break;
    }
    case 2: {
        float y =
            centered("2.  Añade la carpeta de tus juegos", o.y + size.y * 0.14f, 0.95f, white);
        if (!retrobat.empty()) {
            y = centered("Junto a ForkStation hay carpetas de juegos (roms/ps4 y roms/ps5): puedes "
                         "usarlas directamente.",
                         y + 26.0f * s, 0.5f, dim);
            action_label = "Usar las carpetas detectadas";
        } else {
            y = centered("Elige la carpeta que contiene las carpetas (o .zar) de tus juegos. "
                         "Puedes añadir más después con el botón + de la pantalla de inicio.",
                         y + 26.0f * s, 0.5f, dim);
            action_label = "Elegir carpeta";
        }
        if (onboarding.added > 0 || !gameIcons.empty()) {
            centered(fmt::format("Juegos encontrados: {}", gameIcons.size()), y + 20.0f * s, 0.5f,
                     IM_COL32(150, 230, 160, alpha));
        }
        break;
    }
    case 3: {
        float y = centered("3.  Instala sus motores", o.y + size.y * 0.14f, 0.95f, white);
        y = centered("Cada juego se abre con su fork o port (SpartanPort, bloodborne_pc, "
                     "KytyPS5...). Si falta o hay una versión nueva, su icono lleva un punto "
                     "naranja.",
                     y + 26.0f * s, 0.5f, dim) +
            30.0f * s;
        ImGui::SetWindowFontScale(s * 0.5f);
        const char* text = "Actualizar: lo descarga e instala desde su GitHub";
        const float tw = ImGui::CalcTextSize(text).x;
        const float bx = o.x + (size.x - tw) * 0.5f;
        Menu::DrawPadButton(dl, kind, Menu::PadButton::Action, {bx - 30.0f * s, y + 14.0f * s},
                            18.0f * s);
        dl->AddText({bx, y}, white, text);
        break;
    }
    case 4: {
        float y = centered("4.  Elige y juega", o.y + size.y * 0.14f, 0.95f, white) + 34.0f * s;
        ImGui::SetWindowFontScale(s * 0.5f);
        const float bx = o.x + size.x * 0.5f - 250.0f * s;
        const auto line = [&](Menu::PadButton b, const char* text) {
            Menu::DrawPadButton(dl, kind, b, {bx, y + 14.0f * s}, 18.0f * s);
            dl->AddText({bx + 48.0f * s, y}, white, text);
            y += 64.0f * s;
        };
        line(Menu::PadButton::Shoulders, "Cambia entre tus juegos de PS4 y de PS5");
        line(Menu::PadButton::Confirm, "Juega el juego elegido (la cruceta lo elige)");
        line(Menu::PadButton::Confirm, "Elige la resolución: 1080p, 1440p o 4K");
        line(Menu::PadButton::Back, "Select + Start cierra el juego y vuelve");
        ok_label = "Ir a mis juegos";
        break;
    }
    default:
        break;
    }

    // The step's action (adding folders) and OK: pills at the bottom.
    ImGui::SetWindowFontScale(s * 0.52f);
    const float bh = 62.0f * s;
    const float bw = 440.0f * s;
    const float by = o.y + size.y - 190.0f * s;
    if (action_label) {
        const ImVec2 at{o.x + (size.x - bw) * 0.5f, by - bh - 20.0f * s};
        ImGui::SetCursorScreenPos(at);
        action_pressed = ImGui::InvisibleButton("##ob_action", {bw, bh});
        dl->AddRectFilled(at, {at.x + bw, at.y + bh},
                          ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 60)
                                                 : IM_COL32(255, 255, 255, 34),
                          bh * 0.5f);
        dl->AddRect(at, {at.x + bw, at.y + bh}, IM_COL32(255, 255, 255, 120), bh * 0.5f, 0,
                    1.5f * s);
        const ImVec2 ts = ImGui::CalcTextSize(action_label);
        dl->AddText({at.x + (bw - ts.x) * 0.5f, at.y + (bh - ts.y) * 0.5f}, white, action_label);
        Menu::DrawPadButton(dl, kind, Menu::PadButton::Action, {at.x + 36.0f * s, at.y + bh * 0.5f},
                            15.0f * s);
    }
    {
        const ImVec2 at{o.x + (size.x - bw) * 0.5f, by};
        ImGui::SetCursorScreenPos(at);
        next = ImGui::InvisibleButton("##ob_ok", {bw, bh});
        dl->AddRectFilled(at, {at.x + bw, at.y + bh}, IM_COL32(18, 20, 26, 200), bh * 0.5f);
        dl->AddRect(at, {at.x + bw, at.y + bh}, IM_COL32(255, 255, 255, 235), bh * 0.5f, 0,
                    3.0f * s);
        const ImVec2 ts = ImGui::CalcTextSize(ok_label);
        dl->AddText({at.x + (bw - ts.x) * 0.5f, at.y + (bh - ts.y) * 0.5f}, white, ok_label);
    }
    // Where the player is: one bar per step, bottom left.
    for (int i = 0; i < steps; ++i) {
        const float x = o.x + 64.0f * s + i * 70.0f * s;
        const float y = o.y + size.y - 50.0f * s;
        dl->AddRectFilled({x, y}, {x + 60.0f * s, y + 3.0f * s},
                          i == onboarding.step ? IM_COL32(255, 255, 255, 230)
                                               : IM_COL32(255, 255, 255, 60));
    }
    ImGui::SetWindowFontScale(s * 0.36f);
    std::vector<std::pair<Menu::PadButton, const char*>> hints;
    if (action_label) {
        hints.push_back({Menu::PadButton::Action, action_label});
    }
    hints.push_back({Menu::PadButton::Confirm, "Siguiente"});
    if (onboarding.step > 0) {
        hints.push_back({Menu::PadButton::Back, "Atrás"});
    }
    DrawPadHints(dl, hints, o.x + size.x - 64.0f * s, o.y + size.y - 46.0f * s, s);
    ImGui::SetWindowFontScale(s);

    // The pad and the keyboard.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !showSettings && !browser.open) {
        next = next || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (action_label && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) ||
                             ImGui::IsKeyPressed(ImGuiKey_U, false))) {
            action_pressed = true;
        }
    }
    if (action_pressed && onboarding.step == 2) {
        if (!retrobat.empty()) {
            for (const auto& folder : retrobat) {
                std::error_code ec;
                if (std::filesystem::is_directory(folder, ec)) {
                    EmulatorSettings.AddGameInstallDir(folder);
                }
            }
            EmulatorSettings.Save();
            GetGameIconInfo(gameIcons);
            ++onboarding.added;
        } else {
            OpenFolderBrowser();
        }
    }
    if (back && onboarding.step > 0) {
        --onboarding.step;
        onboarding.shown = 0.0f;
    } else if (next) {
        if (onboarding.step + 1 < steps) {
            ++onboarding.step;
            onboarding.shown = 0.0f;
        } else {
            onboarding.active = false;
            std::ofstream{OnboardingFlag()} << "La guia de bienvenida de ForkStation ya se vio.\n";
        }
    }
}

// ---- The start: a short animation of its own, skipped by any button ----

namespace {
struct Intro {
    bool active = false;
    double start = -1.0;
};
Intro intro;
} // namespace

void DrawIntro() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float s = std::clamp(size.x / 1500.0f, 0.6f, 2.6f);
    const double now = ImGui::GetTime();
    if (intro.start < 0.0) {
        intro.start = now;
    }
    const float t = static_cast<float>(now - intro.start);
    constexpr float length = 3.4f;
    DrawParticles(dl, o, size, s);
    // The mark: a ring that draws itself, the fork inside it, then the name.
    const ImVec2 c{o.x + size.x * 0.5f, o.y + size.y * 0.45f};
    const float r = 70.0f * s;
    const float ring = std::clamp((t - 0.4f) / 0.9f, 0.0f, 1.0f);
    const float mark = std::clamp((t - 0.9f) / 0.6f, 0.0f, 1.0f);
    const float name = std::clamp((t - 1.4f) / 0.6f, 0.0f, 1.0f);
    if (ring > 0.0f) {
        dl->PathArcTo(c, r, -1.5708f, -1.5708f + ring * 6.2832f, 96);
        dl->PathStroke(IM_COL32(255, 255, 255, 235), 0, 5.0f * s);
    }
    if (mark > 0.0f) {
        Menu::DrawIcon(dl, Menu::Icon::Fork, c, r * 0.62f,
                       IM_COL32(255, 255, 255, static_cast<int>(mark * 255.0f)));
    }
    if (name > 0.0f) {
        ImGui::SetWindowFontScale(s * 1.0f);
        const char* title = "ForkStation";
        const ImVec2 ts = ImGui::CalcTextSize(title);
        dl->AddText({c.x - ts.x * 0.5f, c.y + r + 34.0f * s - (1.0f - name) * 12.0f * s},
                    IM_COL32(245, 245, 248, static_cast<int>(name * 255.0f)), title);
        ImGui::SetWindowFontScale(s * 0.42f);
        const char* sub = "Juegos de PS4 y PS5 en PC";
        const ImVec2 ss = ImGui::CalcTextSize(sub);
        dl->AddText({c.x - ss.x * 0.5f, c.y + r + 34.0f * s + ts.y + 10.0f * s},
                    IM_COL32(220, 222, 230, static_cast<int>(name * 180.0f)), sub);
        ImGui::SetWindowFontScale(s);
    }
    // In from black, and out to the next screen.
    const float in = std::clamp(t / 0.6f, 0.0f, 1.0f);
    const float out = std::clamp((t - (length - 0.6f)) / 0.6f, 0.0f, 1.0f);
    const float black = std::max(1.0f - in, out);
    if (black > 0.0f) {
        dl->AddRectFilled(o, {o.x + size.x, o.y + size.y},
                          IM_COL32(0, 0, 0, static_cast<int>(black * 255.0f)));
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool skip =
        t > 0.3f && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Space, false) || io.MouseClicked[0]);
    if (t >= length || skip) {
        intro.active = false;
    }
}

/// The home screen. Sets `settingsFor` / `updateFor` to the game whose action was chosen, and
/// starts the chosen game like the grid's cards do.
void DrawHome(int& settingsFor, int& updateFor, bool& openAbout) {
    ImGuiIO& io = ImGui::GetIO();
    // Everything follows the window's size, as on a television.
    const float s = std::clamp(ImGui::GetWindowSize().x / 1500.0f, 0.6f, 2.6f);
    const float dt = std::min(io.DeltaTime, 0.05f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImVec2 end{origin.x + size.x, origin.y + size.y};

    // The tabs: the consoles that have games.
    std::array<std::vector<int>, 2> lists;
    for (int i = 0; i < static_cast<int>(gameIcons.size()); ++i) {
        lists[gameIcons[i].ps5 ? 1 : 0].push_back(i);
    }
    if (home.tab < 0 || lists[home.tab].empty()) {
        home.tab = lists[0].empty() ? 1 : 0;
    }
    auto& list = lists[home.tab];
    home.grow.resize(gameIcons.size(), 0.0f);
    int& sel = home.selected[home.tab];
    sel = std::clamp(sel, 0, std::max(0, static_cast<int>(list.size()) - 1));

    // The pad and the keyboard, while no dialog is open.
    const bool free_input = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !showSettings &&
                            !browser.open && !home.menu;
    bool play = false;
    if (free_input && !list.empty()) {
        const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
            return std::ranges::any_of(keys,
                                       [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); });
        };

        const int top_count = 2;    // adding games and about (the tabs: L1 / R1 only)
        const int action_count = 2; // Play and the options
        if (pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp}) &&
            home.zone > 0) {
            --home.zone;
            home.item = home.zone == 0 ? 0 : home.item;
        }
        if (pressed({ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown}) &&
            home.zone < 1) {
            ++home.zone;
            home.item = 0;
        }
        const int count_here = home.zone == 0 ? top_count : action_count;
        if (pressed(
                {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight})) {
            if (home.zone == 1) {
                sel = std::min(sel + 1, static_cast<int>(list.size()) - 1);
            } else {
                home.item = std::min(home.item + 1, count_here - 1);
            }
        }
        if (pressed({ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft})) {
            if (home.zone == 1) {
                sel = std::max(sel - 1, 0);
            } else {
                home.item = std::max(home.item - 1, 0);
            }
        }
        home.item = std::clamp(home.item, 0, std::max(0, count_here - 1));
        const bool other_tab = !lists[1 - home.tab].empty();
        if (other_tab && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) && home.tab == 0) {
            home.tab = 1;
        } else if (other_tab && ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) && home.tab == 1) {
            home.tab = 0;
        } else if (other_tab && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
            home.tab = 1 - home.tab;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
            if (home.zone == 1) {
                play = true;
            } else if (home.zone == 2) {
                if (home.item == 0) {
                    play = true;
                } else {
                    home.menu = true;
                    home.menu_frame = ImGui::GetFrameCount();
                    home.menu_item = 0;
                }
            } else if (home.item == 0) {
                OpenFolderBrowser();
            } else {
                openAbout = true;
            }
        }
        // Options (DualSense) or View / Back (Xbox) open the game's options, as on a console.
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Menu, false)) {
            home.menu = true;
            home.menu_frame = ImGui::GetFrameCount();
            home.menu_item = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && home.zone != 1) {
            home.zone = 1; // back to the games
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) ||
            ImGui::IsKeyPressed(ImGuiKey_U, false)) {
            updateFor = list[sel];
        }
        if ((ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) ||
             ImGui::IsKeyPressed(ImGuiKey_S, false)) &&
            !gameIcons[list[sel]].external) {
            settingsFor = list[sel];
        }
        if (io.MouseWheel != 0.0f && ImGui::IsWindowHovered()) {
            sel = std::clamp(sel - static_cast<int>(io.MouseWheel), 0,
                             static_cast<int>(list.size()) - 1);
        }
    }
    auto& tabList = lists[home.tab];
    const int chosen =
        tabList.empty()
            ? -1
            : tabList[std::clamp(home.selected[home.tab], 0, static_cast<int>(tabList.size()) - 1)];

    // Transitions on a change of game or tab.
    if (chosen != home.shown_game) {
        home.shown_game = chosen;
        home.info_in = 0.0f;
    }
    if (home.tab != home.shown_tab) {
        home.shown_tab = home.tab;
        home.row_in = 0.0f;
    }
    home.info_in = std::min(1.0f, home.info_in + dt * 4.0f);
    home.row_in = std::min(1.0f, home.row_in + dt * 3.5f);
    home.menu_in = home.menu ? std::min(1.0f, home.menu_in + dt * 7.0f) : 0.0f;
    const auto ease = [](float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); };
    const float info_a = ease(home.info_in);
    const float row_a = ease(home.row_in);

    // The art: the chosen game's, faded in over the previous one.
    SDL_Texture* art = chosen >= 0 ? GameArt(gameIcons[chosen]) : nullptr;
    if (art != home.shown) {
        home.previous = home.shown;
        home.shown = art;
        home.fade = 0.0f;
    }
    home.fade = std::min(1.0f, home.fade + dt * 3.5f);
    dl->AddRectFilledMultiColor(origin, end, IM_COL32(10, 14, 28, 255), IM_COL32(18, 30, 62, 255),
                                IM_COL32(8, 10, 18, 255), IM_COL32(6, 8, 14, 255));
    if (home.previous && home.fade < 1.0f) {
        DrawCover(dl, home.previous, origin, end, 1.0f - home.fade);
    }
    if (!home.shown) {
        Menu::Waves(dl, origin, size, s, 1.0f);
    }
    DrawCover(dl, home.shown, origin, end, home.fade);
    // Shades for the text: the left side, the top bar and the bottom.
    const float w = size.x, h = size.y;
    dl->AddRectFilledMultiColor(origin, {origin.x + w * 0.62f, end.y}, IM_COL32(4, 6, 12, 235),
                                IM_COL32(4, 6, 12, 0), IM_COL32(4, 6, 12, 0),
                                IM_COL32(4, 6, 12, 235));
    dl->AddRectFilledMultiColor(origin, {end.x, origin.y + 230.0f * s}, IM_COL32(0, 0, 0, 190),
                                IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor({origin.x, origin.y + h * 0.55f}, end, IM_COL32(3, 4, 8, 0),
                                IM_COL32(3, 4, 8, 0), IM_COL32(3, 4, 8, 240),
                                IM_COL32(3, 4, 8, 240));

    const float margin = 64.0f * s;

    // Top bar: the consoles on the left; adding games, the information and the clock on the right.
    float x = origin.x + margin;
    const float bar_y = origin.y + 26.0f * s;

    const auto ring = [&](ImVec2 min, ImVec2 max, float rounding) {
        dl->AddRect({min.x - 8.0f * s, min.y - 6.0f * s}, {max.x + 8.0f * s, max.y + 6.0f * s},
                    IM_COL32(255, 255, 255, 240), rounding, 0, 2.5f * s);
    };
    for (int t = 0; t < 2; ++t) {
        if (lists[t].empty()) {
            continue;
        }
        const char* label = t == 0 ? "Juegos de PS4" : "Juegos de PS5";
        ImGui::SetWindowFontScale(s * (t == home.tab ? 0.62f : 0.56f));
        const ImVec2 ts = ImGui::CalcTextSize(label);
        ImGui::SetCursorScreenPos({x, bar_y});
        if (ImGui::InvisibleButton(label, ts)) {
            home.tab = t;
        }
        dl->AddText({x, bar_y},
                    t == home.tab ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 130),
                    label);

        if (t == home.tab) {
            dl->AddRectFilled({x, bar_y + ts.y + 6.0f * s}, {x + ts.x, bar_y + ts.y + 9.0f * s},
                              IM_COL32(255, 255, 255, 230), 2.0f * s);
        }
        x += ts.x + 34.0f * s;
    }
    {
        char clock[16]{};
        const std::time_t now = std::time(nullptr);
        if (const std::tm* t = std::localtime(&now)) {
            std::strftime(clock, sizeof(clock), "%H:%M", t);
        }
        ImGui::SetWindowFontScale(s * 0.6f);
        const ImVec2 cs = ImGui::CalcTextSize(clock);
        float rx = end.x - margin - cs.x;
        dl->AddText({rx, bar_y}, IM_COL32(255, 255, 255, 235), clock);
        // Information (about) and adding games: round glass buttons.
        const float r = 19.0f * s;
        const auto round_button = [&](const char* id, const char* glyph) {
            rx -= r * 2.0f + 20.0f * s;
            const ImVec2 c{rx + r, bar_y + cs.y * 0.5f};
            ImGui::SetCursorScreenPos({c.x - r, c.y - r});
            const bool pressed = ImGui::InvisibleButton(id, {r * 2.0f, r * 2.0f});
            dl->AddCircleFilled(c, r,
                                ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 70)
                                                       : IM_COL32(255, 255, 255, 34));
            ImGui::SetWindowFontScale(s * 0.55f);
            const ImVec2 gs = ImGui::CalcTextSize(glyph);
            dl->AddText({c.x - gs.x * 0.5f, c.y - gs.y * 0.5f}, IM_COL32(255, 255, 255, 240),
                        glyph);
            ImGui::SetWindowFontScale(s * 0.6f);
            return pressed;
        };

        if (round_button("##about", "i")) {
            openAbout = true;
        }
        if (home.zone == 0 && home.item == 1) {
            ring(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), r + 6.0f * s);
        }
        if (round_button("##add", "+")) {
            OpenFolderBrowser();
        }
        if (home.zone == 0 && home.item == 0) {
            ring(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), r + 6.0f * s);
        }
    }

    // The tile row: the chosen tile first, larger and outlined, its name under it.
    const float tile = 104.0f * s;
    const float big = 140.0f * s;
    const float gap = 16.0f * s;
    const float row_y = origin.y + 92.0f * s;
    const int count = static_cast<int>(tabList.size());
    const int cur = std::clamp(home.selected[home.tab], 0, std::max(0, count - 1));
    home.scroll += (cur * (tile + gap) - home.scroll) * std::min(1.0f, dt * 12.0f);
    for (int k = 0; k < count; ++k) {
        const int i = tabList[k];
        float& g = home.grow[i];
        g += ((k == cur ? 1.0f : 0.0f) - g) * std::min(1.0f, dt * 14.0f);
        const float side = tile + (big - tile) * g;
        float tx = origin.x + margin + k * (tile + gap) - home.scroll +
                   (1.0f - row_a) * (60.0f + 18.0f * k) * s;
        if (k > cur) {
            tx += big - tile;
        }
        if (tx > end.x || tx + side < origin.x) {
            continue;
        }
        const ImVec2 tmin{tx, row_y};
        const ImVec2 tmax{tx + side, row_y + side};
        ImGui::SetCursorScreenPos(tmin);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tile", {side, side})) {
            if (k == cur) {
                play = true;
            } else {
                home.selected[home.tab] = k;
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const float round = 14.0f * s;
        if (k == cur && home.zone == 1) {
            dl->AddRectFilled({tmin.x - 6.0f * s, tmin.y - 6.0f * s},
                              {tmax.x + 6.0f * s, tmax.y + 6.0f * s}, IM_COL32(255, 255, 255, 40),
                              round + 6.0f * s);
        }
        if (gameIcons[i].textureId) {
            const int a = k == cur || hovered ? 255 : 200;
            dl->AddImageRounded(gameIcons[i].textureId, tmin, tmax, {0, 0}, {1, 1},
                                IM_COL32(a, a, a, static_cast<int>(255.0f * row_a)), round);
        } else {
            dl->AddRectFilled(tmin, tmax, IM_COL32(40, 46, 60, 255), round);
        }
        if (k == cur && home.zone == 1) {
            dl->AddRect({tmin.x - 3.0f * s, tmin.y - 3.0f * s},
                        {tmax.x + 3.0f * s, tmax.y + 3.0f * s}, IM_COL32(255, 255, 255, 255),
                        round + 3.0f * s, 0, 3.0f * s);
        }
        // An engine with a newer release: a small orange dot on its tile.
        if (const auto hint = Common::GetUpdateHint(gameIcons[i].serial);
            hint && (hint->available || hint->missing)) {
            dl->AddCircleFilled({tmax.x - 10.0f * s, tmin.y + 10.0f * s}, 6.5f * s,
                                hint->missing ? IM_COL32(230, 70, 60, 255)
                                              : IM_COL32(255, 160, 30, 255));
            dl->AddCircle({tmax.x - 10.0f * s, tmin.y + 10.0f * s}, 6.5f * s,
                          IM_COL32(0, 0, 0, 160), 0, 1.5f * s);
        }
    }
    if (chosen < 0) {
        ImGui::SetWindowFontScale(s);
        return;
    }
    const IconInfo& game = gameIcons[chosen];

    // The chosen game: its name, console and engine, and its actions.
    const float info_x = origin.x + margin;
    float y = origin.y + h * 0.40f + (1.0f - info_a) * 18.0f * s;
    ImGui::SetWindowFontScale(s * 1.2f);
    ImGui::PushTextWrapPos(info_x + w * 0.48f - origin.x);
    ImGui::SetCursorScreenPos({info_x, y});
    ImGui::PushStyleColor(ImGuiCol_Text,
                          IM_COL32(255, 255, 255, static_cast<int>(255.0f * info_a)));
    ImGui::TextWrapped("%s", game.title.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    y = ImGui::GetCursorScreenPos().y + 6.0f * s;
    ImGui::SetWindowFontScale(s * 0.5f);
    const std::string engine_line =
        fmt::format("{}   ·   {}", game.ps5 ? "PlayStation 5" : "PlayStation 4",
                    SplitDetail(game.details[0]).second);
    dl->AddText({info_x, y}, IM_COL32(255, 255, 255, static_cast<int>(190.0f * info_a)),
                engine_line.c_str());
    y += ImGui::CalcTextSize(engine_line.c_str()).y + 10.0f * s;

    const auto hint = Common::GetUpdateHint(game.serial);
    if (hint && (hint->available || hint->missing)) {
        const std::string notice =
            hint->missing ? fmt::format("Falta su motor: pulsa Actualizar para instalar {}",
                                        hint->latest_version)
                          : fmt::format("Nueva versión de su fork en GitHub: {}  (tienes {})",
                                        hint->latest_version, hint->installed_version);
        dl->AddCircleFilled({info_x + 6.0f * s, y + ImGui::GetTextLineHeight() * 0.5f}, 5.0f * s,
                            hint->missing ? IM_COL32(230, 70, 60, 255)
                                          : IM_COL32(255, 160, 30, 255));
        dl->AddText({info_x + 20.0f * s, y}, IM_COL32(255, 196, 120, 255), notice.c_str());
        y += ImGui::GetTextLineHeight() + 12.0f * s;
    } else {
        y += 8.0f * s;
    }

    // The actions: Play (white), Update (with a dot when there is a newer release), Settings.
    ImGui::SetWindowFontScale(s * 0.5f);
    float bx = info_x;
    // Playing: the cross on the game, or the options menu (Options / View), which opens here.
    home.menu_at = {bx, y};

    // The details: glass cards along the bottom.
    ImGui::SetWindowFontScale(s * 0.48f);
    const float cards_y = end.y - 190.0f * s;
    dl->AddText({info_x, cards_y - 42.0f * s}, IM_COL32(255, 255, 255, 235), "Detalles");
    float cx = info_x;
    const float card_h = 92.0f * s;
    for (const auto& line : game.details) {
        if (line.empty()) {
            continue;
        }
        auto [label, value] = SplitDetail(line);
        if (label.empty()) {
            label = "Ajuste";
        }
        ImGui::SetWindowFontScale(s * 0.4f);
        const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
        ImGui::SetWindowFontScale(s * 0.52f);
        const ImVec2 vs = ImGui::CalcTextSize(value.c_str());
        const float card_w = std::max({ls.x, vs.x, 150.0f * s}) + 40.0f * s;
        if (cx + card_w > end.x - margin) {
            break;
        }
        dl->AddRectFilled({cx, cards_y}, {cx + card_w, cards_y + card_h},
                          IM_COL32(255, 255, 255, 26), 16.0f * s);
        dl->AddRect({cx, cards_y}, {cx + card_w, cards_y + card_h}, IM_COL32(255, 255, 255, 40),
                    16.0f * s, 0, 1.0f * s);
        ImGui::SetWindowFontScale(s * 0.4f);
        dl->AddText({cx + 20.0f * s, cards_y + 18.0f * s}, IM_COL32(255, 255, 255, 150),
                    label.c_str());
        ImGui::SetWindowFontScale(s * 0.52f);
        dl->AddText({cx + 20.0f * s, cards_y + 18.0f * s + ls.y + 8.0f * s},
                    IM_COL32(255, 255, 255, 245), value.c_str());
        cx += card_w + 14.0f * s;
    }

    // The game's options: a menu under the options button, in the look of a console's.
    if (home.menu) {
        struct Choice {
            const char* label;
            std::string value;
            int action; // 0 play, 1 update, 2 settings
            bool dot;
        };
        std::vector<Choice> choices;
        choices.push_back({"Jugar", "", 0, false});
        std::string update_value = "Comprobar";
        if (hint && hint->missing) {
            update_value = "Instalar " + hint->latest_version;
        } else if (hint && hint->available) {
            update_value = "Nueva: " + hint->latest_version;
        } else if (hint && hint->checked) {
            update_value = "Al día";
        }
        choices.push_back({"Actualizar fork desde su GitHub", update_value, 1,
                           hint && (hint->available || hint->missing)});
        if (!game.external) {
            choices.push_back({"Ajustes del juego", "", 2, false});
        }
        const int count = static_cast<int>(choices.size());
        home.menu_item = std::clamp(home.menu_item, 0, count - 1);
        ImGui::SetWindowFontScale(s * 0.48f);
        const float row_h = 56.0f * s;
        const float pad = 10.0f * s;
        float width = 0.0f;
        for (const auto& c : choices) {
            width = std::max(width, ImGui::CalcTextSize(c.label).x +
                                        ImGui::CalcTextSize(c.value.c_str()).x + 150.0f * s);
        }
        const std::string head = SplitDetail(game.details[0]).second;
        ImGui::SetWindowFontScale(s * 0.38f);
        const float head_h = ImGui::CalcTextSize(head.c_str()).y + 22.0f * s;
        width = std::max(width, ImGui::CalcTextSize(head.c_str()).x + 48.0f * s);
        const float menu_a = ease(home.menu_in);
        const ImVec2 at{home.menu_at.x, home.menu_at.y - (1.0f - menu_a) * 14.0f * s};
        dl->PushClipRectFullScreen();
        const int panel_alpha = static_cast<int>(246.0f * menu_a);
        const ImVec2 max{at.x + width + pad * 2.0f, at.y + head_h + count * row_h + pad * 2.0f};
        dl->AddRectFilled({at.x + 6.0f * s, at.y + 8.0f * s}, {max.x + 6.0f * s, max.y + 8.0f * s},
                          IM_COL32(0, 0, 0, 90), 10.0f * s);
        dl->AddRectFilled(at, max, IM_COL32(40, 42, 50, panel_alpha), 8.0f * s);
        dl->AddRect(at, max, IM_COL32(255, 255, 255, 30), 8.0f * s, 0, 1.0f * s);
        dl->AddText({at.x + pad + 14.0f * s, at.y + pad + 6.0f * s}, IM_COL32(255, 255, 255, 140),
                    head.c_str());
        ImGui::SetWindowFontScale(s * 0.48f);
        int chosen_row = -1;
        for (int i = 0; i < count; ++i) {
            const ImVec2 rmin{at.x + pad, at.y + pad + head_h + i * row_h};
            const ImVec2 rmax{max.x - pad, rmin.y + row_h};
            ImGui::SetCursorScreenPos(rmin);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##choice", {rmax.x - rmin.x, row_h})) {
                chosen_row = i;
            }
            if (ImGui::IsItemHovered() && io.MouseDelta.x != 0.0f) {
                home.menu_item = i;
            }
            ImGui::PopID();
            if (i == home.menu_item) {
                dl->AddRectFilled(rmin, rmax, IM_COL32(255, 255, 255, 18), 6.0f * s);
                dl->AddRect(rmin, rmax, IM_COL32(225, 228, 236, 235), 6.0f * s, 0, 2.0f * s);
            }
            const ImVec2 ls = ImGui::CalcTextSize(choices[i].label);
            dl->AddText({rmin.x + 22.0f * s, rmin.y + (row_h - ls.y) * 0.5f},
                        IM_COL32(242, 243, 247, 255), choices[i].label);
            if (!choices[i].value.empty()) {
                const ImVec2 vs = ImGui::CalcTextSize(choices[i].value.c_str());
                dl->AddText({rmax.x - 20.0f * s - vs.x, rmin.y + (row_h - vs.y) * 0.5f},
                            choices[i].dot ? IM_COL32(255, 190, 110, 255)
                                           : IM_COL32(255, 255, 255, 150),
                            choices[i].value.c_str());
            }
        }
        const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
            return std::ranges::any_of(keys,
                                       [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); });
        };
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !showSettings &&
            ImGui::GetFrameCount() != home.menu_frame) {
            if (pressed(
                    {ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown})) {
                home.menu_item = std::min(home.menu_item + 1, count - 1);
            }
            if (pressed({ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp})) {
                home.menu_item = std::max(home.menu_item - 1, 0);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                chosen_row = home.menu_item;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadBack, false)) {
                home.menu = false;
            }
        }
        dl->PopClipRect();
        if (chosen_row >= 0) {
            home.menu = false;
            switch (choices[chosen_row].action) {
            case 0:
                play = true;
                break;
            case 1:
                updateFor = chosen;
                break;
            default:
                settingsFor = chosen;
                break;
            }
        }
    }

    // The pad's buttons, bottom right (as the connected pad prints them).
    {
        ImGui::SetWindowFontScale(s * 0.38f);
        std::vector<std::pair<Menu::PadButton, const char*>> hints{
            {Menu::PadButton::Confirm, "Jugar"}, {Menu::PadButton::Action, "Actualizar"}};
        hints.push_back({Menu::PadButton::Options, "Opciones"});
        if (!lists[1 - home.tab].empty()) {
            hints.push_back({Menu::PadButton::Shoulders, "PS4 / PS5"});
        }
        DrawPadHints(dl, hints, end.x - margin, end.y - 40.0f * s, s);
    }
    ImGui::SetWindowFontScale(s);

    if (play) {
        done = true;
        Core::FileSys::MntPoints::ignore_game_patches =
            ImGui::IsKeyDown(ImGuiKey::ImGuiKey_LeftCtrl);
        runEbootPath = game.ebootPath;
        runSerial = game.serial;
    }
}

/// Full screen of its own (exclusive, at the desktop's mode), raised and in front of every other
/// window, as a console's screen.
void TakeTheScreen(SDL_Window* window) {
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window))) {
        SDL_SetWindowFullscreenMode(window, mode);
    }
    TakeTheScreen(window);
    Common::BringWindowToFront(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

std::optional<int> ChooseOptionMenu(const std::string& title, const std::string& subtitle,
                                    const std::vector<std::string>& labels, int selected) {
    const int n = static_cast<int>(labels.size());
    const int saved = selected; // the setting in use: ticked, as in a console's menus
    // The pad answers even while another window has the focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD) || n == 0) {
        return selected;
    }
    std::vector<SDL_Gamepad*> pads;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                pads.push_back(pad);
            }
        }
        SDL_free(ids);
    }
    SDL_Window* window = SDL_CreateWindow("ForkStation", 1280, 720,
                                          SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);
    SDL_Renderer* menu_renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!menu_renderer) {
        if (window) {
            SDL_DestroyWindow(window);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return selected;
    }
    Frontend::SetDefaultWindowIcon(window);
    SDL_SetWindowFullscreen(window, true);
    SDL_RaiseWindow(window);

    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    static const ImWchar latin_ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0};
    io.FontDefault = io.Fonts->AddFontFromMemoryCompressedTTF(
        imgui_font_poppins_medium_compressed_data, imgui_font_poppins_medium_compressed_size, 64.0f,
        &cfg, latin_ranges);
    io.FontGlobalScale = 0.5f;
    ImGui_ImplSDL3_InitForSDLRenderer(window, menu_renderer);
    ImGui_ImplSDLRenderer3_Init(menu_renderer);

    std::optional<int> result;
    bool done = false;
    int last_axis = 0;
    // The press that started the game must not also choose here.
    const Uint64 opened = SDL_GetTicks();
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            const bool settling = SDL_GetTicks() - opened < 500;
            if (settling &&
                (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_KEY_DOWN ||
                 e.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.repeat) {
                continue;
            }
            if (e.type == SDL_EVENT_QUIT) {
                done = true;
            } else if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad* pad = SDL_OpenGamepad(e.gdevice.which)) {
                    pads.push_back(pad);
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    selected = (selected + n - 1) % n;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    selected = (selected + 1) % n;
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                case SDL_GAMEPAD_BUTTON_START:
                    result = selected;
                    done = true;
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                case SDL_GAMEPAD_BUTTON_BACK:
                    done = true;
                    break;
                default:
                    break;
                }
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                       e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                const int dir = e.gaxis.value > 20000 ? 1 : e.gaxis.value < -20000 ? -1 : 0;
                if (dir != 0 && dir != last_axis) {
                    selected = (selected + n + dir) % n;
                }
                last_axis = dir;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_UP) {
                    selected = (selected + n - 1) % n;
                } else if (e.key.key == SDLK_DOWN) {
                    selected = (selected + 1) % n;
                } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE) {
                    result = selected;
                    done = true;
                } else if (e.key.key == SDLK_ESCAPE) {
                    done = true;
                }
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##choose", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float s = std::clamp(size.x / 1500.0f, 0.6f, 2.6f);
        Menu::Backdrop(dl, o, {o.x + size.x, o.y + size.y});
        ImGui::SetWindowFontScale(s * 0.95f);
        dl->AddText({o.x + 64.0f * s, o.y + 48.0f * s}, IM_COL32(245, 245, 248, 255),
                    title.c_str());
        ImGui::SetWindowFontScale(s * 0.45f);
        dl->AddText({o.x + 64.0f * s, o.y + 108.0f * s}, IM_COL32(255, 255, 255, 160),
                    subtitle.c_str());
        ImGui::SetWindowFontScale(s * 0.52f);
        const float lx = o.x + size.x * 0.155f;
        const float lw = size.x * 0.69f;
        const float rh = 72.0f * s;
        float y = o.y + 170.0f * s;
        for (int i = 0; i < n; ++i) {
            // "1440p   (2560 x 1440, 2K)": the name on the left, its size on the right.
            std::string label = labels[i];
            std::string value;
            if (const auto open = label.find('('); open != std::string::npos) {
                value = label.substr(open + 1);
                if (!value.empty() && value.back() == ')') {
                    value.pop_back();
                }
                label = label.substr(0, open);
                while (!label.empty() && label.back() == ' ') {
                    label.pop_back();
                }
            }
            ImGui::PushID(i);
            if (Menu::Row("##option", i == saved ? Menu::Icon::Check : Menu::Icon::Screen,
                          label.c_str(), value.c_str(), lx, y, lw, rh, i == selected) &&
                SDL_GetTicks() - opened >= 500) {
                selected = i;
                result = i;
                done = true;
            }
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x != 0.0f) {
                selected = i;
            }
            ImGui::PopID();
            y += rh;
        }
        // The pad's buttons, as the connected pad prints them.
        ImGui::SetWindowFontScale(s * 0.38f);
        Menu::PadKind kind = Menu::PadKind::Keyboard;
        for (SDL_Gamepad* pad : pads) {
            const auto type = SDL_GetGamepadType(pad);
            kind = (type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 ||
                    type == SDL_GAMEPAD_TYPE_PS5)
                       ? Menu::PadKind::PlayStation
                       : Menu::PadKind::Xbox;
        }
        float hx = o.x + size.x - 64.0f * s;
        const float hy = o.y + size.y - 46.0f * s;
        for (const auto& [button, text] : {std::pair{Menu::PadButton::Back, "Volver"},
                                           std::pair{Menu::PadButton::Confirm, "Aceptar"}}) {
            const ImVec2 ts = ImGui::CalcTextSize(text);
            hx -= ts.x;
            dl->AddText({hx, hy - ts.y * 0.5f}, IM_COL32(255, 255, 255, 210), text);
            const float r = 13.0f * s;
            const float wide = kind == Menu::PadKind::Keyboard ? r * 2.2f : r;
            hx -= wide + 8.0f * s;
            Menu::DrawPadButton(dl, kind, button, {hx, hy}, r);
            hx -= wide + 28.0f * s;
        }
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(menu_renderer, 15, 16, 20, 255);
        SDL_RenderClear(menu_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), menu_renderer);
        SDL_RenderPresent(menu_renderer);
        if (SDL_GetTicks() - opened < 1500 &&
            !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
            TakeTheScreen(window);
        }
        SDL_Delay(8);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    for (SDL_Gamepad* pad : pads) {
        SDL_CloseGamepad(pad);
    }
    SDL_DestroyRenderer(menu_renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    return result;
}

void LaunchUpdate(char* executableName, const std::string& serial) {
    updateOnlySerial = serial;
    Launch(executableName, false);
}

void Launch(char* executableName, bool sameProcess) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(ImGui, "SDL_INIT_VIDEO Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOG_ERROR(ImGui, "SDL_INIT_GAMEPAD Error: {}", SDL_GetError());
    }

    LoadSingleGame(executableName);
    SDL_Window* window =
        SDL_CreateWindow(single_game.active ? single_game.title.c_str() : "ForkStation",
                         single_game.active ? 1440 : 1280, single_game.active ? 960 : 800,
                         single_game.active ? SDL_WindowFlags{0} : SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        LOG_ERROR(ImGui, "SDL Window Creation Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    Frontend::SetDefaultWindowIcon(window);
    renderer = SDL_CreateRenderer(window, nullptr);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;

    ImFontConfig cfgBase;
    cfgBase.OversampleH = 2;
    cfgBase.OversampleV = 1;

    // Poppins for everything it covers, with the usual fonts merged in behind it for the rest.
    static const ImWchar latin_ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0};
    io.FontDefault = io.Fonts->AddFontFromMemoryCompressedTTF(
        imgui_font_poppins_medium_compressed_data, imgui_font_poppins_medium_compressed_size, 64.0f,
        &cfgBase, latin_ranges);
    ImFontConfig cfgFallback = cfgBase;
    cfgFallback.MergeMode = true;
    ImGui::FontStack::AddPrimaryUiFont(io.Fonts, 64.0f, EmulatorSettings.GetConsoleLanguage(),
                                       cfgFallback, true);
    io.FontGlobalScale = 0.5f;
    // size the big picture font atlas cap from the renderer limit
    const auto max_dim = SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                                               SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 8192);
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u64>(max_dim, 512)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    ImGuiEmuSettings::SettingsWindow settingsWindow(false);

    // The window takes the size its cards need (up to four in a row), scaled to fit the display.
    auto fitWindow = [&] {
        // The home screen is 16:9, like a television.
        const float want_w = 1600.0f;
        const float want_h = 900.0f;
        SDL_Rect usable{};
        if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable) ||
            usable.w <= 0 || usable.h <= 0) {
            usable = {0, 0, 1920, 1040};
        }
        uiScale = std::clamp(std::min({1.25f, usable.w * 0.92f / want_w, usable.h * 0.9f / want_h}),
                             0.6f, 1.25f);
        SDL_SetWindowSize(window, static_cast<int>(want_w * uiScale),
                          static_cast<int>(want_h * uiScale));
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        TakeTheScreen(window); // the home screen takes the whole screen, like a console's
    };
    auto applySettings = [&] {
        GetGameIconInfo(gameIcons);
        // The launcher always stays in a window; the full screen setting is for the games.
        if (!single_game.active) {
            fitWindow();
        }
    };
    applySettings();
    if (single_game.active) {
        SetSingleGameIcon(window, gameIcons);
        // Its size follows its contents; the size slider is not shown.
        uiScale = 1.0f;
    }

    bool updateRefreshed = false;
    // The first start (or no games yet): the guide through the setup.
    {
        std::error_code ec;
        const char* back = std::getenv("FORKSTATION_BACK");
        intro.active = !single_game.active && updateOnlySerial.empty() && !(back && back[0] == '1');
        onboarding.active = !single_game.active && updateOnlySerial.empty() &&
                            (!std::filesystem::exists(OnboardingFlag(), ec) || gameIcons.empty());
    }

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        Theme::Push(uiScale);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse;
        // With games, the launcher is the home screen (DrawHome), driven by its own pad keys.
        const bool introMode = intro.active;
        const bool onboardingMode = !single_game.active && onboarding.active && !introMode;
        const bool homeMode =
            !single_game.active && !gameIcons.empty() && !onboardingMode && !introMode;
        if (homeMode || onboardingMode || introMode) {
            window_flags |= ImGuiWindowFlags_NoNav;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        }

        ImGui::Begin("Game Window", &done, window_flags);
        if (homeMode || onboardingMode || introMode) {
            ImGui::PopStyleVar();
        }
        ImGui::SetWindowFontScale(uiScale);
        int settingsFor = -1;
        int suggestFor = -1;
        int updateFor = -1;
        bool openAbout = false;
        if (introMode) {
            DrawIntro();
        } else if (onboardingMode) {
            DrawOnboarding();
        } else if (homeMode) {
            DrawHome(settingsFor, updateFor, openAbout);
        } else {

            ImGuiChildFlags child_flags = ImGuiChildFlags_NavFlattened;

            ImGuiWindowFlags child_window_flags = ImGuiWindowFlags_NoTitleBar |
                                                  ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoCollapse;

            if (ImGui::IsWindowAppearing()) {
                ImGui::SetNextWindowFocus();
            }

            // Top bar: the name on the left, the two actions on the right.
            if (!single_game.active) {
                ImGui::SetWindowFontScale(uiScale * 1.5f);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("ForkStation");
                ImGui::SetWindowFontScale(uiScale);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", fmt::format("  {}", CleanEmulatorVersion()).c_str());
                ImGui::SameLine();
            }
            const char* add_label = single_game.active ? Tr("Game folder") : Tr("Add games");
            {
                // Only the folder button: settings belong to each game (its card's Settings button,
                // for the games this emulator runs; the others are set in their own program).
                const float width =
                    ImGui::CalcTextSize(add_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
            }
            if (Theme::Button(add_label)) {
                ImGuiEmuSettings::SettingsWindow::RequestGamesFolder();
            }

            ImGui::Dummy(ImVec2(0.0f, 8.f * uiScale));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                              child_flags, child_window_flags);
            ImGui::PopStyleColor();

            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();
            }

            if (single_game.active && !gameIcons.empty()) {
                DrawSingleGame(settingsWindow, showSettings, updateFor);
            } else {
                SetGameIcons(gameIcons, settingsFor, suggestFor, updateFor);
            }
            if (suggestFor >= 0) {
                settingsWindow.ApplySuggestedTo(gameIcons[suggestFor].serial,
                                                gameIcons[suggestFor].title);
            }
            // A folder picked in the system's dialog arrives here.
            if (settingsWindow.ConsumeGamesFolder()) {
                GetGameIconInfo(gameIcons);
            }
            // Without a system picker, adding a folder goes through the folders page and its
            // built-in one.
            if (ImGuiEmuSettings::SettingsWindow::BuiltinPickerPending() && !showSettings) {
                settingsWindow.Prepare();
                settingsWindow.OpenFolders();
                showSettings = true;
            }
            if (settingsFor >= 0) {
                settingsWindow.Prepare();
                settingsWindow.OpenProfile(gameIcons[settingsFor].serial,
                                           gameIcons[settingsFor].title);
                showSettings = true;
            }
            if (gameIcons.empty()) {
                ImGui::Dummy(ImVec2(0.0f, 40.f * uiScale));
                if (single_game.active) {
                    Overlay::TextCentered(
                        fmt::format(
                            fmt::runtime(Tr("{} was not found. Choose the folder that holds "
                                            "the game folder.")),
                            single_game.title)
                            .c_str());
                } else {
                    Overlay::TextCentered(
                        Tr("No games yet. Add the folder that contains your games."));
                }
                ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));
                const char* label =
                    single_game.active ? Tr("Choose the game folder") : Tr("Add games folder");
                const float width =
                    ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
                if (Theme::AccentButton(label)) {
                    ImGuiEmuSettings::SettingsWindow::RequestGamesFolder();
                }
            }
            ImGui::EndChild();
        } // the grid (single game, or no games yet)
        DrawFolderBrowser();
        if (homeMode || onboardingMode) {
            if (suggestFor >= 0) {
                settingsWindow.ApplySuggestedTo(gameIcons[suggestFor].serial,
                                                gameIcons[suggestFor].title);
            }
            if (settingsWindow.ConsumeGamesFolder()) {
                GetGameIconInfo(gameIcons);
            }
            if (ImGuiEmuSettings::SettingsWindow::BuiltinPickerPending() && !showSettings) {
                settingsWindow.Prepare();
                settingsWindow.OpenFolders();
                showSettings = true;
            }
            if (settingsFor >= 0) {
                settingsWindow.Prepare();
                settingsWindow.OpenProfile(gameIcons[settingsFor].serial,
                                           gameIcons[settingsFor].title);
                showSettings = true;
            }
        }

        if (updateFor >= 0 && updateFor < static_cast<int>(gameIcons.size())) {
            updateRefreshed = false;
            Common::StartEngineUpdate(gameIcons[updateFor].serial, false);
            ImGui::OpenPopup(Tr("Update engine"));
        }
        // Opened for one game's fork (from RetroBat): its update starts right away.
        static bool updateOnlyStarted = false;
        if (!updateOnlySerial.empty() && !updateOnlyStarted) {
            updateOnlyStarted = true;
            updateRefreshed = false;
            Common::StartEngineUpdate(updateOnlySerial, false);
            ImGui::OpenPopup(Tr("Update engine"));
        }

        // The fork's update: a page in the look of a console's settings, over the whole window.
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
        if (ImGui::BeginPopupModal(Tr("Update engine"), nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
            const auto st = Common::GetEngineUpdateStatus();
            if (st.finished && !updateRefreshed) {
                FillGameDetails(gameIcons);
                updateRefreshed = true;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 o = ImGui::GetWindowPos();
            const ImVec2 sz = ImGui::GetWindowSize();
            const float ms = std::clamp(sz.x / 1500.0f, 0.6f, 2.6f);
            Menu::Backdrop(dl, o, {o.x + sz.x, o.y + sz.y});
            ImGui::SetWindowFontScale(ms * 0.95f);
            dl->AddText({o.x + 64.0f * ms, o.y + 48.0f * ms}, IM_COL32(245, 245, 248, 255),
                        "Actualizar fork");
            ImGui::SetWindowFontScale(ms * 0.5f);
            const float lx = o.x + sz.x * 0.155f;
            const float lw = sz.x * 0.69f;
            const float rh = 66.0f * ms;
            float ry = o.y + 120.0f * ms;
            const auto info = [&](Menu::Icon icon, const char* label, const std::string& value) {
                Menu::Row(label, icon, label, value.c_str(), lx, ry, lw, rh, false, false);
                ry += rh;
            };
            info(Menu::Icon::Fork, "Fork", st.engine_name.empty() ? "—" : st.engine_name);
            info(Menu::Icon::Info, "Repositorio",
                 st.repo.empty() ? "—" : fmt::format("github.com/{}", st.repo));
            info(Menu::Icon::Chip, "Versión instalada",
                 st.installed_version.empty() ? "no instalado" : st.installed_version);
            info(Menu::Icon::Clock, "Última versión en GitHub",
                 st.latest_version.empty() ? (st.active ? "consultando..." : "—")
                                           : st.latest_version);
            const bool newer = !st.latest_version.empty() && !st.up_to_date && !st.finished;
            info(st.success && st.finished ? Menu::Icon::Check : Menu::Icon::Update, "Estado",
                 st.message);
            if (st.active) {
                const float frac =
                    st.progress >= 0.0f
                        ? st.progress
                        : static_cast<float>(std::fmod(ImGui::GetTime() * 0.65, 1.0));
                const float bx = lx + 64.0f * (rh / 64.0f);
                const float by = ry + rh * 0.45f;
                const float bw = lw - 90.0f * ms;
                dl->AddRectFilled({bx, by}, {bx + bw, by + 6.0f * ms}, IM_COL32(255, 255, 255, 40),
                                  3.0f * ms);
                dl->AddRectFilled({bx, by}, {bx + bw * frac, by + 6.0f * ms},
                                  IM_COL32(240, 242, 246, 255), 3.0f * ms);
                ry += rh;
            } else {
                ry += 18.0f * ms;
                if (st.up_to_date &&
                    Menu::Row("##reinstall", Menu::Icon::Update, "Reinstalar la misma versión", "",
                              lx, ry, lw, rh, false)) {
                    updateRefreshed = false;
                    Common::StartEngineUpdate(st.serial, true);
                }
                if (st.up_to_date) {
                    ry += rh;
                }
                const char* close_label =
                    updateOnlySerial.empty() ? "Cerrar" : "Continuar al juego";
                const bool closed = Menu::Row(
                    "##close", updateOnlySerial.empty() ? Menu::Icon::Close : Menu::Icon::Play,
                    close_label, "", lx, ry, lw, rh, false);
                if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemFocused()) {
                    ImGui::SetItemDefaultFocus(); // the pad's A closes it
                }
                if (closed || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                    Common::ClearEngineUpdateStatus();
                    ImGui::CloseCurrentPopup();
                    if (!updateOnlySerial.empty()) {
                        done = true; // back to RetroBat
                    }
                }
            }
            (void)newer;
            ImGui::SetWindowFontScale(uiScale);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (openAbout) {
            ImGui::OpenPopup(Tr("About ForkStation"));
        }
        // Whose work this is.
        if (!homeMode && !onboardingMode && !introMode) {
            const char* credit =
                single_game.active ? Tr("Ported to PC") : Tr("PS4 and PS5 games on PC");
            const float width = ImGui::CalcTextSize(credit).x + ImGui::CalcTextSize(Tr("About")).x +
                                ImGui::GetStyle().FramePadding.x * 2.0f +
                                ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", credit);
            ImGui::SameLine();
            if (Theme::Button(Tr("About"))) {
                ImGui::OpenPopup(Tr("About ForkStation"));
            }
        }

        // About: the same settings-like page.
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
        if (ImGui::BeginPopupModal(Tr("About ForkStation"), nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 o = ImGui::GetWindowPos();
            const ImVec2 sz = ImGui::GetWindowSize();
            const float ms = std::clamp(sz.x / 1500.0f, 0.6f, 2.6f);
            Menu::Backdrop(dl, o, {o.x + sz.x, o.y + sz.y});
            ImGui::SetWindowFontScale(ms * 0.95f);
            dl->AddText({o.x + 64.0f * ms, o.y + 48.0f * ms}, IM_COL32(245, 245, 248, 255),
                        single_game.active ? single_game.title.c_str() : "ForkStation");
            ImGui::SetWindowFontScale(ms * 0.5f);
            const float lx = o.x + sz.x * 0.155f;
            const float lw = sz.x * 0.69f;
            const float rh = 62.0f * ms;
            float ry = o.y + 120.0f * ms;
            const auto info = [&](Menu::Icon icon, const char* label, const std::string& value) {
                Menu::Row(label, icon, label, value.c_str(), lx, ry, lw, rh, false, false);
                ry += rh;
            };
            info(Menu::Icon::Chip, "Versión",
                 fmt::format("{}  ({})", Common::g_version, Common::g_scm_desc));
            info(Menu::Icon::Play, "Qué es", "Juegos de PS4 y PS5 en PC con su fork o port");
            info(Menu::Icon::Fork, "Repositorio", "github.com/serbru20066666/ForkStation");
            info(Menu::Icon::Info, "Forks y ports", "de sus autores (ver cada repositorio)");
            info(Menu::Icon::Gear, "Motor propio", "deriva de shadPS4 (GPL-2.0-or-later)");
            info(Menu::Icon::Screen, "Generación de cuadros", "AMD FidelityFX (MIT)");
            info(Menu::Icon::Info, "Tipografía", "Poppins (OFL)");
            info(Menu::Icon::Info, "Contacto", "Bruno Cardenas · brunocardenasproyectos@gmail.com");
            ry += 18.0f * ms;
            if (Menu::Row("##about_guide", Menu::Icon::Play, "Ver la guía de inicio", "", lx, ry,
                          lw, rh, false)) {
                onboarding = {};
                onboarding.active = true;
                ImGui::CloseCurrentPopup();
            }
            ry += rh;
            if (Menu::Row("##about_quit", Menu::Icon::Close, "Salir de ForkStation", "", lx, ry, lw,
                          rh, false)) {
                done = true;
            }
            ry += rh;
            const bool closed =
                Menu::Row("##about_close", Menu::Icon::Close, "Cerrar", "", lx, ry, lw, rh, false);
            if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemFocused()) {
                ImGui::SetItemDefaultFocus();
            }
            if (closed || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetWindowFontScale(uiScale);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (showSettings) {
            settingsWindow.DrawSettings(&showSettings, applySettings);
        }

        Theme::Pop();
        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 9, 9, 11, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        if (single_game.active && single_game.wanted_height > 0.0f) {
            int width{}, height{};
            SDL_GetWindowSize(window, &width, &height);
            SDL_Rect usable{};
            const int screen_height =
                SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable) ? usable.h - 48
                                                                                     : 2000;
            const float wanted = single_game.wanted_height;
            if (wanted > static_cast<float>(screen_height)) {
                uiScale = std::max(0.6f, uiScale * static_cast<float>(screen_height) / wanted);
            }
            const int target = std::min(static_cast<int>(std::ceil(wanted)), screen_height);
            if (std::abs(target - height) > 2) {
                SDL_SetWindowSize(window, width, target);
            }
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    renderer = nullptr;
    SDL_DestroyWindow(window);
    SDL_Quit();

    EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
    EmulatorSettings.Save();

    if (runEbootPath != "") {
        if (sameProcess) {
            auto* emulator = Common::Singleton<Core::Emulator>::Instance();
            emulator->executableName = executableName;
            emulator->Run(runEbootPath);
        } else {
            std::vector<std::string> args{"--log-append", "--game",
                                          Common::FS::PathToUTF8String(runEbootPath)};
            // A patch file named after the game in the patches folder is applied with it.
            const auto patch =
                Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / (runSerial + ".xml");
            // Unless the settings of the game say to leave it as it is.
            const auto own = ReadJson(Common::FS::GetUserPath(Common::FS::PathType::CustomConfigs) /
                                      (runSerial + ".json"));
            bool use_patch = true;
            try {
                use_patch = StoredSetting(own, nlohmann::json{}, "GPU", "use_game_patch", true);
            } catch (const nlohmann::json::exception&) {
            }
            if (!runSerial.empty() && use_patch && std::filesystem::exists(patch)) {
                args.insert(args.end(), {"--patch", Common::FS::PathToUTF8String(patch)});
            }
            // Spawning the new process directly, without constructing an Emulator here, avoids
            // registering its Shutdown() as an at_quick_exit handler: that handler assumes a
            // previous Run() and can hold up this process's exit for no reason, which on macOS
            // left this window appearing frozen while the next process compiled its first
            // shaders and crowded this one off the CPU.
            // Going back from the game's choice screen returns here (external_engines.cpp).
#ifdef _WIN32
            _putenv_s("BRUNO_FROM_LAUNCHER", "1");
#endif
            Core::RelaunchProcess(executableName, std::move(args));
        }
    }
}

} // namespace BigPictureMode
