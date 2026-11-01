// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/external_engines.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/thread.h"
#include "core/file_sys/ifile.h"
#include "imgui/big_picture/big_picture.h"

#ifdef _WIN32
#include <tlhelp32.h>
#include <windows.h>
#pragma comment(lib, "gdi32.lib")
#endif

namespace Common {

namespace {

struct Engine {
    std::string kind;
    std::filesystem::path program;
};

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string ToLower(std::string s) {
    std::ranges::transform(s, s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::filesystem::path ExecutablePath() {
#ifdef _WIN32
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path{path};
#else
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

std::filesystem::path ExecutableFolder() {
    return ExecutablePath().parent_path();
}

/// "CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe"
void ParseLine(const std::string& line, std::map<std::string, Engine>& table) {
    const std::string text = Trim(line.substr(0, line.find('#')));
    const auto equals = text.find('=');
    if (text.empty() || equals == std::string::npos) {
        return;
    }
    const std::string rest = text.substr(equals + 1);
    const auto bar = rest.find('|');
    Engine engine{Trim(rest.substr(0, bar)),
                  bar == std::string::npos ? std::filesystem::path{}
                                           : std::filesystem::path{Trim(rest.substr(bar + 1))}};
    std::stringstream serials{text.substr(0, equals)};
    std::string serial;
    while (std::getline(serials, serial, ',')) {
        if (serial = Trim(serial); !serial.empty()) {
            table[serial] = engine;
        }
    }
}

std::map<std::string, Engine> LoadTable(const std::filesystem::path& folder) {
    std::map<std::string, Engine> table;
    ParseLine(R"(CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe)", table);
    ParseLine(R"(CUSA00004,CUSA00223 = shadps4-release | motores\inFamous\shadPS4.exe)", table);
    ParseLine(
        R"(CUSA02320,CUSA02343,CUSA02344 = uncharted | motores\Uncharted\Uncharted Launcher.exe)",
        table);
    ParseLine(
        R"(CUSA03173,CUSA00900,CUSA00207,CUSA00208,CUSA01363,CUSA03023 = bloodborne | motores\Bloodborne\Bloodborne.exe)",
        table);
    ParseLine(
        R"(PPSA01325,PPSA21564,PPSA21567,PPSA21568 = kyty-astro | motores\AstroBot\kyty_emulator.exe)",
        table);
    ParseLine(
        R"(PPSA01342,PPSA01341,PPSA01340 = kyty-souls | motores\DemonsSouls\kyty_emulator.exe)",
        table);
    ParseLine(R"(CUSA03627,CUSA03745 = tlg | motores\TheLastGuardian\The Last Guardian.exe)",
              table);
    std::ifstream in{folder / "motores.ini"};
    std::string line;
    while (in && std::getline(in, line)) {
        ParseLine(line, table);
    }
    return table;
}

std::filesystem::path ResolveProgramPath(const std::filesystem::path& folder,
                                         const std::filesystem::path& configured) {
    std::error_code ec;
    // When configured is absolute, check if the emulator folder itself contains the engine
    // (e.g. user copied a motores.ini containing hardcoded paths from another machine or path).
    // The local installation in RetroBat or a portable folder must always take priority!
    if (configured.is_absolute()) {
        const auto file_name = configured.filename();
        const auto parent_name = configured.parent_path().filename();
        const auto local_sub = folder / "motores" / parent_name / file_name;
        if (std::filesystem::exists(local_sub, ec)) {
            return local_sub;
        }
        const auto local_direct = folder / "motores" / file_name;
        if (std::filesystem::exists(local_direct, ec)) {
            return local_direct;
        }
        if (std::filesystem::exists(configured, ec)) {
            return configured;
        }
    } else {
        const auto direct = folder / configured;
        if (std::filesystem::exists(direct, ec)) {
            return direct;
        }
    }
    // When running directly from a build worktree (e.g. brunoShadPs4_main/build/shadps4.exe),
    // check if `motores` sits in the workspace root two levels up.
    if (folder.has_parent_path() && folder.parent_path().has_parent_path()) {
        const auto root = folder.parent_path().parent_path();
        const auto up_two = root / configured;
        if (std::filesystem::exists(up_two, ec)) {
            return up_two;
        }
        if (!std::filesystem::exists(folder / "motores", ec) &&
            std::filesystem::exists(root / "motores", ec)) {
            return up_two;
        }
    }
    return folder / configured;
}

/// The configured external engine of a game and its resolved program path, even if not yet
/// downloaded on disk. Returns std::nullopt for games that run on the internal engine ("interno").
std::optional<std::pair<Engine, std::filesystem::path>> FindConfiguredEngine(
    const std::string& serial) {
    if (serial.empty()) {
        return std::nullopt;
    }
    const auto folder = ExecutableFolder();
    const auto table = LoadTable(folder);
    const auto it = table.find(serial);
    if (it == table.end() || it->second.kind == "interno" || it->second.program.empty()) {
        return std::nullopt;
    }
    return std::make_pair(it->second, ResolveProgramPath(folder, it->second.program));
}

/// The engine of a game and its program's full path, if the game has one and the program is there.
std::optional<std::pair<Engine, std::filesystem::path>> FindEngine(const std::string& serial) {
    const auto configured = FindConfiguredEngine(serial);
    if (!configured) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(configured->second, ec)) {
        return std::nullopt;
    }
    return configured;
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << text;
    return static_cast<bool>(out);
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

nlohmann::json ReadJsonFile(const std::filesystem::path& path) {
    if (std::ifstream in{path}; in) {
        auto json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_object()) {
            return json;
        }
    }
    return nlohmann::json::object();
}

/// Sets "game_dir" in a launcher's JSON settings, keeping everything else.
bool SetGameDir(const std::filesystem::path& settings_path, const std::filesystem::path& eboot) {
    auto settings = ReadJsonFile(settings_path);
    settings["game_dir"] = FS::PathToUTF8String(eboot.parent_path());
    std::error_code ec;
    std::filesystem::create_directories(settings_path.parent_path(), ec);
    return WriteText(settings_path, settings.dump(2));
}

/// A launcher's settings file in the user's AppData (the Uncharted collection's and issn_pc's).
std::filesystem::path AppDataFile(const std::filesystem::path& relative) {
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) {
        return std::filesystem::path{appdata} / relative;
    }
#endif
    return {};
}

std::string EngineName(const Engine& engine, const std::filesystem::path& program) {
    if (engine.kind == "spartanport") {
        return "SpartanPort";
    }
    if (engine.kind == "issn") {
        return "issn_pc";
    }
    if (engine.kind == "uncharted") {
        return "Uncharted PC port";
    }
    if (engine.kind == "bloodborne") {
        return "bloodborne_pc";
    }
    if (engine.kind == "shadps4-release") {
        return "shadPS4";
    }
    if (engine.kind == "kyty-astro") {
        return "KytyPS5 Astro";
    }
    if (engine.kind == "kyty-souls") {
        return "KytyPS5 Demon's Souls";
    }
    if (engine.kind == "tlg") {
        return "tlg_pc";
    }
    return FS::PathToUTF8String(program.stem());
}

std::string EngineGitHubRepo(const Engine& engine, const std::filesystem::path& program) {
    if (engine.kind == "spartanport") {
        return "FelipePrado0/SpartanPort";
    }
    if (engine.kind == "shadps4-release") {
        return "axylpup/shadPS4";
    }
    if (engine.kind == "issn") {
        return "Liondarkz/issn_pc";
    }
    if (engine.kind == "uncharted") {
        return "bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port";
    }
    if (engine.kind == "bloodborne") {
        return "Supermedo/bloodborne_pc";
    }
    if (engine.kind == "kyty-astro") {
        return "biggsymobile-png/KytyPS5";
    }
    if (engine.kind == "kyty-souls") {
        return "chenxiao07/KytyPS5";
    }
    if (engine.kind == "tlg") {
        return "HayatoG/tlg_pc";
    }
    if (engine.kind == "shadps4") {
        const auto lower = ToLower(FS::PathToUTF8String(program));
        if (lower.find("infamous") != std::string::npos ||
            lower.find("secondson") != std::string::npos) {
            return "axylpup/shadPS4";
        }
        return "shadps4-emu/shadPS4";
    }
    return "serbru20066666/ForkStation";
}

std::string CleanEmulatorVersion() {
    std::string ver{Common::g_version};
    if (const auto wip = ver.find(" WIP"); wip != std::string::npos) {
        ver.erase(wip);
    }
    return Trim(ver);
}

std::mutex g_version_cache_mutex;
std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::string>>
    g_version_cache;

void InvalidateVersionCache(const std::filesystem::path& program) {
    std::scoped_lock lock{g_version_cache_mutex};
    g_version_cache.erase(program);
}

/// Extracts a version string from the executable binary itself (axylpup's ReleaseTag or a
/// PyInstaller launcher's embedded VERSION constant).
std::string InspectExecutableVersion(const std::filesystem::path& program) {
    std::error_code ec;
    if (!std::filesystem::exists(program, ec)) {
        return {};
    }
    const auto mtime = std::filesystem::last_write_time(program, ec);
    {
        std::scoped_lock lock{g_version_cache_mutex};
        if (const auto it = g_version_cache.find(program);
            it != g_version_cache.end() && it->second.first == mtime) {
            return it->second.second;
        }
    }

    std::string found_version;
    std::ifstream file{program, std::ios::binary | std::ios::ate};
    if (file) {
        const auto total_size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);
        std::vector<char> data(total_size);
        if (file.read(data.data(), static_cast<std::streamsize>(total_size))) {
            const std::string_view view{data.data(), data.size()};
            static const std::regex ver_re{R"((\d+\.\d+(?:\.\d+)*))"};

            // 1. axylpup/shadPS4 embeds "second-son-v1.1.1" in .rdata.
            if (const auto pos = view.find("second-son-v"); pos != std::string_view::npos) {
                const std::string sub{
                    view.substr(pos + 12, std::min<size_t>(24, view.size() - pos - 12))};
                if (std::smatch m; std::regex_search(sub, m, ver_re)) {
                    found_version = m[1].str();
                }
            }

            // 2. PyInstaller ports (SpartanPort, Bloodborne, issn_pc, Uncharted) can store
            //    embedded version strings or release identifiers in the binary.
            if (found_version.empty()) {
                static const std::regex sp_re{R"(SpartanPort\s+(\d+\.\d+(?:\.\d+)*))"};
                const std::string s_view{data.begin(), data.end()};
                if (std::smatch m; std::regex_search(s_view, m, sp_re)) {
                    found_version = m[1].str();
                }
            }
        }
    }

    if (!found_version.empty()) {
        // Persist VERSION beside the executable so subsequent startups read it immediately.
        const auto version_file = program.parent_path() / "VERSION";
        if (!std::filesystem::exists(version_file, ec)) {
            WriteText(version_file, found_version + "\n");
        }
    }

    {
        std::scoped_lock lock{g_version_cache_mutex};
        g_version_cache[program] = {mtime, found_version};
    }
    return found_version;
}

/// Reads the installed version of a fork/port from VERSION, README, executable metadata or folder
/// name.
std::string EngineVersion(const std::filesystem::path& program) {
    const auto folder = program.parent_path();
    // KytyPS5 Astro builds are tagged like "astrobot-dlss-int21-23w": the tag is the version.
    // KytyPS5 and tlg_pc builds are named by their release tag, kept whole in VERSION.
    if (const auto exe = ToLower(FS::PathToUTF8String(program.filename()));
        exe == "kyty_emulator.exe" || exe == "the last guardian.exe") {
        std::ifstream in{folder / "VERSION"};
        std::string line;
        std::getline(in, line);
        return Trim(line);
    }
    static const std::regex version{R"((\d+\.\d+(?:\.\d+)*))"};
    for (const char* name : {"VERSION", "README-Windows.txt", "README.txt"}) {
        std::ifstream in{folder / name};
        std::string line;
        for (int i = 0; i < 5 && std::getline(in, line); ++i) {
            if (std::smatch found; std::regex_search(line, found, version)) {
                return found[1].str();
            }
        }
    }
    if (const auto from_exe = InspectExecutableVersion(program); !from_exe.empty()) {
        return from_exe;
    }
    const std::string folder_name = FS::PathToUTF8String(folder.filename());
    if (std::smatch found; std::regex_search(folder_name, found, version)) {
        return found[1].str();
    }
    return {};
}

/// What a port of the SpartanPort family used the last time it ran (its user\last_run.log).
std::array<std::string, 4> LastRunDetails(const std::filesystem::path& folder) {
    std::array<std::string, 4> details{};
    std::ifstream in{folder / "user" / "last_run.log"};
    if (!in) {
        details[0] = "Ajustes: en el lanzador del port";
        details[1] = "Aún sin jugar con este port";
        return details;
    }
    std::string line, window, fps, vsync, patches;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.starts_with("Window: ") && line.find("controller") == std::string::npos) {
            window = line.substr(8);
        } else if (line.starts_with("Engine frame rate: ")) {
            // "120 FPS, live up to 240" / "60 FPS (the frame rate patch ...)"
            fps = line.substr(19);
            fps = fps.substr(0, fps.find_first_of(",("));
            fps = Trim(fps);
        } else if (line.starts_with("Swapchain: ")) {
            vsync = line.find("VSync on") != std::string::npos ? "V-Sync: activado"
                                                               : "V-Sync: desactivado";
        } else if (line.find("3840x2160") != std::string::npos && line.starts_with("Patches:")) {
            patches = "Resolución: 3840 x 2160 (4K)";
        } else if (line.find("2560x1440") != std::string::npos && line.starts_with("Patches:")) {
            patches = "Resolución: 2560 x 1440";
        }
    }
    const std::map<std::string, std::string> modes{{"Windowed", "En ventana"},
                                                   {"Borderless", "Pantalla completa (sin bordes)"},
                                                   {"Fullscreen", "Pantalla completa"}};
    const auto mode = modes.find(window);
    details[0] = patches.empty() ? "Resolución: 1920 x 1080" : patches;
    details[1] = window.empty() ? "" : (mode != modes.end() ? mode->second : window);
    details[2] = fps.empty() ? "" : "Cuadros: " + fps;
    details[3] = vsync;
    return details;
}

