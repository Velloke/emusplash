//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiEmulatorSettings.cpp
//
//  The menus of the settings of the emulators, of a game and the hotkeys.
//

#include "guis/GuiEmulatorSettings.h"

#include "CollectionSystemsManager.h"
#include "FileData.h"
#include "Settings.h"
#include "SystemData.h"
#include "Window.h"
#include "components/MenuComponent.h"
#include "components/OptionListComponent.h"
#include "components/ScrollableContainer.h"
#include "components/SwitchComponent.h"
#include "components/TextComponent.h"
#include "emulators/EmulatorProfiles.h"
#include "emulators/GameHotkeys.h"
#include "guis/GuiFileBrowser.h"
#include "guis/GuiMsgBox.h"
#include "guis/GuiSettings.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"
#include "views/ViewController.h"

#include <memory>
#include <vector>

namespace GuiEmulatorSettings
{
    namespace
    {
        using EmulatorProfiles::Profile;
        using EmulatorProfiles::Setting;
        using OptionList = OptionListComponent<std::string>;

        // The color of the menu texts, which only components may read.
        struct MenuColor : public GuiComponent {
            static unsigned int primary() { return mMenuColorPrimary; }
        };

        // The row of a setting with the value in the file selected. autoLabel is the first
        // entry, which stands for no value of its own (AUTO); without it a setting that isn't
        // in the file is "EMULATOR DEFAULT". A value the menu doesn't know is shown as it is,
        // so it isn't lost.
        std::shared_ptr<OptionList> makeRow(const Setting& setting,
                                            const std::string& current,
                                            const std::string& autoLabel)
        {
            auto row = std::make_shared<OptionList>(setting.label, false);
            bool found {false};
            if (!autoLabel.empty()) {
                row->add(autoLabel, "", current.empty());
                found = current.empty();
            }
            else if (current.empty()) {
                // Not in the file: the emulator uses its own default, which stays so unless
                // another value is chosen.
                row->add("EMULATOR DEFAULT", "", true);
                found = true;
            }
            for (const auto& choice : setting.choices) {
                const bool selected {!found && choice.value == current};
                row->add(choice.label, choice.value, selected);
                found = found || selected;
            }
            if (!found && !current.empty())
                row->add(Utils::String::toUpper(current) + " (CURRENT)", current, true);
            if (row->getSelectedObjects().empty())
                row->selectEntry(0);
            return row;
        }

        std::string autoLabel(const Setting& setting, const std::string& inherited,
                              const std::string& fallback)
        {
            if (inherited.empty())
                return fallback;
            return "AUTO (" + EmulatorProfiles::labelOf(setting, inherited) + ")";
        }

        // A text in a menu row that scrolls by itself while the row is selected, as many lines
        // as the row has room for at a time.
        class ScrollingText : public ScrollableContainer
        {
        public:
            ScrollingText(const std::string& text, float height)
                : mText {std::make_shared<TextComponent>(text,
                                                         Font::get(FONT_SIZE_SMALL * 0.75f),
                                                         MenuColor::primary(), ALIGN_LEFT,
                                                         ALIGN_TOP)}
            {
                mText->setLineSpacing(1.1f);
                addChild(mText.get());
                setScrollParameters(2500.0f, 4000.0f, 1.0f);
                setAutoScroll(true);
                setSize(0.0f, height);
            }

            ~ScrollingText() override { removeChild(mText.get()); }

            void onSizeChanged() override
            {
                mText->setSize(mSize.x, 0.0f);
                resetComponent();
            }

        private:
            std::shared_ptr<TextComponent> mText;
        };

        void showMessage(const std::string& text)
        {
            Window::getInstance()->pushGui(new GuiMsgBox(text));
        }

        void addInfo(GuiSettings* menu, const std::string& text, float size)
        {
            ComponentListRow info;
            info.addElement(std::make_shared<TextComponent>(text, Font::get(size),
                                                            MenuColor::primary()),
                            true);
            menu->addRow(info, false);
        }

        // The settings of the ROM directories, in their order.
        const std::vector<std::string> ROM_DIRECTORY_SETTINGS {"ROMDirectory", "ROMDirectory2",
                                                                "ROMDirectory3"};

        std::string shortPath(const std::string& path)
        {
            if (path.size() > 34)
                return "..." + path.substr(path.size() - 31);
            return path;
        }

