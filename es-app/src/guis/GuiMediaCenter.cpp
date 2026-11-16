//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiMediaCenter.cpp
//
//  Fullscreen media screen with a black sidebar to the left and the content to the right,
//  on top of the background of the currently selected theme. Video sources are folders
//  added by the user, which are browsed and played using the built-in video player.
//  The Elementum section shows the listings of the streaming add-on, run by AddonHost.
//  Lists can be shown in seven views, chosen in the options panel to the left of the list,
//  which also sorts and filters the list.
//

#include "guis/GuiMediaCenter.h"

#include "Settings.h"
#include "UIModeController.h"
#include "Window.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "guis/GuiTextEditPopup.h"
#include "guis/GuiVideoPlayer.h"
#if defined(EMUSPLASH_DUBX)
#include "dub/GuiDubSettings.h"
#endif
#include "media/HttpClient.h"
#include "media/MediaCoverCache.h"
#include "media/MediaThumbnails.h"
#include "media/SubtitleDownloader.h"
#include "media/Trailer.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"
#include "views/ViewController.h"

#include <algorithm>
#include <regex>
#include <thread>

namespace
{
    // Sidebar sections.
    const std::vector<std::string> SECTIONS {"Media", "Favourites", "Elementum"};
    constexpr int SECTION_MEDIA {0};
    constexpr int SECTION_FAVOURITES {1};
    constexpr int SECTION_ELEMENTUM {2};

    // The play sign of Font Awesome.
    const std::string TRAILER_TAG {"\uf04b"};

    const std::string ICON_FOLDER {":/graphics/media/folder.svg"};
    const std::string ICON_WATCHED {":/graphics/media/watched.svg"};
    const std::string ICON_UNWATCHED {":/graphics/media/unwatched.svg"};

    std::string formatFileSize(long long bytes)
    {
        if (bytes <= 0)
            return "";
        const char* units[] {"B", "KB", "MB", "GB", "TB"};
        double size {static_cast<double>(bytes)};
        int unit {0};
        while (size >= 1024.0 && unit < 4) {
            size /= 1024.0;
            ++unit;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), unit >= 2 ? "%.2f %s" : "%.0f %s", size, units[unit]);
        return buffer;
    }

    // A duration as "1:51:00", or "01:51:00" with the leading zero.
    std::string formatDuration(double seconds, bool leadingZero)
    {
        const long total {static_cast<long>(std::max(0.0, seconds))};
        char buffer[32];
        snprintf(buffer, sizeof(buffer), leadingZero ? "%02ld:%02ld:%02ld" : "%ld:%02ld:%02ld",
                 total / 3600, (total / 60) % 60, total % 60);
        return buffer;
    }

    // Numbers such as ratings are shown with one decimal, e.g. "8.417000" as "8.4".
    std::string formatNumber(const std::string& text)
    {
        if (text.empty() || text.find('.') == std::string::npos)
            return text;
        char* end {nullptr};
        const double value {std::strtod(text.c_str(), &end)};
        if (end == nullptr || *end != '\0')
            return text;
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.1f", value);
        return buffer;
    }

    std::string maskSecret(const std::string& value)
    {
        if (value.empty())
            return "Not set";
        if (value.size() <= 4)
            return "****";
        return value.substr(0, 2) + std::string(std::min<size_t>(value.size() - 4, 12), '*') +
               value.substr(value.size() - 2);
    }

    std::string folderName(const std::string& path)
    {
        const std::string name {Utils::FileSystem::getFileName(path)};
        if (name.empty() || name == "." || name == "/")
            return path;
        return name;
    }

    std::string parentDirectory(const std::string& path)
    {
        std::string parent {Utils::FileSystem::getParent(path)};
        return parent.empty() ? "/" : parent;
    }

    using Choices = std::vector<std::pair<std::string, std::string>>;

    std::string choiceLabel(const Choices& choices, const std::string& value)
    {
        for (const auto& [key, label] : choices) {
            if (key == value)
                return label;
        }
        // Values set from the player, e.g. a language without a name in the list.
        if (!value.empty())
            return Utils::String::toUpper(value);
        return choices.empty() ? "" : choices.front().second;
    }

    std::string cycleChoice(const Choices& choices, const std::string& value, int direction)
    {
        if (choices.empty())
            return value;
        int index {0};
        for (size_t i {0}; i < choices.size(); ++i) {
            if (choices[i].first == value) {
                index = static_cast<int>(i);
                break;
            }
        }
        const int count {static_cast<int>(choices.size())};
        index = ((index + direction) % count + count) % count;
        return choices[index].first;
    }

    const Choices& languageChoices()
    {
        static const Choices choices {
            {"", "Default"},       {"en", "English"},  {"et", "Estonian"},  {"ru", "Russian"},
            {"fi", "Finnish"},     {"de", "German"},   {"fr", "French"},    {"es", "Spanish"},
            {"it", "Italian"},     {"sv", "Swedish"},  {"no", "Norwegian"}, {"da", "Danish"},
            {"pl", "Polish"},      {"lv", "Latvian"},  {"lt", "Lithuanian"},
            {"ja", "Japanese"}};
        return choices;
    }

    // The season and the episode a file of a torrent is named after, -1 where it doesn't say.
    struct EpisodeNumbers {
        int season {-1};
        int episode {-1};
    };

    EpisodeNumbers episodeNumbersOf(const std::string& name)
    {
        EpisodeNumbers numbers;
        static const std::regex seasonEpisode {R"(S(\d{1,2})\s*E(\d{1,3}))",
                                               std::regex::icase};
        static const std::regex absolute {R"((?: - |[ ._]E)(\d{2,3})(?:v\d)?(?:[ ._\[\(-]|$))",
                                          std::regex::icase};
        std::smatch match;
        if (std::regex_search(name, match, seasonEpisode)) {
            numbers.season = std::atoi(match[1].str().c_str());
            numbers.episode = std::atoi(match[2].str().c_str());
        }
        else if (std::regex_search(name, match, absolute)) {
            numbers.episode = std::atoi(match[1].str().c_str());
        }
        return numbers;
    }

    // A movie or a series of Elementum (not an episode or a season), which has a trailer row in
    // its options menu. The TMDb id is 0 if the address has none.
    bool trailerTarget(const AddonHost::Item& item, bool& series, int& tmdbId)
    {
        if (item.path.find("plugin.video.elementum") == std::string::npos)
            return false;
        static const std::regex idPattern {"/(movie|show)/(\\d+)(/|\\?|$)"};
        const auto mediaType {item.info.find("mediatype")};
        const std::string type {mediaType == item.info.cend() ? "" : mediaType->second};
        if (item.path.find("/season/") != std::string::npos || type == "episode" ||
            type == "season")
            return false;
        std::smatch match;
        const bool found {std::regex_search(item.path, match, idPattern)};
        if (!found && type != "movie" && type != "tvshow")
            return false;
        series = found ? match[1] == "show" : type == "tvshow";
        tmdbId = found ? std::atoi(match[2].str().c_str()) : 0;
        return true;
    }
} // namespace

struct GuiMediaCenter::BrowseState {
    std::vector<std::string> paths;
    std::vector<int> cursors;
    std::function<void(const std::string&)> callback;
};

// An installation or update running in the background.
struct GuiMediaCenter::InstallTask {
    std::string title;
    std::mutex mutex;
    std::string message;
    int percent {-1};
    std::atomic<bool> cancel {false};
    std::atomic<bool> done {false};
    AddonStore::Result result;
    std::function<void(const AddonStore::Result&)> doneCallback;

    void progress(const std::string& text, int value)
    {
        std::unique_lock<std::mutex> lock {mutex};
        message = text;
        percent = value;
    }
};

GuiMediaCenter::GuiMediaCenter()
    : mFocus {Focus::CONTENT}
    , mSidebarCursor {0}
    , mTopButton {TOP_NONE}
    , mAddButtonShown {false}
    , mPreviewTimer {0}
    , mAddonRequestKind {AddonRequestKind::LOAD}
    , mAddonRequestCursor {0}
    , mLastHostState {AddonHost::State::STOPPED}
    , mAddonUpdating {false}
    , mView {View::LIST}
    , mPosterBox {0.0f}
    , mFanartBox {0.0f}
    , mSortKey {"default"}
    , mSortDescending {false}
    , mFilter {0}
    , mInfoTimer {0}
    , mSourcesCursor {0}
    , mClockTimer {0}
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    setSize(screenWidth, screenHeight);

    mSidebarTitle = MediaStyle::makeText(MediaStyle::font(0.030f, FONT_PATH_BOLD),
                                         MediaStyle::TEXT_DIM, ALIGN_LEFT);
    mSidebarTitle->setText("MEDIA");
    mAddButton = MediaStyle::makeText(MediaStyle::font(0.050f, FONT_PATH_BOLD), MediaStyle::TEXT,
                                      ALIGN_CENTER);
    mAddButton->setText("+");
    mSettingsButton = std::make_unique<ImageComponent>(true, true);
    mPreviewImage = std::make_unique<ImageComponent>(true, true);
    mPreviewImage->setOrigin(0.5f, 0.5f);
    for (const auto& section : SECTIONS) {
        mSidebarItems.emplace_back(
            MediaStyle::makeText(MediaStyle::font(0.040f), MediaStyle::TEXT, ALIGN_LEFT));
        mSidebarItems.back()->setText(section);
    }

    mHeader = MediaStyle::makeText(MediaStyle::font(0.050f, FONT_PATH_LIGHT), MediaStyle::TEXT,
                                   ALIGN_LEFT);
    mSubHeader = MediaStyle::makeText(MediaStyle::font(0.028f), MediaStyle::TEXT, ALIGN_LEFT);
    mClock = MediaStyle::makeText(MediaStyle::font(0.068f, FONT_PATH_LIGHT), MediaStyle::TEXT,
                                  ALIGN_RIGHT);
    mOptionsHint = MediaStyle::makeText(MediaStyle::font(0.030f), MediaStyle::TEXT, ALIGN_LEFT);
    mOptionsTitle = MediaStyle::makeText(MediaStyle::font(0.028f), MediaStyle::ACCENT,
                                         ALIGN_LEFT);
    mOptionsTitle->setText("View options");
    mDurationText = MediaStyle::makeText(MediaStyle::font(0.024f, FONT_PATH_BOLD),
                                         MediaStyle::TEXT, ALIGN_CENTER);

    // The sidebar, which is shown on top of the content while it has the focus.
    const float sidebarWidth {std::round(screenWidth * 0.20f)};
    const float sidebarPadding {std::round(screenWidth * 0.022f)};

    mSidebarTitle->setSize(sidebarWidth - sidebarPadding * 2.0f, screenHeight * 0.06f);
    mSidebarTitle->setPosition(sidebarPadding, screenHeight * 0.035f);
    layoutTopButtons();
    for (size_t i {0}; i < mSidebarItems.size(); ++i) {
        mSidebarItems[i]->setSize(sidebarWidth - sidebarPadding * 2.0f, screenHeight * 0.075f);
        mSidebarItems[i]->setPosition(sidebarPadding,
                                      screenHeight * (0.16f + 0.075f * static_cast<float>(i)));
    }

    // The header and the clock above the content, which fills the screen.
    mHeader->setSize(screenWidth * 0.70f, screenHeight * 0.065f);
    mHeader->setPosition(screenWidth * 0.018f, screenHeight * 0.005f);
    mSubHeader->setSize(screenWidth * 0.70f, screenHeight * 0.04f);
    mSubHeader->setPosition(screenWidth * 0.018f, screenHeight * 0.048f);
    mClock->setSize(screenWidth * 0.30f, screenHeight * 0.09f);
    mClock->setPosition(screenWidth * 0.69f, screenHeight * 0.003f);
    mClock->setText(MediaStyle::clockText());
    mOptionsHint->setSize(screenWidth * 0.30f, screenHeight * 0.05f);
    mOptionsHint->setPosition(screenWidth * 0.010f, screenHeight * 0.940f);
    mDurationText->setSize(screenWidth * 0.056f, screenHeight * 0.034f);
    mDurationText->setPosition(screenWidth * 0.932f, screenHeight * 0.946f);

    mList.setCursorChangedCallback([this](int) {
        updateTexts();
        updatePreview(true);
        updateInfoPanel();
    });
    mGrid.setCursorChangedCallback([this](int cursor) { mList.setCursor(cursor); });
    mStrip.setCursorChangedCallback([this](int cursor) { mList.setCursor(cursor); });

    // The view options to the left of the content.
    mOptions.setFonts(MediaStyle::font(0.034f), MediaStyle::font(0.034f));
    mOptions.setRowHeight(MediaStyle::height(0.074f));
    mOptions.setSize(screenWidth * 0.235f, MediaStyle::height(0.074f) * 6.0f);
    mOptions.setPosition(screenWidth * 0.004f, screenHeight * 0.105f);
    mOptionsTitle->setSize(screenWidth * 0.22f, screenHeight * 0.05f);
    mOptionsTitle->setPosition(screenWidth * 0.012f, screenHeight * 0.05f);

    mAddonDialogs = std::make_unique<AddonDialogs>(mDialogs, this);
    mBusyText = MediaStyle::makeText(MediaStyle::font(0.024f), MediaStyle::ACCENT, ALIGN_RIGHT);
    // Below the clock, above the progress of the searches.
    mBusyText->setSize(screenWidth * 0.3f, screenHeight * 0.035f);
    mBusyText->setPosition(screenWidth * 0.685f, screenHeight * 0.088f);
    mInfoTitle = std::make_unique<TextComponent>("", MediaStyle::font(0.036f, FONT_PATH_BOLD),
                                                 MediaStyle::TEXT, ALIGN_LEFT, ALIGN_TOP,
                                                 glm::ivec2 {0, 0});
    mInfoTagline = std::make_unique<TextComponent>("", MediaStyle::font(0.030f, FONT_PATH_LIGHT),
                                                   MediaStyle::TEXT, ALIGN_LEFT, ALIGN_TOP,
                                                   glm::ivec2 {0, 0});
    mInfoDetails = std::make_unique<TextComponent>("", MediaStyle::font(0.029f), MediaStyle::TEXT,
                                                   ALIGN_LEFT, ALIGN_TOP, glm::ivec2 {0, 0});
    mInfoDetails->setLineSpacing(1.35f);
    mInfoPlot = std::make_unique<TextComponent>("", MediaStyle::font(0.029f), MediaStyle::TEXT,
                                                ALIGN_LEFT, ALIGN_TOP, glm::ivec2 {0, 0});
    mInfoPlot->setLineSpacing(1.35f);
    mInfoImage = std::make_unique<ImageComponent>(true, true);
    mFanartImage = std::make_unique<ImageComponent>(true, true);
    mFanartBoxImage = std::make_unique<ImageComponent>(true, true);
    layoutContent();

    // Previews of videos that have been removed are deleted in the background.
    MediaThumbnails::getInstance().startCleanup();

    loadContent(0);
    // The sections are shown first, A or right moves to the content.
    setFocus(Focus::SIDEBAR);
    updatePreview(true);
}

GuiMediaCenter::~GuiMediaCenter()
{
    mDialogs.clear();
    mAddonDialogs.reset();
    cancelAddonRequest();
    if (mInstall)
        mInstall->cancel = true;
    // The streaming add-on only runs while the media screen is open.
    AddonHost::getInstance().requestStop();
}

bool GuiMediaCenter::canEditSources() const
{
    return UIModeController::getInstance()->isUIModeFull();
}

void GuiMediaCenter::selectSection(int section)
{
    if (section == mSidebarCursor)
        return;
    mSidebarCursor = section;
    mFilter = 0;
    mLevels.clear();
    mSourcesCursor = 0;
    cancelAddonRequest();
    loadContent(0);
}

bool GuiMediaCenter::showAddButton() const
{
    return canEditSources() && !MediaLibrary::getInstance().getVideoSources().empty();
}

void GuiMediaCenter::layoutTopButtons()
{
    // The add button is placed directly to the right of the title and the settings button
    // to the right of it, or in its place if it's not shown.
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float sidebarPadding {std::round(screenWidth * 0.022f)};
    const float titleWidth {mSidebarTitle->getFont()->sizeText("MEDIA").x};
    const float buttonSize {std::round(screenHeight * 0.058f)};
    const float spacing {std::round(screenWidth * 0.008f)};
    const float buttonY {screenHeight * 0.035f + (screenHeight * 0.06f - buttonSize) / 2.0f};
    float x {sidebarPadding + titleWidth + screenWidth * 0.012f};

    mAddButtonShown = showAddButton();
    mAddButton->setSize(buttonSize, buttonSize);
    mAddButton->setPosition(x, buttonY);
    if (mAddButtonShown)
        x += buttonSize + spacing;

    const float iconSize {std::round(buttonSize * 0.62f)};
    mSettingsButton->setResize(iconSize, iconSize);
    mSettingsButton->setImage(":/graphics/media/settings.svg");
    mSettingsButton->setPosition(x + (buttonSize - iconSize) / 2.0f,
                                 buttonY + (buttonSize - iconSize) / 2.0f);

    if (!mAddButtonShown && mTopButton == TOP_ADD)
        mTopButton = TOP_SETTINGS;
}