/// A shadPS4 release build (shadps4-flags*.txt beside it): its render height and display mode.
std::array<std::string, 4> ReleaseBuildDetails(const std::filesystem::path& folder) {
    std::array<std::string, 4> details{};
    std::string height;
    for (const char* name : {"shadps4-flags.txt", "shadps4-flags-user.txt"}) {
        std::ifstream in{folder / name};
        std::string line;
        while (in && std::getline(in, line)) {
            line = Trim(line);
            if (line.starts_with("SHADPS4_RENDER_HEIGHT=")) {
                height = line.substr(22);
            }
        }
    }
    const std::map<std::string, std::string> sizes{
        {"2160", "3840 x 2160 (4K)"}, {"1440", "2560 x 1440"}, {"1080", "1920 x 1080"}};
    const auto size = sizes.find(height);
    details[0] = "Resolución: " + (size != sizes.end() ? size->second
                                   : height.empty()    ? std::string{"1920 x 1080"}
                                                       : height + " líneas");
    bool full = false;
    for (const char* name : {"user/config.json", "user/custom_configs/CUSA00004.json"}) {
        const auto json = ReadJsonFile(folder / name);
        if (json.contains("GPU") && json["GPU"].contains("full_screen")) {
            full = json["GPU"]["full_screen"].get<bool>();
        }
    }
    details[1] = full ? "Pantalla completa" : "En ventana";
    details[2] = "Ajustes: en su programa (F3)";
    return details;
}

/// The Uncharted collection's launcher: the game chosen last and its preset.
std::array<std::string, 4> UnchartedDetails() {
    std::array<std::string, 4> details{};
    const auto config = ReadJsonFile(AppDataFile("Uncharted Launcher/launcher.json"));
    const std::string game = config.value("game", std::string{"u1"});
    const std::map<std::string, std::string> names{
        {"u1", "Uncharted 1"}, {"u2", "Uncharted 2"}, {"u3", "Uncharted 3"}};
    const std::map<std::string, std::string> presets{
        {"low", "bajo"}, {"medium", "medio"}, {"high", "alto"}, {"ultra", "ultra"}};
    const auto name = names.find(game);
    details[0] = "Último elegido: " + (name != names.end() ? name->second : game);
    if (config.contains("games") && config["games"].contains(game)) {
        const auto& g = config["games"][game];
        details[1] = "Resolución: " + g.value("resolution", std::string{"1920x1080"});
        const auto preset = presets.find(g.value("preset", std::string{"medium"}));
        details[2] = "Calidad: " +
                     (preset != presets.end() ? preset->second : g.value("preset", std::string{}));
    }
    details[3] = "El juego se elige al abrir";
    return details;
}

#ifdef _WIN32
/// NAME=VALUE lines of brunoshadps4.env beside the program, set for it (and what it starts).
void ApplyEnvFile(const std::filesystem::path& folder) {
    std::ifstream in{folder / "brunoshadps4.env"};
    std::string line;
    while (in && std::getline(in, line)) {
        line = Trim(line);
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) {
            continue;
        }
        const auto name = std::filesystem::path{Trim(line.substr(0, equals))}.wstring();
        const auto value = std::filesystem::path{Trim(line.substr(equals + 1))}.wstring();
        SetEnvironmentVariableW(name.c_str(), value.c_str());
        LOG_INFO(Common, "Engine environment: {}", Trim(line.substr(0, equals)));
    }
}

/// The small window shown while the other program prepares the game.
struct Splash {
    HWND window{};
    std::wstring line1, line2;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    int phase{};

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Splash*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_PAINT && self) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            HBRUSH back = CreateSolidBrush(RGB(18, 18, 24));
            FillRect(dc, &r, back);
            DeleteObject(back);
            SetBkMode(dc, TRANSPARENT);
            HFONT big = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                    CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HFONT small = CreateFontW(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                      CLEARTYPE_QUALITY, 0, L"Segoe UI");
            auto old = SelectObject(dc, big);
            SetTextColor(dc, RGB(240, 240, 245));
            RECT t{24, 20, r.right - 24, 60};
            DrawTextW(dc, self->line1.c_str(), -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, small);
            SetTextColor(dc, RGB(160, 165, 180));
            const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::steady_clock::now() - self->start)
                                  .count();
            const std::wstring text = self->line2 + L"   (" + std::to_wstring(secs / 60) + L":" +
                                      (secs % 60 < 10 ? L"0" : L"") + std::to_wstring(secs % 60) +
                                      L")";
            RECT t2{24, 62, r.right - 24, 90};
            DrawTextW(dc, text.c_str(), -1, &t2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, old);
            DeleteObject(big);
            DeleteObject(small);
            // A bar that keeps moving: the other program says nothing about its progress.
            const int bar_top = r.bottom - 34, bar_h = 10, width = r.right - 48;
            HBRUSH track = CreateSolidBrush(RGB(40, 44, 60));
            RECT tr{24, bar_top, 24 + width, bar_top + bar_h};
            FillRect(dc, &tr, track);
            DeleteObject(track);
            const int block = width / 4;
            const int x = 24 + (self->phase * 6) % (width + block) - block;
            RECT br{std::max(24, x), bar_top, std::min(24 + width, x + block), bar_top + bar_h};
            HBRUSH accent = CreateSolidBrush(RGB(40, 140, 255));
            if (br.right > br.left) {
                FillRect(dc, &br, accent);
            }
            DeleteObject(accent);
            EndPaint(hwnd, &ps);
            return 0;
        }
        if (msg == WM_TIMER && self) {
            ++self->phase;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_CLOSE) {
            return 0; // it closes by itself when the game shows
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void Open() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ForkStationAbriendo";
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        RegisterClassW(&wc);
        const int w = 620, h = 150;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"ForkStation",
                                 WS_POPUP | WS_BORDER, x, y, w, h, nullptr, nullptr, wc.hInstance,
                                 nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window, SW_SHOWNOACTIVATE);
        SetTimer(window, 1, 30, nullptr);
    }

    void Pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void Close() {
        if (window) {
            KillTimer(window, 1);
            DestroyWindow(window);
            window = nullptr;
            Pump();
        }
    }
};

struct WindowSearch {
    DWORD own_pid;
    FILETIME started;
    bool found;
};

/// A visible top-level window of a process that started after the game was launched: the other
/// program (or the game it started) is showing something.
BOOL CALLBACK FindNewWindow(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    DWORD pid{};
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == search->own_pid) {
        return TRUE;
    }
    RECT r;
    if (!GetWindowRect(hwnd, &r) || r.right - r.left < 320 || r.bottom - r.top < 200) {
        return TRUE;
    }
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (std::wstring{cls} == L"ConsoleWindowClass" || std::wstring{cls} == L"PseudoConsoleWindow") {
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return TRUE;
    }
    FILETIME created, exited, kernel, user;
    const bool newer = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                       CompareFileTime(&created, &search->started) >= 0;
    CloseHandle(process);
    if (newer) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

