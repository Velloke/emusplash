//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiGameSearch.cpp
//
//  Game search across all systems. The search text is entered using the
//  on-screen keyboard, and once at least three characters have been typed a
//  live preview of the matching games is shown above the keyboard. After
//  accepting, the results are listed and selecting a game jumps to it in
//  its gamelist.
//

#include "guis/GuiGameSearch.h"

#include "FileData.h"
#include "SystemData.h"
#include "Window.h"
#include "components/BackgroundComponent.h"
#include "components/TextComponent.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "utils/StringUtil.h"
#include "views/GamelistView.h"
#include "views/ViewController.h"

#include <algorithm>

#define MAX_RESULT_ROWS 200

// The keyboard is compact and takes the lower part of the screen, same as the keyboard of the
// media center search.
#define SEARCH_KEYBOARD_FRACTION 0.42f

namespace
{
    std::vector<std::string> splitWords(const std::string& text)
    {
        std::vector<std::string> words;
        std::string word;
        for (const char c : text) {
            if (c == ' ' || c == '\t') {
                if (!word.empty())
                    words.emplace_back(word);
                word.clear();
            }
            else {
                word.push_back(c);
            }
        }
        if (!word.empty())
            words.emplace_back(word);
        return words;
    }

    std::string shorten(const std::string& text, size_t maxLength)
    {
        if (Utils::String::unicodeLength(text) <= maxLength)
            return text;
        return text.substr(0, Utils::String::moveCursor(text, 0, static_cast<int>(maxLength) - 3)) +
               "...";
    }
} // namespace

// Live result preview which is shown inside the on-screen keyboard popup, above its title.
// The popup takes care of the background, position and size.
class GameSearchPreview : public GuiComponent
{
public:
    GameSearchPreview()
    {
        mText = std::make_shared<TextComponent>("", Font::get(FONT_SIZE_SMALL),
                                                mMenuColorPrimary, ALIGN_LEFT, ALIGN_TOP,
                                                glm::ivec2 {0, 0});
        mText->setLineSpacing(1.3f);
        addChild(&mFrame);
        addChild(mText.get());
    }

    // Vertical position of the keyboard, placed at the bottom just above the help prompts.
    static float keyboardPosition()
    {
        const float screenHeight {static_cast<float>(Renderer::getScreenHeight())};
        return screenHeight * 0.965f - screenHeight * SEARCH_KEYBOARD_FRACTION;
    }

    // Height of the preview area between the top of the screen and the keyboard.
    static float previewHeight()
    {
        return keyboardPosition() - static_cast<float>(Renderer::getScreenHeight()) * 0.025f;
    }

    void onSizeChanged() override
    {
        mPanelWidth = mSize.x * 0.6f;
        const float lineHeight {Font::get(FONT_SIZE_SMALL)->getHeight(1.3f)};
        mMaxLines = std::max(2, static_cast<int>((mSize.y - mPadding * 2.0f) / lineHeight));
        // Rough number of characters fitting on one line.
        mMaxChars = std::max(
            20, static_cast<int>((mPanelWidth - mPadding * 2.0f) /
                                 (Font::get(FONT_SIZE_SMALL)->getLetterHeight() * 0.95f)));
        setQuery(mLastQuery);
    }

