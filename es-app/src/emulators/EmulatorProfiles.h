//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  EmulatorProfiles.h
//
//  The emulators whose own configuration files EmuSPLASH can change: where the files are, which
//  of their settings are offered in the menus (EMULATOR SETTINGS for the emulator, ADVANCED
//  GAME OPTIONS for a game) and how the hotkeys are written into them.
//
//  The files of the emulators are the only place the settings are kept: the menus show what
//  is in them and write the changes back. A setting of a game left at AUTO is the one of the
//  emulator. RetroArch is the exception: its retroarch.cfg isn't changed, EmuSPLASH keeps the
//  changed settings in files of its own that are added with --appendconfig when a game is
//  started. An emulator is added by adding its profile to the table of EmulatorProfiles.cpp.
//

#ifndef ES_APP_EMULATORS_EMULATOR_PROFILES_H
#define ES_APP_EMULATORS_EMULATOR_PROFILES_H

#include "emulators/GameHotkeys.h"

#include <string>
#include <utility>
#include <vector>

class FileData;

namespace EmulatorProfiles
{
    // The format of a configuration file.
    enum class Format {
        QT_INI, // yuzu, suyu, Eden, citron: "key=value" with "key\default=false"
        INI, // PCSX2, PPSSPP, Cemu game profiles: "Key = Value"
        YAML, // RPCS3 (sections) and Vita3K (no sections): "Key: value"
        CEMU_XML, // Cemu: <Section><key>value</key></Section>
        RETROARCH_CFG // RetroArch: key = "value", no sections
    };

    // How the settings of a single game are kept by the emulator.
    enum class GameFiles {
        NONE, // only the settings of the emulator
        QT_CUSTOM, // custom/<title ID>.ini, "key\use_global=false"
        RPCS3_CUSTOM, // custom_configs/config_<serial>.yml, only the changed keys
        PCSX2_GAMESETTINGS, // gamesettings/<serial>_<CRC>.ini, only the changed keys
        PPSSPP_GAME_INI, // <game ID>_ppsspp.ini, a copy of the settings of the emulator
        CEMU_PROFILE, // gameProfiles/<title ID>.ini, only the changed keys
        RETROARCH_OVERRIDE // a file of EmuSPLASH for the game, appended to the configuration
    };

    struct Choice {
        std::string label;
        std::string value;
    };

    struct Setting {
        std::string label;
        // "Section/key", or "key" for files without sections. Empty for a setting that only
        // games have.
        std::string key;
        std::vector<Choice> choices;
        // Can be set for a single game.
        bool perGame {true};
        // The key in the file of a game, if it isn't the same as in the file of the emulator.
        std::string gameKey {};
    };

    struct Profile {
        std::string id;
        std::string name;
        // Words of the program of a launch command that tell it is this emulator.
        std::vector<std::string> match;
        Format format;
        // The configuration file of the emulator, the first one that exists.
        std::vector<std::string> configFiles;
        GameFiles gameFiles {GameFiles::NONE};
        std::vector<Setting> settings;
        // The emulator closes itself with the close hotkey (it's written into its settings),
        // EmuSPLASH only ends it if it hasn't closed after a few seconds.
        bool closesItself {false};
        // The settings are kept in a file of EmuSPLASH, not in the configuration file (RetroArch).
        bool ownSettingsFile {false};
    };

    // All profiles.
    const std::vector<Profile>& getProfiles();

    // The profile of an emulator launch command (raw or expanded), nullptr if none.
    const Profile* findByCommand(const std::string& command);

    // The profile of the emulator a game is started with, nullptr if none.
    const Profile* findForGame(FileData* game);

    // The configuration file of the emulator, empty if it doesn't exist.
    std::string getConfigFile(const Profile& profile);

    // The file with the settings of a game, empty with the reason if it can't be told (no
    // ID found in the game, or the emulator has no settings for single games).
    std::string getGameFile(const Profile& profile, FileData* game, std::string& reason);

    // The values of the settings of the emulator that EmuSPLASH has written, by the index of the
    // setting. For RetroArch these are the ones of EmuSPLASH's own file, empty if not changed.
    std::vector<std::string> readValues(const Profile& profile);
    // The values the emulator uses without EmuSPLASH (retroarch.cfg), the same as readValues()
    // for the other emulators.
    std::vector<std::string> readBaseValues(const Profile& profile);
    // Writes changed values of the emulator, an empty one removes the setting from EmuSPLASH's own
    // file (RetroArch). False and the reason if it failed.
    bool writeValues(const Profile& profile,
                     const std::vector<std::pair<std::string, std::string>>& values,
                     std::string& error);

    // The values of the settings of a game, empty for AUTO (the one of the emulator).
    std::vector<std::string> readGameValues(const Profile& profile, const std::string& gameFile);
    // Writes the settings of a game, an empty value is AUTO. The keys are the ones of the
    // settings (key or gameKey).
    bool writeGameValues(const Profile& profile,
                         const std::string& gameFile,
                         const std::vector<std::pair<std::string, std::string>>& values,
                         std::string& error);

    // The key of a setting in the file of a game.
    const std::string& gameKeyOf(const Setting& setting);
    // The label of a value of a setting, the value itself if it isn't one of the choices.
    std::string labelOf(const Setting& setting, const std::string& value);

    // Before a game is started with the emulator: writes the hotkeys of Input device settings
    // > HOTKEYS into its settings, adds the settings of EmuSPLASH to the command (RetroArch) and
    // tells what GameHotkeys watches while the game runs.
    GameHotkeys::Launch prepareLaunch(const Profile& profile, FileData* game, std::string& command);
} // namespace EmulatorProfiles

#endif // ES_APP_EMULATORS_EMULATOR_PROFILES_H
