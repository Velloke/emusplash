//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiGameSearch.h
//
//  Game search across all systems. The search text is entered using the
//  on-screen keyboard, and once at least three characters have been typed a
//  live preview of the matching games is shown above the keyboard. After
//  accepting, the results are listed and selecting a game jumps to it in
//  its gamelist.
//

#ifndef ES_APP_GUIS_GUI_GAME_SEARCH_H
#define ES_APP_GUIS_GUI_GAME_SEARCH_H

#include "GuiComponent.h"
#include "components/MenuComponent.h"

#include <memory>
#include <string>
#include <vector>

class FileData;
class GameSearchPreview;

class GuiGameSearch : public GuiComponent
{
public:
    GuiGameSearch();
    ~GuiGameSearch();

    // Opens the on-screen keyboard. Call after the GUI has been pushed to the window.
    void openKeyboard();

    bool input(InputConfig* config, Input input) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

    // Minimum number of characters before a search is performed.
    static constexpr size_t MIN_SEARCH_LENGTH {3};

    // All games (from all non-collection systems) whose name contains every word of the query,
    // ignoring case. Sorted by name.
    static std::vector<FileData*> searchGames(const std::string& query);
    // Display string such as "Grand Theft Auto - San Andreas (USA)  [PS2]".
    static std::string resultLabel(FileData* game);

private:
    void buildMenu();
    static void goToGame(FileData* game);

    std::string mQuery;
    std::unique_ptr<MenuComponent> mMenu;
    std::unique_ptr<GameSearchPreview> mPreview;
};

#endif // ES_APP_GUIS_GUI_GAME_SEARCH_H