/// Starts the program in a job, shows the splash until it shows a window, then waits for the job
/// (the program and everything it started) to end.
/// Whether RetroBat started this process (its parent is RetroBat's emulatorLauncher.exe).
bool LaunchedFromRetroBat() {
    const DWORD own = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == own) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    bool retrobat = false;
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); parent && ok;
         ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == parent) {
            retrobat = _wcsicmp(entry.szExeFile, L"emulatorLauncher.exe") == 0;
            break;
        }
    }
    CloseHandle(snapshot);
    return retrobat;
}

/// Reads the controllers while the game runs; true once Select (Share) and Start (Options) have
/// been held together for a moment on any of them.
class PadExitWatch {
public:
    PadExitWatch() {
        // The game's window has the focus: read the controllers anyway, and leave their lights
        // alone.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "0");
        ready = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    ~PadExitWatch() {
        for (auto& [id, pad] : pads) {
            SDL_CloseGamepad(pad);
        }
        if (ready) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }
    bool Pressed() {
        if (!ready) {
            return false;
        }
        SDL_PumpEvents();
        SDL_UpdateGamepads();
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            for (int i = 0; i < count; ++i) {
                if (!pads.contains(ids[i])) {
                    if (SDL_Gamepad* pad = SDL_OpenGamepad(ids[i])) {
                        pads[ids[i]] = pad;
                    }
                }
            }
            SDL_free(ids);
        }
        bool both = false;
        for (auto& [id, pad] : pads) {
            both |= SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                    SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        }
        held = both ? held + 1 : 0;
        return held >= 6; // about 0.3 s at one check every 50 ms
    }

private:
    bool ready = false;
    int held = 0;
    std::map<SDL_JoystickID, SDL_Gamepad*> pads;
};

/// A KytyPS5 engine on another drive than Windows' (RetroBat's hard disk) keeps its shader cache
/// (_PipelineCache, a few GB read and written while the game streams) on this PC's own drive:
/// %LOCALAPPDATA%\brunoShadPs4\shader-cache\<engine folder>, linked from the engine folder.
/// On a hard disk the game's reads waited 100-500 ms behind the cache's (KytyPS5 issue #91). The
/// cache is made for this PC's graphics card and driver anyway; the copy the engine folder had is
/// kept beside it (_PipelineCache.disco) and fills the cache on a PC that has none yet.
void PlaceShaderCacheOnLocalDrive(const std::filesystem::path& engine_folder) {
    const wchar_t* local_appdata = _wgetenv(L"LOCALAPPDATA");
    if (!local_appdata || !*local_appdata) {
        return;
    }
    std::error_code ec;
    const auto local = std::filesystem::path{local_appdata} / "brunoShadPs4" / "shader-cache" /
                       engine_folder.filename();
    const auto root_of = [](const std::filesystem::path& p) {
        return ToLower(FS::PathToUTF8String(std::filesystem::absolute(p).root_name()));
    };
    if (root_of(engine_folder) == root_of(local)) {
        return; // the engine is on Windows' drive already
    }
    const auto cache = engine_folder / "_PipelineCache";
    const auto kept = engine_folder / "_PipelineCache.disco";
    // Already linked here: done. A link of another PC (its target missing here) is replaced.
    if (std::filesystem::is_symlink(cache, ec) ||
        (GetFileAttributesW(cache.c_str()) != INVALID_FILE_ATTRIBUTES &&
         (GetFileAttributesW(cache.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT))) {
        if (std::filesystem::is_directory(local, ec) &&
            std::filesystem::equivalent(cache, local, ec)) {
            return;
        }
        RemoveDirectoryW(cache.c_str()); // the link only, never what it points to
    } else if (std::filesystem::is_directory(cache, ec)) {
        std::filesystem::rename(cache, kept, ec);
        if (ec) {
            LOG_WARNING(Common, "Shader cache stays in {}: {}", FS::PathToUTF8String(cache),
                        ec.message());
            return;
        }
    }
    std::filesystem::create_directories(local, ec);
    // A new PC's cache starts from the copy kept in the engine folder.
    if (std::filesystem::is_empty(local, ec) && std::filesystem::is_directory(kept, ec)) {
        std::filesystem::copy(kept, local,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::skip_existing,
                              ec);
    }
    std::wstring cmd =
        L"cmd.exe /d /c mklink /J \"" + cache.wstring() + L"\" \"" + local.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    DWORD code = 1;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (code != 0 && !std::filesystem::exists(cache, ec)) {
        // No link: the engine keeps its own folder's cache as before.
        std::filesystem::rename(kept, cache, ec);
        LOG_WARNING(Common, "Shader cache could not be linked to {}", FS::PathToUTF8String(local));
        return;
    }
    LOG_INFO(Common, "Shader cache of {} on this PC's drive: {}",
             FS::PathToUTF8String(engine_folder.filename()), FS::PathToUTF8String(local));
}

bool StartAndWait(const std::filesystem::path& program, const std::vector<std::string>& args,
                  const std::string& title, const std::string& engine_name, bool pad_exit,
                  const std::filesystem::path& work_folder) {
    ApplyEnvFile(work_folder);
    std::wstring cmdline = L"\"" + program.wstring() + L"\"";
    for (const auto& arg : args) {
        cmdline += L" \"" + std::filesystem::path{arg}.wstring() + L"\"";
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        // When this process ends, by itself or because a front end closed it (RetroBat's Select +
        // Start closes the program it started, which is this one), the game goes with it.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    FILETIME started;
    GetSystemTimeAsFileTime(&started);
    const std::wstring folder = work_folder.wstring();
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, folder.c_str(), &si, &pi)) {
        LOG_ERROR(Common, "Could not start {}: error {}", FS::PathToUTF8String(program),
                  GetLastError());
        if (job) {
            CloseHandle(job);
        }
        return false;
    }
    if (job) {
        AssignProcessToJobObject(job, pi.hProcess);
    }
    // Without the focus the new window gets no controller input.
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    Splash splash;
    splash.line1 = L"Abriendo " + std::filesystem::path{title}.wstring();
    splash.line2 =
        L"con " + std::filesystem::path{engine_name}.wstring() + L"  -  la primera vez tarda mas";
    splash.Open();
    const auto running = [&] {
        if (!job) {
            return WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info),
                                  nullptr);
        return info.ActiveProcesses > 0;
    };
    while (running()) {
        splash.Pump();
        WindowSearch search{GetCurrentProcessId(), started, false};
        EnumWindows(FindNewWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            break;
        }
        Sleep(100);
    }
    splash.Close();
    // Stay until the game ends: front ends such as RetroBat wait for this process. Started by
    // RetroBat, Select + Start ends the game and goes back to it (RetroBat's own Select + Start
    // asks this process's window to close, and the game's window belongs to the other program).
    if (pad_exit && job) {
        LOG_INFO(Common, "Started by RetroBat: Select + Start ends the game");
        PadExitWatch watch;
        while (running()) {
            if (watch.Pressed()) {
                LOG_INFO(Common, "Select + Start: ending the game");
                TerminateJobObject(job, 0);
                break;
            }
            Sleep(50);
        }
    }
    while (running()) {
        Sleep(500);
    }
    CloseHandle(pi.hProcess);
    if (job) {
        CloseHandle(job);
    }
    return true;
}
#endif

/// Sets KEY=value in a KEY=value text file (an .ini or a flags file), keeping the other lines.
bool SetKeyValue(const std::filesystem::path& path, const std::string& key,
                 const std::string& value) {
    std::vector<std::string> lines;
    bool done = false;
    {
        std::ifstream in{path};
        std::string line;
        while (in && std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (Trim(line).starts_with(key + "=")) {
                line = key + "=" + value;
                done = true;
            }
            lines.push_back(line);
        }
    }
    if (!done) {
        lines.push_back(key + "=" + value);
    }
    std::string text;
    for (const auto& line : lines) {
        text += line + "\n";
    }
    return WriteText(path, text);
}

std::string ReadKeyValue(const std::filesystem::path& path, const std::string& key) {
    std::ifstream in{path};
    std::string line, value;
    while (in && std::getline(in, line)) {
        line = Trim(line);
        if (line.starts_with(key + "=")) {
            value = Trim(line.substr(key.size() + 1));
        }
    }
    return value;
}

constexpr std::array<const char*, 3> Resolutions{"1080p", "1440p", "4K"};

/// Going back from a choice screen: to this emulator's launcher when the game was started from it
/// (it sets BRUNO_FROM_LAUNCHER), otherwise back to whatever started this process (RetroBat).
void ReturnToLauncher();

bool WentBack(const std::string& serial) {
    LOG_INFO(Common, "{}: the player went back before playing", serial);
    ReturnToLauncher();
    return true;
}

