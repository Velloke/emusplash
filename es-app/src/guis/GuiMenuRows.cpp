//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiMenuRows.cpp
//
//  Rows of the main menu that can be moved, and rows added by the user that run a command,
//  an AppImage or a script. EmuSPLASH waits in the background until the program is closed.
//  The added rows are stored in custom_menu_rows.xml in the settings directory and the
//  order of the rows in the MainMenuRowOrder setting.
//

#include "guis/GuiMenuRows.h"

#include "Log.h"
#include "Settings.h"
#include "Window.h"
#include "components/TextComponent.h"
#include "guis/GuiFileBrowser.h"
#include "guis/GuiMsgBox.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "guis/GuiTextEditPopup.h"
#include "utils/FileSystemUtil.h"
#include "utils/PlatformUtil.h"
#include "utils/StringUtil.h"
#include "views/ViewController.h"

#include <pugixml.hpp>
#include <sys/stat.h>

#include <algorithm>

namespace
{
    std::string customRowsFile()
    {
        return Utils::FileSystem::getAppDataDirectory() + "/settings/custom_menu_rows.xml";
    }

    // Shortened for the menu, the full value is shown when editing.
    std::string shorten(const std::string& text, size_t length)
    {
        if (text.size() <= length)
            return text;
        return text.substr(0, length - 3) + "...";
    }

    std::string quote(const std::string& path)
    {
        return "\"" + Utils::String::replace(path, "\"", "\\\"") + "\"";
    }
} // namespace

namespace MenuRows
{
    std::vector<CustomRow> loadCustomRows()
    {
        std::vector<CustomRow> rows;
        pugi::xml_document doc;
        if (!Utils::FileSystem::exists(customRowsFile()) ||
            !doc.load_file(customRowsFile().c_str()))
            return rows;
        for (pugi::xml_node node {doc.child("menuRows").child("row")}; node;
             node = node.next_sibling("row")) {
            CustomRow row;
            row.id = node.attribute("id").as_string();
            row.name = node.child("name").text().as_string();
            row.type = node.child("type").text().as_string();
            row.command = node.child("command").text().as_string();
            row.file = node.child("file").text().as_string();
            row.arguments = node.child("arguments").text().as_string();
            if (row.id.empty() || row.name.empty())
                continue;
            if (row.type != "file")
                row.type = "command";
            rows.emplace_back(row);
        }
        return rows;
    }

    void saveCustomRows(const std::vector<CustomRow>& rows)
    {
        pugi::xml_document doc;
        pugi::xml_node root {doc.append_child("menuRows")};
        for (const auto& row : rows) {
            pugi::xml_node node {root.append_child("row")};
            node.append_attribute("id") = row.id.c_str();
            node.append_child("name").text() = row.name.c_str();
            node.append_child("type").text() = row.type.c_str();
            if (row.type == "file") {
                node.append_child("file").text() = row.file.c_str();
                node.append_child("arguments").text() = row.arguments.c_str();
            }
            else {
                node.append_child("command").text() = row.command.c_str();
            }
        }
        if (!doc.save_file(customRowsFile().c_str())) {
            LOG(LogError) << "Couldn't write \"" << customRowsFile() << "\"";
        }
    }