void GuiMediaCenter::updatePreview(bool selectionChanged)
{
    std::string source;
    bool folder {false};
    const Entry* entry {mFocus == Focus::CONTENT || mTopButton == TOP_NONE ? selectedEntry() :
                                                                             nullptr};
    if (entry != nullptr && entry->type == EntryType::ADDON_ITEM) {
        // Posters of the add-on, downloaded in the background.
        const std::string image {entryImage(*entry, selectionChanged)};
        const std::string key {"addon:" + entry->path};
        if (key != mPreviewSource || image != mPreviewFile) {
            mPreviewSource = key;
            mPreviewFile = image;
            if (!image.empty()) {
                const float screenWidth {Renderer::getScreenWidth()};
                const float boxWidth {std::round(screenWidth * 0.20f) -
                                      std::round(screenWidth * 0.022f) * 2.0f};
                mPreviewImage->setMaxSize(boxWidth, std::round(boxWidth * 9.0f / 16.0f));
                mPreviewImage->setImage(image);
            }
        }
        return;
    }
    if (entry != nullptr) {
        if (entry->type == EntryType::VIDEO) {
            source = entry->path;
        }
        else if (entry->type == EntryType::FOLDER || entry->type == EntryType::SOURCE) {
            source = entry->path;
            folder = true;
        }
    }
    if (source != mPreviewSource) {
        mPreviewSource = source;
        mPreviewFile.clear();
        selectionChanged = true;
    }
    else if (!selectionChanged && !mPreviewFile.empty()) {
        return;
    }
    if (source.empty())
        return;

    MediaThumbnails& thumbnails {MediaThumbnails::getInstance()};
    // Only the request made when the selection changes moves the video to the front.
    const std::string file {folder ? thumbnails.getFolderPreview(source, selectionChanged) :
                                     thumbnails.getVideoPreview(source, selectionChanged)};
    if (file.empty() || file == mPreviewFile)
        return;
    mPreviewFile = file;

    const float screenWidth {Renderer::getScreenWidth()};
    const float sidebarWidth {std::round(screenWidth * 0.20f)};
    const float sidebarPadding {std::round(screenWidth * 0.022f)};
    const float boxWidth {sidebarWidth - sidebarPadding * 2.0f};
    mPreviewImage->setMaxSize(boxWidth, std::round(boxWidth * 9.0f / 16.0f));
    mPreviewImage->setImage(file);
}

void GuiMediaCenter::setFocus(Focus focus)
{
    mFocus = focus;
    mTopButton = TOP_NONE;
    mList.setFocused(focus == Focus::CONTENT);
    mList.stopScrolling();
    mGrid.setFocused(focus == Focus::CONTENT);
    mGrid.stopScrolling();
    mStrip.setFocused(focus == Focus::CONTENT);
    mStrip.stopScrolling();
    mOptions.setFocused(focus == Focus::OPTIONS);
    mOptions.stopScrolling();
    refreshHelpPrompts();
}

void GuiMediaCenter::loadContent(int cursor)
{
    if (isAddonSection()) {
        loadAddonContent(cursor);
        return;
    }
    cancelAddonRequest();
    mAddonItems.clear();
    mAddonContentType.clear();
    AddonHost::getInstance().setCurrentPath("");
    mEntries.clear();

    if (mLevels.empty() && mSidebarCursor == SECTION_FAVOURITES) {
        for (const auto& favourite : MediaLibrary::getInstance().getFavourites()) {
            mEntries.emplace_back(Entry {favourite.folder ? EntryType::FOLDER : EntryType::VIDEO,
                                         favourite.name, favourite.path, -1});
        }
        if (mEntries.empty())
            mEntries.emplace_back(
                Entry {EntryType::EMPTY, "No favourites yet, add them using X on a video or folder",
                       ""});
    }
    else if (mLevels.empty()) {
        const auto& sources {MediaLibrary::getInstance().getVideoSources()};
        for (size_t i {0}; i < sources.size(); ++i) {
            mEntries.emplace_back(Entry {EntryType::SOURCE, sources[i].name, sources[i].path,
                                         static_cast<int>(i)});
        }
        // Once there are sources, new ones are added using the button in the sidebar.
        if (canEditSources() && sources.empty())
            mEntries.emplace_back(Entry {EntryType::ADD_SOURCE, "Add videos...", ""});
        if (mEntries.empty())
            mEntries.emplace_back(Entry {EntryType::EMPTY, "No video sources added", ""});
    }
    else {
        const Level& level {mLevels.back()};
        mEntries.emplace_back(Entry {EntryType::PARENT, "..", parentDirectory(level.path)});
        bool hasContent {false};
        for (const auto& item : MediaLibrary::listDirectory(level.path)) {
            hasContent = true;
            mEntries.emplace_back(Entry {item.type == MediaLibrary::EntryType::FOLDER ?
                                             EntryType::FOLDER :
                                             EntryType::VIDEO,
                                         item.name, item.path, level.sourceIndex});
        }
        if (!hasContent)
            mEntries.emplace_back(Entry {EntryType::EMPTY, "No videos in this folder", ""});
    }

    std::vector<std::string> videos;
    std::vector<std::pair<std::string, bool>> metadata;
    for (const auto& entry : mEntries) {
        if (entry.type == EntryType::VIDEO)
            videos.emplace_back(entry.path);
        if (entry.type == EntryType::VIDEO || entry.type == EntryType::FOLDER ||
            entry.type == EntryType::SOURCE)
            metadata.emplace_back(entry.path, entry.type != EntryType::VIDEO);
    }
    // The previews of the selected video and folder are created first, see updatePreview().
    MediaThumbnails::getInstance().queueVideos(videos);
    // Titles, descriptions and posters, if an API key has been set.
    MediaMetadata::getInstance().queue(metadata);
    layoutTopButtons();
    presentEntries(cursor);
}

void GuiMediaCenter::presentEntries(int cursor)
{
    mAllEntries = mEntries;
    // The view and the sorting depend on the type of the content.
    mView = viewFor(currentContentType());
    loadSorting();
    mEntries = sortAndFilter(mAllEntries);
    layoutContent();
    std::vector<MediaListView::Item> items;
    for (const auto& entry : mEntries)
        items.emplace_back(makeListItem(entry));
    mList.setItems(items, cursor);
    updateGrid();
    updateTexts();
    updateInfoPanel();
    updateOptionsPanel();
    refreshHelpPrompts();
}

void GuiMediaCenter::representEntries()
{
    // The selected entry stays selected, if it's still shown.
    std::string selectedPath;
    int selectedIndex {-1};
    if (const Entry* entry {selectedEntry()}) {
        selectedPath = entry->path;
        selectedIndex = entry->addonIndex;
    }
    const std::vector<Entry> all {mAllEntries};
    mEntries = sortAndFilter(all);
    int cursor {0};
    for (size_t i {0}; i < mEntries.size(); ++i) {
        if (mEntries[i].path == selectedPath && mEntries[i].addonIndex == selectedIndex) {
            cursor = static_cast<int>(i);
            break;
        }
    }
    layoutContent();
    std::vector<MediaListView::Item> items;
    for (const auto& entry : mEntries)
        items.emplace_back(makeListItem(entry));
    mList.setItems(items, cursor);
    updateGrid();
    updateTexts();
    updateInfoPanel();
    updateOptionsPanel();
    refreshHelpPrompts();
}

std::string GuiMediaCenter::entryInfo(const Entry& entry, const char* key) const
{
    if (entry.type != EntryType::ADDON_ITEM || entry.addonIndex < 0 ||
        entry.addonIndex >= static_cast<int>(mAddonItems.size()))
        return "";
    const AddonHost::Item& item {mAddonItems[entry.addonIndex]};
    const auto it {item.info.find(key)};
    if (it == item.info.cend() || it->second == "0")
        return "";
    return stripAddonFormatting(it->second);
}

double GuiMediaCenter::entryDuration(const Entry& entry) const
{
    if (entry.type == EntryType::ADDON_ITEM)
        return std::atof(entryInfo(entry, "duration").c_str());
    if (entry.type == EntryType::VIDEO) {
        const MediaLibrary::PlayState state {MediaLibrary::getInstance().getPlayState(entry.path)};
        if (state.duration > 0.0)
            return state.duration;
        MediaMetadata::Info info;
        if (MediaMetadata::getInstance().get(entry.path, false, false, info) && info.found &&
            info.runtime > 0)
            return info.runtime * 60.0;
    }
    return 0.0;
}

bool GuiMediaCenter::entryWatched(const Entry& entry) const
{
    if (entry.type == EntryType::ADDON_ITEM) {
        if (std::atoi(entryInfo(entry, "playcount").c_str()) > 0)
            return true;
        if (entry.addonIndex >= 0 && entry.addonIndex < static_cast<int>(mAddonItems.size())) {
            const std::string key {MediaLibrary::streamKey(mAddonItems[entry.addonIndex].path)};
            return !key.empty() && MediaLibrary::getInstance().getPlayState(key).watched;
        }
        return false;
    }
    if (entry.type == EntryType::VIDEO) {
        const MediaLibrary::PlayState state {MediaLibrary::getInstance().getPlayState(entry.path)};
        return state.watched && state.position <= 0.0;
    }
    if (entry.type == EntryType::FOLDER || entry.type == EntryType::SOURCE)
        return MediaLibrary::getInstance().isFolderWatched(entry.path);
    return false;
}

std::vector<GuiMediaCenter::Entry> GuiMediaCenter::sortAndFilter(
    const std::vector<Entry>& entries) const
{
    // Only the videos, folders and listing entries are sorted and filtered, the others keep
    // their place before (the parent) or after them.
    auto sortable = [this](const Entry& entry) {
        // Menu entries of the add-on, e.g. the next page, keep their place after the movies.
        if (entry.type == EntryType::ADDON_ITEM) {
            if (entry.addonIndex < 0 || entry.addonIndex >= static_cast<int>(mAddonItems.size()))
                return false;
            const AddonHost::Item& item {mAddonItems[entry.addonIndex]};
            return item.playable || item.info.count("title") != 0 ||
                   item.info.count("tvshowtitle") != 0;
        }
        return entry.type == EntryType::VIDEO || entry.type == EntryType::FOLDER ||
               entry.type == EntryType::SOURCE;
    };
    std::vector<Entry> before;
    std::vector<Entry> items;
    std::vector<Entry> after;
    for (const auto& entry : entries) {
        if (sortable(entry)) {
            if (mFilter == 1 && entryWatched(entry))
                continue;
            if (mFilter == 2 && !entryWatched(entry))
                continue;
            items.emplace_back(entry);
        }
        else if (items.empty() && entry.type == EntryType::PARENT) {
            before.emplace_back(entry);
        }
        else {
            after.emplace_back(entry);
        }
    }

    if (mSortKey != "default") {
        auto number = [this](const Entry& entry, const std::string& key) {
            if (key == "duration")
                return entryDuration(entry);
            if (key == "size") {
                return entry.type == EntryType::VIDEO ?
                           static_cast<double>(Utils::FileSystem::getFileSize(entry.path)) :
                           0.0;
            }
            return std::atof(entryInfo(entry, key.c_str()).c_str());
        };
        auto name = [this](const Entry& entry) {
            std::string label {entry.name};
            if (entry.type == EntryType::ADDON_ITEM && entry.addonIndex >= 0 &&
                entry.addonIndex < static_cast<int>(mAddonItems.size()))
                label = stripAddonFormatting(mAddonItems[entry.addonIndex].label);
            return Utils::String::toLower(label);
        };
        const std::string key {mSortKey};
        const bool descending {mSortDescending};
        std::stable_sort(items.begin(), items.end(), [&](const Entry& a, const Entry& b) {
            if (key == "name") {
                const std::string nameA {name(a)};
                const std::string nameB {name(b)};
                return descending ? nameB < nameA : nameA < nameB;
            }
            const double valueA {number(a, key)};
            const double valueB {number(b, key)};
            // Entries without a value are placed last in both directions.
            if ((valueA <= 0.0) != (valueB <= 0.0))
                return valueA > 0.0;
            return descending ? valueB < valueA : valueA < valueB;
        });
    }
    else if (mSortDescending) {
        std::reverse(items.begin(), items.end());
    }

    std::vector<Entry> result {before};
    if (items.empty() && mFilter != 0 &&
        std::any_of(entries.cbegin(), entries.cend(), sortable)) {
        // Everything was left out by the filter.
        result.emplace_back(Entry {EntryType::EMPTY,
                                   mFilter == 1 ? "Everything here has been watched" :
                                                  "Nothing has been watched here",
                                   ""});
    }
    result.insert(result.end(), items.begin(), items.end());
    result.insert(result.end(), after.begin(), after.end());
    return result;
}

MediaListView::Item GuiMediaCenter::makeListItem(const Entry& entry) const
{
    MediaListView::Item item;
    item.label = entry.name;

    switch (entry.type) {
        case EntryType::ADD_SOURCE:
        case EntryType::ADDON_INSTALL:
        case EntryType::ADDON_RETRY: {
            item.highlight = true;
            break;
        }
        case EntryType::ADDON_ITEM: {
            if (entry.addonIndex < 0 || entry.addonIndex >= static_cast<int>(mAddonItems.size()))
                break;
            const AddonHost::Item& addonItem {mAddonItems[entry.addonIndex]};
            item.label = stripAddonFormatting(addonItem.label);
            // A movie or a series with a trailer row in the options menu: a play sign and
            // "Trailer" after the title, only the sign if the title is long.
            bool series {false};
            int tmdbId {0};
            if (trailerTarget(addonItem, series, tmdbId)) {
                item.tag = TRAILER_TAG + "  Trailer";
                item.tagShort = TRAILER_TAG;
            }
            const double duration {entryDuration(entry)};
            // The rating of TMDb after the title, the wide list has a column for it.
            if (mView != View::WIDELIST) {
                const std::string rating {formatNumber(entryInfo(entry, "rating"))};
                if (!rating.empty() && rating != "0.0")
                    item.badge = ViewController::FAVORITE_CHAR + " " + rating;
            }
            if (mView == View::WIDELIST) {
                // The year, the rating and the duration in columns.
                std::string rating {formatNumber(entryInfo(entry, "rating"))};
                if (rating == "0.0")
                    rating.clear();
                const std::string year {entryInfo(entry, "year")};
                std::string value;
                for (const std::string& part :
                     {year, rating.empty() ? std::string() : ViewController::FAVORITE_CHAR + " " + rating,
                      duration > 0.0 ? formatDuration(duration, false) : std::string()}) {
                    if (!part.empty())
                        value.append((value.empty() ? "" : "      ") + part);
                }
                item.value = value;
            }
            else if (duration > 0.0) {
                item.value = formatDuration(duration, false);
            }
            if (item.value.empty())
                item.value = formatNumber(stripAddonFormatting(addonItem.label2));
            if (item.value.empty())
                item.value = entryInfo(entry, "year");
            // The icons of the menus are files of the add-on, posters are shown elsewhere.
            for (const std::string& image : {addonItem.icon, addonItem.thumbnail}) {
                if (!image.empty() && image[0] == '/' && Utils::FileSystem::isRegularFile(image)) {
                    item.icon = image;
                    break;
                }
            }
            if (item.icon.empty() && addonItem.playable) {
                const auto playcount {addonItem.info.find("playcount")};
                const std::string key {MediaLibrary::streamKey(addonItem.path)};
                const bool watchedHere {!key.empty() &&
                                        MediaLibrary::getInstance().getPlayState(key).watched};
                item.icon = watchedHere || (playcount != addonItem.info.cend() &&
                                            std::atoi(playcount->second.c_str()) > 0) ?
                                ICON_WATCHED :
                                ICON_UNWATCHED;
            }
            const auto resume {addonItem.properties.find("resumetime")};
            const auto total {addonItem.properties.find("totaltime")};
            if (resume != addonItem.properties.cend() && total != addonItem.properties.cend()) {
                const double position {std::atof(resume->second.c_str())};
                const double duration {std::atof(total->second.c_str())};
                if (position > 0.0 && duration > 0.0)
                    item.progress = static_cast<float>(position / duration);
            }
            break;
        }
        case EntryType::SOURCE:
        case EntryType::FOLDER: {
            item.icon = ICON_FOLDER;
            if (!Utils::FileSystem::isDirectory(entry.path)) {
                item.value = "Not available";
                item.dimmed = true;
            }
            else if (MediaLibrary::getInstance().isFolderWatched(entry.path)) {
                item.icon = ICON_WATCHED;
            }
            break;
        }
        case EntryType::EMPTY: {
            item.dimmed = true;
            break;
        }
        case EntryType::VIDEO: {
            const MediaLibrary::PlayState state {
                MediaLibrary::getInstance().getPlayState(entry.path)};
            item.icon = state.watched && state.position <= 0.0 ? ICON_WATCHED : ICON_UNWATCHED;
            if (state.position > 0.0) {
                item.value = "Resume " + MediaLibrary::formatTime(state.position);
                if (state.duration > 0.0)
                    item.progress = static_cast<float>(state.position / state.duration);
            }
            else {
                item.value = formatFileSize(Utils::FileSystem::getFileSize(entry.path));
            }
            if (!Utils::FileSystem::exists(entry.path)) {
                item.value = "Not available";
                item.dimmed = true;
            }
            break;
        }
        default: {
            break;
        }
    }
    return item;
}

const GuiMediaCenter::Entry* GuiMediaCenter::selectedEntry() const
{
    const int cursor {mList.getCursor()};
    if (cursor < 0 || cursor >= static_cast<int>(mEntries.size()))
        return nullptr;
    return &mEntries[cursor];
}