/// Starts the launcher again when it started this game (it sets BRUNO_FROM_LAUNCHER).
void ReturnToLauncher() {
#ifdef _WIN32
    const char* from = std::getenv("BRUNO_FROM_LAUNCHER");
    if (from && from[0] == '1') {
        SetEnvironmentVariableW(L"BRUNO_FROM_LAUNCHER", nullptr);
        // Back from a game: no start animation again.
        SetEnvironmentVariableW(L"FORKSTATION_BACK", L"1");
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmdline = L"\"" + std::wstring{path} + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(path, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi)) {
            AllowSetForegroundWindow(pi.dwProcessId);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
#endif
}

#ifdef _WIN32
/// Brings a window to the front even when Windows would refuse it (a process started by another
/// one that has just ended, as this one is when the launcher or RetroBat starts it).
void ForceForeground(HWND hwnd) {
    const DWORD front = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD own = GetCurrentThreadId();
    // A key event lets this process set the foreground window (the usual workaround).
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    if (front != own) {
        AttachThreadInput(front, own, TRUE);
    }
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    if (front != own) {
        AttachThreadInput(front, own, FALSE);
    }
}
#endif

/// A screen before the game to choose one of a few options (its resolution, a game of a
/// collection), with the pad (D-pad, cross to accept, circle to go back), the keyboard or the
/// mouse: front ends such as RetroBat start the game directly, without this emulator's launcher.
/// Returns the chosen index, or nothing when the player went back.
std::optional<int> ChooseOption(const std::string& title, const std::string& subtitle,
                                const std::vector<std::string>& labels, int selected) {
    return BigPictureMode::ChooseOptionMenu(title, subtitle, labels, selected);
}

/// The resolution screen: "1080p", "1440p" or "4K", or nothing when the player went back.
std::optional<std::string> ChooseResolution(const std::string& title, const std::string& current) {
    int selected = 0;
    for (int i = 0; i < 3; ++i) {
        if (current == Resolutions[i]) {
            selected = i;
        }
    }
    const auto choice = ChooseOption(
        title, "Elige la resolución",
        {"1080p   (1920 x 1080)", "1440p   (2560 x 1440, 2K)", "4K      (3840 x 2160)"}, selected);
    if (!choice) {
        return std::nullopt;
    }
    return std::string{Resolutions[*choice]};
}

// ---------------------------------------------------------------------------
// Official GitHub Release updater for each fork/port
// ---------------------------------------------------------------------------

std::mutex g_update_mutex;
EngineUpdateStatus g_update_status;

void ModifyUpdateStatus(const std::function<void(EngineUpdateStatus&)>& fn) {
    std::scoped_lock lock{g_update_mutex};
    fn(g_update_status);
}

std::array<int, 4> ParseVersionTuple(std::string_view text) {
    std::array<int, 4> out{};
    static const std::regex ver_re{R"((\d+\.\d+(?:\.\d+)*))"};
    const std::string s{text};
    std::smatch m;
    if (!std::regex_search(s, m, ver_re)) {
        return out;
    }
    std::stringstream ss{m[1].str()};
    std::string part;
    for (size_t i = 0; i < out.size() && std::getline(ss, part, '.'); ++i) {
        out[i] = std::atoi(part.c_str());
    }
    return out;
}

std::string ExtractVersionString(const std::string& tag, const std::string& name) {
    static const std::regex ver_re{R"((\d+\.\d+(?:\.\d+)*))"};
    std::smatch m;
    if (std::regex_search(tag, m, ver_re)) {
        return m[1].str();
    }
    if (std::regex_search(name, m, ver_re)) {
        return m[1].str();
    }
    return tag;
}

bool SplitUrl(const std::string& url, std::string& base_url, std::string& path) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return false;
    }
    const auto host_start = scheme_end + 3;
    const auto path_start = url.find('/', host_start);
    if (path_start == std::string::npos) {
        base_url = url;
        path = "/";
    } else {
        base_url = url.substr(0, path_start);
        path = url.substr(path_start);
    }
    return true;
}

void ConfigureHttpClient(httplib::Client& cli, int connect_sec, int read_sec) {
    cli.set_connection_timeout(connect_sec);
    cli.set_read_timeout(read_sec);
    cli.set_follow_location(true);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(false);
#endif
}

std::string FetchUrlText(const std::string& url) {
    std::string base, path;
    if (SplitUrl(url, base, path)) {
        httplib::Client cli(base);
        ConfigureHttpClient(cli, 10, 20);
        const httplib::Headers headers = {{"User-Agent", "ForkStation"},
                                          {"Accept", "application/vnd.github+json"}};
        if (const auto res = cli.Get(path.c_str(), headers); res && res->status == 200) {
            return res->body;
        }
    }
#ifdef _WIN32
    // Fallback to Windows built-in curl.exe if LibreSSL/httplib is blocked by local certs/proxy.
    const auto tmp = std::filesystem::temp_directory_path() / "brunoshadps4_release.json";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    std::wstring cmd = L"curl.exe -fsSL -H \"User-Agent: ForkStation\" -o \"" + tmp.wstring() +
                       L"\" \"" + std::filesystem::path{url}.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 25000);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (exit_code == 0) {
            std::ifstream in{tmp, std::ios::binary};
            std::ostringstream ss;
            ss << in.rdbuf();
            std::filesystem::remove(tmp, ec);
            return ss.str();
        }
    }
#endif
    return {};
}

bool DownloadFileWithProgress(const std::string& url, const std::filesystem::path& dest,
                              const std::string& asset_name, uint64_t expected_size) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    std::string base, path;
    if (SplitUrl(url, base, path)) {
        std::ofstream out{dest, std::ios::binary | std::ios::trunc};
        if (out) {
            httplib::Client cli(base);
            ConfigureHttpClient(cli, 15, 120);
            const httplib::Headers headers = {{"User-Agent", "ForkStation"},
                                              {"Accept", "application/octet-stream"}};
            uint64_t received = 0;
            const auto res = cli.Get(path.c_str(), headers, [&](const char* data, size_t len) {
                out.write(data, static_cast<std::streamsize>(len));
                received += len;
                const uint64_t total = std::max<uint64_t>(expected_size, received);
                const float ratio =
                    total > 0 ? static_cast<float>(received) / static_cast<float>(total) : 0.0f;
                ModifyUpdateStatus([&](EngineUpdateStatus& st) {
                    st.progress = std::clamp(ratio * 0.80f, 0.01f, 0.80f);
                    st.message =
                        fmt::format("Descargando {} ({:.1f} / {:.1f} MB)...", asset_name,
                                    received / (1024.0 * 1024.0), total / (1024.0 * 1024.0));
                });
                return static_cast<bool>(out);
            });
            out.close();
            if (res && res->status == 200 && std::filesystem::file_size(dest, ec) > 1024) {
                return true;
            }
        }
    }
#ifdef _WIN32
    ModifyUpdateStatus([&](EngineUpdateStatus& st) {
        st.progress = -1.0f;
        st.message = fmt::format("Descargando {}...", asset_name);
    });
    std::wstring cmd = L"curl.exe -fL -H \"User-Agent: ForkStation\" -o \"" + dest.wstring() +
                       L"\" \"" + std::filesystem::path{url}.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 300000);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (exit_code == 0 && std::filesystem::file_size(dest, ec) > 1024) {
            return true;
        }
    }
#endif
    return false;
}

FILE* OpenFileWide(const std::filesystem::path& path, const wchar_t* mode) {
#ifdef _WIN32
    return _wfopen(path.c_str(), mode);
#else
    const std::string m = mode[0] == L'w' ? "wb" : "rb";
    return std::fopen(path.string().c_str(), m.c_str());
#endif
}

bool ExtractZipArchive(const std::filesystem::path& zip_path,
                       const std::filesystem::path& target_dir, bool is_internal_self) {
    std::error_code ec;
    std::filesystem::create_directories(target_dir, ec);

    // User files/directories that an update must never overwrite if they already exist.
    static const std::set<std::string> preserved_names = {
        "user",
        "out",
        "mods",
        "fsr4vk",
        "spartanport.ini",
        "gow3.ini",
        "bbport.ini",
        "issn.ini",
        "settings.json",
        "mods.json",
        "mods_presets.json",
        "patches.json",
        "last_run.log",
        "shadps4-flags-user.txt",
        "shadps4-game.txt",
        "brunoshadps4.env",
        "brunoshadps4-kyty.ini",
        "_savedata",
        "_pipelinecache",
        "kyty.ini",
        "motores.ini",
        "juego.ini",
    };

    const auto extract_tmp = target_dir / ".extract_tmp";
    std::filesystem::remove_all(extract_tmp, ec);
    std::filesystem::create_directories(extract_tmp, ec);

#ifdef _WIN32
    // Windows native tar extracts .zip archives directly.
    std::wstring cmd =
        L"tar.exe -xf \"" + zip_path.wstring() + L"\" -C \"" + extract_tmp.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi)) {
        LOG_ERROR(Common, "ExtractZipArchive: could not run tar.exe");
        std::filesystem::remove_all(extract_tmp, ec);
        return false;
    }
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (code != 0) {
        LOG_ERROR(Common, "ExtractZipArchive: tar.exe exited with {}", code);
        std::filesystem::remove_all(extract_tmp, ec);
        return false;
    }
#else
    const std::string cmd =
        "tar -xf \"" + zip_path.string() + "\" -C \"" + extract_tmp.string() + "\"";
    const int code = std::system(cmd.c_str());
    if (code != 0) {
        LOG_ERROR(Common, "ExtractZipArchive: tar exited with {}", code);
        std::filesystem::remove_all(extract_tmp, ec);
        return false;
    }
#endif

    // Check if the zip had a single wrapper directory (e.g. spartanport-windows/...)
    std::filesystem::path source_root = extract_tmp;
    std::vector<std::filesystem::path> top_entries;
    for (const auto& entry : std::filesystem::directory_iterator(extract_tmp, ec)) {
        top_entries.push_back(entry.path());
    }
    if (top_entries.size() == 1 && std::filesystem::is_directory(top_entries[0], ec)) {
        source_root = top_entries[0];
    }

    const auto own_exe = ExecutablePath();

    // Copy files into target_dir preserving user saves and configs.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root, ec)) {
        const auto rel = std::filesystem::relative(entry.path(), source_root, ec);
        const auto dest = target_dir / rel;

        const auto first_part = ToLower(rel.begin()->string());
        if (preserved_names.contains(first_part) &&
            std::filesystem::exists(target_dir / *rel.begin(), ec)) {
            continue;
        }

        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(dest, ec);
            continue;
        }

        std::filesystem::create_directories(dest.parent_path(), ec);
        if (is_internal_self && std::filesystem::equivalent(dest, own_exe, ec)) {
            const auto old_exe = std::filesystem::path{own_exe}.replace_extension(".exe.old");
            std::filesystem::remove(old_exe, ec);
            std::filesystem::rename(own_exe, old_exe, ec);
        }

        std::filesystem::copy_file(entry.path(), dest,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            const auto bak = std::filesystem::path{dest}.concat(".old");
            std::filesystem::remove(bak, ec);
            std::filesystem::rename(dest, bak, ec);
            ec.clear();
            std::filesystem::copy_file(entry.path(), dest,
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    std::filesystem::remove_all(extract_tmp, ec);
    return true;
}

} // namespace

