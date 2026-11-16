//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiAlternativeEmulators.cpp
//
//  User interface to select between alternative emulators per system
//  based on configuration entries in es_systems.xml.
//

#include "guis/GuiAlternativeEmulators.h"

#include "EmulatorConfig.h"
#include "GamelistFileParser.h"
#include "SystemData.h"
#include "guis/GuiFileBrowser.h"
#include "guis/GuiMsgBox.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "guis/GuiTextEditPopup.h"
#include "utils/LocalizationUtil.h"
#include "views/ViewController.h"

namespace
{
    // A system with a single command doesn't need a label for it, it's then named after
    // its emulator.
    std::string commandLabel(SystemData* system, const std::string& label)
    {
        if (!label.empty())
            return label;
        const auto& commands {system->getSystemEnvData()->mLaunchCommands};
        return commands.empty() ? std::string("DEFAULT") :
                                  EmulatorConfig::displayLabel(commands.front().first, "");
    }

    void editText(Window* window,
                  const std::string& title,
                  const std::string& value,
                  const std::function<void(const std::string&)>& callback)
    {
        if (Settings::getInstance()->getBool("VirtualKeyboard")) {
            window->pushGui(new GuiTextEditKeyboardPopup(Renderer::getScreenHeight() * 0.2f, title,
                                                         value, callback, false, _("OK"),
                                                         _("SAVE CHANGES?")));
        }
        else {
            window->pushGui(
                new GuiTextEditPopup(title, value, callback, false, _("OK"), _("SAVE CHANGES?")));
        }
    }
} // namespace

GuiAlternativeEmulators::GuiAlternativeEmulators()
    : mMenu {_("ALTERNATIVE EMULATORS")}
    , mHasSystems {false}
{
    addChild(&mMenu);
    mMenu.addButton(_("BACK"), _("back"), [this] { delete this; });

    // Horizontal sizes for the system and label entries.
    float systemSizeX {mMenu.getSize().x / 3.27f};

    std::vector<SystemData*> sortedSystems {SystemData::sSystemVector};

    // Sort systems by short name.
    std::sort(std::begin(sortedSystems), std::end(sortedSystems), [](SystemData* a, SystemData* b) {
        return Utils::String::toUpper(a->getName()) < Utils::String::toUpper(b->getName());
    });

    for (auto it = sortedSystems.cbegin(); it != sortedSystems.cend(); ++it) {
        // All systems are included, as an AppImage or a script can be added to any of them.
        if ((*it)->getSystemEnvData()->mLaunchCommands.empty())
            continue;

        ComponentListRow row;

        std::string name {(*it)->getName()};
        std::shared_ptr<TextComponent> systemText {
            std::make_shared<TextComponent>(name, Font::get(FONT_SIZE_MEDIUM), mMenuColorPrimary,
                                            ALIGN_LEFT, ALIGN_CENTER, glm::ivec2 {0, 0})};

        systemText->setSize(systemSizeX, systemText->getSize().y);
        row.addElement(systemText, false);

        std::string configuredLabel {(*it)->getAlternativeEmulator()};
        std::string label;

        if (configuredLabel == "") {
            label = commandLabel(*it, (*it)->getSystemEnvData()->mLaunchCommands.front().second);
        }
        else {
            for (auto command : (*it)->getSystemEnvData()->mLaunchCommands) {
                if (command.second == configuredLabel) {
                    label = command.second;
                    break;
                }
            }
        }

        bool invalidEntry {false};

        if (label.empty()) {
            label = ViewController::EXCLAMATION_CHAR + " " + _("INVALID ENTRY");
            invalidEntry = true;
        }

        std::shared_ptr<TextComponent> labelText;

        if (label == commandLabel(*it, (*it)->getSystemEnvData()->mLaunchCommands.front().second)) {
            labelText = std::make_shared<TextComponent>(
                label, Font::get(FONT_SIZE_MEDIUM, FONT_PATH_LIGHT), mMenuColorPrimary, ALIGN_RIGHT,
                ALIGN_CENTER, glm::ivec2 {0, 0});
        }
        else {
            // Mark any non-default value with bold and add a gear symbol as well.
            labelText = std::make_shared<TextComponent>(
                label + (!invalidEntry ? " " + ViewController::GEAR_CHAR : ""),
                Font::get(FONT_SIZE_MEDIUM, FONT_PATH_BOLD), mMenuColorPrimary, ALIGN_RIGHT,
                ALIGN_CENTER, glm::ivec2 {0, 0});
        }

        // Mark invalid entries with red color.
        if (invalidEntry)
            labelText->setColor(mMenuColorRed);

        mCommandRows[name] = labelText;
        labelText->setSize(mMenu.getSize().x - systemSizeX -
                               20.0f * Renderer::getScreenHeightModifier(),
                           systemText->getSize().y);

        row.addElement(labelText, false);

        SystemData* systemEntry {
            *std::find(SystemData::sSystemVector.cbegin(), SystemData::sSystemVector.cend(), *it)};

        row.makeAcceptInputHandler([this, systemEntry, labelText] {
            if (labelText->getValue() ==
                ViewController::CROSSEDCIRCLE_CHAR + " " + _("CLEARED ENTRY"))
                return;
            selectorWindow(systemEntry);
        });

        mMenu.addRow(row);
        mHasSystems = true;
    }

    // Add a dummy row if no enabled systems have any alternative emulators defined in
    // es_systems.xml.
    if (!mHasSystems) {
        ComponentListRow row;
        std::shared_ptr<TextComponent> systemText {std::make_shared<TextComponent>(
            ViewController::EXCLAMATION_CHAR + " " + _("NO ALTERNATIVE EMULATORS DEFINED"),
            Font::get(FONT_SIZE_MEDIUM), mMenuColorPrimary, ALIGN_CENTER)};
        row.addElement(systemText, true);
        mMenu.addRow(row);
    }

    setSize(mMenu.getSize());
    setPosition((Renderer::getScreenWidth() - mSize.x) / 2.0f, Renderer::getScreenHeight() * 0.13f);
}