    void setQuery(const std::string& query)
    {
        mLastQuery = query;
        const std::string trimmed {Utils::String::trim(query)};
        if (Utils::String::unicodeLength(trimmed) < GuiGameSearch::MIN_SEARCH_LENGTH) {
            showText("TYPE AT LEAST " + std::to_string(GuiGameSearch::MIN_SEARCH_LENGTH) +
                     " CHARACTERS TO SEARCH");
            return;
        }

        const std::vector<FileData*> results {GuiGameSearch::searchGames(trimmed)};
        if (results.empty()) {
            showText("NO GAMES FOUND FOR '" + trimmed + "'");
            return;
        }

        std::string text {std::to_string(results.size()) +
                          (results.size() == 1 ? " GAME FOUND" : " GAMES FOUND")};
        const size_t maxGames {static_cast<size_t>(mMaxLines - 1)};
        for (size_t i {0}; i < results.size() && i < maxGames; ++i) {
            if (i == maxGames - 1 && results.size() > maxGames) {
                text.append("\n... AND " + std::to_string(results.size() - i) + " MORE");
                break;
            }
            text.append("\n" + shorten(GuiGameSearch::resultLabel(results[i]),
                                       static_cast<size_t>(mMaxChars)));
        }
        showText(text);
    }

private:
    // Shows the text in a panel which is only as high as the text, so that the rest of the
    // area stays free.
    void showText(const std::string& text)
    {
        const int lines {1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'))};
        const float height {std::min(mSize.y, lines * Font::get(FONT_SIZE_SMALL)->getHeight(1.3f) +
                                                  mPadding * 2.0f)};
        mFrame.fitTo(glm::vec2 {mPanelWidth, height});
        mText->setPosition(mPadding, mPadding);
        mText->setSize(mPanelWidth - mPadding * 2.0f, height - mPadding * 2.0f);
        mText->setText(text);
    }

    BackgroundComponent mFrame;
    float mPanelWidth {0.0f};
    float mPadding {10.0f};
    std::shared_ptr<TextComponent> mText;
    std::string mLastQuery;
    int mMaxLines {2};
    int mMaxChars {40};
};

GuiGameSearch::GuiGameSearch()
{
    setSize(static_cast<float>(Renderer::getScreenWidth()),
            static_cast<float>(Renderer::getScreenHeight()));
    mPreview = std::make_unique<GameSearchPreview>();
    buildMenu();
}

GuiGameSearch::~GuiGameSearch()
{
    if (mMenu)
        removeChild(mMenu.get());
}

void GuiGameSearch::openKeyboard()
{
    auto keyboard = new GuiTextEditKeyboardPopup(
        GameSearchPreview::keyboardPosition(), "SEARCH", mQuery,
        [this](const std::string& text) {
            mQuery = Utils::String::trim(text);
            buildMenu();
        },
        false, "SEARCH", "");
    keyboard->setCompact(SEARCH_KEYBOARD_FRACTION);
    mPreview->setQuery(mQuery);
    keyboard->setTextChangedCallback(
        [this](const std::string& text) { mPreview->setQuery(text); });
    keyboard->setPreviewComponent(mPreview.get(), GameSearchPreview::previewHeight(), true);
    // Pressing up from the top row of the keyboard goes directly to the results list.
    keyboard->setNavigateUpCallback([this](const std::string& text) {
        const std::string query {Utils::String::trim(text)};
        if (Utils::String::unicodeLength(query) < MIN_SEARCH_LENGTH || searchGames(query).empty())
            return false;
        mQuery = query;
        buildMenu();
        return true;
    });
    mWindow->pushGui(keyboard);
}

void GuiGameSearch::buildMenu()
{
    if (mMenu)
        removeChild(mMenu.get());

    mMenu = std::make_unique<MenuComponent>("SEARCH");
    const unsigned int color {mMenuColorPrimary};

    // First row shows the search text and opens the keyboard.
    ComponentListRow row;
    auto searchText = std::make_shared<TextComponent>(
        mQuery.empty() ? "ENTER SEARCH TEXT" : "SEARCH: " + mQuery, Font::get(FONT_SIZE_MEDIUM),
        color);
    searchText->setSelectable(true);
    row.addElement(searchText, true);
    row.addElement(mMenu->makeArrow(), false);
    row.makeAcceptInputHandler([this] { openKeyboard(); });
    mMenu->addRow(row);

    if (!mQuery.empty()) {
        if (Utils::String::unicodeLength(mQuery) < MIN_SEARCH_LENGTH) {
            mMenu->setSubtitle("TYPE AT LEAST " + std::to_string(MIN_SEARCH_LENGTH) +
                               " CHARACTERS");
        }
        else {
            const std::vector<FileData*> results {searchGames(mQuery)};
            if (results.empty()) {
                mMenu->setSubtitle("NO GAMES FOUND");
            }
            else {
                std::string subtitle {std::to_string(results.size()) +
                                      (results.size() == 1 ? " GAME FOUND" : " GAMES FOUND")};
                if (results.size() > MAX_RESULT_ROWS)
                    subtitle.append(", SHOWING FIRST " + std::to_string(MAX_RESULT_ROWS));
                mMenu->setSubtitle(subtitle);

                const size_t rowCount {
                    std::min(results.size(), static_cast<size_t>(MAX_RESULT_ROWS))};
                for (size_t i {0}; i < rowCount; ++i) {
                    FileData* game {results[i]};
                    ComponentListRow resultRow;
                    auto text = std::make_shared<TextComponent>(
                        resultLabel(game), Font::get(FONT_SIZE_MEDIUM), color);
                    text->setSelectable(true);
                    resultRow.addElement(text, true);
                    resultRow.makeAcceptInputHandler([game] { goToGame(game); });
                    // Place the cursor on the first result and only recalculate the menu
                    // size once, after the last row.
                    mMenu->addRow(resultRow, i == 0, i + 1 == rowCount);
                }
            }
        }
    }

    addChild(mMenu.get());
    mMenu->setPosition((mSize.x - mMenu->getSize().x) / 2.0f,
                       Renderer::getScreenHeight() * 0.13f);

    if (mWindow->peekGui() == this)
        updateHelpPrompts();
}

bool GuiGameSearch::input(InputConfig* config, Input input)
{
    if (input.value != 0 && (config->isMappedTo("b", input) || config->isMappedTo("back", input))) {
        delete this;
        return true;
    }
    if (input.value != 0 && config->isMappedTo("y", input)) {
        openKeyboard();
        return true;
    }
    return GuiComponent::input(config, input);
}

std::vector<HelpPrompt> GuiGameSearch::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts {mMenu->getHelpPrompts()};
    prompts.push_back(HelpPrompt("a", "select"));
    prompts.push_back(HelpPrompt("y", "new search"));
    prompts.push_back(HelpPrompt("b", "back"));
    return prompts;
}