void GuiMediaCenter::updateTexts()
{
    // E.g. "Popular / Elementum", the current list and the section.
    std::string header {SECTIONS[mSidebarCursor]};
    if (!mLevels.empty())
        header = mLevels.back().title + " / " + header;
    MediaStyle::setText(*mHeader, header);

    int count {0};
    int position {0};
    const int cursor {mList.getCursor()};
    for (size_t i {0}; i < mEntries.size(); ++i) {
        const EntryType type {mEntries[i].type};
        if (type == EntryType::SOURCE || type == EntryType::FOLDER || type == EntryType::VIDEO ||
            type == EntryType::ADDON_ITEM) {
            ++count;
            if (static_cast<int>(i) <= cursor)
                position = count;
        }
    }
    std::string sortName {"Default"};
    for (const auto& [key, label] : sortChoices()) {
        if (key == mSortKey)
            sortName = label;
    }
    std::string subHeader {"Sort by: " + sortName};
    if (count > 0)
        subHeader.append("  \u00B7  " + std::to_string(position) + " / " + std::to_string(count));
    MediaStyle::setText(*mSubHeader, subHeader);

    const double duration {selectedEntry() != nullptr ? entryDuration(*selectedEntry()) : 0.0};
    MediaStyle::setText(*mDurationText, duration > 0.0 ? formatDuration(duration, true) : "");
    // Left goes to the sections on the first level and to the options in the lists.
    if (mLevels.empty() && !usesStrip())
        MediaStyle::setText(*mOptionsHint, ViewController::ARROW_LEFT_CHAR + "   Sections");
    else
        MediaStyle::setText(*mOptionsHint,
                            (usesStrip() ? Utils::String::wideStringToString(L"\uf106") :
                                           ViewController::ARROW_LEFT_CHAR) +
                                "   Options");
}

void GuiMediaCenter::activateEntry()
{
    const Entry* selected {selectedEntry()};
    if (selected == nullptr)
        return;
    const Entry entry {*selected};
    const int cursor {mList.getCursor()};

    switch (entry.type) {
        case EntryType::ADD_SOURCE: {
            addSource();
            break;
        }
        case EntryType::SOURCE: {
            if (!Utils::FileSystem::isDirectory(entry.path)) {
                mWindow->queueInfoPopup("THE FOLDER '" + entry.path + "' IS NOT AVAILABLE", 4000);
                break;
            }
            mSourcesCursor = cursor;
            mLevels.emplace_back(Level {entry.path, entry.name, entry.sourceIndex, 0});
            loadContent(0);
            break;
        }
        case EntryType::FOLDER: {
            if (!Utils::FileSystem::isDirectory(entry.path)) {
                mWindow->queueInfoPopup("THE FOLDER '" + entry.path + "' IS NOT AVAILABLE", 4000);
                break;
            }
            // Favourite folders are opened from the top level.
            if (mLevels.empty())
                mSourcesCursor = cursor;
            else
                mLevels.back().cursor = cursor;
            mLevels.emplace_back(Level {entry.path, entry.name, entry.sourceIndex, 0});
            loadContent(0);
            break;
        }
        case EntryType::PARENT: {
            goBack();
            break;
        }
        case EntryType::VIDEO: {
            playVideo(entry.path, true);
            break;
        }
        case EntryType::ADDON_ITEM: {
            if (entry.addonIndex >= 0 && entry.addonIndex < static_cast<int>(mAddonItems.size()))
                activateAddonItem(mAddonItems[entry.addonIndex], cursor);
            break;
        }
        case EntryType::ADDON_INSTALL: {
            startInstall();
            break;
        }
        case EntryType::ADDON_RETRY: {
            AddonHost::getInstance().start();
            loadAddonContent(0);
            break;
        }
        default: {
            break;
        }
    }
}

void GuiMediaCenter::addSource()
{
    openFolderBrowser([this](const std::string& path) {
        MediaLibrary& library {MediaLibrary::getInstance()};
        for (size_t i {0}; i < library.getVideoSources().size(); ++i) {
            if (library.getVideoSources()[i].path == path) {
                mWindow->queueInfoPopup("THIS FOLDER HAS ALREADY BEEN ADDED", 3000);
                return;
            }
        }
        const std::string name {folderName(path)};
        library.addVideoSource(name, path);
        mWindow->queueInfoPopup("ADDED '" + Utils::String::toUpper(name) + "'", 3000);
        mLevels.clear();
        loadContent(static_cast<int>(library.getVideoSources().size()) - 1);
        setFocus(Focus::CONTENT);
    });
}

void GuiMediaCenter::goBack()
{
    // Back first cancels a request that is still loading.
    if (isAddonSection() && mAddonRequest && mAddonRequestKind != AddonRequestKind::LOAD) {
        cancelAddonRequest();
        refreshHelpPrompts();
        return;
    }
    if (mLevels.empty()) {
        setFocus(Focus::SIDEBAR);
        return;
    }
    mLevels.pop_back();
    if (mLevels.empty()) {
        loadContent(mSourcesCursor);
    }
    else {
        // Place the cursor on the folder that was just left.
        loadContent(mLevels.back().cursor);
    }
}

void GuiMediaCenter::playVideo(const std::string& path, bool askResume)
{
    const MediaLibrary::PlayState state {MediaLibrary::getInstance().getPlayState(path)};
    const std::string resumeMode {Settings::getInstance()->getString("MediaResumeMode")};

    if (state.position <= 0.0 || resumeMode == "never") {
        startPlayer(path, 0.0);
        return;
    }
    if (resumeMode == "always" || !askResume) {
        startPlayer(path, state.position);
        return;
    }

    MediaDialog* dialog {mDialogs.push(
        std::make_unique<MediaDialog>(Utils::FileSystem::getStem(path), 0.40f))};
    std::vector<MediaDialog::Option> options;
    MediaDialog::Option resume;
    resume.label = "Resume from " + MediaLibrary::formatTime(state.position);
    resume.onSelect = [this, dialog, path, state] {
        dialog->close();
        startPlayer(path, state.position);
    };
    options.emplace_back(resume);
    MediaDialog::Option beginning;
    beginning.label = "Play from beginning";
    beginning.onSelect = [this, dialog, path] {
        dialog->close();
        startPlayer(path, 0.0);
    };
    options.emplace_back(beginning);
    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiMediaCenter::startPlayer(const std::string& path, double startPosition)
{
    mList.stopScrolling();

    // The other videos in the folder make up the playlist, for previous/next.
    std::vector<std::string> playlist;
    size_t index {0};
    if (mLevels.empty()) {
        // A favourite, so the folder of the video is listed.
        for (const auto& item : MediaLibrary::listDirectory(Utils::FileSystem::getParent(path))) {
            if (item.type != MediaLibrary::EntryType::VIDEO)
                continue;
            if (item.path == path)
                index = playlist.size();
            playlist.emplace_back(item.path);
        }
    }
    else {
        for (const auto& entry : mEntries) {
            if (entry.type != EntryType::VIDEO)
                continue;
            if (entry.path == path)
                index = playlist.size();
            playlist.emplace_back(entry.path);
        }
    }
    if (playlist.empty() || playlist[index] != path) {
        playlist = {path};
        index = 0;
    }

    mWindow->pushGui(new GuiVideoPlayer(playlist, index, startPosition,
                                        [this](const std::string& lastPath) {
                                            onPlaybackFinished(lastPath);
                                        }));
}

void GuiMediaCenter::onPlaybackFinished(const std::string& lastPath)
{
    // Refresh the resume positions and watched states, and move to the last played video.
    int cursor {mList.getCursor()};
    for (size_t i {0}; i < mEntries.size(); ++i) {
        if (mEntries[i].path == lastPath && mEntries[i].type == EntryType::VIDEO)
            cursor = static_cast<int>(i);
    }
    loadContent(cursor);
}

void GuiMediaCenter::openContextMenu()
{
    const Entry* selected {selectedEntry()};
    if (selected == nullptr)
        return;
    const Entry entry {*selected};
    if (entry.type == EntryType::ADDON_ITEM) {
        if (entry.addonIndex >= 0 && entry.addonIndex < static_cast<int>(mAddonItems.size()))
            openAddonContextMenu(mAddonItems[entry.addonIndex]);
        return;
    }
    if (entry.type != EntryType::SOURCE && entry.type != EntryType::FOLDER &&
        entry.type != EntryType::VIDEO)
        return;

    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(entry.name, 0.40f))};
    std::vector<MediaDialog::Option> options;
    const std::string path {entry.path};
    const bool isVideo {entry.type == EntryType::VIDEO};
    MediaLibrary& library {MediaLibrary::getInstance()};

    if (isVideo) {
        const MediaLibrary::PlayState state {library.getPlayState(path)};
        if (state.position > 0.0) {
            MediaDialog::Option resume;
            resume.label = "Resume from " + MediaLibrary::formatTime(state.position);
            resume.onSelect = [this, dialog, path, state] {
                dialog->close();
                startPlayer(path, state.position);
            };
            options.emplace_back(resume);
            MediaDialog::Option beginning;
            beginning.label = "Play from beginning";
            beginning.onSelect = [this, dialog, path] {
                dialog->close();
                startPlayer(path, 0.0);
            };
            options.emplace_back(beginning);
        }
    }

    MediaDialog::Option watched;
    watched.label = "Mark as watched";
    watched.onSelect = [this, dialog, path, isVideo] {
        dialog->close();
        if (isVideo)
            MediaLibrary::getInstance().setWatched(path, true);
        else
            MediaLibrary::getInstance().setFolderWatched(path, true);
        loadContent(mList.getCursor());
    };
    options.emplace_back(watched);

    MediaDialog::Option unwatched;
    unwatched.label = "Mark as unwatched";
    unwatched.onSelect = [this, dialog, path, isVideo] {
        dialog->close();
        if (isVideo)
            MediaLibrary::getInstance().setWatched(path, false);
        else
            MediaLibrary::getInstance().setFolderWatched(path, false);
        loadContent(mList.getCursor());
    };
    options.emplace_back(unwatched);

    MediaDialog::Option favourite;
    const bool isFavourite {library.isFavourite(path)};
    favourite.label = isFavourite ? "Remove from favourites" : "Add to favourites";
    const std::string name {entry.name};
    favourite.onSelect = [this, dialog, path, name, isVideo, isFavourite] {
        dialog->close();
        if (isFavourite) {
            MediaLibrary::getInstance().removeFavourite(path);
            mWindow->queueInfoPopup("REMOVED FROM FAVOURITES", 2000);
            loadContent(std::max(0, mList.getCursor() -
                                        (mSidebarCursor == SECTION_FAVOURITES &&
                                                 mLevels.empty() ?
                                             1 :
                                             0)));
        }
        else {
            MediaLibrary::getInstance().addFavourite(name, path, !isVideo);
            mWindow->queueInfoPopup("ADDED TO FAVOURITES", 2000);
        }
    };
    options.emplace_back(favourite);

    // Titles, descriptions and posters from TMDb.
    if (MediaMetadata::isEnabled()) {
        MediaDialog::Option identify;
        identify.label = "Choose title (TMDb)...";
        identify.onSelect = [this, dialog, entry] {
            dialog->close();
            openIdentifyDialog(entry);
        };
        options.emplace_back(identify);
        MediaDialog::Option refresh;
        refresh.label = "Refresh information";
        refresh.onSelect = [this, dialog, entry] {
            dialog->close();
            MediaMetadata::getInstance().refresh(entry.path, entry.type != EntryType::VIDEO);
            mInfoImagePath.clear();
        };
        options.emplace_back(refresh);
    }
    MediaDialog::Option view;
    view.label = "View";
    view.value = [this] { return viewName(mView); };
    view.onSelect = [this] { cycleView(); };
    view.onAdjust = [this](int) { cycleView(); };
    options.emplace_back(view);

    if (entry.type == EntryType::SOURCE && canEditSources()) {
        const int index {entry.sourceIndex};
        MediaDialog::Option edit;
        edit.label = "Edit source";
        edit.onSelect = [this, dialog, index] {
            dialog->close();
            const auto& sources {MediaLibrary::getInstance().getVideoSources()};
            if (index >= 0 && index < static_cast<int>(sources.size()))
                openSourceEditor(index, sources[index].name, sources[index].path);
        };
        options.emplace_back(edit);
        MediaDialog::Option remove;
        remove.label = "Remove source";
        remove.onSelect = [this, dialog, index] {
            dialog->close();
            confirmRemoveSource(index);
        };
        options.emplace_back(remove);
    }

    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiMediaCenter::openSettingsDialog()
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>("Media settings", 0.40f))};
    std::vector<MediaDialog::Option> options;

    auto category = [this](const std::string& label, void (GuiMediaCenter::*open)()) {
        MediaDialog::Option option;
        option.label = label;
        option.onSelect = [this, open] { (this->*open)(); };
        return option;
    };
    options.emplace_back(category("Playback", &GuiMediaCenter::openPlaybackSettings));
    options.emplace_back(category("Audio", &GuiMediaCenter::openAudioSettings));
    options.emplace_back(category("Subtitles", &GuiMediaCenter::openSubtitleSettings));
#if defined(EMUSPLASH_DUBX)
    options.emplace_back(category("Dubx", &GuiMediaCenter::openDubSettings));
#endif
    if (canEditSources())
        options.emplace_back(category("Sources", &GuiMediaCenter::openSourcesSettings));
    options.emplace_back(
        category("Titles and posters (TMDb)", &GuiMediaCenter::openMetadataSettings));
    // The settings of the add-ons, as defined by the add-ons themselves.
    for (const char* addonId : {AddonStore::STREAMING_ADDON, AddonStore::PROVIDER_ADDON}) {
        AddonStore::Addon addon;
        if (!AddonStore::getInstance().getAddon(addonId, addon))
            continue;
        MediaDialog::Option option;
        option.label = addon.name;
        const std::string id {addonId};
        option.onSelect = [this, id] { mAddonDialogs->openSettings(id); };
        options.emplace_back(option);
    }
    if (AddonStore::getInstance().isInstalled(AddonStore::STREAMING_ADDON)) {
        options.emplace_back(
            switchSetting("Update Elementum automatically", "MediaAddonAutoUpdate"));
        options.back().onAdjust = [](int) {
            Settings* settings {Settings::getInstance()};
            settings->setBool("MediaAddonAutoUpdate", !settings->getBool("MediaAddonAutoUpdate"));
            settings->saveFile();
        };
        MediaDialog::Option restart;
        restart.label = "Restart Elementum";
        restart.onSelect = [this, dialog] {
            dialog->close();
            restartAddon();
        };
        options.emplace_back(restart);
    }

    dialog->setOptions(options);
    dialog->setCancelCallback([this] { refreshHelpPrompts(); });
    refreshHelpPrompts();
}

MediaDialog* GuiMediaCenter::pushSettingsDialog(const std::string& title,
                                                const std::vector<MediaDialog::Option>& options,
                                                float widthFraction)
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(title, widthFraction))};
    dialog->setOptions(options);
    dialog->setCancelCallback([this] {
        Settings::getInstance()->saveFile();
        refreshHelpPrompts();
    });
    refreshHelpPrompts();
    return dialog;
}

MediaDialog::Option GuiMediaCenter::choiceSetting(const std::string& label,
                                                  const std::string& key,
                                                  const Choices& choices)
{
    Settings* settings {Settings::getInstance()};
    MediaDialog::Option option;
    option.label = label;
    option.value = [settings, key, choices] {
        return choiceLabel(choices, settings->getString(key));
    };
    option.onAdjust = [settings, key, choices](int direction) {
        settings->setString(key, cycleChoice(choices, settings->getString(key), direction));
    };
    return option;
}

MediaDialog::Option GuiMediaCenter::switchSetting(const std::string& label, const std::string& key)
{
    Settings* settings {Settings::getInstance()};
    MediaDialog::Option option;
    option.label = label;
    option.value = [settings, key] { return settings->getBool(key) ? "On" : "Off"; };
    option.onAdjust = [settings, key](int) { settings->setBool(key, !settings->getBool(key)); };
    return option;
}

MediaDialog::Option GuiMediaCenter::textSetting(const std::string& label,
                                                const std::string& key,
                                                bool secret)
{
    Settings* settings {Settings::getInstance()};
    MediaDialog::Option option;
    option.label = label;
    option.value = [settings, key, secret] {
        const std::string value {settings->getString(key)};
        if (secret)
            return maskSecret(value);
        return value.empty() ? std::string("Not set") : value;
    };
    option.onSelect = [this, settings, key, label] {
        openTextInput(Utils::String::toUpper(label), settings->getString(key),
                      [this, settings, key](const std::string& value) {
                          settings->setString(key, Utils::String::trim(value));
                          settings->saveFile();
                          mDialogs.refresh();
                      });
    };
    option.onRemove = [this, settings, key] {
        settings->setString(key, "");
        settings->saveFile();
        mDialogs.refresh();
    };
    return option;
}

void GuiMediaCenter::openPlaybackSettings()
{
    std::vector<MediaDialog::Option> options;
    options.emplace_back(choiceSetting("Resume playback", "MediaResumeMode",
                                       {{"ask", "Ask"},
                                        {"always", "Always resume"},
                                        {"never", "Start from beginning"}}));
    options.emplace_back(switchSetting("Play next video automatically", "MediaPlayNext"));
    options.emplace_back(choiceSetting("Hardware decoding", "MediaHardwareDecoding",
                                       {{"auto-copy-safe", "Automatic"},
                                        {"auto-safe", "Automatic (direct)"},
                                        {"no", "Off"}}));
    pushSettingsDialog("Playback", options);
}

void GuiMediaCenter::openAudioSettings()
{
    std::vector<MediaDialog::Option> options;
    options.emplace_back(switchSetting("Audio passthrough", "MediaAudioPassthrough"));
    options.emplace_back(
        choiceSetting("Preferred audio language", "MediaAudioLanguage", languageChoices()));
    pushSettingsDialog("Audio", options);
}