        // Closes the menus and loads the games of all ROM directories again.
        void reloadGames(const std::string& message)
        {
            Window* window {Window::getInstance()};
            window->pushGui(new GuiMsgBox(
                message, "OK",
                [window] {
                    if (CollectionSystemsManager::getInstance()->isEditing())
                        CollectionSystemsManager::getInstance()->exitEditMode();
                    window->stopInfoPopup();
                    while (window->peekGui() != ViewController::getInstance())
                        delete window->peekGui();
                    // Changes of the gamelists are written before the games are loaded again.
                    if (Settings::getInstance()->getString("SaveGamelistsMode") == "on exit") {
                        for (SystemData* system : SystemData::sSystemVector)
                            system->writeMetaData();
                    }
                    ViewController::getInstance()->rescanROMDirectory();
                },
                "", nullptr, "", nullptr, "", nullptr, nullptr, true));
        }

        // Sets a ROM directory and makes the folders of the systems in it, as the first time.
        void setRomDirectory(const std::string& setting, const std::string& directory)
        {
            Settings::getInstance()->setString(setting, directory);
            Settings::getInstance()->saveFile();
            if (directory.empty()) {
                reloadGames("THE ROMS DIRECTORY WAS REMOVED, THE GAMES ARE LOADED AGAIN");
                return;
            }
            std::string withSeparator {directory};
            if (withSeparator.back() != '/')
                withSeparator += "/";
            if (SystemData::createSystemDirectories(withSeparator)) {
                showMessage("THE FOLDERS OF THE SYSTEMS COULDN'T BE MADE IN '" +
                            Utils::String::toUpper(directory) +
                            "', PERMISSION PROBLEMS OR DISK FULL? SEE THE LOG FILE");
                return;
            }
            reloadGames("THE FOLDERS OF THE SYSTEMS ARE IN '" + Utils::String::toUpper(directory) +
                        "', THE GAMES ARE LOADED AGAIN");
        }

        // The browser for a ROM directory, starting in the current one.
        void chooseRomDirectory(const std::string& setting, const std::string& title)
        {
            std::string current {Settings::getInstance()->getString(setting)};
            if (current.empty() && setting == "ROMDirectory")
                current = FileData::getROMDirectory();
            Window::getInstance()->pushGui(new GuiFileBrowser(
                title, Utils::FileSystem::expandHomePath(current),
                [setting](const std::string& path) {
                    // The same directory twice would load its games twice.
                    for (const std::string& other : ROM_DIRECTORY_SETTINGS) {
                        std::string value {Settings::getInstance()->getString(other)};
                        if (other == "ROMDirectory" && value.empty())
                            value = FileData::getROMDirectory();
                        value = Utils::FileSystem::expandHomePath(value);
                        while (value.size() > 1 && value.back() == '/')
                            value.pop_back();
                        if (other != setting && value == path) {
                            showMessage("'" + Utils::String::toUpper(path) +
                                        "' IS ALREADY A ROMS DIRECTORY");
                            return;
                        }
                    }
                    setRomDirectory(setting, path);
                },
                true));
        }

        void openEmulator(const Profile* profile)
        {
            auto menu = new GuiSettings(Utils::String::toUpper(profile->name));
            const std::vector<std::string> values {EmulatorProfiles::readValues(*profile)};
            const std::vector<std::string> base {EmulatorProfiles::readBaseValues(*profile)};
            auto rows = std::make_shared<std::vector<std::shared_ptr<OptionList>>>();
            auto indexes = std::make_shared<std::vector<size_t>>();
            if (profile->ownSettingsFile)
                addInfo(menu, "AUTO KEEPS THE SETTING OF RETROARCH.CFG", FONT_SIZE_SMALL * 0.75f);
            for (size_t index {0}; index < profile->settings.size(); ++index) {
                const Setting& setting {profile->settings[index]};
                if (setting.key.empty())
                    continue;
                const std::string firstEntry {
                    profile->ownSettingsFile ? autoLabel(setting, base[index], "AUTO") : ""};
                rows->emplace_back(makeRow(setting, values[index], firstEntry));
                indexes->emplace_back(index);
                menu->addWithLabel(setting.label, rows->back());
            }
            menu->addSaveFunc([profile, rows, indexes, values] {
                std::vector<std::pair<std::string, std::string>> changed;
                for (size_t row {0}; row < rows->size(); ++row) {
                    const size_t index {(*indexes)[row]};
                    const std::string selected {(*rows)[row]->getSelected()};
                    if (selected != values[index])
                        changed.emplace_back(profile->settings[index].key, selected);
                }
                std::string error;
                if (!changed.empty() &&
                    !EmulatorProfiles::writeValues(*profile, changed, error))
                    showMessage(Utils::String::toUpper(error));
            });
            Window::getInstance()->pushGui(menu);
        }
    } // namespace

