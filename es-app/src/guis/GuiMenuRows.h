//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiMenuRows.h
//
//  Rows of the main menu that can be moved, and rows added by the user that run a command,
//  an AppImage or a script. EmuSPLASH waits in the background until the program is closed.
//  The added rows are stored in custom_menu_rows.xml in the settings directory and the
//  order of the rows in the MainMenuRowOrder setting.
//

#ifndef ES_APP_GUIS_GUI_MENU_ROWS_H
#define ES_APP_GUIS_GUI_MENU_ROWS_H

#include "GuiComponent.h"
#include "components/MenuComponent.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class TextComponent;

namespace MenuRows
{
    struct CustomRow {
        std::string id; // E.g. "custom1", used for the order of the rows.
        std::string name;
        std::string type; // "command" or "file".
        std::string command;
        std::string file;
        std::string arguments;
    };

    std::vector<CustomRow> loadCustomRows();
    void saveCustomRows(const std::vector<CustomRow>& rows);
    // Runs the command of the row and waits until it has finished.
    void launch(const CustomRow& row);

    // The rows in the saved order. Rows missing from the saved order, e.g. new ones, keep
    // their place relative to the row before them in the default order.
    std::vector<std::string> applyOrder(const std::vector<std::string>& defaultIds);
    void saveOrder(const std::vector<std::string>& ids);
} // namespace MenuRows

// Moves the rows of the main menu: A picks up a row, up and down move it and A puts it down.
class GuiMenuRowOrder : public GuiComponent
{
public:
    // The rows as id and label, in their current order.
    GuiMenuRowOrder(const std::vector<std::pair<std::string, std::string>>& rows,
                    const std::vector<std::string>& defaultIds,
                    const std::function<void()>& changed);

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void updateTexts();

    MenuComponent mMenu;
    std::vector<std::string> mIds;
    std::vector<std::string> mLabels;
    std::vector<std::string> mDefaultIds;
    std::vector<std::shared_ptr<TextComponent>> mTexts;
    std::function<void()> mChanged;
    int mMoving;
    bool mClose;
    bool mSave;
};

// The rows added by the user, with an entry for adding a new one.
class GuiCustomRows : public GuiComponent
{
public:
    explicit GuiCustomRows(const std::function<void()>& changed);

    bool input(InputConfig* config, Input input) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    MenuComponent mMenu;
    std::function<void()> mChanged;
};

// Adds or edits a row: its name and either a command or a file with arguments.
class GuiCustomRowEditor : public GuiComponent
{
public:
    GuiCustomRowEditor(const MenuRows::CustomRow& row,
                       bool isNew,
                       const std::function<void()>& changed);

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void addField(const std::string& label,
                  const std::shared_ptr<TextComponent>& value,
                  const std::function<void()>& func);
    void editText(const std::string& title,
                  const std::string& value,
                  const std::function<void(const std::string&)>& callback);
    void updateTexts();
    bool save();

    MenuComponent mMenu;
    MenuRows::CustomRow mRow;
    bool mIsNew;
    std::function<void()> mChanged;
    std::shared_ptr<TextComponent> mNameText;
    std::shared_ptr<TextComponent> mTypeText;
    std::shared_ptr<TextComponent> mCommandText;
    std::shared_ptr<TextComponent> mFileText;
    std::shared_ptr<TextComponent> mArgumentsText;
    // Handled in update(), when the menus are rebuilt.
    bool mDone;
    bool mDelete;
};

#endif // ES_APP_GUIS_GUI_MENU_ROWS_H