void GuiMediaCenter::openSubtitleSettings()
{
    Settings* settings {Settings::getInstance()};
    std::vector<MediaDialog::Option> options;

    Choices subtitleChoices {languageChoices()};
    subtitleChoices.insert(subtitleChoices.begin() + 1, {"off", "Off"});
    options.emplace_back(
        choiceSetting("Preferred subtitles", "MediaSubtitleLanguage", subtitleChoices));
    options.emplace_back(
        switchSetting("Always load subtitles when available", "MediaSubtitleAlwaysLoad"));

    MediaDialog::Option subtitleSize;
    subtitleSize.label = "Subtitle size";
    subtitleSize.value = [settings] {
        return std::to_string(settings->getInt("MediaSubtitleScale")) + "%";
    };
    subtitleSize.onAdjust = [settings](int direction) {
        settings->setInt("MediaSubtitleScale",
                         std::clamp(settings->getInt("MediaSubtitleScale") + direction * 10, 50,
                                    250));
    };
    options.emplace_back(subtitleSize);

    MediaDialog::Option languages;
    languages.label = "Download languages";
    languages.value = [settings] {
        std::string names;
        const std::string defaultLanguage {settings->getString("MediaSubtitleDownloadLanguage")};
        if (!defaultLanguage.empty())
            names = SubtitleDownloader::getLanguageName(defaultLanguage);
        for (const auto& code : Utils::String::delimitedStringToVector(
                 settings->getString("MediaSubtitleExtraLanguages"), ",")) {
            if (!code.empty() && code != defaultLanguage)
                names.append((names.empty() ? "" : ", ") +
                             SubtitleDownloader::getLanguageName(code));
        }
        return names.empty() ? std::string("Not set") : names;
    };
    languages.onSelect = [this] {
        MediaDialog* picker {
            mDialogs.push(std::make_unique<MediaDialog>("Subtitle languages", 0.50f))};
        buildSubtitleLanguagePicker(picker, false, [this] { mDialogs.refresh(); });
        refreshHelpPrompts();
    };
    options.emplace_back(languages);

    options.emplace_back(
        switchSetting("Download automatically for series", "MediaSubtitleAutoDownload"));
    options.emplace_back(textSetting("OpenSubtitles API key", "MediaOpenSubtitlesApiKey", true));
    options.emplace_back(textSetting("OpenSubtitles user name", "MediaOpenSubtitlesUser", false));
    options.emplace_back(textSetting("OpenSubtitles password", "MediaOpenSubtitlesPassword", true));
    options.emplace_back(textSetting("Subdl API key", "MediaSubdlApiKey", true));

    MediaDialog::Option custom;
    custom.label = "Custom subtitle API";
    custom.value = [settings] {
        const std::string id {settings->getString("MediaCustomSubtitleService")};
        for (const auto& service : SubtitleDownloader::getCustomServices()) {
            if (service.id == id)
                return service.name;
        }
        return std::string("Off");
    };
    custom.onSelect = [this] { openCustomSubtitleSettings(); };
    options.emplace_back(custom);

    pushSettingsDialog("Subtitles", options, 0.56f);
}

#if defined(EMUSPLASH_DUBX)
void GuiMediaCenter::openDubSettings()
{
    // The settings of the dub module.
    if (!mDubMenu) {
        mDubMenu = std::make_unique<DubSettingsMenu>(
            mDialogs, mWindow, [this] { refreshHelpPrompts(); },
            [this](const std::string& title, const std::string& value,
                   const std::function<void(const std::string&)>& callback) {
                openTextInput(title, value, callback);
            });
        // The voice removal of Dubx is a part of the same settings.
        mDubMenu->setVoiceRemovalCallback([this] { openDubxSettings(); });
    }
    mDubMenu->openSettings();
}

void GuiMediaCenter::openDubxSettings()
{
    // The settings of the dubx module.
    if (!mDubxMenu) {
        mDubxMenu = std::make_unique<DubxSettingsMenu>(mDialogs, mWindow,
                                                       [this] { refreshHelpPrompts(); });
    }
    mDubxMenu->openSettings();
}
#endif

void GuiMediaCenter::openCustomSubtitleSettings()
{
    Settings* settings {Settings::getInstance()};
    std::vector<MediaDialog::Option> options;

    Choices services {{"", "Off"}};
    for (const auto& service : SubtitleDownloader::getCustomServices())
        services.emplace_back(service.id, service.name);
    options.emplace_back(choiceSetting("Service", "MediaCustomSubtitleService", services));

    // The fields not used by the chosen service are shown as such.
    auto field = [this, settings](const std::string& label, const std::string& key, bool secret,
                                  bool isKey) {
        MediaDialog::Option option {textSetting(label, key, secret)};
        const std::function<std::string()> value {option.value};
        option.value = [settings, value, isKey] {
            const std::string id {settings->getString("MediaCustomSubtitleService")};
            for (const auto& service : SubtitleDownloader::getCustomServices()) {
                if (service.id == id && !(isKey ? service.needsKey : service.needsUser))
                    return std::string("Not needed");
            }
            return value();
        };
        return option;
    };
    options.emplace_back(field("API key", "MediaCustomSubtitleKey", true, true));
    options.emplace_back(field("User name", "MediaCustomSubtitleUser", false, false));
    options.emplace_back(field("Password", "MediaCustomSubtitlePassword", true, false));

    MediaDialog* dialog {pushSettingsDialog("Custom subtitle API", options, 0.50f)};
    dialog->setSubtitle("Used together with the services above");
}

void GuiMediaCenter::openMetadataSettings()
{
    std::vector<MediaDialog::Option> options;
    options.emplace_back(textSetting("TMDb API key", "MediaTmdbApiKey", true));
    options.emplace_back(choiceSetting("Language", "MediaTmdbLanguage",
                                       {{"en-US", "English"},
                                        {"et-EE", "Estonian"},
                                        {"fi-FI", "Finnish"},
                                        {"ru-RU", "Russian"},
                                        {"de-DE", "German"},
                                        {"fr-FR", "French"},
                                        {"es-ES", "Spanish"},
                                        {"it-IT", "Italian"},
                                        {"sv-SE", "Swedish"},
                                        {"nb-NO", "Norwegian"},
                                        {"da-DK", "Danish"},
                                        {"pl-PL", "Polish"},
                                        {"lv-LV", "Latvian"},
                                        {"lt-LT", "Lithuanian"},
                                        {"ja-JP", "Japanese"}}));
    MediaDialog* dialog {pushSettingsDialog("Titles and posters (TMDb)", options, 0.56f)};
    dialog->setSubtitle("The key or read access token from themoviedb.org (Settings, API)");
}

void GuiMediaCenter::openSourcesSettings()
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>("Sources", 0.56f))};
    std::vector<MediaDialog::Option> options;

    MediaDialog::Option add;
    add.label = "Add videos...";
    add.highlight = true;
    add.onSelect = [this, dialog] {
        dialog->close();
        addSource();
    };
    options.emplace_back(add);

    const auto& sources {MediaLibrary::getInstance().getVideoSources()};
    for (size_t i {0}; i < sources.size(); ++i) {
        MediaDialog::Option source;
        source.label = sources[i].name;
        const std::string path {sources[i].path};
        source.value = [path] { return path; };
        const int index {static_cast<int>(i)};
        source.onSelect = [this, dialog, index] {
            dialog->close();
            const auto& current {MediaLibrary::getInstance().getVideoSources()};
            if (index < static_cast<int>(current.size()))
                openSourceEditor(index, current[index].name, current[index].path);
        };
        source.onRemove = [this, dialog, index] {
            dialog->close();
            confirmRemoveSource(index);
        };
        options.emplace_back(source);
    }
    dialog->setSubtitle("A edits a source, X removes it");
    dialog->setOptions(options);
    dialog->setCancelCallback([this] { refreshHelpPrompts(); });
    refreshHelpPrompts();
}

void GuiMediaCenter::openSourceEditor(int sourceIndex,
                                      const std::string& name,
                                      const std::string& path)
{
    mEditName = name;
    mEditPath = path;

    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(
        sourceIndex < 0 ? "Add video source" : "Edit video source", 0.55f))};

    std::vector<MediaDialog::Option> options;
    MediaDialog::Option folder;
    folder.label = "Folder";
    folder.value = [this] { return mEditPath.empty() ? "Choose..." : mEditPath; };
    folder.onSelect = [this] {
        openFolderBrowser([this](const std::string& chosenPath) {
            if (mEditName.empty() || mEditName == folderName(mEditPath))
                mEditName = folderName(chosenPath);
            mEditPath = chosenPath;
            mDialogs.refresh();
        });
    };
    options.emplace_back(folder);

    MediaDialog::Option sourceName;
    sourceName.label = "Name";
    sourceName.value = [this] { return mEditName; };
    sourceName.onSelect = [this] {
        openTextInput("SOURCE NAME", mEditName, [this](const std::string& value) {
            const std::string trimmed {Utils::String::trim(value)};
            if (!trimmed.empty())
                mEditName = trimmed;
            mDialogs.refresh();
        });
    };
    options.emplace_back(sourceName);

    MediaDialog::Option save;
    save.label = "Save";
    save.highlight = true;
    save.onSelect = [this, dialog, sourceIndex] {
        if (mEditPath.empty() || !Utils::FileSystem::isDirectory(mEditPath)) {
            mWindow->queueInfoPopup("CHOOSE AN EXISTING FOLDER FIRST", 3000);
            return;
        }
        const std::string sourceName {mEditName.empty() ? folderName(mEditPath) : mEditName};
        MediaLibrary& library {MediaLibrary::getInstance()};
        int cursor {sourceIndex};
        if (sourceIndex < 0) {
            library.addVideoSource(sourceName, mEditPath);
            cursor = static_cast<int>(library.getVideoSources().size()) - 1;
        }
        else {
            library.updateVideoSource(static_cast<size_t>(sourceIndex), sourceName, mEditPath);
        }
        dialog->close();
        mLevels.clear();
        loadContent(cursor);
        setFocus(Focus::CONTENT);
    };
    options.emplace_back(save);

    MediaDialog::Option cancel;
    cancel.label = "Cancel";
    cancel.onSelect = [dialog] { dialog->close(); };
    options.emplace_back(cancel);

    dialog->setOptions(options, path.empty() ? 0 : 2);
    refreshHelpPrompts();
}

void GuiMediaCenter::openFolderBrowser(
    const std::function<void(const std::string&)>& chosenCallback)
{
    auto state = std::make_shared<BrowseState>();
    state->callback = chosenCallback;
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>("Choose folder", 0.55f))};
    dialog->setBackHandler([this, dialog, state] {
        if (state->paths.empty())
            return false;
        state->paths.pop_back();
        const int cursor {state->cursors.empty() ? 0 : state->cursors.back()};
        if (!state->cursors.empty())
            state->cursors.pop_back();
        rebuildFolderBrowser(dialog, state, cursor);
        return true;
    });
    rebuildFolderBrowser(dialog, state, 0);
    refreshHelpPrompts();
}

void GuiMediaCenter::rebuildFolderBrowser(MediaDialog* dialog,
                                          const std::shared_ptr<BrowseState>& state,
                                          int cursor)
{
    std::vector<MediaDialog::Option> options;

    auto enterFolder = [this, dialog, state](const std::string& path) {
        state->cursors.emplace_back(dialog->getCursor());
        state->paths.emplace_back(path);
        rebuildFolderBrowser(dialog, state, 0);
    };

    if (state->paths.empty()) {
        dialog->setSubtitle("Choose a location");
        for (const auto& root : MediaLibrary::getBrowseRoots()) {
            MediaDialog::Option option;
            option.label = root.name;
            const std::string path {root.path};
            option.value = [path] { return path; };
            option.onSelect = [enterFolder, path] { enterFolder(path); };
            options.emplace_back(option);
        }
    }
    else {
        const std::string current {state->paths.back()};
        dialog->setSubtitle(current);

        MediaDialog::Option use;
        use.label = "Use this folder";
        use.highlight = true;
        use.onSelect = [dialog, state, current] {
            dialog->close();
            if (state->callback)
                state->callback(current);
        };
        options.emplace_back(use);

        MediaDialog::Option parent;
        parent.label = "..";
        parent.onSelect = [this, dialog, state] {
            state->paths.pop_back();
            const int previous {state->cursors.empty() ? 0 : state->cursors.back()};
            if (!state->cursors.empty())
                state->cursors.pop_back();
            rebuildFolderBrowser(dialog, state, previous);
        };
        options.emplace_back(parent);

        for (const auto& folder : MediaLibrary::listSubdirectories(current)) {
            MediaDialog::Option option;
            option.label = folder.name;
            const std::string path {folder.path};
            option.onSelect = [enterFolder, path] { enterFolder(path); };
            options.emplace_back(option);
        }
    }

    dialog->setOptions(options, cursor);
}

void GuiMediaCenter::confirmRemoveSource(int sourceIndex)
{
    const auto& sources {MediaLibrary::getInstance().getVideoSources()};
    if (sourceIndex < 0 || sourceIndex >= static_cast<int>(sources.size()))
        return;

    MediaDialog* dialog {
        mDialogs.push(std::make_unique<MediaDialog>("Remove video source", 0.45f))};
    dialog->setSubtitle("'" + sources[sourceIndex].name +
                        "' is removed from the list, no files are deleted");

    std::vector<MediaDialog::Option> options;
    MediaDialog::Option remove;
    remove.label = "Remove";
    remove.highlight = true;
    remove.onSelect = [this, dialog, sourceIndex] {
        dialog->close();
        MediaLibrary::getInstance().removeVideoSource(static_cast<size_t>(sourceIndex));
        mLevels.clear();
        loadContent(std::max(0, sourceIndex - 1));
    };
    options.emplace_back(remove);
    MediaDialog::Option cancel;
    cancel.label = "Cancel";
    cancel.onSelect = [dialog] { dialog->close(); };
    options.emplace_back(cancel);
    dialog->setOptions(options, 1);
    refreshHelpPrompts();
}

void GuiMediaCenter::openTextInput(const std::string& title,
                                   const std::string& value,
                                   const std::function<void(const std::string&)>& callback)
{
    if (Settings::getInstance()->getBool("VirtualKeyboard")) {
        mWindow->pushGui(new GuiTextEditKeyboardPopup(Renderer::getScreenHeight() * 0.2f, title,
                                                      value, callback, false, "OK",
                                                      "SAVE CHANGES?"));
    }
    else {
        mWindow->pushGui(new GuiTextEditPopup(title, value, callback, false, "OK",
                                              "SAVE CHANGES?"));
    }
}

bool GuiMediaCenter::input(InputConfig* config, Input input)
{
    // An installation in progress, back cancels it.
    if (mInstall) {
        if (input.value != 0 && (config->isMappedTo("b", input) ||
                                 (config->getDeviceId() == DEVICE_KEYBOARD &&
                                  input.id == SDLK_ESCAPE)))
            mInstall->cancel = true;
        return true;
    }
    // Progress dialogs of the streaming add-on.
    if (mAddonDialogs->input(config, input))
        return true;
    if (!mDialogs.empty()) {
        mDialogs.input(config, input);
        refreshHelpPrompts();
        return true;
    }

    const bool keyboard {config->getDeviceId() == DEVICE_KEYBOARD};
    // The escape key goes back one step, the same as the back button.
    const bool escape {keyboard && input.id == SDLK_ESCAPE};
    const bool backPressed {config->isMappedTo("b", input) || escape};

    if (input.value != 0 && !escape && config->isMappedTo("start", input)) {
        // Close the media screen together with the main menu.
        Window* window {mWindow};
        while (window->peekGui() != ViewController::getInstance())
            delete window->peekGui();
        return true;
    }
    if (input.value != 0 && (config->isMappedTo("back", input) || (keyboard && input.id == SDLK_s))) {
        openSettingsDialog();
        return true;
    }

    if (mFocus == Focus::SIDEBAR) {
        if (input.value == 0)
            return true;
        if (backPressed) {
            delete this;
            return true;
        }
        const bool up {config->isMappedLike("up", input)};
        const bool down {config->isMappedLike("down", input)};
        const bool left {config->isMappedLike("left", input)};
        const bool right {config->isMappedLike("right", input)};

        if (mTopButton != TOP_NONE) {
            // The buttons next to the title.
            if (config->isMappedTo("a", input)) {
                if (mTopButton == TOP_ADD)
                    addSource();
                else
                    openSettingsDialog();
            }
            else if (left && mTopButton == TOP_SETTINGS && mAddButtonShown) {
                mTopButton = TOP_ADD;
            }
            else if (right && mTopButton == TOP_ADD) {
                mTopButton = TOP_SETTINGS;
            }
            else if (right) {
                setFocus(Focus::CONTENT);
            }
            else if (down) {
                mTopButton = TOP_NONE;
            }
            refreshHelpPrompts();
            return true;
        }

        if (config->isMappedTo("a", input) || right) {
            setFocus(Focus::CONTENT);
            return true;
        }
        if (up || down) {
            // The buttons are above the sections, the content follows the section.
            const int sections {static_cast<int>(SECTIONS.size())};
            if (up) {
                if (mSidebarCursor > 0)
                    selectSection(mSidebarCursor - 1);
                else
                    mTopButton = mAddButtonShown ? TOP_ADD : TOP_SETTINGS;
            }
            else if (mSidebarCursor < sections - 1) {
                selectSection(mSidebarCursor + 1);
            }
            updatePreview(true);
            refreshHelpPrompts();
            return true;
        }
        return true;
    }

    // The view options panel.
    if (mFocus == Focus::OPTIONS) {
        if (mOptions.input(config, input)) {
            refreshHelpPrompts();
            return true;
        }
        if (input.value == 0)
            return true;
        if (config->isMappedTo("a", input)) {
            changeOption(1);
            return true;
        }
        if (config->isMappedLike("left", input)) {
            // The sections and the settings are to the left of the options.
            setFocus(Focus::SIDEBAR);
            updatePreview(true);
            return true;
        }
        if (backPressed || config->isMappedLike("right", input) ||
            (usesStrip() && config->isMappedLike("down", input))) {
            setFocus(Focus::CONTENT);
            return true;
        }
        return true;
    }

    // Content area. In the lists, right selects the trailer sign of a movie or a series and
    // the confirm button plays the trailer.
    if (!usesGrid() && !usesStrip() && input.value != 0) {
        if (mList.isTagFocused()) {
            if (config->isMappedTo("a", input)) {
                const Entry* entry {selectedEntry()};
                if (entry != nullptr && entry->type == EntryType::ADDON_ITEM &&
                    entry->addonIndex >= 0 &&
                    entry->addonIndex < static_cast<int>(mAddonItems.size()))
                    watchTrailerOf(mAddonItems[entry->addonIndex]);
                return true;
            }
            if (config->isMappedLike("left", input) || backPressed) {
                mList.setTagFocused(false);
                refreshHelpPrompts();
                return true;
            }
            if (config->isMappedLike("right", input))
                return true;
        }
        else if (config->isMappedLike("right", input) && mList.cursorHasTag()) {
            mList.setTagFocused(true);
            refreshHelpPrompts();
            return true;
        }
    }
    bool handled {false};
    if (usesGrid())
        handled = mGrid.input(config, input);
    else if (usesStrip())
        handled = mStrip.input(config, input);
    else
        handled = mList.input(config, input);
    if (handled) {
        refreshHelpPrompts();
        return true;
    }
    if (input.value == 0)
        return true;

    if (config->isMappedTo("y", input) || (keyboard && input.id == SDLK_v)) {
        cycleView();
        return true;
    }

    if (config->isMappedTo("a", input)) {
        activateEntry();
        return true;
    }
    if (backPressed) {
        goBack();
        return true;
    }
    // On the first level of a section left goes to the sections.
    if (mLevels.empty() && config->isMappedLike("left", input)) {
        setFocus(Focus::SIDEBAR);
        updatePreview(true);
        return true;
    }
    // The options are opened using left, or up in the views that scroll sideways.
    if ((!usesStrip() && config->isMappedLike("left", input)) ||
        (usesStrip() && config->isMappedLike("up", input))) {
        openOptionsPanel();
        return true;
    }
    if (config->isMappedTo("x", input) || (keyboard && input.id == SDLK_c)) {
        openContextMenu();
        return true;
    }
    return true;
}

