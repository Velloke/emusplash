//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiMenu.h
//
//  Main menu.
//  Some submenus are covered in separate source files.
//

#ifndef ES_APP_GUIS_GUI_MENU_H
#define ES_APP_GUIS_GUI_MENU_H

#include "GuiComponent.h"
#include "components/MenuComponent.h"
#include "guis/GuiSettings.h"
#include "views/ViewController.h"

class GuiMenu : public GuiComponent
{
public:
    GuiMenu();
    ~GuiMenu();

    bool input(InputConfig* config, Input input) override;
    void onSizeChanged() override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void close(bool closeAllWindows);
    void addEntry(const std::string& name,
                  unsigned int color,
                  bool add_arrow,
                  const std::function<void()>& func);
    void addVersionInfo();
    // The rows are collected first and then added in the order chosen by the user.
    void queueEntry(const std::string& id,
                    const std::string& name,
                    bool arrow,
                    const std::function<void()>& func);
    std::function<void()> reopenFunction();

    struct MenuEntry {
        std::string id;
        std::string name;
        bool arrow;
        std::function<void()> func;
    };
    std::vector<MenuEntry> mEntries;
    std::vector<std::string> mDefaultIds;

    void openScraperOptions();
    void openUIOptions();
    void openThemeDownloader(GuiSettings* settings);
    void openSystemStatusOptions();
    void openMediaViewerOptions();
    void openScreensaverOptions();
    void openScreenLaunchOptions();
    void openSoundOptions();
    void openInputDeviceOptions();
    void openConfigInput(GuiSettings* settings);
    void openCollectionSystemOptions();
    void openOtherOptions();
    void openUtilities();
    void openQuitMenu();

    Renderer* mRenderer;
    MenuComponent mMenu;
    TextComponent mVersion;
    int mThemeDownloaderReloadCounter;
};

#endif // ES_APP_GUIS_GUI_MENU_H