    void openEmulators()
    {
        auto menu = new GuiSettings("EMULATOR SETTINGS");
        bool any {false};
        for (const Profile& profile : EmulatorProfiles::getProfiles()) {
            if (EmulatorProfiles::getConfigFile(profile).empty())
                continue;
            any = true;
            const Profile* chosen {&profile};
            ComponentListRow row;
            row.addElement(std::make_shared<TextComponent>(Utils::String::toUpper(profile.name),
                                                           Font::get(FONT_SIZE_MEDIUM),
                                                           MenuColor::primary()),
                           true);
            row.makeAcceptInputHandler([chosen] { openEmulator(chosen); });
            menu->addRow(row);
        }
        if (!any)
            addInfo(menu, "NO EMULATOR SETTINGS FOUND, START AN EMULATOR ONCE", FONT_SIZE_SMALL);
        Window::getInstance()->pushGui(menu);
    }

    bool hasGameSettings(FileData* game)
    {
        return EmulatorProfiles::findForGame(game) != nullptr;
    }

    void openGame(FileData* game)
    {
        const Profile* profile {EmulatorProfiles::findForGame(game)};
        if (profile == nullptr) {
            showMessage("THE EMULATOR OF THIS GAME HAS NO SETTINGS IN EMUSPLASH");
            return;
        }
        std::string reason;
        const std::string gameFile {EmulatorProfiles::getGameFile(*profile, game, reason)};
        std::vector<size_t> indexes;
        for (size_t index {0}; index < profile->settings.size(); ++index) {
            const Setting& setting {profile->settings[index]};
            if (setting.perGame && !EmulatorProfiles::gameKeyOf(setting).empty())
                indexes.emplace_back(index);
        }
        if (gameFile.empty() || indexes.empty()) {
            showMessage(reason.empty() ? std::string("THIS GAME CAN'T HAVE ITS OWN SETTINGS") :
                                         reason);
            return;
        }
        auto menu = new GuiSettings("ADVANCED GAME OPTIONS");
        addInfo(menu,
                Utils::String::toUpper(profile->name + " - " + game->metadata.get("name")),
                FONT_SIZE_SMALL);
        if (profile->gameFiles == EmulatorProfiles::GameFiles::CEMU_PROFILE)
            addInfo(menu, "WITH THESE CEMU DOESN'T USE ITS OWN PROFILE OF THE GAME",
                    FONT_SIZE_SMALL * 0.75f);
        const std::vector<std::string> values {
            EmulatorProfiles::readGameValues(*profile, gameFile)};
        // What AUTO is: the setting of the emulator (for RetroArch the one of EmuSPLASH, else
        // retroarch.cfg).
        std::vector<std::string> inherited {EmulatorProfiles::readValues(*profile)};
        const std::vector<std::string> base {EmulatorProfiles::readBaseValues(*profile)};
        for (size_t index {0}; index < inherited.size(); ++index) {
            if (inherited[index].empty())
                inherited[index] = base[index];
        }
        auto rows = std::make_shared<std::vector<std::shared_ptr<OptionList>>>();
        std::vector<std::string> shown;
        for (const size_t index : indexes) {
            const Setting& setting {profile->settings[index]};
            const std::string inheritedValue {setting.key.empty() ? "" : inherited[index]};
            rows->emplace_back(
                makeRow(setting, values[index], autoLabel(setting, inheritedValue,
                                                          "AUTO (EMULATOR)")));
            shown.emplace_back(values[index]);
            menu->addWithLabel(setting.label, rows->back());
        }
        menu->addSaveFunc([profile, rows, shown, indexes, gameFile] {
            std::vector<std::pair<std::string, std::string>> changed;
            for (size_t row {0}; row < rows->size(); ++row) {
                const std::string selected {(*rows)[row]->getSelected()};
                if (selected != shown[row])
                    changed.emplace_back(
                        EmulatorProfiles::gameKeyOf(profile->settings[indexes[row]]), selected);
            }
            std::string error;
            if (!changed.empty() &&
                !EmulatorProfiles::writeGameValues(*profile, gameFile, changed, error))
                showMessage(Utils::String::toUpper(error));
        });
        Window::getInstance()->pushGui(menu);
    }

