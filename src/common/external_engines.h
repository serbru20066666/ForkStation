// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace Common {

/// Games that run better on another program: motores.ini next to the executable names, per game
/// serial, the program and how to hand it the game. When the game being started has one and it is
/// there, it is started instead. Built in (paths relative to the executable's folder; motores.ini
/// replaces them line by line):
///   CUSA01623,CUSA01715 = spartanport | motores\SpartanPort\SpartanPort.exe
///   CUSA00004,CUSA00223 = shadps4-release | motores\inFamous\shadPS4.exe
///   CUSA02320,CUSA02343,CUSA02344 = uncharted | motores\Uncharted\Uncharted Launcher.exe
///   CUSA03173,CUSA00900,... = bloodborne | motores\Bloodborne\Bloodborne.exe
///   PPSA01325,PPSA21564,... = kyty-astro | motores\AstroBot\kyty_emulator.exe
/// Kinds:
///   spartanport      writes the game folder into the program's settings.json, runs it with --play
///   issn             the same launcher with its settings in %APPDATA%\issn-launcher\settings.json
///   shadps4-release  writes the eboot path into shadps4-game.txt beside it, runs with no args
///   shadps4          runs it with --game <eboot>
///   uncharted        writes the game folder into %APPDATA%\Uncharted Launcher\launcher.json and
///                    opens that launcher, to choose one of the collection's three games
///   bloodborne       the game folder in %APPDATA%\bbport-launcher\settings.json, the picture in
///                    bbport.ini beside it, runs it with --play
///   kyty-astro       biggsymobile's KytyPS5 fork for Astro's Playroom / Astro Bot (PS5): runs it
///                    with --game <game folder> at the chosen screen size; its own settings
///                    (render resolution, FSR, frame generation) are in u59-preset.json beside it
/// "interno" as the kind keeps the game on this program.

/// A PS5 game folder (sce_sys/param.json instead of param.sfo): its title ID and name, Spanish
/// first. PS5 games only run on another program (motores.ini), never here.
struct Ps5GameInfo {
    std::string serial;
    std::string title;
};
std::optional<Ps5GameInfo> ReadPs5GameInfo(const std::filesystem::path& game_folder);

/// What a game card says about a game that runs on another program, from that program's own
/// settings (or what it used the last time). Empty for games that run here.
struct ExternalEngineCard {
    std::string engine;
    std::string version;
    bool installed = true;
    std::array<std::string, 4> details;
};
std::optional<ExternalEngineCard> DescribeExternalEngine(const std::string& serial);

/// Progress and result of updating a fork/port from its official GitHub Releases.
struct EngineUpdateStatus {
    bool active = false;
    bool finished = false;
    bool success = false;
    bool up_to_date = false;
    float progress = 0.0f; ///< 0..1 while downloading or extracting (< 0 if indeterminate)
    std::string serial;
    std::string engine_name;
    std::string repo;
    std::string installed_version;
    std::string latest_version;
    std::string message;
};

/// Starts checking/downloading/installing the latest official GitHub release for the game's
/// fork/port. If `force_reinstall` is true, downloads and installs even when already up to date.
void StartEngineUpdate(const std::string& serial, bool force_reinstall = false);

/// Current state of the ongoing or last engine update.
EngineUpdateStatus GetEngineUpdateStatus();

/// Dismisses the finished update status.
void ClearEngineUpdateStatus();

/// The render resolution of a game on another program, as "1080p", "1440p" or "4K" (empty when
/// that program's resolution cannot be set from here), and the next one, written into that
/// program's own settings.
std::string ExternalEngineResolution(const std::string& serial);
std::string CycleExternalEngineResolution(const std::string& serial);

/// Starts the game on its program, if it has one there. While that program prepares the game a
/// small window says so; this returns once the program and everything it started have ended, so
/// front ends such as RetroBat wait for the game. Returns false if the game runs here.
bool LaunchExternalEngine(const std::string& serial, const std::string& title,
                          const std::filesystem::path& eboot);

} // namespace Common