    void launch(const CustomRow& row)
    {
        std::string command;
        if (row.type == "file") {
            if (!Utils::FileSystem::isRegularFile(row.file)) {
                Window::getInstance()->queueInfoPopup(
                    "THE FILE '" + row.file + "' DOESN'T EXIST", 5000);
                return;
            }
            // Downloaded AppImages are often not executable yet, scripts are run using bash.
            struct stat info {};
            const bool executable {stat(row.file.c_str(), &info) == 0 && (info.st_mode & S_IXUSR)};
            if (Utils::String::toLower(Utils::FileSystem::getExtension(row.file)) == ".sh" &&
                !executable) {
                command = "bash " + quote(row.file);
            }
            else {
                if (!executable)
                    chmod(row.file.c_str(), info.st_mode | S_IXUSR);
                command = quote(row.file);
            }
            if (!Utils::String::trim(row.arguments).empty())
                command.append(" " + Utils::String::trim(row.arguments));
        }
        else {
            command = Utils::String::trim(row.command);
        }
        if (command.empty())
            return;

        LOG(LogInfo) << "Running the menu row \"" << row.name << "\": " << command;
        Log::flush();
        Window* window {Window::getInstance()};
        ViewController::getInstance()->stopViewVideos();
        const std::string directory {row.type == "file" ?
                                         Utils::FileSystem::getParent(row.file) :
                                         Utils::FileSystem::getHomePath()};
        const int result {Utils::Platform::launchGameUnix(
            command, Utils::FileSystem::getEscapedPath(directory), false)};
        window->normalizeNextUpdate();
        ViewController::getInstance()->startViewVideos();
        if (result != 0) {
            LOG(LogWarning) << "The menu row \"" << row.name << "\" ended with code " << result;
            window->queueInfoPopup("'" + Utils::String::toUpper(row.name) +
                                       "' ENDED WITH ERROR CODE " + std::to_string(result),
                                   5000);
        }
    }

    std::vector<std::string> applyOrder(const std::vector<std::string>& defaultIds)
    {
        std::vector<std::string> result;
        for (const auto& id : Utils::String::delimitedStringToVector(
                 Settings::getInstance()->getString("MainMenuRowOrder"), ",")) {
            if (std::find(defaultIds.cbegin(), defaultIds.cend(), id) != defaultIds.cend() &&
                std::find(result.cbegin(), result.cend(), id) == result.cend())
                result.emplace_back(id);
        }
        for (size_t i {0}; i < defaultIds.size(); ++i) {
            if (std::find(result.cbegin(), result.cend(), defaultIds[i]) != result.cend())
                continue;
            // Placed after the row that is before it in the default order.
            auto position = result.begin();
            for (size_t j {i}; j > 0; --j) {
                const auto previous {std::find(result.begin(), result.end(), defaultIds[j - 1])};
                if (previous != result.end()) {
                    position = previous + 1;
                    break;
                }
            }
            result.insert(position, defaultIds[i]);
        }
        return result;
    }

    void saveOrder(const std::vector<std::string>& ids)
    {
        std::string value;
        for (const auto& id : ids)
            value.append((value.empty() ? "" : ",") + id);
        Settings::getInstance()->setString("MainMenuRowOrder", value);
        Settings::getInstance()->saveFile();
    }
} // namespace MenuRows

//--------------------------------------------------------------------------------------------------
// GuiMenuRowOrder
//--------------------------------------------------------------------------------------------------

GuiMenuRowOrder::GuiMenuRowOrder(const std::vector<std::pair<std::string, std::string>>& rows,
                                 const std::vector<std::string>& defaultIds,
                                 const std::function<void()>& changed)
    : mMenu {"CHANGE MENU ROW POSITION"}
    , mDefaultIds {defaultIds}
    , mChanged {changed}
    , mMoving {-1}
    , mClose {false}
    , mSave {false}
{
    addChild(&mMenu);
    mMenu.setSubtitle("A PICKS UP A ROW, UP AND DOWN MOVE IT, A PUTS IT DOWN");
    for (const auto& [id, label] : rows) {
        mIds.emplace_back(id);
        mLabels.emplace_back(label);
        ComponentListRow row;
        auto text = std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                    mMenuColorPrimary);
        mTexts.emplace_back(text);
        row.addElement(text, true);
        row.makeAcceptInputHandler([this] {
            const int cursor {mMenu.getList()->getCursorId()};
            mMoving = mMoving == cursor ? -1 : cursor;
            updateTexts();
        });
        mMenu.addRow(row);
    }
    mMenu.addButton("SAVE", "save", [this] {
        mSave = true;
        mClose = true;
    });
    mMenu.addButton("DEFAULT ORDER", "default order", [this] {
        // The labels follow the ids in the default order.
        std::vector<std::string> labels;
        std::vector<std::string> ids;
        for (const auto& id : mDefaultIds) {
            const auto it {std::find(mIds.cbegin(), mIds.cend(), id)};
            if (it == mIds.cend())
                continue;
            ids.emplace_back(id);
            labels.emplace_back(mLabels[it - mIds.cbegin()]);
        }
        for (size_t i {0}; i < mIds.size(); ++i) {
            if (std::find(ids.cbegin(), ids.cend(), mIds[i]) == ids.cend()) {
                ids.emplace_back(mIds[i]);
                labels.emplace_back(mLabels[i]);
            }
        }
        mIds = ids;
        mLabels = labels;
        mMoving = -1;
        updateTexts();
    });
    mMenu.addButton("CANCEL", "cancel", [this] { mClose = true; });

    setSize(mMenu.getSize());
    setPosition((Renderer::getScreenWidth() - mSize.x) / 2.0f, Renderer::getScreenHeight() * 0.13f);
}

