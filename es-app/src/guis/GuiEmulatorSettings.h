//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiEmulatorSettings.h
//
//  The menus of the settings of the emulators (Game settings > EMULATOR SETTINGS), of a game
//  (game options > ADVANCED GAME OPTIONS) and the hotkeys (Input device settings > HOTKEYS).
//

#ifndef ES_APP_GUIS_GUI_EMULATOR_SETTINGS_H
#define ES_APP_GUIS_GUI_EMULATOR_SETTINGS_H

class FileData;

namespace GuiEmulatorSettings
{
    // The emulators whose settings file exists, each opens its settings.
    void openEmulators();
    // The settings of a game for its emulator, AUTO being the one of the emulator.
    void openGame(FileData* game);
    // True if the emulator of the game has settings EmuSPLASH can change.
    bool hasGameSettings(FileData* game);
    // The hotkeys while a game runs.
    void openHotkeys();
    // The ROM directories: ROMDirectory and up to two more, whose games are in the same
    // systems. A change loads the games again.
    void openRomDirectories();
} // namespace GuiEmulatorSettings

#endif // ES_APP_GUIS_GUI_EMULATOR_SETTINGS_H
