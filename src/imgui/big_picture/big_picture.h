// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include <filesystem>
#include <vector>
#include <SDL3/SDL.h>
#include <imgui.h>

#include "common/types.h"

namespace BigPictureMode {

struct IconInfo {
    ImTextureID textureId;
    std::filesystem::path ebootPath;
    std::string title;
    std::string serial;
    bool focusState;
    /// What the card says under the name: the size and the way the game will be shown, its
    /// language, and how much and when it was played.
    std::array<std::string, 7> details;
    /// Runs on another program (motores.ini): its settings are that program's, not this one's.
    bool external{};
    /// A PS5 game (sce_sys/param.json): listed in its own section after the PS4 games.
    bool ps5{};
};

void Launch(char* executableName, bool sameProcess);
void GetGameIconInfo(std::vector<IconInfo>& icons);
SDL_Texture* LoadSdlTextureData(std::vector<u8> data);
SDL_Texture* LoadSdlTextureDataFromFile(std::filesystem::path filePath);

} // namespace BigPictureMode