std::optional<Ps5GameInfo> ReadPs5GameInfo(const std::filesystem::path& game_folder) {
    // A folder or a ZArchive (.zar) of one.
    const auto data = Core::FileSys::ReadGameFile(game_folder, "sce_sys/param.json");
    if (!data) {
        return std::nullopt;
    }
    auto param = nlohmann::json::parse(data->begin(), data->end(), nullptr, false);
    if (!param.is_object()) {
        return std::nullopt;
    }
    Ps5GameInfo info;
    info.serial = param.value("titleId", std::string{});
    if (info.serial.empty()) {
        return std::nullopt;
    }
    if (param.contains("localizedParameters") && param["localizedParameters"].is_object()) {
        const auto& names = param["localizedParameters"];
        const std::string fallback = names.value("defaultLanguage", std::string{"en-US"});
        for (const std::string& lang :
             {std::string{"es-ES"}, std::string{"es-419"}, fallback, std::string{"en-US"}}) {
            if (names.contains(lang) && names[lang].is_object()) {
                info.title = names[lang].value("titleName", std::string{});
                if (!info.title.empty()) {
                    break;
                }
            }
        }
    }
    if (info.title.empty()) {
        info.title = info.serial;
    }
    return info;
}

std::string ExternalEngineResolution(const std::string& serial) {
    const auto found = FindEngine(serial);
    if (!found) {
        return {};
    }
    const auto& [engine, program] = *found;
    const auto folder = program.parent_path();
    if (engine.kind == "shadps4-release") {
        const auto height =
            ReadKeyValue(folder / "shadps4-flags-user.txt", "SHADPS4_RENDER_HEIGHT");
        return height == "2160" ? "4K" : height == "1440" ? "1440p" : "1080p";
    }
    if (engine.kind == "spartanport") {
        const auto res = ReadKeyValue(folder / "spartanport.ini", "render_resolution");
        return res == "4K" ? "4K" : res == "1440p" ? "1440p" : "1080p";
    }
    if (engine.kind == "bloodborne") {
        const auto res = ReadKeyValue(folder / "bbport.ini", "output_res");
        return res == "3840x2160" ? "4K" : res == "2560x1440" ? "1440p" : "1080p";
    }
    if (engine.kind == "kyty-astro" || engine.kind == "kyty-souls") {
        const auto res = ReadKeyValue(folder / "brunoshadps4-kyty.ini", "pantalla");
        return res == "1440p" ? "1440p" : res == "1080p" ? "1080p" : "4K";
    }
    if (engine.kind == "uncharted") {
        const auto config = ReadJsonFile(AppDataFile("Uncharted Launcher/launcher.json"));
        const std::string game = config.value("game", std::string{"u1"});
        std::string res = "1920x1080";
        if (config.contains("games") && config["games"].contains(game)) {
            res = config["games"][game].value("resolution", res);
        }
        return res == "3840x2160" ? "4K" : res == "2560x1440" ? "1440p" : "1080p";
    }
    return {};
}

std::string CycleExternalEngineResolution(const std::string& serial) {
    const auto current = ExternalEngineResolution(serial);
    if (current.empty()) {
        return {};
    }
    size_t index = 0;
    while (index < Resolutions.size() && current != Resolutions[index]) {
        ++index;
    }
    const std::string next = Resolutions[(index + 1) % Resolutions.size()];
    const auto& [engine, program] = *FindEngine(serial);
    const auto folder = program.parent_path();
    if (engine.kind == "shadps4-release") {
        SetKeyValue(folder / "shadps4-flags-user.txt", "SHADPS4_RENDER_HEIGHT",
                    next == "4K"      ? "2160"
                    : next == "1440p" ? "1440"
                                      : "1080");
    } else if (engine.kind == "kyty-astro" || engine.kind == "kyty-souls") {
        SetKeyValue(folder / "brunoshadps4-kyty.ini", "pantalla", next);
    } else if (engine.kind == "spartanport") {
        const auto ini = folder / "spartanport.ini";
        SetKeyValue(ini, "render_resolution", next == "1080p" ? "native" : next);
        SetKeyValue(ini, "output_res",
                    next == "4K"      ? "3840x2160"
                    : next == "1440p" ? "2560x1440"
                                      : "1920x1080");
    } else if (engine.kind == "uncharted") {
        const auto path = AppDataFile("Uncharted Launcher/launcher.json");
        auto config = ReadJsonFile(path);
        const std::string res = next == "4K"      ? "3840x2160"
                                : next == "1440p" ? "2560x1440"
                                                  : "1920x1080";
        if (config.contains("games") && config["games"].is_object()) {
            for (auto& [key, game] : config["games"].items()) {
                if (game.is_object()) {
                    game["resolution"] = res;
                }
            }
        }
        WriteText(path, config.dump(2));
    }
    return next;
}

std::optional<ExternalEngineCard> DescribeExternalEngine(const std::string& serial) {
    const auto configured = FindConfiguredEngine(serial);
    if (!configured) {
        return std::nullopt;
    }
    const auto& [engine, program] = *configured;
    std::error_code ec;
    const bool exists = std::filesystem::exists(program, ec);
    ExternalEngineCard card;
    card.installed = exists;
    if (exists) {
        card.version = EngineVersion(program);
        if (engine.kind == "kyty-astro" && card.version.rfind('-') != std::string::npos) {
            card.version = card.version.substr(card.version.rfind('-') + 1); // "...-int21-23v": 23v
        }
        card.engine =
            EngineName(engine, program) + (card.version.empty() ? "" : " " + card.version);
        if (engine.kind == "spartanport" || engine.kind == "issn" || engine.kind == "bloodborne") {
            card.details = LastRunDetails(program.parent_path());
        } else if (engine.kind == "uncharted") {
            card.details = UnchartedDetails();
        } else if (engine.kind == "shadps4-release") {
            card.details = ReleaseBuildDetails(program.parent_path());
        } else if (engine.kind == "kyty-astro") {
            card.details[0] = "Pantalla: " + ExternalEngineResolution(serial);
            card.details[1] = "Render 1368p + FSR 3.1";
            card.details[2] = "45 fps + generación (90)";
        } else if (engine.kind == "kyty-souls") {
            const auto screen = ExternalEngineResolution(serial);
            card.details[0] = "Pantalla: " + screen;
            card.details[1] = std::string{"Render "} + (screen == "1080p" ? "1080p" : "1440p");
            card.details[2] = "60 fps";
        } else {
            card.details[0] = "Ajustes: en su propio programa";
        }
    } else {
        card.version = {};
        card.engine = EngineName(engine, program) + " (sin instalar)";
        card.details[0] = "Pulsa Actualizar para instalarlo";
        card.details[1] = "Repositorio: " + EngineGitHubRepo(engine, program);
    }
    return card;
}

EngineUpdateStatus GetEngineUpdateStatus() {
    std::scoped_lock lock{g_update_mutex};
    return g_update_status;
}

void ClearEngineUpdateStatus() {
    std::scoped_lock lock{g_update_mutex};
    if (!g_update_status.active) {
        g_update_status = {};
    }
}

void MarkEngineCurrent(const std::filesystem::path& program, const std::string& version);

namespace {

/// Releases of an engine published in a repository that also has other releases start with this
/// (none yet: every engine has a repository of its own).
std::string ReleaseTagPrefix(const Engine& /*engine*/) {
    return "";
}

/// KytyPS5 builds are named by their tag, compared as they are: any other tag is an update.
bool TagVersioned(const Engine& engine) {
    return engine.kind == "kyty-astro" || engine.kind == "kyty-souls" || engine.kind == "tlg";
}

struct LatestRelease {
    bool ok = false;
    nlohmann::json json;
    std::string version;
    std::string error;
};

/// The newest release of the engine's repository: its latest one, or for engines published as
/// pre-releases (biggsymobile's Astro builds) or beside other releases (a tag prefix) the newest
/// one in the list that is theirs.
/// GitHub answers 60 questions an hour without an account: what it said is kept for a while in
/// the user folder (forkstation_github_cache.json), and used again when it cannot be asked.
std::mutex g_github_cache_mutex;

std::filesystem::path GitHubCachePath() {
    return FS::GetUserPath(FS::PathType::UserDir) / "forkstation_github_cache.json";
}

std::string GitHubAnswer(const std::string& url, bool fresh) {
    constexpr std::time_t max_age = 3 * 60 * 60;
    const std::time_t now = std::time(nullptr);
    std::scoped_lock lock{g_github_cache_mutex};
    auto cache = ReadJsonFile(GitHubCachePath());
    const auto cached = [&]() -> std::string {
        if (cache.contains(url) && cache[url].is_object() && cache[url].contains("body")) {
            return cache[url].value("body", std::string{});
        }
        return {};
    };
    if (!fresh && cache.contains(url) && cache[url].is_object() &&
        now - cache[url].value("time", std::time_t{0}) < max_age) {
        if (auto body = cached(); !body.empty()) {
            return body;
        }
    }
    std::string body = FetchUrlText(url);
    if (body.empty() || body.find("\"tag_name\"") == std::string::npos) {
        return cached(); // offline or over the limit: the last answer, if any
    }
    cache[url] = {{"time", now}, {"body", body}};
    WriteText(GitHubCachePath(), cache.dump());
    return body;
}

LatestRelease FetchLatestRelease(const Engine& engine, const std::string& repo,
                                 bool fresh = false) {
    LatestRelease result;
    const std::string prefix = ReleaseTagPrefix(engine);
    const bool listed = engine.kind == "kyty-astro" || !prefix.empty();
    const std::string body =
        GitHubAnswer("https://api.github.com/repos/" + repo +
                         (listed ? "/releases?per_page=30" : "/releases/latest"),
                     fresh);
    if (body.empty()) {
        result.error = fmt::format("No se pudo conectar con GitHub ({}).", repo);
        return result;
    }
    const auto json = nlohmann::json::parse(body, nullptr, false);
    nlohmann::json release;
    if (listed && json.is_array()) {
        for (const auto& item : json) {
            if (item.is_object() && item.contains("tag_name") && item["tag_name"].is_string() &&
                item["tag_name"].get<std::string>().starts_with(prefix)) {
                release = item;
                break;
            }
        }
    } else if (json.is_object()) {
        release = json;
    }
    if (!release.is_object() || !release.contains("tag_name") || !release["tag_name"].is_string()) {
        result.error = prefix.empty()
                           ? std::string{"Respuesta inesperada de la API de GitHub."}
                           : fmt::format("{} no tiene ninguna versión publicada todavía.", repo);
        return result;
    }
    const std::string tag_name = release.value("tag_name", std::string{});
    const std::string rel_name = release.value("name", std::string{});
    result.version = TagVersioned(engine) ? tag_name : ExtractVersionString(tag_name, rel_name);
    result.json = std::move(release);
    result.ok = true;
    return result;
}

/// Whether `latest` is an update for `installed` (an empty installed version always is).
bool IsNewerVersion(const Engine& engine, const std::string& installed, const std::string& latest) {
    if (latest.empty()) {
        return false;
    }
    if (installed.empty()) {
        return true;
    }
    return TagVersioned(engine) ? installed != latest
                                : ParseVersionTuple(installed) < ParseVersionTuple(latest);
}

} // namespace

