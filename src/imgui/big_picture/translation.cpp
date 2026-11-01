// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <unordered_map>
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>

#include "core/emulator_settings.h"
#include "imgui/big_picture/translation.h"

namespace BigPictureMode {

namespace {

const std::unordered_map<std::string_view, std::string_view> Spanish = {
    // Launcher
    {"Play", "Jugar"},
    {"Settings", "Ajustes"},
    {"Update", "Actualizar"},
    {"Reinstall", "Reinstalar"},
    {"Engine: ", "Motor: "},
    {"Update engine", "Actualizar motor"},
    {"Repository: ", "Repositorio: "},
    {"Installed version: ", "Versión instalada: "},
    {"Latest official release: ", "Último release oficial: "},
    {"not installed", "sin instalar"},
    {"Add games", "Agregar juegos"},
    {"Add games folder", "Agregar carpeta de juegos"},
    {"No games yet. Add the folder that contains your games.",
     "Aún no hay juegos. Agrega la carpeta que contiene tus juegos."},
    {"Use recommended settings", "Usar ajustes recomendados"},
    {"Game folder", "Carpeta del juego"},
    {"Choose the game folder", "Elegir la carpeta del juego"},
    {"{} was not found. Choose the folder that holds the game folder.",
     "No se encontró {}. Elige la carpeta que contiene la carpeta del juego."},
    {"Main settings", "Ajustes principales"},
    {"Brought to PC with the help of shadPS4", "Porteado a PC con la ayuda de shadPS4"},
    {"Ported to PC", "Porteado a PC"},
    {"All settings", "Todos los ajustes"},
    {"This game", "Este juego"},
    {"Higher resolution (game patch)", "Más resolución (parche del juego)"},
    {"Enlarge with FSR", "Ampliar con FSR"},
    {"Sharpen the enlarged picture", "Dar nitidez a la imagen ampliada"},
    {"FXAA anti-aliasing", "Suavizado FXAA"},
    {"Frame generation", "Generación de cuadros (Frame generation)"},
    {"Resolution", "Resolución"},
    {"Show FPS and resolution", "Mostrar FPS y resolución"},
    {"Game language", "Idioma del juego"},
    {"Apply", "Aplicar"},
    {"Changes not applied yet", "Cambios sin aplicar"},
    {"Settings applied", "Ajustes aplicados"},
    {"Original of the game", "Original del juego"},
    {"Vertical sync", "Sincronización vertical"},
    {"Settings tested with this game. You can change them later.",
     "Ajustes probados con este juego. Puedes cambiarlos después."},
    {"Size", "Tamaño"},
    {"Full screen", "Pantalla completa"},
    {"Output: ", "Salida: "},
    {"Resolution: ", "Resolución: "},
    {"Enlarged with FSR", "Ampliada con FSR"},
    {"Enlarged to {} with FSR", "Ampliada a {} con FSR"},
    {"Without FSR", "Sin FSR"},
    {"original of the game", "la del juego"},
    {"patched", "con parche"},
    {"Render Resolution", "Resolución de render"},
    {"Original of the game (more fps)", "Original del juego (más fps)"},
    {"no FSR", "sin FSR"},
    {"Frame generation: on", "Generación de cuadros: sí"},
    {"Frame generation: off", "Generación de cuadros: no"},
    {"Not played yet", "Sin jugar aún"},
    {"Played: ", "Jugado: "},
    {"Last played: ", "Última vez: "},
    {"%Y-%m-%d", "%d/%m/%Y"},
    {"English (United States)", "Inglés (Estados Unidos)"},
    {"English (United Kingdom)", "Inglés (Reino Unido)"},
    {"Spanish (Latin America)", "Español (Latinoamérica)"},
    {"Spanish (Spain)", "Español (España)"},
    {"French (France)", "Francés (Francia)"},
    {"French (Canada)", "Francés (Canadá)"},
    {"Portuguese (Brazil)", "Portugués (Brasil)"},
    {"Portuguese (Portugal)", "Portugués (Portugal)"},
    {"German", "Alemán"},
    {"Italian", "Italiano"},
    {"Japanese", "Japonés"},
    {"Korean", "Coreano"},
    {"Russian", "Ruso"},
    {"Dutch", "Neerlandés"},
    {"Polish", "Polaco"},
    {"Turkish", "Turco"},
    {"Arabic", "Árabe"},
    {"Simplified Chinese", "Chino simplificado"},
    {"Traditional Chinese", "Chino tradicional"},
    {"A fork of shadPS4", "Un fork de shadPS4"},
    {"About", "Acerca de"},
    {"About ForkStation", "Acerca de ForkStation"},
    {"PS4 and PS5 games on PC", "Juegos de PS4 y PS5 en PC"},
    {"A fork of shadPS4, the PlayStation 4 emulator.",
     "Un fork de shadPS4, el emulador de PlayStation 4."},
    {"All the credit for the emulator goes to the shadPS4 Emulator",
     "Todo el crédito del emulador es del shadPS4 Emulator"},
    {"Project and its contributors.", "Project y sus colaboradores."},
    {"This fork adds", "Este fork agrega"},
    {"Frame generation with AMD FSR 3 (FidelityFX SDK, MIT)",
     "Generación de cuadros con AMD FSR 3 (FidelityFX SDK, MIT)"},
    {"Fixes and speed-ups for God of War III and inFamous Second Son",
     "Correcciones y mejoras de rendimiento para God of War III e inFamous Second Son"},
    {"This launcher, set in Poppins (OFL)", "Este lanzador, con la fuente Poppins (OFL)"},
    {"Close", "Cerrar"},
    {"Contact", "Contacto"},
    {"Version", "Versión"},

    // Settings window
    {"Profiles", "Perfiles"},
    {"General", "General"},
    {"Graphics", "Gráficos"},
    {"Input", "Controles"},
    {"Trophy", "Trofeos"},
    {"Game Folders", "Carpetas"},
    {"Log", "Registro"},
    {"Experimental", "Experimental"},
    {"Patches", "Parches"},
    {"Patches of this game", "Parches de este juego"},
    {"A patch changes how the game runs: its resolution, its frame rate and the like. The "
     "patches switched on here are applied when the game starts from this launcher.",
     "Un parche cambia cómo corre el juego: su resolución, sus cuadros por segundo y cosas así. "
     "Los parches activados aquí se aplican cuando el juego se inicia desde este lanzador."},
    {"File: ", "Archivo: "},
    {"Add a patch file...", "Agregar un archivo de parches..."},
    {"Replace the patch file...", "Reemplazar el archivo de parches..."},
    {"Open the patches folder", "Abrir la carpeta de parches"},
    {"This game has no patch file yet.", "Este juego aún no tiene archivo de parches."},
    {"The patch file has no patches in it.", "El archivo de parches no contiene parches."},
    {"Apply the patches of this game", "Aplicar los parches de este juego"},
    {"Off: the game starts as it is, whatever is switched on below. Kept with Save.",
     "Apagado: el juego inicia tal cual, sin importar lo activado abajo. Se conserva con Guardar."},
    {"Patch", "Parche"},
    {"by ", "por "},
    {"patch ", "parche "},
    {"for game version ", "para la versión del juego "},
    {"That file is not a patch file.", "Ese archivo no es un archivo de parches."},
    {"That patch file is for another game.", "Ese archivo de parches es de otro juego."},
    {"The patch file could not be copied.", "No se pudo copiar el archivo de parches."},
    {"Patch file added.", "Archivo de parches agregado."},
    {"Folders that contain your games", "Carpetas que contienen tus juegos"},
    {"Editing: Global settings (all games)", "Editando: ajustes globales (todos los juegos)"},
    {"Editing: ", "Editando: "},
    {"Use suggested settings", "Usar ajustes sugeridos"},
    {"Save", "Guardar"},
    {"Apply", "Aplicar"},
    {"Cancel", "Cancelar"},
    {"OK", "Aceptar"},
    {"Save Confirmation", "Confirmar guardado"},
    {"Profile Saved:\n", "Perfil guardado:\n"},

    // Profiles
    {"Which settings do you want to change?", "¿Qué ajustes quieres cambiar?"},
    {"Global applies to every game. A game with its own settings ignores Global.",
     "Los globales valen para todos los juegos. Un juego con ajustes propios ignora los globales."},
    {"Global settings", "Ajustes globales"},
    {"Used by every game that has no settings of its own",
     "Los usan todos los juegos que no tienen ajustes propios"},
    {"  -  has its own settings", "  -  tiene ajustes propios"},
    {"  -  uses the global settings", "  -  usa los ajustes globales"},
    {"Editing", "Editando"},
    {"Custom", "Propio"},
    {"Remove", "Quitar"},
    {"Remove this game's own settings and use Global again",
     "Quita los ajustes propios de este juego y vuelve a usar los globales"},
    {"Confirm Delete", "Confirmar"},
    {"Remove the settings of ", "¿Quitar los ajustes de "},
    {"?\nThe game will use the global settings again.",
     "?\nEl juego volverá a usar los ajustes globales."},

    // Game folders
    {"Where are your games?", "¿Dónde están tus juegos?"},
    {"Add the folder that holds your games, with one folder per game inside.",
     "Agrega la carpeta que guarda tus juegos, con una carpeta por juego dentro."},
    {"Add a folder...", "Agregar una carpeta..."},
    {"Add a folder of games", "Agregar una carpeta de juegos"},
    {"Show the games of this folder", "Mostrar los juegos de esta carpeta"},
    {"Take this folder off the list. Nothing is deleted from disk.",
     "Quita esta carpeta de la lista. No se borra nada del disco."},
    {"No folders yet.", "Aún no hay carpetas."},

    // General
    {"Interface Language", "Idioma de la interfaz"},
    {"System", "Sistema"},
    {"Console Language", "Idioma de la consola"},
    {"Volume", "Volumen"},
    {"Show Splash Screen When Launching Game", "Mostrar pantalla de inicio al abrir el juego"},
    {"Audio Backend", "Motor de audio"},

    // Graphics
    {"Display Mode", "Modo de pantalla"},
    {"Windowed", "Ventana"},
    {"Fullscreen", "Pantalla completa exclusiva"},
    {"Fullscreen (Borderless)", "Pantalla completa sin bordes"},
    {"Present Mode", "Modo de presentación"},
    {"Mailbox", "Mailbox (sin cortes, baja latencia)"},
    {"Fifo", "Fifo (sincronización vertical)"},
    {"Immediate", "Inmediato (sin sincronización)"},
    {"Window Size", "Tamaño de la ventana"},
    {"The resolution the game renders at is what sets its speed: lower means more fps. The "
     "picture is then enlarged to fill the window or the screen, with FSR or without it, at "
     "next to no cost.",
     "La resolución a la que el juego renderiza es la que marca su velocidad: más baja, más fps. "
     "Después la imagen se amplía para llenar la ventana o la pantalla, con FSR o sin él, casi "
     "sin costo."},
    {"Enable HDR", "Activar HDR"},
    {"Enable FSR", "Activar FSR"},
    {"Enable RCAS", "Activar RCAS"},
    {"RCAS Attenuation", "Atenuación de RCAS"},
    {"Frame Generation (FSR 3)", "Generación de cuadros (FSR 3)"},
    {"Enhance Game Quality", "Realzar la calidad del juego"},
    {"Direct Readbacks", "Lecturas directas (readbacks)"},
    {"Render Target Sync", "Sincronizar destinos de render"},

    // Input
    {"Enable Motion Controls", "Activar controles de movimiento"},
    {"Enable Background Controller Input", "Aceptar el mando con la ventana en segundo plano"},
    {"Hide Cursor", "Ocultar el cursor"},
    {"Never", "Nunca"},
    {"Idle", "Al estar inactivo"},
    {"Always", "Siempre"},
    {"Hide Cursor Idle Timeout", "Segundos de inactividad para ocultar el cursor"},

    // Trophy
    {"Disable Trophy Notification", "Desactivar el aviso de trofeos"},
    {"Trophy Notification Position", "Posición del aviso de trofeos"},
    {"Trophy Notification Duration", "Duración del aviso de trofeos"},
    {"left", "izquierda"},
    {"right", "derecha"},
    {"top", "arriba"},
    {"bottom", "abajo"},

    // Log
    {"Enable Logging", "Activar el registro"},
    {"Separate Log Files", "Un archivo de registro por juego"},
    {"Log Sync", "Registro sincrónico"},

    // Experimental
    {"Additional DMem Allocation", "Memoria directa adicional (MB)"},
    {"Vblank Frequency", "Frecuencia de vblank"},
    {"Readbacks Mode", "Modo de readbacks"},
    {"Disabled", "Desactivado"},
    {"Relaxed", "Relajado"},
    {"Precise", "Preciso"},
    {"Enable Readback Linear Images", "Activar readback de imágenes lineales"},
    {"Enable Direct Memory Access", "Activar acceso directo a memoria"},
    {"Windows Guest Red Zone Protection (Requires Restart)",
     "Protección de la zona roja en Windows (requiere reiniciar)"},
    {"Enable Devkit Console Mode", "Activar modo consola devkit"},
    {"Enable PS4 Neo Mode", "Activar modo PS4 Neo (Pro)"},
    {"Enable ShadNet", "Activar ShadNet"},
    {"Set Network Connected to True", "Simular red conectada"},
    {"Enable Shader Cache", "Activar caché de shaders"},
    {"Compress Shader Cache to Zip File", "Comprimir la caché de shaders en un zip"},

    // Loading screen
    {"Reading the shader cache", "Leyendo la caché de shaders"},
    {"Compiling cached shaders", "Compilando los shaders guardados"},
    {"Starting the game", "Iniciando el juego"},
};

std::optional<bool> spanish;

bool SystemIsSpanish() {
    bool result = false;
    int count = 0;
    if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
        result =
            count > 0 && locales[0]->language && std::string_view{locales[0]->language} == "es";
        SDL_free(locales);
    }
    return result;
}

} // Anonymous namespace

void SetGuiLanguage(std::string_view language) {
    if (language == GuiLanguageSpanish) {
        spanish = true;
    } else if (language == GuiLanguageEnglish) {
        spanish = false;
    } else {
        spanish = SystemIsSpanish();
    }
}

const char* Tr(std::string_view english) {
    if (!spanish) {
        SetGuiLanguage(EmulatorSettings.GetGuiLanguage());
    }
    // Every text asked for is kept, so that the pointers handed out stay valid.
    static std::unordered_map<std::string, std::string> texts[2];
    auto& cache = texts[*spanish ? 1 : 0];
    const std::string key{english};
    if (const auto it = cache.find(key); it != cache.end()) {
        return it->second.c_str();
    }
    const auto id = english.find("##");
    const std::string_view text = english.substr(0, id);
    std::string result{text};
    if (*spanish) {
        if (const auto it = Spanish.find(text); it != Spanish.end()) {
            result = it->second;
        }
    }
    if (id != std::string_view::npos) {
        // Three hashes: the identifier is all that follows, whatever the visible text is.
        result += "###";
        result += english.substr(id + 2);
    }
    return cache.emplace(key, std::move(result)).first->second.c_str();
}

} // namespace BigPictureMode
