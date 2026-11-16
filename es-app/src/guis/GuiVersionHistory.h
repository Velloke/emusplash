//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiVersionHistory.h
//
//  The list of the versions of this build and what was done in each, read from the file
//  versions.txt of the resources. Every patch adds an entry at the top of the file.
//

#ifndef ES_APP_GUIS_GUI_VERSION_HISTORY_H
#define ES_APP_GUIS_GUI_VERSION_HISTORY_H

#include <string>
#include <vector>

class Window;

namespace VersionHistory
{
    struct Version {
        std::string name;
        std::string date;
        std::string title;
        std::vector<std::string> changes;
    };

    // The versions, newest first.
    std::vector<Version> load();

    // Opens the list, selecting a version shows its changes.
    void show(Window* window, unsigned int textColor);
} // namespace VersionHistory

#endif // ES_APP_GUIS_GUI_VERSION_HISTORY_H
