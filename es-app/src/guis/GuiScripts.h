//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiScripts.h
//
//  "SKRIPTID" menu which lists the user's fish functions from
//  ~/.config/fish/functions and runs the selected one in the background.
//  A notification popup is shown when the script has finished.
//

#ifndef ES_APP_GUIS_GUI_SCRIPTS_H
#define ES_APP_GUIS_GUI_SCRIPTS_H

#include <string>
#include <vector>

class GuiSettings;

namespace GuiScripts
{
    // Directory that is scanned for *.fish files.
    std::string getScriptsDirectory();

    // Names of the available scripts (file names without the .fish extension).
    std::vector<std::string> getScripts();

    // Runs the script in a background thread and shows a popup when done.
    void runScript(const std::string& name);

    // Creates the menu, which is then pushed by the caller.
    GuiSettings* create(unsigned int textColor);
} // namespace GuiScripts

#endif // ES_APP_GUIS_GUI_SCRIPTS_H