void StartEngineUpdate(const std::string& serial, bool force_reinstall) {
    {
        std::scoped_lock lock{g_update_mutex};
        if (g_update_status.active) {
            return;
        }
    }

    const auto configured = FindConfiguredEngine(serial);
    const bool is_external = configured.has_value();
    const Engine engine = is_external ? configured->first : Engine{"interno", ExecutablePath()};
    const std::filesystem::path program = is_external ? configured->second : ExecutablePath();
    const std::filesystem::path target_dir = program.parent_path();
    const std::string engine_name =
        is_external ? EngineName(engine, program) : std::string{"ForkStation"};
    const std::string repo = EngineGitHubRepo(engine, program);

    std::error_code ec;
    const bool installed = std::filesystem::exists(program, ec);
    const std::string installed_ver =
        is_external ? (installed ? EngineVersion(program) : std::string{}) : CleanEmulatorVersion();

    ModifyUpdateStatus([&](EngineUpdateStatus& st) {
        st = {};
        st.active = true;
        st.progress = -1.0f;
        st.serial = serial;
        st.engine_name = engine_name;
        st.repo = repo;
        st.installed_version = installed_ver;
        st.message = fmt::format("Consultando última versión en GitHub ({})...", repo);
    });

    std::thread([=] {
        SetCurrentThreadName("EngineUpdater");
        const auto release = FetchLatestRelease(engine, repo, true); // the button asks GitHub now
        if (!release.ok) {
            ModifyUpdateStatus([&](EngineUpdateStatus& st) {
                st.active = false;
                st.finished = true;
                st.success = false;
                st.message = release.error;
            });
            return;
        }
        const auto& json = release.json;
        const std::string& latest_ver = release.version;

        ModifyUpdateStatus([&](EngineUpdateStatus& st) { st.latest_version = latest_ver; });

        if (installed && !force_reinstall && !IsNewerVersion(engine, installed_ver, latest_ver)) {
            if (is_external) {
                WriteText(target_dir / "VERSION", installed_ver + "\n");
                InvalidateVersionCache(program);
            }
            MarkEngineCurrent(program, installed_ver);
            ModifyUpdateStatus([&](EngineUpdateStatus& st) {
                st.active = false;
                st.finished = true;
                st.success = true;
                st.up_to_date = true;
                st.progress = 1.0f;
                st.message = fmt::format("{} ya está en la última versión ({}).", engine_name,
                                         installed_ver);
            });
            return;
        }

        std::string best_url;
        std::string best_name;
        uint64_t best_size = 0;
        int best_score = -1;

        if (json.contains("assets") && json["assets"].is_array()) {
            for (const auto& asset : json["assets"]) {
                const std::string name = asset.value("name", std::string{});
                const std::string url = asset.value("browser_download_url", std::string{});
                const std::string lower = ToLower(name);
                if (url.empty() || !lower.ends_with(".zip")) {
                    continue;
                }
                if (lower.find("src") != std::string::npos ||
                    lower.find("source") != std::string::npos ||
                    lower.find("macos") != std::string::npos ||
                    lower.find("linux") != std::string::npos ||
                    lower.find("steamdeck") != std::string::npos ||
                    lower.find("appimage") != std::string::npos) {
                    continue;
                }
                int score = 1;
                if (engine.kind == "interno") {
                    if (lower.find("shadps4-win64") != std::string::npos) {
                        score += 20;
                    }
                } else {
                    if (lower.find("windows") != std::string::npos ||
                        lower.find("win64") != std::string::npos ||
                        lower.find("win") != std::string::npos) {
                        score += 10;
                    }
                    if (lower.find(ToLower(engine.kind)) != std::string::npos) {
                        score += 5;
                    }
                }
                if (score > best_score) {
                    best_score = score;
                    best_url = url;
                    best_name = name;
                    best_size = asset.value("size", uint64_t{0});
                }
            }
        }

        if (best_url.empty()) {
            ModifyUpdateStatus([&](EngineUpdateStatus& st) {
                st.active = false;
                st.finished = true;
                st.success = false;
                st.message = fmt::format(
                    "No se encontró un paquete .zip de Windows en el release {}.", latest_ver);
            });
            return;
        }

        std::error_code zip_ec;
        std::filesystem::create_directories(target_dir, zip_ec);
        const auto temp_zip = target_dir / ".brunoshadps4_update.zip";
        std::filesystem::remove(temp_zip, zip_ec);

        if (!DownloadFileWithProgress(best_url, temp_zip, best_name, best_size)) {
            std::filesystem::remove(temp_zip, zip_ec);
            ModifyUpdateStatus([&](EngineUpdateStatus& st) {
                st.active = false;
                st.finished = true;
                st.success = false;
                st.message = fmt::format("Error al descargar {}.", best_name);
            });
            return;
        }

        ModifyUpdateStatus([&](EngineUpdateStatus& st) {
            st.progress = 0.80f;
            st.message = fmt::format("Descomprimiendo {}...", best_name);
        });

        const bool extracted = ExtractZipArchive(temp_zip, target_dir, !is_external);
        std::filesystem::remove(temp_zip, zip_ec);

        if (!extracted) {
            ModifyUpdateStatus([&](EngineUpdateStatus& st) {
                st.active = false;
                st.finished = true;
                st.success = false;
                st.message = fmt::format("Error al descomprimir {} en {}.", best_name,
                                         FS::PathToUTF8String(target_dir));
            });
            return;
        }

        if (is_external && !latest_ver.empty()) {
            WriteText(target_dir / "VERSION", latest_ver + "\n");
            InvalidateVersionCache(program);
        }
        if (is_external) {
            MarkEngineCurrent(program, latest_ver);
        }

        ModifyUpdateStatus([&](EngineUpdateStatus& st) {
            st.active = false;
            st.finished = true;
            st.success = true;
            st.up_to_date = false;
            st.progress = 1.0f;
            st.installed_version = latest_ver;
            st.message = fmt::format("{} actualizado correctamente a la versión {}.", engine_name,
                                     latest_ver);
        });
    }).detach();
}

namespace {
std::mutex g_hints_mutex;
std::map<std::string, EngineUpdateHint> g_hints;            // by serial
std::map<std::string, std::filesystem::path> g_hint_engine; // the program each serial runs on
bool g_checks_started = false;
} // namespace

/// After an update or a check that found it current, the games on that program are up to date.
void MarkEngineCurrent(const std::filesystem::path& program, const std::string& version) {
    std::scoped_lock lock{g_hints_mutex};
    for (auto& [serial, hint] : g_hints) {
        if (g_hint_engine[serial] == program) {
            hint.missing = false;
            hint.available = false;
            hint.installed_version = version;
        }
    }
}

void StartUpdateChecks(const std::vector<std::string>& serials) {
    struct Target {
        Engine engine;
        std::filesystem::path program;
        bool external;
        std::vector<std::string> serials;
    };
    std::map<std::filesystem::path, Target> targets;
    {
        std::scoped_lock lock{g_hints_mutex};
        if (g_checks_started) {
            return;
        }
        g_checks_started = true;
        for (const auto& serial : serials) {
            const auto configured = FindConfiguredEngine(serial);
            const bool external = configured.has_value();
            const Engine engine =
                external ? configured->first : Engine{"interno", ExecutablePath()};
            const auto program = external ? configured->second : ExecutablePath();
            auto& target = targets[program];
            target.engine = engine;
            target.program = program;
            target.external = external;
            target.serials.push_back(serial);
            g_hint_engine[serial] = program;
        }
    }
    std::thread([targets = std::move(targets)] {
        SetCurrentThreadName("UpdateChecks");
        for (const auto& [program, target] : targets) {
            std::error_code ec;
            const bool installed = std::filesystem::exists(program, ec);
            const std::string installed_ver =
                target.external ? (installed ? EngineVersion(program) : std::string{})
                                : CleanEmulatorVersion();
            const auto release =
                FetchLatestRelease(target.engine, EngineGitHubRepo(target.engine, program));
            if (!release.ok) {
                LOG_INFO(Common, "Update check of {}: {}", FS::PathToUTF8String(program),
                         release.error);
                continue;
            }
            EngineUpdateHint hint;
            hint.checked = true;
            hint.missing = !installed;
            hint.available =
                installed && IsNewerVersion(target.engine, installed_ver, release.version);
            hint.installed_version = installed_ver;
            hint.latest_version = release.version;
            std::scoped_lock lock{g_hints_mutex};
            for (const auto& serial : target.serials) {
                g_hints[serial] = hint;
            }
        }
    }).detach();
}

std::optional<EngineUpdateHint> GetUpdateHint(const std::string& serial) {
    std::scoped_lock lock{g_hints_mutex};
    const auto it = g_hints.find(serial);
    if (it == g_hints.end()) {
        return std::nullopt;
    }
    return it->second;
}

/// Synchronizes saved games between shadPS4 / RetroBat (home_dir / 1000 / savedata) and the port's
/// user directory (program_folder / user / savedata / 1 / serial), ensuring that saves are never
/// lost or disassociated regardless of how or where the game is launched.
std::filesystem::path GetConfiguredHomeDir() {
    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    const auto config = ReadJsonFile(user_dir / "config.json");
    if (config.contains("General") && config["General"].contains("home_dir")) {
        const std::string h = config["General"]["home_dir"].get<std::string>();
        if (!h.empty()) {
            return std::filesystem::path{h};
        }
    }
    return Common::FS::GetUserPath(Common::FS::PathType::HomeDir);
}

void SyncDirectoryFiles(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (!std::filesystem::exists(from, ec) || !std::filesystem::is_directory(from, ec)) {
        return;
    }
    std::filesystem::create_directories(to, ec);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(from, ec)) {
        if (ec) {
            break;
        }
        const auto rel = std::filesystem::relative(entry.path(), from, ec);
        if (ec) {
            continue;
        }
        const auto target = to / rel;
        if (entry.is_directory(ec)) {
            std::filesystem::create_directories(target, ec);
        } else if (entry.is_regular_file(ec)) {
            if (!std::filesystem::exists(target, ec)) {
                std::filesystem::create_directories(target.parent_path(), ec);
                std::filesystem::copy_file(entry.path(), target,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                const auto t = std::filesystem::last_write_time(entry.path(), ec);
                if (!ec) {
                    std::filesystem::last_write_time(target, t, ec);
                }
            } else {
                const auto t_src = std::filesystem::last_write_time(entry.path(), ec);
                const auto t_dst = std::filesystem::last_write_time(target, ec);
                if (!ec && t_src > t_dst) {
                    std::filesystem::copy_file(entry.path(), target,
                                               std::filesystem::copy_options::overwrite_existing,
                                               ec);
                    std::filesystem::last_write_time(target, t_src, ec);
                }
            }
        }
    }
}

