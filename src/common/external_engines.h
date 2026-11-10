// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

/// What the launcher found about a game's engine on GitHub when it opened.
struct EngineUpdateHint {
    bool checked = false;   ///< GitHub answered for this engine
    bool missing = false;   ///< the engine is not installed: it has to be
    bool available = false; ///< a newer version than the installed one
    std::string installed_version;
    std::string latest_version;
};

/// Checks, in the background and once per engine, whether each game's engine has a newer release
/// (ForkStation itself for the games that run here).
void StartUpdateChecks(const std::vector<std::string>& serials);

/// The check's result for a game, once it has one.
std::optional<EngineUpdateHint> GetUpdateHint(const std::string& serial);

/// A game just added: its fork, being found on GitHub and installed in the background.
struct AutoInstallStatus {
    bool active = false;   ///< queued, searching, downloading or unpacking
    bool finished = false; ///< done (successfully or not)
    bool success = false;
    float progress = 0.0f; ///< 0..1, < 0 while it is not known (searching)
    std::string message;
};

/// For each game (serial, "its name|its folder's name"): the fork this program knows for it when
/// it is missing, or else the most suitable and up to date one found on GitHub (its name or serial
/// in a PC port's or emulator fork's repository, with a Windows .zip release), downloaded into
/// motores\ and written into motores.ini. One game at a time, in the background.
void StartAutoInstall(const std::vector<std::pair<std::string, std::string>>& games);

/// The game's install, while it runs and after it.
std::optional<AutoInstallStatus> GetAutoInstall(const std::string& serial);

/// Grows each time an install ends (to refresh what the cards say).
int AutoInstallGeneration();

/// A folder's name as a game's name, without its serial and extension.
std::string GameNameFromFolder(const std::string& folder_name);

/// Brings a window (its native handle) to the front, even when Windows would refuse it.
void BringWindowToFront(void* native_window);

/// Plays the sound the player put at user\intro.mp3 (or .wav) beside the program, at full
/// volume, without waiting for it. Nothing happens when there is no such file.
void PlayStartupSound();

/// Whether RetroBat (its emulatorLauncher) started this program.
bool StartedByRetroBat();

/// A game's option in RetroBat's "Opciones avanzadas del juego" (es_settings.cfg, the key
/// `<system>["<rom>"].<option>`), for the game path RetroBat handed over; empty when unset.
std::string RetroBatGameOption(const std::filesystem::path& game, const std::string& option);

/// RetroBat's game options for the forks (es_features.cfg, PS4 emulator): "Actualizar fork desde
/// su GitHub" (brunoshadps4_fork: No / Sí, al iniciar el juego) and "Versión de los forks", whose
/// description lists each installed fork's version, the latest on GitHub and whether it is newer.
/// Checks GitHub in the background; RetroBat shows the result from its next start.
void RefreshRetroBatForkOptions();

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