void GuiAlternativeEmulators::updateMenu(const std::string& systemName,
                                         const std::string& label,
                                         bool defaultEmulator)
{
    // If another label was selected, then update the menu accordingly.
    if (defaultEmulator) {
        mCommandRows[systemName].get()->setFont(Font::get(FONT_SIZE_MEDIUM, FONT_PATH_LIGHT));
        mCommandRows[systemName].get()->setValue(label);
    }
    else {
        // Mark any non-default value with bold and add a gear symbol as well.
        mCommandRows[systemName].get()->setFont(Font::get(FONT_SIZE_MEDIUM, FONT_PATH_BOLD));
        mCommandRows[systemName].get()->setValue(label + " " + ViewController::GEAR_CHAR);
    }

    mCommandRows[systemName].get()->setColor(mMenuColorPrimary);
}

void GuiAlternativeEmulators::selectorWindow(SystemData* system)
{
    auto s = new GuiSettings(Utils::String::toUpper(system->getFullName()));

    std::string selectedLabel {system->getAlternativeEmulator()};
    std::string label;
    bool hasCustom {false};
    bool hasRows {false};

    for (auto entry : system->getSystemEnvData()->mLaunchCommands) {
        // Only the emulators that are installed are shown, and the selected one.
        const bool installed {EmulatorConfig::isInstalled(system, entry.first)};
        if (!installed && entry.second != selectedLabel && entry.second != "")
            continue;
        const bool custom {entry.second != "" &&
                           EmulatorConfig::isCustomCommand(system, entry.second)};
        hasCustom = hasCustom || custom;
        hasRows = true;

        ComponentListRow row;

        if (entry.second == "" && system->getSystemEnvData()->mLaunchCommands.size() > 1)
            label = ViewController::CROSSEDCIRCLE_CHAR + " " + _("CLEAR INVALID ENTRY");
        else
            label = commandLabel(system, entry.second);

        std::shared_ptr<TextComponent> labelText = std::make_shared<TextComponent>(
            label, Font::get(FONT_SIZE_MEDIUM), mMenuColorPrimary, ALIGN_LEFT);
        labelText->setSelectable(true);

        if (system->getSystemEnvData()->mLaunchCommands.front().second == entry.second)
            labelText->setValue(
                labelText->getValue().append(" [").append(_("DEFAULT")).append("]"));
        if (!installed)
            labelText->setValue(labelText->getValue().append("  (NOT FOUND)"));

        row.addElement(labelText, true);
        auto select = [this, s, system, labelText, entry, selectedLabel] {
            if (entry.second != selectedLabel) {
                if (entry.second == system->getSystemEnvData()->mLaunchCommands.front().second)
                    system->setAlternativeEmulator("");
                else
                    system->setAlternativeEmulator(entry.second);
                GamelistFileParser::updateGamelist(system, true);

                if (entry.second == system->getSystemEnvData()->mLaunchCommands.front().second) {
                    if (system->getSystemEnvData()->mLaunchCommands.front().second == "" &&
                        system->getSystemEnvData()->mLaunchCommands.size() > 1) {
                        updateMenu(system->getName(),
                                   ViewController::CROSSEDCIRCLE_CHAR + " " + _("CLEARED ENTRY"),
                                   true);
                    }
                    else {
                        updateMenu(system->getName(),
                                   commandLabel(system, system->getSystemEnvData()
                                                            ->mLaunchCommands.front()
                                                            .second),
                                   true);
                    }
                }
                else {
                    updateMenu(system->getName(), entry.second, false);
                }
            }
            delete s;
        };
        // X removes an emulator that has been added.
        row.inputHandler = [this, s, system, entry, custom, select](InputConfig* config,
                                                                      Input input) {
            if (input.value == 0)
                return false;
            if (config->isMappedTo("a", input)) {
                select();
                return true;
            }
            if (custom && config->isMappedTo("y", input)) {
                editEmulator(system, entry.second, s);
                return true;
            }
            if (custom && config->isMappedTo("x", input)) {
                const std::string removeLabel {entry.second};
                mWindow->pushGui(new GuiMsgBox(
                    "REMOVE THE EMULATOR '" + Utils::String::toUpper(removeLabel) + "'?", _("YES"),
                    [this, s, system, removeLabel] {
                        std::string error;
                        if (!EmulatorConfig::removeCustomCommand(system, removeLabel, error)) {
                            mWindow->queueInfoPopup(Utils::String::toUpper(error), 4000);
                            return;
                        }
                        const std::string current {system->getAlternativeEmulator()};
                        updateMenu(system->getName(),
                                   current.empty() ?
                                       commandLabel(system, system->getSystemEnvData()
                                                                ->mLaunchCommands.front()
                                                                .second) :
                                       current,
                                   current.empty());
                        delete s;
                    },
                    _("NO"), nullptr));
                return true;
            }
            return false;
        };

        // Select the row that corresponds to the selected label.
        if (selectedLabel == entry.second && selectedLabel != "")
            s->addRow(row, true);
        else
            s->addRow(row, false);
    }

    if (!hasRows) {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>("NO INSTALLED EMULATORS WERE FOUND",
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary, ALIGN_LEFT),
                       true);
        s->addRow(row);
    }

    // An AppImage, a script or a program (bin file) chosen by the user, which becomes the default emulator.
    {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>("ADD APPIMAGE, .SH SCRIPT OR BIN FILE...",
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary, ALIGN_LEFT),
                       true);
        row.makeAcceptInputHandler([this, s, system] {
            Window* window {mWindow};
            window->pushGui(new GuiFileBrowser(
                "CHOOSE APPIMAGE, .SH SCRIPT OR BIN FILE", "", [this, s, system, window](const std::string& path) {
                    editText(
                        window, "LAUNCH COMMAND (%ROM% IS THE GAME)",
                        EmulatorConfig::commandForFile(system, path),
                        [this, s, system, window, path](const std::string& command) {
                            if (Utils::String::trim(command).empty())
                                return;
                            editText(
                                window, "EMULATOR NAME", Utils::FileSystem::getStem(path),
                                [this, s, system, command](const std::string& name) {
                                    const std::string label {Utils::String::trim(name)};
                                    if (label.empty())
                                        return;
                                    std::string error;
                                    if (!EmulatorConfig::addCustomCommand(
                                            system, label, Utils::String::trim(command), error)) {
                                        mWindow->queueInfoPopup(
                                            "COULDN'T ADD THE EMULATOR: " +
                                                Utils::String::toUpper(error),
                                            5000);
                                        return;
                                    }
                                    updateMenu(system->getName(), label, true);
                                    mWindow->queueInfoPopup(
                                        "'" + Utils::String::toUpper(label) +
                                            "' IS NOW THE DEFAULT EMULATOR",
                                        4000);
                                    delete s;
                                });
                        });
                }));
        });
        s->addRow(row);
    }
    if (hasCustom)
        s->getMenu().setSubtitle("Y EDITS AND X REMOVES AN EMULATOR THAT HAS BEEN ADDED");

    // Set a maximum width depending on the aspect ratio of the screen, to make the screen look
    // somewhat coherent regardless of screen type. The 1.778 aspect ratio value is the 16:9
    // reference.
    float aspectValue {1.778f / Renderer::getScreenAspectRatio()};
    float maxWidthModifier {glm::clamp(0.77f * aspectValue, 0.50f,
                                       (Renderer::getIsVerticalOrientation() ? 0.94f : 0.92f))};
    float maxWidth {Renderer::getScreenWidth() * maxWidthModifier};

    // Set the width of the selector window to the menu width, unless the system full name is
    // too large to fit. If so, allow the size to be exceeded up to the maximum size calculated
    // above.
    float systemTextWidth {
        Font::get(FONT_SIZE_LARGE)->sizeText(Utils::String::toUpper(system->getFullName())).x *
        1.15f};

    float width {0.0f};
    float menuWidth {mMenu.getSize().x};

    if (systemTextWidth <= menuWidth)
        width = menuWidth;
    else if (systemTextWidth > maxWidth)
        width = maxWidth;
    else
        width = systemTextWidth;

    s->setMenuSize(glm::vec2 {width, s->getMenuSize().y});

    auto menuSize = s->getMenuSize();
    auto menuPos = s->getMenuPosition();

    s->setMenuPosition(glm::vec3 {(s->getSize().x - menuSize.x) / 2.0f, menuPos.y, menuPos.z});

    // Hack to properly update the window and set the scroll indicators. Why this is required
    // is currently a mystery.
    auto list = s->getMenu().getList();
    list->update(1);
    int cursor {list->getCursorId()};
    list->setCursor(list->getFirst());
    list->moveCursor(cursor);

    mWindow->pushGui(s);
}