void GuiMediaCenter::update(int deltaTime)
{
    mList.update(deltaTime);
    mGrid.update(deltaTime);
    mStrip.update(deltaTime);
    mOptions.update(deltaTime);
    mDialogs.update(deltaTime);
    mAddonDialogs->update(deltaTime);
    updateSearchPreview(deltaTime);
    pollAddonHost();
    pollInstallTask();

    mClockTimer -= deltaTime;
    if (mClockTimer <= 0) {
        MediaStyle::setText(*mClock, MediaStyle::clockText());
        mClockTimer = 1000;
    }
    // Previews are created in the background, check for the selected one now and then.
    mPreviewTimer -= deltaTime;
    if (mPreviewTimer <= 0) {
        mPreviewTimer = 300;
        updatePreview(false);
    }
    // Posters and information that have been downloaded in the meantime.
    mInfoTimer -= deltaTime;
    if (mInfoTimer <= 0) {
        mInfoTimer = 400;
        refreshImages();
        updateInfoPanel();
    }
    refreshHelpPrompts();
}

void GuiMediaCenter::render(const glm::mat4& parentTrans)
{
    const glm::mat4 trans {parentTrans * getTransform()};
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};

    mBackground.render(trans);
    // The fanart of the selected movie or series fills the background.
    if (!mFanartPath.empty())
        mFanartImage->render(trans);
    MediaStyle::drawRect(trans, 0.0f, 0.0f, screenWidth, screenHeight,
                         mFanartPath.empty() ? 0x0A1218C8 : 0x0A1218B0);

    mHeader->render(trans);
    mSubHeader->render(trans);
    mClock->render(trans);

    renderInfoPanel(trans);
    if (usesGrid())
        mGrid.render(trans);
    else if (usesStrip())
        mStrip.render(trans);
    else
        mList.render(trans);

    // The hint for the options and the duration of the selected entry.
    if (mFocus != Focus::OPTIONS)
        mOptionsHint->render(trans);
    if (!mDurationText->getValue().empty()) {
        const glm::vec3 pos {mDurationText->getPosition()};
        const glm::vec2 size {mDurationText->getSize()};
        const float border {std::max(2.0f, std::round(screenHeight * 0.002f))};
        MediaStyle::drawRect(trans, pos.x, pos.y, size.x, size.y, 0xFFFFFFFF);
        MediaStyle::drawRect(trans, pos.x + border, pos.y + border, size.x - border * 2.0f,
                             size.y - border * 2.0f, 0x000000FF);
        mDurationText->render(trans);
    }

    if (mFocus == Focus::OPTIONS)
        renderOptionsPanel(trans);
    if (mFocus == Focus::SIDEBAR)
        renderSidebar(trans);

    if (mAddonRequest && mAddonRequestKind != AddonRequestKind::LOAD) {
        MediaStyle::setText(*mBusyText, "Working...   Back to cancel");
        mBusyText->render(trans);
    }

    mDialogs.render(trans);
    mAddonDialogs->render(trans, true);
    if (mInstall) {
        std::string message;
        int percent {-1};
        {
            std::unique_lock<std::mutex> lock {mInstall->mutex};
            message = mInstall->message;
            percent = mInstall->percent;
        }
        mAddonDialogs->renderProgressPanel(trans, mInstall->title, message, percent,
                                           mInstall->cancel ? "Cancelling..." :
                                                              "Back to cancel");
    }
}

void GuiMediaCenter::renderSidebar(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float sidebarWidth {std::round(screenWidth * 0.20f)};

    // Drawn on top of the content while it has the focus.
    MediaStyle::drawRect(trans, sidebarWidth, 0.0f, screenWidth - sidebarWidth, screenHeight,
                         0x00000070);
    MediaStyle::drawRect(trans, 0.0f, 0.0f, sidebarWidth, screenHeight, MediaStyle::SIDEBAR);
    mSidebarTitle->render(trans);
    const bool sidebarFocused {mFocus == Focus::SIDEBAR};
    const glm::vec2 buttonSize {mAddButton->getSize()};
    if (mAddButtonShown) {
        const glm::vec3 pos {mAddButton->getPosition()};
        const bool focused {sidebarFocused && mTopButton == TOP_ADD};
        MediaStyle::drawRect(trans, pos.x, pos.y, buttonSize.x, buttonSize.y,
                             focused ? MediaStyle::ACCENT : 0xFFFFFF1E, buttonSize.y * 0.2f);
        const unsigned int color {focused ? 0xFFFFFFFF : MediaStyle::TEXT};
        if (mAddButton->getColor() != color)
            mAddButton->setColor(color);
        mAddButton->render(trans);
    }
    {
        const glm::vec3 iconPos {mSettingsButton->getPosition()};
        const glm::vec2 iconSize {mSettingsButton->getSize()};
        const bool focused {sidebarFocused && mTopButton == TOP_SETTINGS};
        MediaStyle::drawRect(trans, iconPos.x - (buttonSize.x - iconSize.x) / 2.0f,
                             iconPos.y - (buttonSize.y - iconSize.y) / 2.0f, buttonSize.x,
                             buttonSize.y, focused ? MediaStyle::ACCENT : 0xFFFFFF1E,
                             buttonSize.y * 0.2f);
        mSettingsButton->setColorShift(focused ? 0xFFFFFFFF : MediaStyle::TEXT);
        mSettingsButton->render(trans);
    }
    for (size_t i {0}; i < mSidebarItems.size(); ++i) {
        TextComponent& item {*mSidebarItems[i]};
        const glm::vec3 pos {item.getPosition()};
        const bool selected {static_cast<int>(i) == mSidebarCursor};
        unsigned int color {MediaStyle::TEXT_SECONDARY};
        if (selected && sidebarFocused && mTopButton == TOP_NONE) {
            const float inset {std::round(screenWidth * 0.010f)};
            MediaStyle::drawRect(trans, inset, pos.y, sidebarWidth - inset * 2.0f,
                                 item.getSize().y, MediaStyle::ACCENT, screenHeight * 0.008f);
            color = 0xFFFFFFFF;
        }
        else if (selected) {
            MediaStyle::drawRect(trans, 0.0f, pos.y + item.getSize().y * 0.18f,
                                 std::max(3.0f, std::round(screenWidth * 0.004f)),
                                 item.getSize().y * 0.64f, MediaStyle::ACCENT);
            color = MediaStyle::TEXT;
        }
        if (item.getColor() != color)
            item.setColor(color);
        item.render(trans);
    }

    // Preview of the selected video or folder below the sections.
    {
        const float sidebarPadding {std::round(screenWidth * 0.022f)};
        const glm::vec3 lastItem {mSidebarItems.back()->getPosition()};
        const float boxX {sidebarPadding};
        const float boxY {lastItem.y + mSidebarItems.back()->getSize().y + screenHeight * 0.04f};
        const float boxWidth {sidebarWidth - sidebarPadding * 2.0f};
        const float boxHeight {std::round(boxWidth * 9.0f / 16.0f)};
        MediaStyle::drawRect(trans, boxX, boxY, boxWidth, boxHeight, 0x14141CFF,
                             screenHeight * 0.008f);
        if (!mPreviewFile.empty()) {
            mPreviewImage->setPosition(boxX + boxWidth / 2.0f, boxY + boxHeight / 2.0f);
            mPreviewImage->render(trans);
        }
    }
}

void GuiMediaCenter::renderOptionsPanel(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float panelWidth {std::round(screenWidth * 0.245f)};
    MediaStyle::drawRect(trans, panelWidth, 0.0f, screenWidth - panelWidth, screenHeight,
                         0x00000060);
    MediaStyle::drawRect(trans, 0.0f, 0.0f, panelWidth, screenHeight, 0x0B0B0CF5);
    mOptionsTitle->render(trans);
    mOptions.render(trans);
}

std::vector<HelpPrompt> GuiMediaCenter::getHelpPrompts()
{
    if (!mDialogs.empty())
        return mDialogs.getHelpPrompts();

    std::vector<HelpPrompt> prompts;
    if (mFocus == Focus::SIDEBAR) {
        prompts.push_back(HelpPrompt("up/down", "choose"));
        prompts.push_back(HelpPrompt("a", mTopButton == TOP_ADD      ? "add videos" :
                                          mTopButton == TOP_SETTINGS ? "settings" :
                                                                       "select"));
        prompts.push_back(HelpPrompt("b", "close"));
        prompts.push_back(HelpPrompt("back", "settings"));
        return prompts;
    }
    if (mFocus == Focus::OPTIONS) {
        prompts.push_back(HelpPrompt("up/down", "choose"));
        prompts.push_back(HelpPrompt("a", "change"));
        prompts.push_back(HelpPrompt("left/right", "sections / close"));
        prompts.push_back(HelpPrompt("b", "close"));
        return prompts;
    }

    if (mInstall)
        return {HelpPrompt("b", "cancel")};
    prompts.push_back(HelpPrompt(usesGrid()  ? "up/down/left/right" :
                                 usesStrip() ? "left/right" :
                                               "up/down",
                                 "choose"));
    const Entry* entry {selectedEntry()};
    if (entry != nullptr) {
        bool playable {entry->type == EntryType::VIDEO};
        if (entry->type == EntryType::ADDON_ITEM && entry->addonIndex >= 0 &&
            entry->addonIndex < static_cast<int>(mAddonItems.size()))
            playable = mAddonItems[entry->addonIndex].playable;
        if (!usesGrid() && !usesStrip() && mList.isTagFocused())
            prompts.push_back(HelpPrompt("a", "play trailer"));
        else if (playable)
            prompts.push_back(HelpPrompt("a", "play"));
        else if (entry->type != EntryType::EMPTY)
            prompts.push_back(HelpPrompt("a", "select"));
        if (entry->type == EntryType::VIDEO || entry->type == EntryType::FOLDER ||
            entry->type == EntryType::SOURCE || entry->type == EntryType::ADDON_ITEM)
            prompts.push_back(HelpPrompt("x", "options"));
    }
    // The help system only has icons for the combined directions.
    if (usesStrip())
        prompts.push_back(HelpPrompt("up/down", "view options"));
    else
        prompts.push_back(HelpPrompt("left/right", mLevels.empty() ? "sections" : "view options"));
    prompts.push_back(HelpPrompt("y", "view"));
    prompts.push_back(HelpPrompt("b", mAddonRequest && mAddonRequestKind != AddonRequestKind::LOAD ?
                                          "cancel" :
                                          "back"));
    prompts.push_back(HelpPrompt("back", "settings"));
    return prompts;
}

void GuiMediaCenter::refreshHelpPrompts()
{
    if (mWindow->peekGui() != this)
        return;
    const std::vector<HelpPrompt> prompts {getHelpPrompts()};
    if (prompts == mLastHelpPrompts)
        return;
    mLastHelpPrompts = prompts;
    mWindow->setHelpPrompts(prompts);
}

//--------------------------------------------------------------------------------------------------
// Elementum.
//--------------------------------------------------------------------------------------------------

bool GuiMediaCenter::isAddonSection() const { return mSidebarCursor == SECTION_ELEMENTUM; }

void GuiMediaCenter::loadAddonContent(int cursor)
{
    mEntries.clear();
    mAddonItems.clear();
    mAddonContentType.clear();
    AddonHost& host {AddonHost::getInstance()};
    const AddonStore& store {AddonStore::getInstance()};

    if (mInstall) {
        mEntries.emplace_back(Entry {EntryType::EMPTY, mInstall->title + "...", ""});
    }
    else if (AddonStore::isAutoUpdating()) {
        // Updated in the background after starting, the program is started afterwards.
        mEntries.emplace_back(Entry {EntryType::EMPTY, "Updating Elementum...", ""});
    }
    else if (!store.isInstalled(AddonStore::STREAMING_ADDON)) {
        mEntries.emplace_back(Entry {EntryType::ADDON_INSTALL, "Install Elementum", ""});
        mEntries.emplace_back(Entry {EntryType::EMPTY,
                                     "Downloads Elementum and Elementum Burst from GitHub", ""});
    }
    else {
        // After a failure the program is only started again when asked to.
        if (host.getState() == AddonHost::State::STOPPED)
            host.start();
        mLastHostState = host.getState();
        mLastHostMessage = host.getStateMessage();
        if (mLastHostState == AddonHost::State::RUNNING) {
            const std::string url {mLevels.empty() ? AddonHost::getRootUrl() :
                                                     mLevels.back().path};
            requestAddon(url, AddonRequestKind::LOAD, "", cursor);
            mEntries.emplace_back(Entry {EntryType::EMPTY, "Loading...", ""});
        }
        else if (mLastHostState == AddonHost::State::FAILED) {
            mEntries.emplace_back(Entry {EntryType::EMPTY, mLastHostMessage, ""});
            mEntries.emplace_back(Entry {EntryType::ADDON_RETRY, "Try again", ""});
        }
        else {
            mEntries.emplace_back(Entry {EntryType::EMPTY,
                                         mLastHostMessage.empty() ? "Starting Elementum..." :
                                                                    mLastHostMessage + "...",
                                         ""});
        }
    }

    presentEntries(0);
}

void GuiMediaCenter::requestAddon(const std::string& url,
                                  AddonRequestKind kind,
                                  const std::string& title,
                                  int cursor)
{
    if (kind == AddonRequestKind::RUN && mAddonRequest && mAddonRequestKind != AddonRequestKind::RUN) {
        // An action while a listing is loading is run on its own.
        AddonHost::getInstance().runAction(url);
        return;
    }
    cancelAddonRequest();
    mAddonRequest = AddonHost::getInstance().open(url);
    mAddonRequestUrl = url;
    mAddonRequestKind = kind;
    mAddonRequestTitle = title;
    mAddonRequestCursor = cursor;
    if (kind != AddonRequestKind::PLAY)
        mAddonRequestItem = AddonHost::Item();
    if (kind == AddonRequestKind::LOAD || kind == AddonRequestKind::OPEN)
        AddonHost::getInstance().setCurrentPath(url);
    refreshHelpPrompts();
}

void GuiMediaCenter::cancelAddonRequest()
{
    if (mAddonRequest)
        mAddonRequest->cancel();
    mAddonRequest.reset();
}

void GuiMediaCenter::pollAddonHost()
{
    if (mAddonRequest && mAddonRequest->isReady()) {
        AddonHost::Response response {mAddonRequest->take()};
        mAddonRequest.reset();
        handleAddonResponse(std::move(response));
    }

    // The list is loaded once the update done when starting has finished.
    if (isAddonSection() && !mInstall && mAddonUpdating && !AddonStore::isAutoUpdating()) {
        mAddonUpdating = false;
        loadAddonContent(0);
        return;
    }
    mAddonUpdating = AddonStore::isAutoUpdating();

    // The content follows the state of the program, e.g. while it's starting.
    if (isAddonSection() && !mInstall) {
        const AddonHost& host {AddonHost::getInstance()};
        const AddonHost::State state {host.getState()};
        const std::string message {host.getStateMessage()};
        if (state != mLastHostState ||
            (state == AddonHost::State::STARTING && message != mLastHostMessage)) {
            const bool started {state == AddonHost::State::RUNNING};
            if (started || state != AddonHost::State::RUNNING)
                loadAddonContent(mLevels.empty() ? mSourcesCursor : mLevels.back().cursor);
        }
    }
}

