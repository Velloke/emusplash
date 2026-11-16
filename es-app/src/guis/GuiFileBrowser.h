//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiFileBrowser.h
//
//  Menu for choosing a file, e.g. an AppImage or a script. Folders are opened using A and
//  the parent folder is reached using the ".." entry or back.
//

#ifndef ES_APP_GUIS_GUI_FILE_BROWSER_H
#define ES_APP_GUIS_GUI_FILE_BROWSER_H

#include "GuiComponent.h"
#include "components/MenuComponent.h"

#include <functional>
#include <string>

class GuiFileBrowser : public GuiComponent
{
public:
    using Callback = std::function<void(const std::string&)>;

    // Starts in the given folder, or in the folder used last time if it's empty. With
    // chooseFolder set only folders are listed and the folder itself is chosen.
    GuiFileBrowser(const std::string& title,
                   const std::string& directory,
                   const Callback& callback,
                   bool chooseFolder = false);

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void addEntry(const std::string& label, bool folder, const std::function<void()>& func);

    MenuComponent mMenu;
    std::string mTitle;
    std::string mDirectory;
    Callback mCallback;
    bool mChooseFolder;
    // Handled in update(), as the menu can't be replaced while it handles the input.
    std::string mPendingDirectory;
    std::string mPendingFile;
};

#endif // ES_APP_GUIS_GUI_FILE_BROWSER_H