void GuiAlternativeEmulators::editEmulator(SystemData* system,
                                           const std::string& label,
                                           GuiSettings* selector)
{
    auto s = new GuiSettings(Utils::String::toUpper(label));
    s->getMenu().setSubtitle(Utils::String::toUpper(system->getFullName()));
    std::string command;
    for (const auto& entry : system->getSystemEnvData()->mLaunchCommands) {
        if (entry.second == label)
            command = entry.first;
    }
    auto shorten = [](const std::string& text) {
        return text.size() > 34 ? "..." + text.substr(text.size() - 31) : text;
    };

    // The launch command, %ROM% is the game.
    auto commandText = std::make_shared<TextComponent>(
        shorten(command), Font::get(FONT_SIZE_SMALL, FONT_PATH_LIGHT), mMenuColorPrimary,
        ALIGN_RIGHT);
    {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>("LAUNCH COMMAND", Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary),
                       true);
        row.addElement(commandText, false);
        row.makeAcceptInputHandler([this, system, label, commandText, shorten] {
            std::string current;
            for (const auto& entry : system->getSystemEnvData()->mLaunchCommands) {
                if (entry.second == label)
                    current = entry.first;
            }
            editText(mWindow, "LAUNCH COMMAND (%ROM% IS THE GAME)", current,
                     [this, system, label, commandText, shorten](const std::string& value) {
                         const std::string newCommand {Utils::String::trim(value)};
                         if (newCommand.empty())
                             return;
                         std::string error;
                         if (!EmulatorConfig::updateCustomCommand(system, label, newCommand,
                                                                  error)) {
                             mWindow->queueInfoPopup(Utils::String::toUpper(error), 4000);
                             return;
                         }
                         commandText->setValue(shorten(newCommand));
                     });
        });
        s->addRow(row);
    }

    // Forced fullscreen, off by default.
    auto fullscreenText = std::make_shared<TextComponent>(
        EmulatorConfig::isForceFullscreen(system->getName(), label) ? "ON" : "OFF",
        Font::get(FONT_SIZE_MEDIUM, FONT_PATH_LIGHT), mMenuColorPrimary, ALIGN_RIGHT);
    {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>("FORCE FULLSCREEN",
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary),
                       true);
        row.addElement(fullscreenText, false);
        row.makeAcceptInputHandler([this, system, label, fullscreenText] {
            const bool enable {!EmulatorConfig::isForceFullscreen(system->getName(), label)};
            std::string error;
            if (!EmulatorConfig::setForceFullscreen(system, label, enable, error)) {
                mWindow->queueInfoPopup(Utils::String::toUpper(error), 4000);
                return;
            }
            fullscreenText->setValue(enable ? "ON" : "OFF");
            if (!enable)
                return;
            // What the setting does and what it needs.
            std::string text {"The window of the emulator is made fullscreen after it has "
                              "opened. Use this if the emulator doesn't start the game in "
                              "fullscreen with its own arguments.\n\n"};
            const std::string tool {EmulatorConfig::getFullscreenTool()};
            if (tool.empty()) {
                text.append("This needs wmctrl or xdotool, neither is installed. Install one "
                            "of them, e.g.:\nsudo apt install wmctrl (Debian, Ubuntu)\n"
                            "sudo dnf install wmctrl (Fedora)\nsudo pacman -S wmctrl (Arch)");
            }
            else {
                text.append("Using " + tool + ".");
            }
            if (EmulatorConfig::isWaylandSession()) {
                text.append("\n\nOn Wayland these tools only work for X11 windows, so Qt "
                            "based emulators are started using XWayland. Emulators that only "
                            "run on Wayland can't be made fullscreen this way.");
            }
            mWindow->pushGui(new GuiMsgBox(text));
        });
        s->addRow(row);
    }
    {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>("REMOVE EMULATOR",
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary),
                       true);
        row.makeAcceptInputHandler([this, s, system, label, selector] {
            mWindow->pushGui(new GuiMsgBox(
                "REMOVE THE EMULATOR '" + Utils::String::toUpper(label) + "'?", _("YES"),
                [this, s, system, label, selector] {
                    std::string error;
                    if (!EmulatorConfig::removeCustomCommand(system, label, error)) {
                        mWindow->queueInfoPopup(Utils::String::toUpper(error), 4000);
                        return;
                    }
                    const std::string current {system->getAlternativeEmulator()};
                    updateMenu(system->getName(),
                               current.empty() ?
                                   commandLabel(system, system->getSystemEnvData()
                                                            ->mLaunchCommands.front()
                                                            .second) :
                                   current,
                               current.empty());
                    // The editor and the selector, which still lists the removed emulator.
                    delete s;
                    delete selector;
                },
                _("NO"), nullptr));
        });
        s->addRow(row);
    }
    mWindow->pushGui(s);
}