void GuiMediaCenter::handleAddonResponse(AddonHost::Response response)
{
    using Type = AddonHost::Response::Type;
    const AddonRequestKind kind {mAddonRequestKind};

    if (response.type == Type::FAILED) {
        if (!response.error.empty())
            mWindow->queueInfoPopup(Utils::String::replace(stripAddonFormatting(response.error),
                                                           "\n", " "),
                                    5000);
        if (kind == AddonRequestKind::LOAD && isAddonSection()) {
            mEntries.clear();
            mAddonItems.clear();
            mEntries.emplace_back(Entry {EntryType::EMPTY, "Couldn't load this list", ""});
            mEntries.emplace_back(Entry {EntryType::ADDON_RETRY, "Try again", ""});
            mAddonContentType.clear();
            presentEntries(1);
        }
        refreshHelpPrompts();
        return;
    }
    if (response.type == Type::PLAY) {
        playStream(response.url, mAddonRequestItem);
        return;
    }
    if (response.type == Type::NAVIGATE) {
        addonNavigate(response.url, response.replace || kind == AddonRequestKind::LOAD);
        return;
    }
    if (kind == AddonRequestKind::RUN && mRefreshAfterRun) {
        mRefreshAfterRun = false;
        if (isAddonSection() && !mAddonRequest) {
            loadAddonContent(mList.getCursor());
            return;
        }
    }
    if (response.type == Type::DONE || !isAddonSection()) {
        refreshHelpPrompts();
        return;
    }
    if (kind == AddonRequestKind::RUN)
        return;

    // A listing: a new level when an entry was opened, otherwise the current one.
    int cursor {0};
    if (kind == AddonRequestKind::OPEN || kind == AddonRequestKind::PLAY) {
        if (mLevels.empty())
            mSourcesCursor = mAddonRequestCursor;
        else
            mLevels.back().cursor = mAddonRequestCursor;
        mLevels.emplace_back(Level {mAddonRequestUrl, mAddonRequestTitle, -1, 0});
    }
    else {
        cursor = mAddonRequestCursor;
    }

    mAddonItems = std::move(response.items);
    mAddonContentType = response.contentType;
    mEntries.clear();
    if (!mLevels.empty())
        mEntries.emplace_back(Entry {EntryType::PARENT, "..", ""});
    for (size_t i {0}; i < mAddonItems.size(); ++i) {
        mEntries.emplace_back(Entry {EntryType::ADDON_ITEM, mAddonItems[i].label,
                                     mAddonItems[i].path, -1, static_cast<int>(i)});
    }
    if (mAddonItems.empty() && !mLevels.empty())
        mEntries.emplace_back(Entry {EntryType::EMPTY, "Nothing found", ""});

    // The parent entry is skipped when entering a list.
    if (kind != AddonRequestKind::LOAD && !mLevels.empty() && mEntries.size() > 1)
        cursor = 1;
    presentEntries(cursor);
    updatePreview(true);
}

void GuiMediaCenter::activateAddonItem(const AddonHost::Item& item, int cursor)
{
    // A second press while the previous one is still being handled is ignored, starting the
    // same search twice made the add-on stop answering. Back cancels the request.
    if (mAddonRequest && mAddonRequestKind != AddonRequestKind::LOAD)
        return;
    const std::string title {stripAddonFormatting(item.label)};
    if (item.playable) {
        requestAddon(item.path, AddonRequestKind::PLAY, title, cursor);
        mAddonRequestItem = item;
    }
    else if (item.isAction) {
        requestAddon(item.path, AddonRequestKind::RUN, title, cursor);
    }
    else {
        requestAddon(item.path, AddonRequestKind::OPEN, title, cursor);
    }
}

void GuiMediaCenter::addonNavigate(const std::string& url, bool replace)
{
    if (!isAddonSection())
        return;
    if (!AddonHost::isPluginUrl(url))
        return;
    std::string title {mAddonRequestTitle};
    if (title.empty()) {
        // Named after the address, e.g. "plugin://.../movies/popular" gives "Popular".
        std::string path {url.substr(0, url.find('?'))};
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        title = Utils::String::toCapitalized(Utils::FileSystem::getFileName(path));
    }
    if (replace && !mLevels.empty()) {
        mLevels.back().path = url;
        mLevels.back().title = title;
    }
    else {
        if (mLevels.empty())
            mSourcesCursor = mList.getCursor();
        else
            mLevels.back().cursor = mList.getCursor();
        mLevels.emplace_back(Level {url, title, -1, 0});
    }
    loadAddonContent(0);
}

void GuiMediaCenter::addonRun(const std::string& url)
{
    LOG(LogInfo) << "GuiMediaCenter: Running " << url;
    requestAddon(url, AddonRequestKind::RUN, "", mList.getCursor());
    // Removing something from the history changes the list that is shown.
    if (url.find("/history/") != std::string::npos || url.find("/remove") != std::string::npos)
        mRefreshAfterRun = true;
}

void GuiMediaCenter::addonRefresh()
{
    if (!isAddonSection())
        return;
    // The add-on asks for it while the action is still being handled, the list is loaded
    // again once that has finished.
    if (mAddonRequest) {
        if (mAddonRequestKind == AddonRequestKind::RUN)
            mRefreshAfterRun = true;
        return;
    }
    loadAddonContent(mList.getCursor());
}

void GuiMediaCenter::addonPlay(const std::string& url)
{
    if (AddonHost::isPluginUrl(url)) {
        const AddonHost::Item item {mAddonRequestItem};
        requestAddon(url, AddonRequestKind::PLAY, "", mList.getCursor());
        mAddonRequestItem = item;
        return;
    }
    playStream(url, mAddonRequestItem);
}

std::string GuiMediaCenter::addonSearchAddress()
{
    if (!isAddonSection() || !mAddonRequest || mAddonRequestKind == AddonRequestKind::LOAD)
        return "";
    std::string address {mAddonRequestUrl.substr(0, mAddonRequestUrl.find('?'))};
    while (!address.empty() && address.back() == '/')
        address.pop_back();
    const std::string ending {"/search"};
    if (address.size() < ending.size() ||
        address.compare(address.size() - ending.size(), ending.size(), ending) != 0)
        return "";
    return address;
}

void GuiMediaCenter::addonSearchPreview(const std::string& address, const std::string& text)
{
    // The text is looked up once it has three characters and has stopped changing for a moment.
    if (!mPreviewSession) {
        // What the history of the searches has now, and how an entry is removed from it.
        mPreviewSession = true;
        mPreviewQueries.clear();
        mPreviewKnownQueries.clear();
        mPreviewRemoveUrl.clear();
        for (const auto& item : mAddonItems) {
            mPreviewKnownQueries.insert(Utils::String::trim(stripAddonFormatting(item.label)));
            for (const auto& [label, action] : item.contextMenu) {
                const size_t start {action.find("plugin://")};
                const size_t end {action.find(')', start)};
                if (start == std::string::npos)
                    continue;
                const std::string url {action.substr(
                    start, end == std::string::npos ? std::string::npos : end - start)};
                if (mPreviewRemoveUrl.empty() && url.find("remove") != std::string::npos &&
                    url.find("query=") != std::string::npos)
                    mPreviewRemoveUrl = url;
            }
        }
        if (mPreviewRemoveUrl.empty()) {
            const size_t slash {address.rfind('/', address.size() - 8)};
            const std::string type {slash == std::string::npos ?
                                        "movies" :
                                        address.substr(slash + 1, address.size() - 8 - slash)};
            mPreviewRemoveUrl = "plugin://plugin.video.elementum/history/remove?type=" + type +
                                "&query=";
        }
    }
    mPreviewAddress = address;
    mPreviewText = Utils::String::trim(text);
    mSearchPreviewTimer = Utils::String::unicodeLength(mPreviewText) >= 3 ? 700 : -1;
}

void GuiMediaCenter::addonSearchUpdate(int deltaTime)
{
    // The screen below the text input isn't updated by the window, so it's done from here.
    update(deltaTime);
}

void GuiMediaCenter::addonSearchPreviewEnd(bool cancelled, const std::string& text)
{
    // The add-on keeps every search in its history, the ones that were only typed on the way
    // are removed again.
    const std::string accepted {Utils::String::trim(text)};
    for (const auto& query : mPreviewQueries) {
        if (mPreviewKnownQueries.count(query) != 0 || (!cancelled && query == accepted))
            continue;
        std::string url {mPreviewRemoveUrl};
        const size_t start {url.find("query=")};
        if (start == std::string::npos)
            continue;
        const size_t valueStart {start + 6};
        size_t valueEnd {url.find('&', valueStart)};
        if (valueEnd == std::string::npos)
            valueEnd = url.size();
        url.replace(valueStart, valueEnd - valueStart, HttpClient::urlEncode(query));
        LOG(LogInfo) << "GuiMediaCenter: Removing the search \"" << query
                     << "\" from the history: " << url;
        AddonHost::getInstance().runAction(url);
    }
    mPreviewSession = false;
    mPreviewQueries.clear();
    mSearchPreviewTimer = -1;
    if (mPreviewRequest)
        mPreviewRequest->cancel();
    mPreviewRequest.reset();
    if (mPreviewShown && cancelled) {
        mAddonItems = std::move(mPreviewSavedItems);
        mAddonContentType = mPreviewSavedType;
        mEntries = std::move(mPreviewSavedEntries);
        presentEntries(mPreviewSavedCursor);
    }
    mPreviewShown = false;
    mPreviewSavedItems.clear();
    mPreviewSavedEntries.clear();
}

void GuiMediaCenter::updateSearchPreview(int deltaTime)
{
    if (mSearchPreviewTimer >= 0) {
        mSearchPreviewTimer -= deltaTime;
        if (mSearchPreviewTimer < 0) {
            if (mPreviewRequest)
                mPreviewRequest->cancel();
            if (std::find(mPreviewQueries.cbegin(), mPreviewQueries.cend(), mPreviewText) ==
                mPreviewQueries.cend())
                mPreviewQueries.emplace_back(mPreviewText);
            mPreviewRequest = AddonHost::getInstance().open(
                mPreviewAddress + (mPreviewAddress.find('?') == std::string::npos ? "?q=" : "&q=") +
                HttpClient::urlEncode(mPreviewText));
        }
    }
    if (!mPreviewRequest || !mPreviewRequest->isReady())
        return;
    AddonHost::Response response {mPreviewRequest->take()};
    mPreviewRequest.reset();
    if (response.type != AddonHost::Response::Type::LISTING || !isAddonSection())
        return;

    // The list that was shown is kept, to be shown again if the search is cancelled.
    if (!mPreviewShown) {
        mPreviewSavedItems = mAddonItems;
        mPreviewSavedType = mAddonContentType;
        mPreviewSavedEntries = mEntries;
        mPreviewSavedCursor = mList.getCursor();
        mPreviewShown = true;
    }
    mAddonItems = std::move(response.items);
    mAddonContentType = response.contentType;
    mEntries.clear();
    if (!mLevels.empty())
        mEntries.emplace_back(Entry {EntryType::PARENT, "..", ""});
    for (size_t i {0}; i < mAddonItems.size(); ++i) {
        mEntries.emplace_back(Entry {EntryType::ADDON_ITEM, mAddonItems[i].label,
                                     mAddonItems[i].path, -1, static_cast<int>(i)});
    }
    if (mAddonItems.empty())
        mEntries.emplace_back(Entry {EntryType::EMPTY, "Nothing found", ""});
    presentEntries(mAddonItems.empty() || mLevels.empty() ? 0 : 1);
    updatePreview(true);
}

void GuiMediaCenter::playStream(const std::string& url, const AddonHost::Item& item,
                                bool checked)
{
    if (url.empty())
        return;

    // A season pack without the episode makes the add-on take another file, e.g. the first.
    // That isn't played quietly.
    const auto seasonInfo {item.info.find("season")};
    const auto episodeInfo {item.info.find("episode")};
    const int wantedSeason {seasonInfo == item.info.cend() ? 0 :
                                                              std::atoi(seasonInfo->second.c_str())};
    const int wantedEpisode {episodeInfo == item.info.cend() ?
                                 0 :
                                 std::atoi(episodeInfo->second.c_str())};
    if (!checked && wantedEpisode > 0 && url.find("/files/") != std::string::npos) {
        const std::string fileName {Utils::FileSystem::getFileName(
            HttpClient::urlDecode(url.substr(0, url.find('?'))))};
        const EpisodeNumbers found {episodeNumbersOf(fileName)};
        bool mismatch {false};
        if (found.season >= 0 && found.episode >= 0)
            mismatch = found.season != wantedSeason || found.episode != wantedEpisode;
        // A number without a season can't be compared with the episode of a later season.
        else if (found.episode >= 0 && wantedSeason <= 1)
            mismatch = found.episode != wantedEpisode;
        LOG(LogInfo) << "GuiMediaCenter: Wanted S" << wantedSeason << "E" << wantedEpisode
                     << ", the file is \"" << fileName << "\" ("
                     << (found.episode < 0 ? "no numbers in the name" :
                                             (mismatch ? "doesn't match" : "matches"))
                     << ")";
        if (mismatch) {
            char wanted[16];
            snprintf(wanted, sizeof(wanted), "S%02dE%02d", wantedSeason, wantedEpisode);
            MediaDialog* dialog {mDialogs.push(
                std::make_unique<MediaDialog>("Another episode?", 0.62f))};
            dialog->setMessage(
                "The source gave a file that doesn't look like " + std::string(wanted) + ":\n" +
                fileName + "\n\nThe torrent may not have this episode.");
            std::vector<MediaDialog::Option> options;
            // The sources of the episode, instead of the one chosen by the add-on.
            std::string linksUrl {item.path};
            const size_t play {linksUrl.rfind("/play")};
            if (play != std::string::npos) {
                linksUrl.replace(play, 5, "/links");
                MediaDialog::Option links;
                links.label = "Choose another source";
                links.onSelect = [this, dialog, linksUrl] {
                    dialog->close();
                    addonNavigate(linksUrl, false);
                };
                options.emplace_back(links);
            }
            MediaDialog::Option anyway;
            anyway.label = "Play anyway";
            anyway.onSelect = [this, dialog, url, item] {
                dialog->close();
                playStream(url, item, true);
            };
            options.emplace_back(anyway);
            MediaDialog::Option cancel;
            cancel.label = "Cancel";
            cancel.onSelect = [dialog] { dialog->close(); };
            options.emplace_back(cancel);
            dialog->setOptions(options);
            return;
        }
    }
    GuiVideoPlayer::StreamInfo stream;
    stream.active = true;
    auto info = [&item](const char* key) {
        const auto it {item.info.find(key)};
        return it == item.info.cend() ? std::string() : it->second;
    };
    stream.title = info("title").empty() ? stripAddonFormatting(item.label) : info("title");
    stream.showTitle = info("tvshowtitle");
    stream.originalTitle = info("originaltitle");
    stream.mediaType = info("mediatype");
    stream.imdb = info("imdbnumber").empty() ? info("code") : info("imdbnumber");
    stream.year = std::atoi(info("year").c_str());
    stream.season = std::atoi(info("season").c_str());
    stream.episode = std::atoi(info("episode").c_str());
    stream.watchKey = MediaLibrary::streamKey(item.path);
    // The English name of the first country, replaced by the code from TMDb when there's an
    // API key (looked up by the player).
    stream.originCountry = info("country");
    for (const std::string separator : {" / ", ",", ";"}) {
        const size_t found {stream.originCountry.find(separator)};
        if (found != std::string::npos)
            stream.originCountry = stream.originCountry.substr(0, found);
    }
    stream.originCountry = Utils::String::trim(stream.originCountry);
    if (stream.title.empty()) {
        // Started from a torrent or the history, named after the file in the address.
        std::string name {HttpClient::urlDecode(url.substr(0, url.find('?')))};
        name = Utils::FileSystem::getFileName(name);
        stream.title = name.empty() || name == "play" ? "Elementum" : name;
    }
    mList.stopScrolling();
    mGrid.stopScrolling();
    mWindow->pushGui(new GuiVideoPlayer(
        {url}, 0, 0.0,
        [this](const std::string&) {
            // The watched states and progress may have changed.
            addonRefresh();
        },
        stream));
}

void GuiMediaCenter::showAddonInformation(const AddonHost::Item& item)
{
    std::string text;
    auto add = [&text, &item](const char* key, const std::string& label) {
        const auto it {item.info.find(key)};
        if (it != item.info.cend() && !it->second.empty() && it->second != "0")
            text.append(label + ": " + stripAddonFormatting(it->second) + "\n");
    };
    add("tagline", "Tagline");
    add("year", "Year");
    add("genre", "Genre");
    const auto rating {item.info.find("rating")};
    if (rating != item.info.cend() && std::atof(rating->second.c_str()) > 0.0) {
        const auto votes {item.info.find("votes")};
        text.append("Rating: " + formatNumber(rating->second) +
                    (votes != item.info.cend() && !votes->second.empty() ?
                         " (" + votes->second + " votes)" :
                         "") +
                    "\n");
    }
    add("mpaa", "Age rating");
    add("director", "Director");
    add("writer", "Writer");
    add("studio", "Studio");
    add("country", "Country");
    add("premiered", "Premiered");
    add("status", "Status");
    const auto duration {item.info.find("duration")};
    if (duration != item.info.cend() && std::atoi(duration->second.c_str()) > 0)
        text.append("Duration: " + MediaLibrary::formatTime(std::atof(duration->second.c_str())) +
                    "\n");
    const auto plot {item.info.find("plot")};
    if (plot != item.info.cend())
        text.append("\n" + stripAddonFormatting(plot->second));
    mAddonDialogs->showText(stripAddonFormatting(item.label),
                            text.empty() ? "No information available" : text);
}