void GuiMenuRowOrder::updateTexts()
{
    for (size_t i {0}; i < mTexts.size() && i < mLabels.size(); ++i) {
        const bool moving {static_cast<int>(i) == mMoving};
        mTexts[i]->setFont(Font::get(FONT_SIZE_MEDIUM, moving ? FONT_PATH_BOLD : FONT_PATH_REGULAR));
        mTexts[i]->setValue(moving ? Utils::String::wideStringToString(L"") + "  " +
                                         mLabels[i] :
                                     mLabels[i]);
    }
}

bool GuiMenuRowOrder::input(InputConfig* config, Input input)
{
    const bool up {config->isMappedLike("up", input)};
    const bool down {config->isMappedLike("down", input)};
    if (mMoving >= 0 && (up || down)) {
        if (input.value == 0)
            return true;
        const int target {mMoving + (up ? -1 : 1)};
        if (target < 0 || target >= static_cast<int>(mIds.size()))
            return true;
        std::swap(mIds[mMoving], mIds[target]);
        std::swap(mLabels[mMoving], mLabels[target]);
        mMoving = target;
        mMenu.getList()->moveCursor(up ? -1 : 1);
        updateTexts();
        return true;
    }
    if (input.value != 0 && config->isMappedTo("b", input)) {
        if (mMoving >= 0) {
            // Back first puts the row down.
            mMoving = -1;
            updateTexts();
            return true;
        }
        mSave = true;
        mClose = true;
        return true;
    }
    return mMenu.input(config, input);
}

void GuiMenuRowOrder::update(int deltaTime)
{
    GuiComponent::update(deltaTime);
    if (!mClose)
        return;
    if (!mSave) {
        delete this;
        return;
    }
    MenuRows::saveOrder(mIds);
    // The main menu is built again, which also closes this menu.
    const std::function<void()> changed {mChanged};
    changed();
}

std::vector<HelpPrompt> GuiMenuRowOrder::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts;
    prompts.push_back(HelpPrompt("up/down", mMoving >= 0 ? "move" : "choose"));
    prompts.push_back(HelpPrompt("a", mMoving >= 0 ? "put down" : "pick up"));
    prompts.push_back(HelpPrompt("b", mMoving >= 0 ? "put down" : "save"));
    return prompts;
}

//--------------------------------------------------------------------------------------------------
// GuiCustomRows
//--------------------------------------------------------------------------------------------------

GuiCustomRows::GuiCustomRows(const std::function<void()>& changed)
    : mMenu {"CREATE NEW ROW"}
    , mChanged {changed}
{
    addChild(&mMenu);
    mMenu.setSubtitle("ROWS THAT RUN A COMMAND, AN APPIMAGE OR A SCRIPT");

    auto addRow = [this](const std::string& label, const std::string& value,
                         const std::function<void()>& func) {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary),
                       true);
        if (!value.empty()) {
            row.addElement(std::make_shared<TextComponent>(
                               value, Font::get(FONT_SIZE_SMALL, FONT_PATH_LIGHT),
                               mMenuColorPrimary, ALIGN_RIGHT),
                           false);
        }
        row.addElement(mMenu.makeArrow(), false);
        row.makeAcceptInputHandler(func);
        mMenu.addRow(row);
    };

    addRow("ADD NEW ROW", "", [this] {
        MenuRows::CustomRow row;
        row.type = "command";
        mWindow->pushGui(new GuiCustomRowEditor(row, true, mChanged));
    });
    for (const auto& row : MenuRows::loadCustomRows()) {
        const std::string value {row.type == "file" ? Utils::FileSystem::getFileName(row.file) :
                                                      row.command};
        addRow(Utils::String::toUpper(row.name), shorten(value, 34),
               [this, row] { mWindow->pushGui(new GuiCustomRowEditor(row, false, mChanged)); });
    }
    mMenu.addButton("BACK", "back", [this] { delete this; });
    setSize(mMenu.getSize());
    setPosition((Renderer::getScreenWidth() - mSize.x) / 2.0f, Renderer::getScreenHeight() * 0.13f);
}

