//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  EmulatorConfig.h
//
//  Emulators added by the user, e.g. an AppImage or a script, which are written to the
//  custom es_systems.xml file. The system is copied there from the bundled file the first
//  time, so its other emulators stay available, and the added emulator becomes the default.
//

#ifndef ES_APP_EMULATOR_CONFIG_H
#define ES_APP_EMULATOR_CONFIG_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class SystemData;

namespace EmulatorConfig
{
    // The launch commands of the system whose emulator can be found, as command and label.
    std::vector<std::pair<std::string, std::string>> getInstalledCommands(SystemData* system);
    bool isInstalled(SystemData* system, const std::string& command);

    // The label of a command, or a name made from its emulator if it has no label.
    std::string displayLabel(const std::string& command, const std::string& label);

    // A launch command for a file, e.g. "/path/to/Suyu.AppImage -f -g %ROM%". The emulator is
    // recognized by the file name, common emulators get arguments that start the game in
    // fullscreen and the others the arguments used in the bundled systems file.
    std::string commandForFile(SystemData* system, const std::string& path);

    // Adds the command as the default emulator of the system, or as a choice after the other
    // emulators, replacing an added emulator with the same label. The change is used at once and stored in the custom file.
    bool addCustomCommand(SystemData* system,
                          const std::string& label,
                          const std::string& command,
                          std::string& error,
                          bool makeDefault = true);
    bool removeCustomCommand(SystemData* system, const std::string& label, std::string& error);
    // Changes the launch command of an added emulator.
    bool updateCustomCommand(SystemData* system,
                             const std::string& label,
                             const std::string& command,
                             std::string& error);
    // Whether the command was added by the user, which can then be removed.
    bool isCustomCommand(SystemData* system, const std::string& label);

    std::string getCustomSystemsFile();

    // Looks for AppImages in the emulator folder chosen by the user, including its
    // subfolders. New ones are added to the systems they are for and that have games, as
    // choices next to the default emulator, and the ones that have been removed from the
    // folder are removed. RetroArch is left out as it needs a core for each system. Returns
    // the notifications to show.
    std::vector<std::string> scanEmulatorDirectory();

    // Forced fullscreen for an added emulator, off by default: the window of the emulator is
    // made fullscreen once it has opened, using wmctrl or xdotool. These only work for X11
    // windows, on Wayland Qt based emulators are therefore started using XWayland.
    bool isForceFullscreen(const std::string& systemName, const std::string& label);
    bool setForceFullscreen(SystemData* system,
                            const std::string& label,
                            bool enabled,
                            std::string& error);
    // "wmctrl" or "xdotool", whichever is installed, or empty.
    std::string getFullscreenTool();
    bool isWaylandSession();

    // Watches for the window of an emulator while it runs and makes it fullscreen.
    class FullscreenWatcher
    {
    public:
        FullscreenWatcher();
        ~FullscreenWatcher();

    private:
        void run(std::vector<std::string> before);

        std::string mTool;
        std::atomic<bool> mStop;
        std::thread mThread;
    };
} // namespace EmulatorConfig

#endif // ES_APP_EMULATOR_CONFIG_H