void GuiMediaCenter::openAddonContextMenu(const AddonHost::Item& item)
{
    MediaDialog* dialog {
        mDialogs.push(std::make_unique<MediaDialog>(stripAddonFormatting(item.label), 0.48f))};
    std::vector<MediaDialog::Option> options;
    for (const auto& [label, action] : item.contextMenu) {
        MediaDialog::Option option;
        option.label = stripAddonFormatting(label);
        const std::string command {action};
        option.onSelect = [this, dialog, command, item] {
            dialog->close();
            // Actions may play, the information of the entry is used for the player.
            mAddonRequestItem = item;
            // The watched state is kept here too, as the add-on has no library for it.
            const std::string lowerCommand {Utils::String::toLower(command)};
            const std::string watchKey {MediaLibrary::streamKey(item.path)};
            if (!watchKey.empty() && lowerCommand.find("/watched/add") != std::string::npos) {
                MediaLibrary::getInstance().setWatched(watchKey, true);
                addonRefresh();
            }
            else if (!watchKey.empty() &&
                     lowerCommand.find("/watched/remove") != std::string::npos) {
                MediaLibrary::getInstance().setWatched(watchKey, false);
                addonRefresh();
            }
            if (lowerCommand == "action(info)")
                showAddonInformation(item);
            else
                mAddonDialogs->runBuiltin(command);
            if (mAddonRequest && mAddonRequestKind == AddonRequestKind::RUN)
                mAddonRequestItem = item;
        };
        options.emplace_back(option);
    }
    MediaDialog::Option information;
    information.label = "Information";
    information.onSelect = [this, dialog, item] {
        dialog->close();
        showAddonInformation(item);
    };
    options.emplace_back(information);
    MediaDialog::Option view;
    view.label = "View";
    view.value = [this] { return viewName(mView); };
    view.onSelect = [this] { cycleView(); };
    view.onAdjust = [this](int) { cycleView(); };
    options.emplace_back(view);
    // The trailer is looked up from TMDb, so the add-on isn't involved. It's only offered for
    // entries of the add-on that are a movie or a series (the TMDb id is in the address).
    // The trailer is looked up from TMDb (with the API key of the Media settings) or searched
    // on YouTube, so the add-on isn't involved. Only for a movie or a series of the add-on.
    bool series {false};
    int tmdbId {0};
    if (trailerTarget(item, series, tmdbId)) {
        MediaDialog::Option trailer;
        trailer.label = MediaMetadata::isEnabled() && tmdbId > 0 ? "Watch trailer from TMDb" :
                                                                   "Watch trailer from YouTube";
        const AddonHost::Item target {item};
        trailer.onSelect = [this, dialog, target] {
            dialog->close();
            watchTrailerOf(target);
        };
        // The first row, so that it's seen without scrolling.
        options.insert(options.begin(), trailer);
    }
    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiMediaCenter::watchTrailerOf(const AddonHost::Item& item)
{
    bool series {false};
    int tmdbId {0};
    if (!trailerTarget(item, series, tmdbId))
        return;
    const auto info = [&item](const std::string& key) {
        const auto it {item.info.find(key)};
        return it == item.info.cend() ? std::string() : it->second;
    };
    const std::string showTitle {info("tvshowtitle")};
    const std::string title {stripAddonFormatting(
        !showTitle.empty() ? showTitle : !info("title").empty() ? info("title") : item.label)};
    watchTrailer(series, tmdbId, title, std::atoi(info("year").c_str()));
}

void GuiMediaCenter::watchTrailer(bool series, int tmdbId, const std::string& title, int year)
{
    auto task {Trailer::start(series, tmdbId, title, year)};
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>("Trailer", 0.40f))};
    dialog->setMessage("Looking for the trailer of " + title + "...");
    std::vector<MediaDialog::Option> options;
    MediaDialog::Option cancel;
    cancel.label = "Cancel";
    cancel.onSelect = [dialog, task] {
        task->cancel = true;
        dialog->close();
    };
    options.emplace_back(cancel);
    dialog->setOptions(options);
    dialog->setCancelCallback([task] { task->cancel = true; });
    auto finished {std::make_shared<bool>(false)};
    auto shownStage {std::make_shared<int>(Trailer::Task::LOOKING)};
    dialog->setUpdateCallback([this, dialog, task, finished, shownStage, title, year](int) {
        if (!*finished && task->stage != *shownStage) {
            *shownStage = task->stage;
            dialog->setMessage(*shownStage == Trailer::Task::DOWNLOADING ?
                                   "Downloading yt-dlp to " + Trailer::ownYtDlp() + "..." :
                               *shownStage == Trailer::Task::UPDATING ?
                                   "Updating yt-dlp..." :
                                   "Looking for the trailer of " + title + "...");
        }
        if (*finished || !task->done)
            return;
        *finished = true;
        dialog->close();
        if (task->cancel)
            return;
        if (task->url.empty()) {
            MediaDialog* failure {mDialogs.push(std::make_unique<MediaDialog>("Trailer", 0.55f))};
            failure->setMessage(task->error);
            std::vector<MediaDialog::Option> failureOptions;
            MediaDialog::Option ok;
            ok.label = "OK";
            ok.onSelect = [failure] { failure->close(); };
            failureOptions.emplace_back(ok);
            failure->setOptions(failureOptions);
            return;
        }
        GuiVideoPlayer::StreamInfo stream;
        stream.trailer = true;
        stream.trailerAudio = task->audioUrl;
        stream.title = title + " - Trailer";
        stream.year = year;
        mList.stopScrolling();
        mGrid.stopScrolling();
        mWindow->pushGui(new GuiVideoPlayer({task->url}, 0, 0.0, [](const std::string&) {},
                                            stream));
    });
}

void GuiMediaCenter::runInstallTask(const std::string& title,
                                    const std::function<AddonStore::Result(InstallTask&)>& task,
                                    const std::function<void(const AddonStore::Result&)>& done)
{
    if (mInstall)
        return;
    auto install = std::make_shared<InstallTask>();
    install->title = title;
    install->doneCallback = done;
    mInstall = install;
    std::thread([install, task] {
        install->result = task(*install);
        install->done = true;
    }).detach();
    refreshHelpPrompts();
}

void GuiMediaCenter::pollInstallTask()
{
    if (!mInstall || !mInstall->done)
        return;
    const std::shared_ptr<InstallTask> install {mInstall};
    mInstall.reset();
    if (install->doneCallback)
        install->doneCallback(install->result);
    refreshHelpPrompts();
}

void GuiMediaCenter::startInstall()
{
    runInstallTask(
        "Installing Elementum",
        [](InstallTask& task) {
            auto progress = [&task](const std::string& message, int percent) {
                task.progress(message, percent);
            };
            AddonStore& store {AddonStore::getInstance()};
            AddonStore::Result result {
                store.installFromRepository(AddonStore::STREAMING_ADDON, progress, &task.cancel)};
            if (!result.success)
                return result;
            // The search provider, without it nothing can be found.
            const AddonStore::Result provider {
                store.installFromRepository(AddonStore::PROVIDER_ADDON, progress, &task.cancel)};
            if (!provider.success)
                result.error = provider.error;
            return result;
        },
        [this](const AddonStore::Result& result) {
            if (!result.success)
                mWindow->queueInfoPopup("INSTALLATION FAILED: " + result.error, 6000);
            else if (!result.error.empty())
                mWindow->queueInfoPopup("ELEMENTUM INSTALLED, BUT BURST FAILED: " + result.error,
                                        6000);
            else
                mWindow->queueInfoPopup("ELEMENTUM AND ELEMENTUM BURST HAVE BEEN INSTALLED",
                                        4000);
            if (isAddonSection()) {
                mLevels.clear();
                loadAddonContent(0);
            }
        });
    if (isAddonSection())
        loadAddonContent(0);
}

void GuiMediaCenter::restartAddon()
{
    // Gets the add-on going again if it has stopped answering, e.g. after a search that
    // never finished.
    cancelAddonRequest();
    AddonHost::getInstance().restartDaemon();
    mWindow->queueInfoPopup("RESTARTING ELEMENTUM", 3000);
    if (isAddonSection())
        loadAddonContent(mLevels.empty() ? mSourcesCursor : mLevels.back().cursor);
}

//--------------------------------------------------------------------------------------------------
// Views and the information panel.
//--------------------------------------------------------------------------------------------------

std::string GuiMediaCenter::currentContentType() const
{
    if (!isAddonSection())
        return "files";
    return mAddonContentType.empty() ? "menus" : mAddonContentType;
}

GuiMediaCenter::View GuiMediaCenter::viewFor(const std::string& contentType) const
{
    // Stored as "type=view" pairs, e.g. "movies=wall;files=list".
    for (const auto& pair : Utils::String::delimitedStringToVector(
             Settings::getInstance()->getString("MediaListViews"), ";")) {
        const size_t equals {pair.find('=')};
        if (equals == std::string::npos || pair.substr(0, equals) != contentType)
            continue;
        const std::string name {pair.substr(equals + 1)};
        for (View view : {View::LIST, View::FANART, View::WIDELIST, View::POSTER, View::SHIFT,
                          View::INFOWALL, View::WALL}) {
            if (Utils::String::toLower(viewName(view)) == name)
                return view;
        }
        // The views of earlier versions.
        if (name == "info")
            return View::LIST;
        return View::LIST;
    }
    if (contentType == "movies" || contentType == "tvshows")
        return View::WALL;
    if (contentType == "seasons" || contentType == "episodes" || contentType == "files")
        return View::LIST;
    return View::WIDELIST;
}

std::string GuiMediaCenter::viewName(View view)
{
    switch (view) {
        case View::FANART:
            return "Fanart";
        case View::WIDELIST:
            return "WideList";
        case View::POSTER:
            return "Poster";
        case View::SHIFT:
            return "Shift";
        case View::INFOWALL:
            return "InfoWall";
        case View::WALL:
            return "Wall";
        default:
            return "List";
    }
}

void GuiMediaCenter::setView(View view)
{
    const std::string type {currentContentType()};
    std::string value;
    for (const auto& pair : Utils::String::delimitedStringToVector(
             Settings::getInstance()->getString("MediaListViews"), ";")) {
        if (!pair.empty() && pair.substr(0, pair.find('=')) != type)
            value.append((value.empty() ? "" : ";") + pair);
    }
    value.append((value.empty() ? "" : ";") + type + "=" + Utils::String::toLower(viewName(view)));
    Settings::getInstance()->setString("MediaListViews", value);
    Settings::getInstance()->saveFile();

    const int cursor {mList.getCursor()};
    mView = view;
    layoutContent();
    // The values of the list depend on the view.
    std::vector<MediaListView::Item> items;
    for (const auto& entry : mEntries)
        items.emplace_back(makeListItem(entry));
    mList.setItems(items, cursor);
    updateGrid();
    updateTexts();
    mInfoImagePath.clear();
    mFanartPath.clear();
    updateInfoPanel();
    updateOptionsPanel();
    refreshHelpPrompts();
}

void GuiMediaCenter::cycleView()
{
    static const std::vector<View> order {View::LIST,  View::FANART,   View::WIDELIST, View::POSTER,
                                          View::SHIFT, View::INFOWALL, View::WALL};
    const auto it {std::find(order.cbegin(), order.cend(), mView)};
    const size_t index {it == order.cend() ? 0 : static_cast<size_t>(it - order.cbegin())};
    setView(order[(index + 1) % order.size()]);
    mDialogs.refresh();
    mWindow->queueInfoPopup("VIEW: " + Utils::String::toUpper(viewName(mView)), 1500);
}

std::vector<std::pair<std::string, std::string>> GuiMediaCenter::sortChoices() const
{
    if (isAddonSection()) {
        return {{"default", "Default"},
                {"name", "Name"},
                {"year", "Year"},
                {"rating", "Rating"},
                {"duration", "Duration"}};
    }
    return {{"default", "Default"}, {"name", "Name"}, {"size", "Size"}};
}

void GuiMediaCenter::loadSorting()
{
    // Stored as "type=key:order" pairs, e.g. "movies=year:desc".
    mSortKey = "default";
    mSortDescending = false;
    const std::string type {currentContentType()};
    for (const auto& pair : Utils::String::delimitedStringToVector(
             Settings::getInstance()->getString("MediaListSorts"), ";")) {
        const size_t equals {pair.find('=')};
        if (equals == std::string::npos || pair.substr(0, equals) != type)
            continue;
        const std::string value {pair.substr(equals + 1)};
        const size_t colon {value.find(':')};
        const std::string key {value.substr(0, colon)};
        for (const auto& choice : sortChoices()) {
            if (choice.first == key)
                mSortKey = key;
        }
        mSortDescending = colon != std::string::npos && value.substr(colon + 1) == "desc";
    }
}

void GuiMediaCenter::saveSorting()
{
    const std::string type {currentContentType()};
    std::string value;
    for (const auto& pair : Utils::String::delimitedStringToVector(
             Settings::getInstance()->getString("MediaListSorts"), ";")) {
        if (!pair.empty() && pair.substr(0, pair.find('=')) != type)
            value.append((value.empty() ? "" : ";") + pair);
    }
    value.append((value.empty() ? "" : ";") + type + "=" + mSortKey +
                 (mSortDescending ? ":desc" : ":asc"));
    Settings::getInstance()->setString("MediaListSorts", value);
    Settings::getInstance()->saveFile();
}

void GuiMediaCenter::openOptionsPanel()
{
    updateOptionsPanel();
    mOptions.setCursor(0);
    setFocus(Focus::OPTIONS);
}

void GuiMediaCenter::updateOptionsPanel()
{
    std::string sortName {"Default"};
    for (const auto& [key, label] : sortChoices()) {
        if (key == mSortKey)
            sortName = label;
    }
    int count {0};
    for (const auto& entry : mEntries) {
        if (entry.type == EntryType::SOURCE || entry.type == EntryType::FOLDER ||
            entry.type == EntryType::VIDEO || entry.type == EntryType::ADDON_ITEM)
            ++count;
    }
    std::vector<MediaListView::Item> items(4);
    items[0].label = "Viewtype";
    items[0].value = viewName(mView);
    items[1].label = "Sort by";
    items[1].value = sortName;
    items[2].label = "Order";
    items[2].value = mSortDescending ? "Descending" : "Ascending";
    items[3].label = mFilter == 1 ? "Unwatched" : (mFilter == 2 ? "Watched" : "All videos");
    items[3].value = std::to_string(count);
    mOptions.setItems(items, mOptions.getCursor());
}

void GuiMediaCenter::changeOption(int direction)
{
    switch (mOptions.getCursor()) {
        case 0: {
            static const std::vector<View> order {View::LIST,     View::FANART, View::WIDELIST,
                                                  View::POSTER,   View::SHIFT,  View::INFOWALL,
                                                  View::WALL};
            const auto it {std::find(order.cbegin(), order.cend(), mView)};
            const int count {static_cast<int>(order.size())};
            const int index {it == order.cend() ? 0 : static_cast<int>(it - order.cbegin())};
            setView(order[((index + direction) % count + count) % count]);
            break;
        }
        case 1: {
            const auto choices {sortChoices()};
            mSortKey = cycleChoice(choices, mSortKey, direction);
            // Names are sorted from A, numbers from the largest.
            mSortDescending = mSortKey != "name" && mSortKey != "default";
            saveSorting();
            representEntries();
            break;
        }
        case 2: {
            mSortDescending = !mSortDescending;
            saveSorting();
            representEntries();
            break;
        }
        case 3: {
            mFilter = (mFilter + (direction > 0 ? 1 : 2)) % 3;
            representEntries();
            break;
        }
        default: {
            break;
        }
    }
    updateOptionsPanel();
}

int GuiMediaCenter::contentCursor() const { return mList.getCursor(); }

void GuiMediaCenter::setContentCursor(int cursor) { mList.setCursor(cursor); }

