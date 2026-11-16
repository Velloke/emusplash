//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiFileBrowser.cpp
//
//  Menu for choosing a file, e.g. an AppImage or a script. Folders are opened using A and
//  the parent folder is reached using the ".." entry or back.
//

#include "guis/GuiFileBrowser.h"

#include "Window.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"
#include "views/ViewController.h"

#include <algorithm>

namespace
{
    // The folder of the last chosen file, where browsing starts the next time.
    std::string sLastDirectory;
} // namespace

GuiFileBrowser::GuiFileBrowser(const std::string& title,
                               const std::string& directory,
                               const Callback& callback,
                               bool chooseFolder)
    : mMenu {title}
    , mTitle {title}
    , mCallback {callback}
    , mChooseFolder {chooseFolder}
{
    std::string start {directory};
    if (start.empty())
        start = sLastDirectory;
    if (start.empty() || !Utils::FileSystem::isDirectory(start))
        start = Utils::FileSystem::getHomePath();
    mDirectory = start;

    addChild(&mMenu);
    mMenu.setSubtitle(mDirectory);

    if (mChooseFolder)
        addEntry("USE THIS FOLDER", false, [this] { mPendingFile = mDirectory; });

    if (mDirectory != "/") {
        const std::string parent {Utils::FileSystem::getParent(mDirectory)};
        addEntry("..", true, [this, parent] { mPendingDirectory = parent.empty() ? "/" : parent; });
    }

    std::vector<std::string> folders;
    std::vector<std::string> files;
    for (const auto& path : Utils::FileSystem::getDirContent(mDirectory)) {
        const std::string name {Utils::FileSystem::getFileName(path)};
        if (name.empty() || name.front() == '.')
            continue;
        if (Utils::FileSystem::isDirectory(path))
            folders.emplace_back(path);
        else if (!mChooseFolder &&
                 (Utils::FileSystem::isRegularFile(path) || Utils::FileSystem::isSymlink(path)))
            files.emplace_back(path);
    }
    auto byName = [](const std::string& a, const std::string& b) {
        return Utils::String::toLower(Utils::FileSystem::getFileName(a)) <
               Utils::String::toLower(Utils::FileSystem::getFileName(b));
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    for (const auto& path : folders) {
        addEntry(ViewController::FOLDER_CHAR + "  " + Utils::FileSystem::getFileName(path), true,
                 [this, path] { mPendingDirectory = path; });
    }
    for (const auto& path : files) {
        addEntry(Utils::FileSystem::getFileName(path), false,
                 [this, path] { mPendingFile = path; });
    }
    if (folders.empty() && files.empty() && !mChooseFolder) {
        ComponentListRow row;
        row.addElement(std::make_shared<TextComponent>("THIS FOLDER IS EMPTY",
                                                       Font::get(FONT_SIZE_MEDIUM),
                                                       mMenuColorPrimary, ALIGN_CENTER),
                       true);
        mMenu.addRow(row);
    }

    mMenu.addButton("BACK", "back", [this] { mPendingDirectory = "\x01"; });
    setSize(mMenu.getSize());
    setPosition((Renderer::getScreenWidth() - mSize.x) / 2.0f, Renderer::getScreenHeight() * 0.13f);
    // Start on the first entry below "..", the folder itself is chosen using the first entry.
    if (!mChooseFolder && mDirectory != "/" && (!folders.empty() || !files.empty()))
        mMenu.getList()->moveCursor(1);
}

void GuiFileBrowser::addEntry(const std::string& label,
                              bool folder,
                              const std::function<void()>& func)
{
    ComponentListRow row;
    row.addElement(std::make_shared<TextComponent>(label, Font::get(FONT_SIZE_MEDIUM),
                                                   mMenuColorPrimary),
                   true);
    if (folder)
        row.addElement(mMenu.makeArrow(), false);
    row.makeAcceptInputHandler(func);
    mMenu.addRow(row);
}

bool GuiFileBrowser::input(InputConfig* config, Input input)
{
    if (input.value != 0 && config->isMappedTo("b", input)) {
        // Back closes the browser.
        mPendingDirectory = "\x01";
        return true;
    }
    return mMenu.input(config, input);
}

void GuiFileBrowser::update(int deltaTime)
{
    GuiComponent::update(deltaTime);
    if (!mPendingFile.empty()) {
        sLastDirectory = mDirectory;
        const std::string file {mPendingFile};
        const Callback callback {mCallback};
        delete this;
        if (callback)
            callback(file);
        return;
    }
    if (!mPendingDirectory.empty()) {
        const std::string directory {mPendingDirectory};
        Window* window {mWindow};
        if (directory != "\x01")
            window->pushGui(new GuiFileBrowser(mTitle, directory, mCallback, mChooseFolder));
        delete this;
    }
}

std::vector<HelpPrompt> GuiFileBrowser::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts {mMenu.getHelpPrompts()};
    prompts.push_back(HelpPrompt("a", "select"));
    prompts.push_back(HelpPrompt("b", "back"));
    return prompts;
}