    void openHotkeys()
    {
        auto menu = new GuiSettings("HOTKEYS");
        const std::vector<std::pair<std::string, std::string>> actions {
            {"HotkeyCloseGame", "CLOSE GAME"},
            {"HotkeySaveState", "SAVE STATE"},
            {"HotkeyLoadState", "LOAD STATE"},
            {"HotkeyFastForward", "FAST FORWARD"}};
        for (const auto& [setting, label] : actions) {
            auto row = std::make_shared<OptionList>(label, false);
            const std::string current {Settings::getInstance()->getString(setting)};
            bool found {false};
            for (const auto& [value, name] :
                 GameHotkeys::getCombinations(setting == "HotkeyCloseGame")) {
                row->add(name, value, value == current);
                found = found || value == current;
            }
            if (!found)
                row->selectEntry(0);
            menu->addWithLabel(label, row);
            const std::string key {setting};
            menu->addSaveFunc([row, key, menu] {
                if (row->getSelected() != Settings::getInstance()->getString(key)) {
                    Settings::getInstance()->setString(key, row->getSelected());
                    menu->setNeedsSaving();
                }
            });
        }
        auto toEmulators = std::make_shared<SwitchComponent>();
        toEmulators->setState(Settings::getInstance()->getBool("HotkeysToEmulators"));
        menu->addWithLabel("WRITE THE HOTKEYS INTO THE EMULATORS", toEmulators);
        menu->addSaveFunc([toEmulators, menu] {
            if (toEmulators->getState() != Settings::getInstance()->getBool("HotkeysToEmulators")) {
                Settings::getInstance()->setBool("HotkeysToEmulators", toEmulators->getState());
                menu->setNeedsSaving();
            }
        });
        // One text that scrolls by itself while the row is selected.
        ComponentListRow info;
        auto text = std::make_shared<ScrollingText>(
            "CLOSE GAME WORKS WITH EVERY EMULATOR AND GAME, ALSO WITHOUT HOTKEYS OF ITS OWN: "
            "EMUSPLASH ENDS THE GAME. IF IT DOESN'T CLOSE AT ONCE, PRESS THE COMBINATION AGAIN. "
            "THE HOTKEYS ARE WRITTEN INTO PCSX2, PPSSPP, RETROARCH, EDEN, SUYU AND YUZU WHEN A "
            "GAME IS STARTED, RPCS3 GETS SAVE STATE AS ITS KEYBOARD SHORTCUT. RETROARCH HAS ONE "
            "ENABLE BUTTON: SAVE STATE, LOAD STATE AND FAST FORWARD MUST START WITH THE SAME "
            "BUTTON, CLOSE GAME CAN START WITH ANOTHER ONE.",
            menu->getMenu().getList()->getRowHeight());
        info.addElement(text, true);
        menu->addRow(info, false);
        Window::getInstance()->pushGui(menu);
    }

    void openRomDirectories()
    {
        auto menu = new GuiSettings("ROMS DIRECTORIES");
        const std::vector<std::string> labels {"ROMS DIRECTORY", "SECOND ROMS DIRECTORY",
                                               "THIRD ROMS DIRECTORY"};
        bool canAdd {false};
        std::string free;
        for (size_t index {0}; index < ROM_DIRECTORY_SETTINGS.size(); ++index) {
            const std::string setting {ROM_DIRECTORY_SETTINGS[index]};
            std::string directory {Settings::getInstance()->getString(setting)};
            if (index == 0 && directory.empty())
                directory = FileData::getROMDirectory();
            if (directory.empty()) {
                if (free.empty())
                    free = setting;
                canAdd = true;
                continue;
            }
            const std::string label {labels[index]};
            ComponentListRow row;
            row.addElement(std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                           MenuColor::primary()),
                           true);
            row.addElement(std::make_shared<TextComponent>(
                               shortPath(directory), Font::get(FONT_SIZE_SMALL, FONT_PATH_LIGHT),
                               MenuColor::primary(), ALIGN_RIGHT),
                           false);
            if (index == 0) {
                row.makeAcceptInputHandler([setting, label] { chooseRomDirectory(setting, label); });
            }
            else {
                row.makeAcceptInputHandler([setting, label, directory] {
                    Window::getInstance()->pushGui(new GuiMsgBox(
                        Utils::String::toUpper(directory), "CHANGE",
                        [setting, label] { chooseRomDirectory(setting, label); }, "REMOVE",
                        [setting] { setRomDirectory(setting, ""); }, "CANCEL", nullptr));
                });
            }
            menu->addRow(row);
        }
        if (canAdd) {
            ComponentListRow row;
            row.addElement(std::make_shared<TextComponent>("ADD ANOTHER ROMS DIRECTORY",
                                                           Font::get(FONT_SIZE_MEDIUM),
                                                           MenuColor::primary()),
                           true);
            row.makeAcceptInputHandler(
                [free] { chooseRomDirectory(free, "ADD ANOTHER ROMS DIRECTORY"); });
            menu->addRow(row);
        }
        addInfo(menu, "THE GAMES OF ALL ROMS DIRECTORIES ARE IN THE SAME SYSTEMS",
                FONT_SIZE_SMALL * 0.75f);
        Window::getInstance()->pushGui(menu);
    }
} // namespace GuiEmulatorSettings