std::vector<FileData*> GuiGameSearch::searchGames(const std::string& query)
{
    std::vector<FileData*> results;
    const std::vector<std::string> words {splitWords(Utils::String::toLower(query))};
    if (words.empty())
        return results;

    for (SystemData* system : SystemData::sSystemVector) {
        if (system->isCollection())
            continue;
        // Only games that are currently shown, i.e. respecting hidden games, kid mode and
        // gamelist filters.
        for (FileData* game : system->getRootFolder()->getFilesRecursive(GAME, true)) {
            const std::string name {Utils::String::toLower(game->getName())};
            bool match {true};
            for (const auto& word : words) {
                if (name.find(word) == std::string::npos) {
                    match = false;
                    break;
                }
            }
            if (match)
                results.emplace_back(game);
        }
    }

    std::sort(results.begin(), results.end(), [](FileData* a, FileData* b) {
        const std::string nameA {Utils::String::toLower(a->getName())};
        const std::string nameB {Utils::String::toLower(b->getName())};
        if (nameA != nameB)
            return nameA < nameB;
        return a->getSystem()->getName() < b->getSystem()->getName();
    });
    return results;
}

std::string GuiGameSearch::resultLabel(FileData* game)
{
    return game->getName() + "  [" + Utils::String::toUpper(game->getSystem()->getName()) + "]";
}

void GuiGameSearch::goToGame(FileData* game)
{
    // Close the search and the main menu.
    Window* window {Window::getInstance()};
    while (window->peekGui() != ViewController::getInstance())
        delete window->peekGui();

    // If the game is inside a folder link, then select the folder instead.
    FileData* selectGame {game};
    for (FileData* entry {game->getParent()}; entry != nullptr; entry = entry->getParent()) {
        if (entry->metadata.get("folderlink") != "")
            selectGame = entry;
    }

    ViewController::getInstance()->goToGamelist(game->getSystem());
    GamelistView* view {ViewController::getInstance()->getGamelistView(game->getSystem()).get()};
    view->setCursor(selectGame);
    view->stopListScrolling();
    ViewController::getInstance()->cancelViewTransitions();
}