bool GuiCustomRows::input(InputConfig* config, Input input)
{
    if (input.value != 0 && config->isMappedTo("b", input)) {
        delete this;
        return true;
    }
    return mMenu.input(config, input);
}

std::vector<HelpPrompt> GuiCustomRows::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts {mMenu.getHelpPrompts()};
    prompts.push_back(HelpPrompt("a", "select"));
    prompts.push_back(HelpPrompt("b", "back"));
    return prompts;
}

//--------------------------------------------------------------------------------------------------
// GuiCustomRowEditor
//--------------------------------------------------------------------------------------------------

GuiCustomRowEditor::GuiCustomRowEditor(const MenuRows::CustomRow& row,
                                       bool isNew,
                                       const std::function<void()>& changed)
    : mMenu {isNew ? "NEW ROW" : "EDIT ROW"}
    , mRow {row}
    , mIsNew {isNew}
    , mChanged {changed}
    , mDone {false}
    , mDelete {false}
{
    addChild(&mMenu);
    auto makeValue = [this] {
        return std::make_shared<TextComponent>("", Font::get(FONT_SIZE_MEDIUM, FONT_PATH_LIGHT),
                                               mMenuColorPrimary, ALIGN_RIGHT);
    };
    mNameText = makeValue();
    mTypeText = makeValue();
    mCommandText = makeValue();
    mFileText = makeValue();
    mArgumentsText = makeValue();

    addField("NAME", mNameText, [this] {
        editText("ROW NAME", mRow.name, [this](const std::string& value) {
            mRow.name = Utils::String::trim(value);
            updateTexts();
        });
    });
    addField("TYPE", mTypeText, [this] {
        mRow.type = mRow.type == "file" ? "command" : "file";
        updateTexts();
    });
    addField("COMMAND", mCommandText, [this] {
        mRow.type = "command";
        editText("COMMAND, E.G. FLATPAK RUN ORG.MOZILLA.FIREFOX", mRow.command,
                 [this](const std::string& value) {
                     mRow.command = Utils::String::trim(value);
                     updateTexts();
                 });
    });
    addField("APPIMAGE OR SCRIPT", mFileText, [this] {
        mRow.type = "file";
        updateTexts();
        mWindow->pushGui(new GuiFileBrowser(
            "CHOOSE APPIMAGE OR SCRIPT",
            mRow.file.empty() ? "" : Utils::FileSystem::getParent(mRow.file),
            [this](const std::string& path) {
                mRow.file = path;
                if (mRow.name.empty())
                    mRow.name = Utils::FileSystem::getStem(path);
                updateTexts();
            }));
    });
    addField("ARGUMENTS", mArgumentsText, [this] {
        mRow.type = "file";
        editText("ARGUMENTS FOR THE APPIMAGE OR SCRIPT", mRow.arguments,
                 [this](const std::string& value) {
                     mRow.arguments = Utils::String::trim(value);
                     updateTexts();
                 });
    });

    mMenu.addButton("SAVE", "save", [this] {
        if (save())
            mDone = true;
    });
    if (!mIsNew) {
        mMenu.addButton("DELETE", "delete", [this] {
            mWindow->pushGui(new GuiMsgBox(
                "REALLY DELETE THE ROW '" + Utils::String::toUpper(mRow.name) + "'?",
                "YES",
                [this] {
                    mDelete = true;
                    mDone = true;
                },
                "NO", nullptr));
        });
    }
    mMenu.addButton("CANCEL", "cancel", [this] { delete this; });

    updateTexts();
    setSize(mMenu.getSize());
    setPosition((Renderer::getScreenWidth() - mSize.x) / 2.0f, Renderer::getScreenHeight() * 0.13f);
}