void GuiMediaCenter::layoutContent()
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    auto W = [screenWidth](float fraction) { return std::round(screenWidth * fraction); };
    auto H = [screenHeight](float fraction) { return std::round(screenHeight * fraction); };
    const float top {H(0.115f)};
    const float bottom {H(0.925f)};

    // The rows of the list fill the height.
    auto placeList = [&](float x, float width, float y, float height) {
        const int rows {std::max(3, static_cast<int>(height / mList.getRowHeight()))};
        if (mList.getSize() != glm::vec2 {width, rows * mList.getRowHeight()})
            mList.setSize(width, rows * mList.getRowHeight());
        mList.setPosition(x, y);
    };
    auto placeGrid = [&](float x, float width, int columns) {
        mGrid.setColumns(columns);
        if (mGrid.getSize() != glm::vec2 {width, bottom - top})
            mGrid.setSize(width, bottom - top);
        mGrid.setPosition(x, top);
    };

    mPosterBox = glm::vec4 {0.0f};
    mFanartBox = glm::vec4 {0.0f};
    for (TextComponent* text :
         {mInfoTitle.get(), mInfoTagline.get(), mInfoDetails.get(), mInfoPlot.get()})
        text->setVisible(true);
    // The information panel to the left, used by several views.
    auto leftPanel = [&](float width) {
        const float x {W(0.018f)};
        mInfoTitle->setSize(width, H(0.11f));
        mInfoTitle->setPosition(x, top + H(0.02f));
        mInfoTagline->setSize(width, H(0.075f));
        mInfoTagline->setPosition(x, top + H(0.13f));
        // The texts are placed below each other in updateInfoPanel(), by their heights.
        mInfoDetails->setSize(width, H(0.30f));
        mInfoDetails->setPosition(x, top + H(0.215f));
        mInfoPlot->setSize(width, bottom - top - H(0.40f));
        mInfoPlot->setPosition(x, top + H(0.395f));
    };

    switch (mView) {
        case View::LIST: {
            leftPanel(W(0.275f));
            placeList(W(0.31f), W(0.38f), top, bottom - top);
            mPosterBox = glm::vec4 {W(0.705f), top + H(0.03f), W(0.28f), H(0.74f)};
            break;
        }
        case View::FANART: {
            placeList(0.0f, W(0.375f), top, bottom - top);
            mFanartBox = glm::vec4 {W(0.388f), top, W(0.605f), W(0.605f) * 9.0f / 16.0f};
            mInfoTitle->setVisible(false);
            mInfoTagline->setVisible(false);
            mInfoDetails->setVisible(false);
            mInfoPlot->setSize(W(0.59f), bottom - (mFanartBox.y + mFanartBox.w) - H(0.04f));
            mInfoPlot->setPosition(W(0.393f), mFanartBox.y + mFanartBox.w + H(0.02f));
            break;
        }
        case View::WIDELIST: {
            mInfoTitle->setVisible(false);
            mInfoTagline->setVisible(false);
            mInfoDetails->setVisible(false);
            mInfoPlot->setVisible(false);
            placeList(W(0.02f), W(0.96f), top, bottom - top);
            break;
        }
        case View::POSTER: {
            mPosterBox = glm::vec4 {W(0.053f), H(0.19f), W(0.25f), H(0.67f)};
            const float stripX {W(0.323f)};
            mStrip.setPosition(stripX, H(0.19f));
            mStrip.setSize(screenWidth - stripX, H(0.35f));
            mStrip.setLayout(W(0.13f), H(0.35f), W(0.035f), 0, true, false);
            mInfoTitle->setSize(W(0.63f), H(0.07f));
            mInfoTitle->setPosition(W(0.336f), H(0.565f));
            mInfoTagline->setVisible(false);
            mInfoDetails->setVisible(false);
            mInfoPlot->setSize(W(0.63f), H(0.24f));
            mInfoPlot->setPosition(W(0.336f), H(0.645f));
            break;
        }
        case View::SHIFT: {
            mInfoTitle->setVisible(false);
            mInfoTagline->setVisible(false);
            mInfoDetails->setSize(W(0.47f), H(0.17f));
            mInfoDetails->setPosition(W(0.016f), H(0.135f));
            mInfoPlot->setSize(W(0.47f), H(0.17f));
            mInfoPlot->setPosition(W(0.516f), H(0.135f));
            mStrip.setPosition(-W(0.064f), H(0.345f));
            mStrip.setSize(screenWidth + W(0.13f), H(0.41f));
            mStrip.setLayout(W(0.153f), H(0.405f), W(0.04f), 1, false, true);
            break;
        }
        case View::INFOWALL: {
            leftPanel(W(0.27f));
            placeGrid(W(0.325f), W(0.66f), 5);
            break;
        }
        case View::WALL: {
            mInfoTitle->setVisible(false);
            mInfoTagline->setVisible(false);
            mInfoDetails->setVisible(false);
            mInfoPlot->setVisible(false);
            placeGrid(W(0.035f), W(0.94f), 7);
            break;
        }
    }

    mList.setVisible(usesList());
    mGrid.setVisible(usesGrid());
    mStrip.setVisible(usesStrip());
    mGrid.setFocused(mFocus == Focus::CONTENT);
    mStrip.setFocused(mFocus == Focus::CONTENT);
}

std::string GuiMediaCenter::entryImage(const Entry& entry, bool urgent)
{
    switch (entry.type) {
        case EntryType::ADDON_ITEM: {
            if (entry.addonIndex < 0 || entry.addonIndex >= static_cast<int>(mAddonItems.size()))
                return "";
            const AddonHost::Item& item {mAddonItems[entry.addonIndex]};
            for (const char* key : {"poster", "thumb"}) {
                const auto it {item.art.find(key)};
                if (it != item.art.cend() && !it->second.empty())
                    return MediaCoverCache::getInstance().get(it->second, urgent);
            }
            if (!item.thumbnail.empty() && item.thumbnail[0] != '/')
                return MediaCoverCache::getInstance().get(item.thumbnail, urgent);
            return "";
        }
        case EntryType::VIDEO:
        case EntryType::FOLDER:
        case EntryType::SOURCE: {
            const bool folder {entry.type != EntryType::VIDEO};
            MediaMetadata::Info info;
            if (MediaMetadata::isEnabled() &&
                MediaMetadata::getInstance().get(entry.path, folder, urgent, info) &&
                info.found && !info.poster.empty())
                return info.poster;
            return folder ? MediaThumbnails::getInstance().getFolderPreview(entry.path, urgent) :
                            MediaThumbnails::getInstance().getVideoPreview(entry.path, urgent);
        }
        default: {
            return "";
        }
    }
}

std::string GuiMediaCenter::entryFanart(const Entry& entry)
{
    if (entry.type == EntryType::ADDON_ITEM && entry.addonIndex >= 0 &&
        entry.addonIndex < static_cast<int>(mAddonItems.size())) {
        const AddonHost::Item& item {mAddonItems[entry.addonIndex]};
        const auto it {item.art.find("fanart")};
        if (it != item.art.cend())
            return MediaCoverCache::getInstance().get(it->second, true);
        return "";
    }
    if (entry.type == EntryType::VIDEO || entry.type == EntryType::FOLDER ||
        entry.type == EntryType::SOURCE) {
        MediaMetadata::Info info;
        if (MediaMetadata::getInstance().get(entry.path, entry.type != EntryType::VIDEO, false,
                                             info) &&
            info.found)
            return info.fanart;
    }
    return "";
}

void GuiMediaCenter::updateGrid()
{
    if (usesList())
        return;
    std::vector<MediaGridView::Item> items;
    const std::vector<MediaListView::Item>& listItems {mList.getItems()};
    for (size_t i {0}; i < mEntries.size(); ++i) {
        MediaGridView::Item item;
        item.label = i < listItems.size() ? listItems[i].label : mEntries[i].name;
        item.image = entryImage(mEntries[i], false);
        if (i < listItems.size()) {
            const std::string& icon {listItems[i].icon};
            // The menu icons, the watched marks are shown differently.
            if (!icon.empty() && icon[0] == '/')
                item.icon = icon;
            else if (!icon.empty() && Utils::String::startsWith(icon, ":/") &&
                     mEntries[i].type != EntryType::VIDEO && mEntries[i].type != EntryType::ADDON_ITEM)
                item.icon = icon;
            item.dimmed = listItems[i].dimmed;
            item.progress = listItems[i].progress;
            item.watched = icon == ICON_WATCHED;
        }
        if (mEntries[i].type == EntryType::PARENT)
            item.icon = ICON_FOLDER;
        items.emplace_back(item);
    }
    if (usesGrid())
        mGrid.setItems(items, mList.getCursor());
    else
        mStrip.setItems(items, mList.getCursor());
}

void GuiMediaCenter::refreshImages()
{
    // Posters that have been downloaded in the meantime.
    if (usesList())
        return;
    int first {0};
    int last {-1};
    if (usesGrid())
        mGrid.getVisibleRange(first, last);
    else
        mStrip.getVisibleRange(first, last);
    const int count {usesGrid() ? mGrid.size() : mStrip.size()};
    const int cursor {mList.getCursor()};
    for (int i {std::max(0, first)};
         i <= last && i < static_cast<int>(mEntries.size()) && i < count; ++i) {
        const std::string& current {usesGrid() ? mGrid.getItems()[i].image :
                                                 mStrip.getItems()[i].image};
        if (!current.empty())
            continue;
        const std::string image {entryImage(mEntries[i], i == cursor)};
        if (image.empty())
            continue;
        if (usesGrid())
            mGrid.setImage(i, image);
        else
            mStrip.setImage(i, image);
    }
}

void GuiMediaCenter::updateInfoPanel()
{
    const Entry* entry {selectedEntry()};
    InfoData info;
    std::string image;
    std::string fanart;

    if (entry != nullptr && entry->type == EntryType::ADDON_ITEM && entry->addonIndex >= 0 &&
        entry->addonIndex < static_cast<int>(mAddonItems.size())) {
        // The information of the add-on, which it gets from TMDb.
        const AddonHost::Item& item {mAddonItems[entry->addonIndex]};
        auto value = [this, entry](const char* key) { return entryInfo(*entry, key); };
        info.title = value("title").empty() ? stripAddonFormatting(item.label) : value("title");
        if (!value("season").empty() && !value("episode").empty()) {
            char code[32];
            snprintf(code, sizeof(code), "S%02dE%02d  ", std::atoi(value("season").c_str()),
                     std::atoi(value("episode").c_str()));
            info.title = code + info.title;
        }
        info.year = value("year");
        info.tagline = value("tagline");
        info.plot = value("plot").empty() ? value("plotoutline") : value("plot");
        info.genre = value("genre");
        info.director = value("director");
        info.writer = value("writer");
        info.duration = entryDuration(*entry);
        std::string details;
        auto line = [&details](const std::string& label, const std::string& text) {
            if (!text.empty())
                details.append(label + ":  " + text + "\n");
        };
        line("Genre", info.genre);
        line("Director", info.director);
        line("Writer", info.writer);
        std::string rating {formatNumber(value("rating"))};
        if (!rating.empty() && rating != "0.0") {
            const std::string votes {value("votes")};
            line("Rating", rating + (votes.empty() ? "" : "  (" + votes + " votes)"));
        }
        info.details = details;
        image = entryImage(*entry, true);
        fanart = entryFanart(*entry);
    }
    else if (entry != nullptr &&
             (entry->type == EntryType::VIDEO || entry->type == EntryType::FOLDER ||
              entry->type == EntryType::SOURCE)) {
        const bool folder {entry->type != EntryType::VIDEO};
        MediaMetadata::Info metadata;
        const bool known {MediaMetadata::getInstance().get(entry->path, folder, true, metadata)};
        if (known && metadata.found) {
            info.title = metadata.title;
            if (metadata.type == "episode") {
                char code[32];
                snprintf(code, sizeof(code), "S%02dE%02d  ", metadata.season, metadata.episode);
                info.title = metadata.showTitle + "\n" + code + metadata.episodeTitle;
            }
            info.year = metadata.year > 0 ? std::to_string(metadata.year) : "";
            info.genre = metadata.genres;
            info.plot = metadata.plot;
            std::string details;
            if (!info.genre.empty())
                details.append("Genre:  " + info.genre + "\n");
            if (metadata.rating > 0.0) {
                char buffer[16];
                snprintf(buffer, sizeof(buffer), "%.1f", metadata.rating);
                details.append(std::string("Rating:  ") + buffer + "\n");
            }
            if (!info.year.empty())
                details.append("Year:  " + info.year + "\n");
            info.details = details;
            fanart = metadata.fanart;
        }
        else {
            info.title = entry->name;
            if (!MediaMetadata::isEnabled())
                info.plot = "Titles, descriptions and posters are shown when a TMDb API key has "
                            "been set in the media settings.";
            else if (known)
                info.plot = "No information was found, X lets you choose the title.";
        }
        if (entry->type == EntryType::VIDEO && info.plot.empty())
            info.plot = entry->path;
        info.duration = entryDuration(*entry);
        image = entryImage(*entry, true);
    }
    else if (entry != nullptr) {
        info.title = entry->name;
    }
    mInfo = info;

    // The texts depend on the view.
    std::string title {info.title};
    if (mView == View::POSTER && !info.year.empty() &&
        info.title.find(info.year) == std::string::npos)
        title.append(" (" + info.year + ")");
    MediaStyle::setText(*mInfoTitle, title);
    MediaStyle::setText(*mInfoTagline, info.tagline);
    MediaStyle::setText(*mInfoDetails, Utils::String::trim(info.details));
    MediaStyle::setText(*mInfoPlot, info.plot);
    const double duration {info.duration};
    MediaStyle::setText(*mDurationText, duration > 0.0 ? formatDuration(duration, true) : "");

    // The texts of the panel to the left follow each other, the plot uses the rest.
    if (mView == View::LIST || mView == View::INFOWALL) {
        const float screenHeight {Renderer::getScreenHeight()};
        const float gap {std::round(screenHeight * 0.018f)};
        const float bottom {std::round(screenHeight * 0.925f)};
        float y {mInfoTitle->getPosition().y};
        auto place = [&y, gap](TextComponent& text) {
            if (text.getValue().empty())
                return;
            text.setPosition(text.getPosition().x, y);
            y += std::min(text.getSize().y, text.getTextCache()->metrics.size.y) + gap;
        };
        place(*mInfoTitle);
        place(*mInfoTagline);
        if (!mInfoDetails->getValue().empty()) {
            place(*mInfoDetails);
            y += gap * 0.5f;
        }
        mInfoPlot->setPosition(mInfoPlot->getPosition().x, y);
        if (std::abs(mInfoPlot->getSize().y - (bottom - y)) > 1.0f)
            mInfoPlot->setSize(mInfoPlot->getSize().x, std::max(gap, bottom - y));
    }

    if (image != mInfoImagePath) {
        mInfoImagePath = image;
        if (!image.empty())
            mInfoImage->setImage(image);
    }
    if (fanart != mFanartPath) {
        mFanartPath = fanart;
        if (!fanart.empty()) {
            mFanartImage->setResize(Renderer::getScreenWidth(), Renderer::getScreenHeight());
            mFanartImage->setImage(fanart);
            mFanartBoxImage->setImage(fanart);
        }
    }
}

void GuiMediaCenter::renderInfoPanel(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};

    // Panels behind the texts.
    if (mView == View::FANART) {
        MediaStyle::drawRect(trans, 0.0f, mList.getPosition().y - screenHeight * 0.01f,
                             mList.getSize().x, screenHeight * 0.82f, 0x00000070);
    }
    else if (mView == View::SHIFT) {
        MediaStyle::drawRect(trans, 0.0f, screenHeight * 0.12f, screenWidth, screenHeight * 0.19f,
                             0x00000090);
    }

    if (mPosterBox.z > 0.0f && !mInfoImagePath.empty()) {
        renderImageInBox(trans, *mInfoImage, mPosterBox.x, mPosterBox.y, mPosterBox.z,
                         mPosterBox.w);
    }
    if (mFanartBox.z > 0.0f) {
        if (!mFanartPath.empty()) {
            renderImageInBox(trans, *mFanartBoxImage, mFanartBox.x, mFanartBox.y, mFanartBox.z,
                             mFanartBox.w);
        }
        else if (!mInfoImagePath.empty()) {
            // Without fanart the poster is shown instead.
            renderImageInBox(trans, *mInfoImage, mFanartBox.x, mFanartBox.y, mFanartBox.z,
                             mFanartBox.w);
        }
    }

    for (TextComponent* text :
         {mInfoTitle.get(), mInfoTagline.get(), mInfoDetails.get(), mInfoPlot.get()}) {
        if (text->isVisible() && !text->getValue().empty())
            text->render(trans);
    }
}

void GuiMediaCenter::openIdentifyDialog(const Entry& entry)
{
    const bool folder {entry.type != EntryType::VIDEO};
    const std::string path {entry.path};
    const SubtitleDownloader::Query parsed {
        SubtitleDownloader::parseVideoPath(folder ? path + ".mkv" : path)};
    mAddonDialogs->openTextInput("SEARCH TMDB", parsed.title, [this, path, folder](
                                                                  const std::string& query) {
        if (Utils::String::trim(query).empty())
            return;
        auto results = std::make_shared<std::vector<MediaMetadata::Candidate>>();
        auto error = std::make_shared<std::string>();
        auto done = std::make_shared<std::atomic<bool>>(false);
        MediaMetadata::getInstance().search(
            query, [results, error, done](std::vector<MediaMetadata::Candidate> candidates,
                                          std::string message) {
                *results = std::move(candidates);
                *error = message;
                *done = true;
            });
        // The results are shown once they have arrived.
        MediaDialog* dialog {
            mDialogs.push(std::make_unique<MediaDialog>("Choose the title", 0.62f))};
        MediaDialog::Option waiting;
        waiting.label = "Searching...";
        waiting.dimmed = true;
        dialog->setOptions({waiting});
        dialog->setUpdateCallback([this, dialog, results, error, done, path, folder](int) {
            if (!*done)
                return;
            dialog->setUpdateCallback(nullptr);
            std::vector<MediaDialog::Option> options;
            for (const auto& candidate : *results) {
                MediaDialog::Option option;
                option.label = candidate.title +
                               (candidate.year > 0 ? " (" + std::to_string(candidate.year) + ")" :
                                                     "");
                option.value = [candidate] {
                    return candidate.type == "tv" ? std::string("Series") : std::string("Movie");
                };
                option.detail = candidate.overview;
                option.onSelect = [this, dialog, candidate, path, folder] {
                    dialog->close();
                    MediaMetadata::getInstance().identify(path, folder, candidate);
                    mInfoImagePath.clear();
                    mWindow->queueInfoPopup("UPDATING THE INFORMATION", 2000);
                };
                options.emplace_back(option);
            }
            if (options.empty()) {
                MediaDialog::Option none;
                none.label = error->empty() ? "Nothing found" : *error;
                none.dimmed = true;
                options.emplace_back(none);
            }
            dialog->setOptions(options);
        });
    });
}