void GuiAlternativeEmulators::chooseEmulatorDirectory(const std::function<void()>& done)
{
    Window* window {Window::getInstance()};
    Settings::getInstance()->setBool("EmulatorDirectoryAsked", true);
    Settings::getInstance()->saveFile();
    window->pushGui(new GuiFileBrowser(
        "CUSTOM EMULATORS", Settings::getInstance()->getString("EmulatorDirectory"),
        [window, done](const std::string& path) {
            Settings::getInstance()->setString("EmulatorDirectory", path);
            Settings::getInstance()->saveFile();
            const std::vector<std::string> messages {EmulatorConfig::scanEmulatorDirectory()};
            if (messages.empty())
                window->queueInfoPopup("NO NEW EMULATORS WERE FOUND IN '" + path + "'", 4000);
            else
                window->queueStackedNotifications(messages);
            if (done)
                done();
        },
        true));
}

bool GuiAlternativeEmulators::input(InputConfig* config, Input input)
{
    if (input.value != 0 && (config->isMappedTo("b", input) || config->isMappedTo("back", input))) {
        delete this;
        return true;
    }

    return mMenu.input(config, input);
}

std::vector<HelpPrompt> GuiAlternativeEmulators::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts {mMenu.getHelpPrompts()};
    prompts.push_back(HelpPrompt("b", _("back")));
    if (mHasSystems)
        prompts.push_back(HelpPrompt("a", _("select")));
    return prompts;
}