void SyncGameSaves(const std::string& serial, const std::filesystem::path& program_folder) {
    if (serial.empty()) {
        return;
    }

    const auto home_dir = GetConfiguredHomeDir();
    const auto user_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir);
    const auto default_home = Common::FS::GetUserPath(Common::FS::PathType::HomeDir);

    const std::vector<std::filesystem::path> main_locations{
        home_dir / "1000" / "savedata" / serial,
        home_dir / "savedata" / serial,
        user_dir / "savedata" / "1" / serial,
        user_dir / "savedata" / serial,
        user_dir / "home" / "1000" / "savedata" / serial,
        default_home / "1000" / "savedata" / serial,
        default_home / "savedata" / serial,
    };

    const auto ext_user = program_folder / "user";
    const std::vector<std::filesystem::path> engine_locations{
        ext_user / "savedata" / "1" / serial,
        ext_user / "savedata" / "1000" / serial,
        ext_user / "home" / "1000" / "savedata" / serial,
        ext_user / "savedata" / serial,
    };

    // Propagate from main emulator locations to external engine
    for (const auto& m : main_locations) {
        std::error_code ec;
        if (std::filesystem::exists(m, ec) && !std::filesystem::is_empty(m, ec)) {
            for (const auto& e : engine_locations) {
                SyncDirectoryFiles(m, e);
            }
        }
    }

    // Propagate from external engine to main emulator locations
    for (const auto& e : engine_locations) {
        std::error_code ec;
        if (std::filesystem::exists(e, ec) && !std::filesystem::is_empty(e, ec)) {
            for (const auto& m : main_locations) {
                SyncDirectoryFiles(e, m);
            }
        }
    }
}

bool LaunchExternalEngine(const std::string& serial, const std::string& title,
                          const std::filesystem::path& eboot) {
    if (serial.empty()) {
        return false;
    }
    const auto found = FindEngine(serial);
    if (!found) {
        return false;
    }
    const auto& [engine, program] = *found;
    const auto program_folder = program.parent_path();
    std::vector<std::string> args;
#ifdef _WIN32
    // This process only hands the game over: its console window has nothing to show.
    if (HWND console = GetConsoleWindow()) {
        ShowWindow(console, SW_HIDE);
    }
    FreeConsole(); // a console hosted by Windows Terminal ignores the hide
#endif
    if (engine.kind == "spartanport") {
        // SpartanPort keeps its options in settings.json beside it.
        if (!SetGameDir(program_folder / "settings.json", eboot)) {
            LOG_ERROR(Common, "Could not write SpartanPort's settings.json");
            return false;
        }
        // The resolution here (the port's launcher is hard to use with a pad), the rest at the
        // best the port offers, then straight in.
        const auto choice =
            ChooseResolution(title.empty() ? serial : title, ExternalEngineResolution(serial));
        if (!choice) {
            return WentBack(serial);
        }
        const auto ini = program_folder / "spartanport.ini";
        SetKeyValue(ini, "render_resolution", *choice == "1080p" ? "native" : *choice);
        SetKeyValue(ini, "output_res",
                    *choice == "4K"      ? "3840x2160"
                    : *choice == "1440p" ? "2560x1440"
                                         : "1920x1080");
        SetKeyValue(ini, "display_mode", "fullscreen");
        SetKeyValue(ini, "engine_fps", "144");
        SetKeyValue(ini, "vsync", "1");
        SetKeyValue(ini, "async_shaders", "1");
        SetKeyValue(ini, "deferred_readback", "1");
        args.emplace_back("--play");
    } else if (engine.kind == "shadps4-release") {
        const auto choice =
            ChooseResolution(title.empty() ? serial : title, ExternalEngineResolution(serial));
        if (!choice) {
            return WentBack(serial);
        }
        SetKeyValue(program_folder / "shadps4-flags-user.txt", "SHADPS4_RENDER_HEIGHT",
                    *choice == "4K"      ? "2160"
                    : *choice == "1440p" ? "1440"
                                         : "1080");
        if (!WriteText(program_folder / "shadps4-game.txt", FS::PathToUTF8String(eboot) + "\n")) {
            LOG_ERROR(Common, "Could not write shadps4-game.txt");
            return false;
        }
    } else if (engine.kind == "issn") {
        const auto settings = AppDataFile("issn-launcher/settings.json");
        if (settings.empty() || !SetGameDir(settings, eboot)) {
            LOG_ERROR(Common, "Could not write issn_pc's settings.json");
            return false;
        }
    } else if (engine.kind == "bloodborne") {
        // Its launcher keeps the game folder and language in AppData; the picture in bbport.ini.
        const auto settings = AppDataFile("bbport-launcher/settings.json");
        if (settings.empty() || !SetGameDir(settings, eboot)) {
            LOG_ERROR(Common, "Could not write bloodborne_pc's settings.json");
            return false;
        }
        auto config = ReadJsonFile(settings);
        config["language"] = "3"; // Spanish
        WriteText(settings, config.dump(2));
        const auto choice =
            ChooseResolution(title.empty() ? serial : title, ExternalEngineResolution(serial));
        if (!choice) {
            return WentBack(serial);
        }
        // Native at the chosen size (no upscaler), full screen, the frame rate unlocked (the port's
        // default with game version 1.09).
        const auto ini = program_folder / "bbport.ini";
        SetKeyValue(ini, "output_res",
                    *choice == "4K"      ? "3840x2160"
                    : *choice == "1440p" ? "2560x1440"
                                         : "1920x1080");
        SetKeyValue(ini, "upscaler", "off");
        SetKeyValue(ini, "preset", "0");
        SetKeyValue(ini, "display_mode", "fullscreen");
        args.emplace_back("--play");
    } else if (engine.kind == "uncharted") {
        const auto settings = AppDataFile("Uncharted Launcher/launcher.json");
        if (settings.empty() || !SetGameDir(settings, eboot)) {
            LOG_ERROR(Common, "Could not write the Uncharted launcher's settings");
            return false;
        }
        // Three games in one: first the game, then its resolution, the rest at the port's best
        // (its "ultra" preset), then straight in (--play u1|u2|u3).
        auto config = ReadJsonFile(settings);
        const std::array<const char*, 3> keys{"u1", "u2", "u3"};
        int game = 0;
        for (int i = 0; i < 3; ++i) {
            if (config.value("game", std::string{"u1"}) == keys[i]) {
                game = i;
            }
        }
        const std::string name = title.empty() ? serial : title;
        while (true) {
            const auto picked = ChooseOption(name, "Elige el juego",
                                             {"Uncharted: El Tesoro de Drake",
                                              "Uncharted 2: El Reino de los Ladrones",
                                              "Uncharted 3: La Traicion de Drake"},
                                             game);
            if (!picked) {
                return WentBack(serial);
            }
            game = *picked;
            config["game"] = keys[game];
            WriteText(settings, config.dump(2));
            const auto choice = ChooseResolution(name, ExternalEngineResolution(serial));
            if (!choice) {
                continue; // back to the game list
            }
            const std::string res = *choice == "4K"      ? "3840x2160"
                                    : *choice == "1440p" ? "2560x1440"
                                                         : "1920x1080";
            config = ReadJsonFile(settings);
            config["games"][keys[game]]["resolution"] = res;
            config["games"][keys[game]]["preset"] = "ultra";
            WriteText(settings, config.dump(2));
            // The rest lives in the game's own ini beside the launcher.
            const auto ini = program_folder / "analysis" / "uncharted-user" /
                             (std::string{"probe-"} + keys[game] + ".ini");
            SetKeyValue(ini, "display_mode", "fullscreen");
            // 60: the port calls more experimental, and a session at 120 ended in a guest fault.
            SetKeyValue(ini, "fps_limit", "60");
            SetKeyValue(ini, "frames_queued", "1");
            SetKeyValue(ini, "vsync", "1");
            SetKeyValue(ini, "async_shaders", "1");
            SetKeyValue(ini, "rcas", "1");
            SetKeyValue(ini, "rcas_strength", "50");
            // Below 4K the picture is scaled to the screen: FSR 1 keeps it sharp.
            SetKeyValue(ini, "fsr1", *choice == "4K" ? "0" : "1");
            break;
        }
        args = {"--play", keys[game]};
    } else if (engine.kind == "kyty-astro") {
        const auto choice =
            ChooseResolution(title.empty() ? serial : title, ExternalEngineResolution(serial));
        if (!choice) {
            return WentBack(serial);
        }
        SetKeyValue(program_folder / "brunoshadps4-kyty.ini", "pantalla", *choice);
        // What measured best on an RX 6800 XT (CPU Plaza, frame times logged with KYTY_PACE_LOG):
        // render 1368p upscaled by FSR 3.1 to 4K, FSR frame generation, and the game held at 45
        // real fps (vblank 45, the fork's built-in Decouple keeps its speed). At 60 the emulated
        // CPU misses vblanks and frames jump between 16.7 and 33 ms (about 60 hitches a minute,
        // the same at 1080p); at 45 they stay at 22.2 ms (one a minute).
        const auto preset_path = program_folder / "u59-preset.json";
        auto preset = ReadJsonFile(preset_path);
        preset["KYTY_ASTRO_RENDER_RES"] = "1368";
        preset["KYTY_FSR"] = "1";
        preset["KYTY_FSR_SHARPNESS"] = "0.5";
        preset["KYTY_FRAMEGEN"] = "1";
        preset["KYTY_FRAMEGEN_BACKEND"] = "fsr";
        preset["KYTY_PRESENT_ABS_VBLANK"] = "1";
        preset["KYTY_FLOAT_PROFILE"] = "relaxed";
        WriteText(preset_path, preset.dump(2));
        const std::string width = *choice == "4K" ? "3840" : *choice == "1440p" ? "2560" : "1920";
        const std::string height = *choice == "4K" ? "2160" : *choice == "1440p" ? "1440" : "1080";
        args = {"--game",
                FS::PathToUTF8String(eboot.parent_path()),
                "--screen-width",
                width,
                "--screen-height",
                height,
                "--fullscreen",
                "--vblank-frequency",
                "45",
                "--present-mode",
                "Immediate",
                "--console-language",
                "20",
                "--skip-notice-screen",
                "true"};
        // No --amd-cpu: the emulator turns its AMD-instruction patch on by itself on Intel CPUs.
    } else if (engine.kind == "kyty-souls") {
        // Bruno's Demon's Souls build starts through its run-windows.ps1 (the switches of its
        // run-windows.json); the game is the folder or its .zar.
        const auto choice =
            ChooseResolution(title.empty() ? serial : title, ExternalEngineResolution(serial));
        if (!choice) {
            return WentBack(serial);
        }
        SetKeyValue(program_folder / "brunoshadps4-kyty.ini", "pantalla", *choice);
        const std::string width = *choice == "4K" ? "3840" : *choice == "1440p" ? "2560" : "1920";
        const std::string height = *choice == "4K" ? "2160" : *choice == "1440p" ? "1440" : "1080";
        // The game draws its 3D at 1440 at most (run-windows.ps1 -RenderHeight takes 720-1440) and
        // scales it to its 4K output, so 1080p screens also render at 1080 (faster).
        const std::string render = *choice == "1080p" ? "1080" : "1440";
        args = {"-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-WindowStyle",
                "Hidden",
                "-File",
                FS::PathToUTF8String(program_folder / "run-windows.ps1"),
                "-Game",
                FS::PathToUTF8String(eboot.parent_path()),
                "-Width",
                width,
                "-Height",
                height,
                "-RenderHeight",
                render,
                "-Fullscreen",
                "-Language",
                "3"};
    } else if (engine.kind == "tlg") {
        // HayatoG's port plays straight away with --play; the game's files are in its own folder
        // (TLG_GAME_DIR below, so nothing is set up on the PC).
        args = {"--play"};
    } else if (engine.kind == "shadps4") {
        args = {"--game", FS::PathToUTF8String(eboot)};
    } else {
        LOG_WARNING(Common, "Unknown kind '{}' for {} in motores.ini", engine.kind, serial);
        return false;
    }
    LOG_INFO(Common, "{} runs on {} ({})", serial, FS::PathToUTF8String(program), engine.kind);
    SyncGameSaves(serial, program_folder);
#ifdef _WIN32
    // kyty-souls starts through PowerShell; the job still holds the emulator it starts.
    if (engine.kind == "kyty-astro" || engine.kind == "kyty-souls") {
        PlaceShaderCacheOnLocalDrive(program_folder);
    }
    if (engine.kind == "tlg") {
        SetEnvironmentVariableW(L"TLG_GAME_DIR", program_folder.wstring().c_str());
    }
    std::filesystem::path run = program;
    if (engine.kind == "kyty-souls") {
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        run = std::filesystem::path{system} / "WindowsPowerShell" / "v1.0" / "powershell.exe";
    }
    const bool success = StartAndWait(run, args, title.empty() ? serial : title,
                                      EngineName(engine, program), true, program_folder);
    SyncGameSaves(serial, program_folder);
    ReturnToLauncher();
    return success;
#else
    LOG_ERROR(Common, "Other programs for a game are only started on Windows");
    return false;
#endif
}

