//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GameHotkeys.h
//
//  The hotkeys of Input device settings > HOTKEYS while a game runs. EmuSPLASH watches the pads:
//  the close game hotkey ends the emulator, so it works with every emulator, and for
//  emulators that only have keyboard shortcuts (RPCS3) a hotkey presses the keys of the
//  shortcut. Emulators that close themselves with the same hotkey (it's written into their
//  settings) get some seconds first.
//

#ifndef ES_APP_EMULATORS_GAME_HOTKEYS_H
#define ES_APP_EMULATORS_GAME_HOTKEYS_H

#include <string>
#include <utility>
#include <vector>

namespace GameHotkeys
{
    // The buttons of the combinations, as kept in the settings ("back+east").
    struct Button {
        std::string id;
        std::string label;
    };
    const std::vector<Button>& getButtons();

    // The combinations offered in the menu: {setting value, label}. Closing a game has more
    // of them (the ones with START).
    std::vector<std::pair<std::string, std::string>> getCombinations(bool closeGame = false);

    // The buttons of the combination of a setting ("HotkeyCloseGame"), empty for OFF.
    std::vector<std::string> getCombination(const std::string& setting);

    // A hotkey that presses keys in the window of the emulator ("Ctrl+S").
    struct KeyAction {
        std::string name;
        std::vector<std::string> buttons;
        std::string keys;
    };

    struct Launch {
        // The time the emulator gets to close itself before EmuSPLASH ends it.
        int closeWaitMs {0};
        // The time the emulator gets to end after it was asked to, before it's killed.
        int killWaitMs {2000};
        std::vector<KeyAction> keyActions;
    };

    // Starts watching before a game is launched.
    void begin(const Launch& launch);
    // Stops watching after the game has ended.
    void end();
} // namespace GameHotkeys

#endif // ES_APP_EMULATORS_GAME_HOTKEYS_H