void GuiCustomRowEditor::addField(const std::string& label,
                                  const std::shared_ptr<TextComponent>& value,
                                  const std::function<void()>& func)
{
    ComponentListRow row;
    row.addElement(std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                   mMenuColorPrimary),
                   true);
    row.addElement(value, false);
    row.addElement(mMenu.makeArrow(), false);
    row.makeAcceptInputHandler(func);
    mMenu.addRow(row);
}

void GuiCustomRowEditor::editText(const std::string& title,
                                  const std::string& value,
                                  const std::function<void(const std::string&)>& callback)
{
    if (Settings::getInstance()->getBool("VirtualKeyboard")) {
        mWindow->pushGui(new GuiTextEditKeyboardPopup(mMenu.getPosition().y, title, value,
                                                      callback, false, "OK", "SAVE CHANGES?"));
    }
    else {
        mWindow->pushGui(new GuiTextEditPopup(title, value, callback, false, "OK",
                                              "SAVE CHANGES?"));
    }
}

void GuiCustomRowEditor::updateTexts()
{
    const bool file {mRow.type == "file"};
    auto set = [](const std::shared_ptr<TextComponent>& text, const std::string& value,
                  bool used) {
        text->setValue(value.empty() ? (used ? "NOT SET" : "") : shorten(value, 30));
        text->setOpacity(used ? 1.0f : 0.45f);
    };
    set(mNameText, mRow.name, true);
    mTypeText->setValue(file ? "APPIMAGE OR SCRIPT" : "COMMAND");
    set(mCommandText, mRow.command, !file);
    set(mFileText, mRow.file.empty() ? "" : Utils::FileSystem::getFileName(mRow.file), file);
    set(mArgumentsText, mRow.arguments, file && !mRow.arguments.empty());
    if (file && mRow.arguments.empty())
        mArgumentsText->setValue("NONE");
}

bool GuiCustomRowEditor::save()
{
    if (mRow.name.empty()) {
        mWindow->pushGui(new GuiMsgBox("THE ROW NEEDS A NAME"));
        return false;
    }
    if (mRow.type == "file" ? mRow.file.empty() : mRow.command.empty()) {
        mWindow->pushGui(new GuiMsgBox(mRow.type == "file" ?
                                                           "CHOOSE THE APPIMAGE OR SCRIPT" :
                                                           "THE ROW NEEDS A COMMAND"));
        return false;
    }
    return true;
}

bool GuiCustomRowEditor::input(InputConfig* config, Input input)
{
    if (input.value != 0 && config->isMappedTo("b", input)) {
        delete this;
        return true;
    }
    return mMenu.input(config, input);
}

void GuiCustomRowEditor::update(int deltaTime)
{
    GuiComponent::update(deltaTime);
    if (!mDone)
        return;

    std::vector<MenuRows::CustomRow> rows {MenuRows::loadCustomRows()};
    if (mIsNew) {
        int number {1};
        for (const auto& row : rows) {
            if (Utils::String::startsWith(row.id, "custom"))
                number = std::max(number, std::atoi(row.id.substr(6).c_str()) + 1);
        }
        mRow.id = "custom" + std::to_string(number);
        rows.emplace_back(mRow);
    }
    else {
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            if (it->id != mRow.id)
                continue;
            if (mDelete)
                rows.erase(it);
            else
                *it = mRow;
            break;
        }
    }
    MenuRows::saveCustomRows(rows);
    // The main menu is built again with the new row, which also closes these menus.
    const std::function<void()> changed {mChanged};
    changed();
}

std::vector<HelpPrompt> GuiCustomRowEditor::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts {mMenu.getHelpPrompts()};
    prompts.push_back(HelpPrompt("a", "select"));
    prompts.push_back(HelpPrompt("b", "cancel"));
    return prompts;
}