bool StartedByRetroBat() {
#ifdef _WIN32
    return LaunchedFromRetroBat();
#else
    return false;
#endif
}

namespace {

/// RetroBat's own folder (with emulationstation\.emulationstation), above this executable.
std::filesystem::path RetroBatFolder() {
    std::error_code ec;
    for (auto dir = ExecutablePath().parent_path(); !dir.empty() && dir != dir.parent_path();
         dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / "emulationstation" / ".emulationstation", ec)) {
            return dir;
        }
    }
    return {};
}

std::string XmlUnescape(std::string text) {
    static const std::pair<const char*, const char*> entities[] = {
        {"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + 1)) {
            text.replace(at, std::strlen(from), to);
        }
    }
    return text;
}

} // namespace

std::string RetroBatGameOption(const std::filesystem::path& game, const std::string& option) {
    const auto root = RetroBatFolder();
    if (root.empty()) {
        return {};
    }
    // The ROM RetroBat started: the folder of an eboot.bin, or the file itself (a .zar).
    auto rom = game;
    if (ToLower(FS::PathToUTF8String(rom.filename())) == "eboot.bin") {
        rom = rom.parent_path();
    }
    const std::string system = ToLower(FS::PathToUTF8String(rom.parent_path().filename()));
    const std::string key = system + "[\"" + FS::PathToUTF8String(rom.filename()) + "\"]." + option;
    // es_settings.cfg lines: <string name="ps4[&quot;Game.ps4&quot;].option" value="1" />
    std::ifstream in{root / "emulationstation" / ".emulationstation" / "es_settings.cfg"};
    std::string line;
    static const std::regex entry{R"re(name="([^"]*)"\s+value="([^"]*)")re"};
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, entry) && XmlUnescape(m[1].str()) == key) {
            return XmlUnescape(m[2].str());
        }
    }
    return {};
}

namespace {

/// The games each kind of engine runs, for RetroBat's list of forks.
std::string GamesOfKind(const std::string& kind) {
    if (kind == "spartanport")
        return "God of War III";
    if (kind == "shadps4-release")
        return "inFAMOUS Second Son";
    if (kind == "uncharted")
        return "Uncharted";
    if (kind == "bloodborne")
        return "Bloodborne";
    if (kind == "kyty-astro")
        return "ASTRO BOT, ASTRO's PLAYROOM";
    if (kind == "kyty-souls")
        return "Demon's Souls";
    if (kind == "tlg")
        return "The Last Guardian";
    if (kind == "interno")
        return "los demás juegos de PS4";
    return {};
}

std::string AttributeEscape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\n':
            out += "&#10;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

/// Replaces the feature with `value` under the PS4 emulator in es_features.cfg (or adds it).
void SetFeature(std::string& xml, const std::string& value, const std::string& feature) {
    constexpr std::string_view anchor = "<emulator name=\"shadps4\">";
    const auto emulator = xml.find(anchor);
    if (emulator == std::string::npos) {
        return;
    }
    const auto found = xml.find("value=\"" + value + "\"", emulator);
    const auto emulator_end = xml.find("</emulator>", emulator);
    if (found != std::string::npos && found < emulator_end) {
        const auto begin = xml.rfind("<feature", found);
        const auto end = xml.find("</feature>", found);
        if (begin != std::string::npos && end != std::string::npos) {
            xml.replace(begin, end + 10 - begin, feature);
            return;
        }
    }
    xml.insert(emulator + anchor.size(), "\r\n    " + feature);
}

} // namespace

void RefreshRetroBatForkOptions() {
    const auto root = RetroBatFolder();
    if (root.empty()) {
        return;
    }
    std::thread([root] {
        SetCurrentThreadName("RetroBatForks");
        // One line per installed fork: its version, the latest on GitHub and what to do.
        std::vector<std::pair<Engine, std::filesystem::path>> programs;
        std::set<std::filesystem::path> seen;
        const auto folder = ExecutableFolder();
        for (const auto& [serial, engine] : LoadTable(folder)) {
            if (engine.kind == "interno" || engine.program.empty()) {
                continue;
            }
            const auto program = ResolveProgramPath(folder, engine.program);
            std::error_code ec;
            if (std::filesystem::exists(program, ec) && seen.insert(program).second) {
                programs.emplace_back(engine, program);
            }
        }
        programs.emplace_back(Engine{"interno", ExecutablePath()}, ExecutablePath());
        std::string info;
        bool any_update = false;
        for (const auto& [engine, program] : programs) {
            const bool own = engine.kind == "interno";
            const std::string installed = own ? CleanEmulatorVersion() : EngineVersion(program);
            const std::string name = own ? std::string{"ForkStation"} : EngineName(engine, program);
            const auto release = FetchLatestRelease(engine, EngineGitHubRepo(engine, program));
            std::string status;
            if (!release.ok) {
                status = "no se pudo consultar GitHub";
            } else if (IsNewerVersion(engine, installed, release.version)) {
                status = "NUEVA VERSIÓN " + release.version + " DISPONIBLE";
                any_update = true;
            } else {
                status = "al día";
            }
            info += fmt::format("• {} ({}): {} — {}\n", name, GamesOfKind(engine.kind),
                                installed.empty() ? "sin versión" : installed, status);
        }
        const std::time_t now = std::time(nullptr);
        if (const std::tm* when = std::localtime(&now)) {
            char date[32]{};
            std::strftime(date, sizeof(date), "%d/%m/%Y %H:%M", when);
            info += fmt::format("Comprobado el {}.", date);
        }

        const auto features = root / "emulationstation" / ".emulationstation" / "es_features.cfg";
        std::string xml = ReadText(features);
        if (xml.empty()) {
            return;
        }
        SetFeature(xml, "brunoshadps4_fork",
                   "<feature submenu=\"FORK\" name=\"ACTUALIZAR FORK DESDE SU GITHUB\" "
                   "group=\"BRUNOSHADPS4\" value=\"brunoshadps4_fork\" description=\"" +
                       AttributeEscape("Al iniciar el juego muestra la versión instalada de su "
                                       "fork y la última publicada en su GitHub, y permite "
                                       "actualizarlo antes de jugar.") +
                       "\" order=\"1\">\r\n      <choice name=\"No\" value=\"0\"/>\r\n      "
                       "<choice name=\"Sí, al iniciar el juego\" value=\"1\"/>\r\n    </feature>");
        SetFeature(
            xml, "brunoshadps4_forks_info",
            "<feature submenu=\"FORK\" name=\"" +
                std::string{any_update ? "VERSIÓN DE LOS FORKS: HAY ACTUALIZACIONES"
                                       : "VERSIÓN DE LOS FORKS: TODOS AL DÍA"} +
                "\" group=\"BRUNOSHADPS4\" value=\"brunoshadps4_forks_info\" description=\"" +
                AttributeEscape(info) +
                "\" order=\"2\">\r\n      <choice name=\"Ver descripción\" value=\"0\"/>\r\n"
                "    </feature>");
        const auto temp = features.parent_path() / "es_features.cfg.brunoshadps4";
        std::error_code ec;
        if (WriteText(temp, xml)) {
            std::filesystem::rename(temp, features, ec);
        }
    }).detach();
}

} // namespace Common
