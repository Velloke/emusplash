//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiMediaCenter.h
//
//  Fullscreen media screen with a black sidebar to the left and the content to the right,
//  on top of the background of the currently selected theme. Video sources are folders
//  added by the user, which are browsed and played using the built-in video player.
//  The Elementum section shows the listings of the streaming add-on, run by AddonHost.
//  Lists can be shown in seven views, chosen in the options panel to the left of the list,
//  which also sorts and filters the list.
//

#ifndef ES_APP_GUIS_GUI_MEDIA_CENTER_H
#define ES_APP_GUIS_GUI_MEDIA_CENTER_H

#include "GuiComponent.h"
#include "components/ImageComponent.h"
#include "media/AddonDialogs.h"
#include "media/AddonHost.h"
#include "media/AddonStore.h"
#include "media/MediaComponents.h"
#include "media/MediaLibrary.h"
#include "media/MediaMetadata.h"
#if defined(EMUSPLASH_DUBX)
#include "dub/GuiDubSettings.h"
#include "dubx/GuiDubxSettings.h"
#endif

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class GuiMediaCenter : public GuiComponent, public AddonDialogs::Delegate
{
public:
    GuiMediaCenter();
    ~GuiMediaCenter();

    bool isFullscreenGui() const override { return true; }

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

    // Requests of the streaming add-on.
    void addonNavigate(const std::string& url, bool replace) override;
    void addonRun(const std::string& url) override;
    void addonRefresh() override;
    void addonPlay(const std::string& url) override;
    std::string addonSearchAddress() override;
    void addonSearchPreview(const std::string& address, const std::string& text) override;
    void addonSearchUpdate(int deltaTime) override;
    void addonSearchPreviewEnd(bool cancelled, const std::string& text) override;

private:
    enum class Focus {
        SIDEBAR,
        CONTENT,
        OPTIONS // The view options panel to the left.
    };

    enum class EntryType {
        ADD_SOURCE,
        SOURCE,
        PARENT,
        FOLDER,
        VIDEO,
        EMPTY,
        ADDON_ITEM,     // An entry of a listing of the streaming add-on.
        ADDON_INSTALL,  // Installs the streaming add-on.
        ADDON_RETRY     // Starts the streaming add-on again after a failure.
    };

    struct Entry {
        EntryType type;
        std::string name;
        std::string path;
        int sourceIndex {-1};
        int addonIndex {-1};
    };

    // One level of the folder hierarchy, the top level (the list of sources) is not included.
    // In the Elementum section the path is the address of the listing.
    struct Level {
        std::string path;
        std::string title;
        int sourceIndex;
        int cursor;
    };

    enum class View {
        LIST,
        FANART,
        WIDELIST,
        POSTER,
        SHIFT,
        INFOWALL,
        WALL
    };

    // The information about the selected entry shown next to the list.
    struct InfoData {
        std::string title;
        std::string year;
        std::string tagline;
        std::string plot;
        std::string genre;
        std::string director;
        std::string writer;
        std::string details;
        double duration {0.0};
    };

    enum class AddonRequestKind {
        LOAD,  // The listing of the current level.
        OPEN,  // An entry was chosen, its listing is opened as a new level.
        RUN,   // An action, a listing in the result is ignored.
        PLAY   // A playable entry.
    };

    struct InstallTask;

    void setFocus(Focus focus);
    void selectSection(int section);
    bool showAddButton() const;
    // The buttons next to the title, the add button is only shown when there are sources.
    enum TopButton {
        TOP_NONE = -1,
        TOP_ADD = 0,
        TOP_SETTINGS = 1
    };
    void layoutTopButtons();
    void updatePreview(bool selectionChanged);
    void addSource();
    void loadContent(int cursor);
    // Shows the entries that have been built in mEntries, sorted and filtered.
    void presentEntries(int cursor);
    void updateSearchPreview(int deltaTime);
    // Shows the entries again, e.g. after the sorting has changed, keeping the selection.
    void representEntries();
    std::vector<Entry> sortAndFilter(const std::vector<Entry>& entries) const;
    bool entryWatched(const Entry& entry) const;
    std::string entryInfo(const Entry& entry, const char* key) const;
    double entryDuration(const Entry& entry) const;
    MediaListView::Item makeListItem(const Entry& entry) const;
    void updateTexts();
    void activateEntry();
    void goBack();
    const Entry* selectedEntry() const;

    void playVideo(const std::string& path, bool askResume);
    void startPlayer(const std::string& path, double startPosition);
    void onPlaybackFinished(const std::string& lastPath);

    void openContextMenu();

    // Elementum.
    bool isAddonSection() const;
    void loadAddonContent(int cursor);
    void requestAddon(const std::string& url,
                      AddonRequestKind kind,
                      const std::string& title,
                      int cursor);
    void cancelAddonRequest();
    void handleAddonResponse(AddonHost::Response response);
    void activateAddonItem(const AddonHost::Item& item, int cursor);
    void openAddonContextMenu(const AddonHost::Item& item);
    // Looks up the trailer of the movie or series on TMDb and plays it.
    void watchTrailer(bool series, int tmdbId, const std::string& title, int year);
    void watchTrailerOf(const AddonHost::Item& item);
    void showAddonInformation(const AddonHost::Item& item);
    void playStream(const std::string& url, const AddonHost::Item& item, bool checked = false);
    void startInstall();
    void restartAddon();
    void runInstallTask(const std::string& title,
                        const std::function<AddonStore::Result(InstallTask&)>& task,
                        const std::function<void(const AddonStore::Result&)>& done);
    void pollInstallTask();
    void pollAddonHost();

    // Views and the information panel.
    std::string currentContentType() const;
    View viewFor(const std::string& contentType) const;
    void setView(View view);
    void cycleView();
    static std::string viewName(View view);
    void loadSorting();
    void saveSorting();
    std::vector<std::pair<std::string, std::string>> sortChoices() const;
    void openOptionsPanel();
    void updateOptionsPanel();
    void changeOption(int direction);
    bool usesGrid() const { return mView == View::WALL || mView == View::INFOWALL; }
    bool usesStrip() const { return mView == View::POSTER || mView == View::SHIFT; }
    bool usesList() const { return !usesGrid() && !usesStrip(); }
    int contentCursor() const;
    void setContentCursor(int cursor);
    void layoutContent();
    void updateGrid();
    void refreshImages();
    std::string entryImage(const Entry& entry, bool urgent);
    std::string entryFanart(const Entry& entry);
    void updateInfoPanel();
    void renderInfoPanel(const glm::mat4& trans);
    void renderOptionsPanel(const glm::mat4& trans);
    void renderSidebar(const glm::mat4& trans);
    void openIdentifyDialog(const Entry& entry);
    void openSettingsDialog();
    using Choices = std::vector<std::pair<std::string, std::string>>;
    MediaDialog* pushSettingsDialog(const std::string& title,
                                    const std::vector<MediaDialog::Option>& options,
                                    float widthFraction = 0.50f);
    MediaDialog::Option choiceSetting(const std::string& label,
                                      const std::string& key,
                                      const Choices& choices);
    MediaDialog::Option switchSetting(const std::string& label, const std::string& key);
    MediaDialog::Option textSetting(const std::string& label, const std::string& key, bool secret);
    void openPlaybackSettings();
    void openAudioSettings();
    void openSubtitleSettings();
    void openCustomSubtitleSettings();
#if defined(EMUSPLASH_DUBX)
    // Dubs, the subtitles read aloud using speech synthesis, see the dub module.
    void openDubSettings();
    // The voice removal settings of Dubx (Dubx > Voice removal and songs), see the dubx module.
    void openDubxSettings();
#endif
    void openSourcesSettings();
    void openMetadataSettings();
    void openSourceEditor(int sourceIndex, const std::string& name, const std::string& path);
    struct BrowseState;
    void openFolderBrowser(const std::function<void(const std::string&)>& chosenCallback);
    void rebuildFolderBrowser(MediaDialog* dialog,
                              const std::shared_ptr<BrowseState>& state,
                              int cursor);
    void confirmRemoveSource(int sourceIndex);
    void openTextInput(const std::string& title,
                       const std::string& value,
                       const std::function<void(const std::string&)>& callback);
    bool canEditSources() const;
    void refreshHelpPrompts();

    MediaBackground mBackground;
    MediaListView mList;
    MediaGridView mGrid;
    MediaStripView mStrip;
    MediaListView mOptions;
    MediaDialogStack mDialogs;
    std::unique_ptr<AddonDialogs> mAddonDialogs;
#if defined(EMUSPLASH_DUBX)
    std::unique_ptr<DubSettingsMenu> mDubMenu;
    std::unique_ptr<DubxSettingsMenu> mDubxMenu;
#endif

    std::vector<std::unique_ptr<TextComponent>> mSidebarItems;
    std::unique_ptr<TextComponent> mSidebarTitle;
    std::unique_ptr<TextComponent> mAddButton;
    std::unique_ptr<ImageComponent> mSettingsButton;
    std::unique_ptr<ImageComponent> mPreviewImage;
    std::unique_ptr<TextComponent> mHeader;
    std::unique_ptr<TextComponent> mSubHeader;
    std::unique_ptr<TextComponent> mClock;
    std::unique_ptr<TextComponent> mOptionsHint;
    std::unique_ptr<TextComponent> mOptionsTitle;
    std::unique_ptr<TextComponent> mDurationText;

    Focus mFocus;
    int mSidebarCursor;
    int mTopButton;
    bool mAddButtonShown;
    std::string mPreviewSource;
    std::string mPreviewFile;
    int mPreviewTimer;

    // Elementum.
    std::vector<AddonHost::Item> mAddonItems;
    std::string mAddonContentType;
    std::shared_ptr<AddonHost::Request> mAddonRequest;
    // The listing shown while a search is typed, and the one to return to.
    bool mRefreshAfterRun {false};
    // The searches typed for the preview, to be removed from the history of the add-on.
    bool mPreviewSession {false};
    std::vector<std::string> mPreviewQueries;
    std::set<std::string> mPreviewKnownQueries;
    std::string mPreviewRemoveUrl;
    std::shared_ptr<AddonHost::Request> mPreviewRequest;
    std::string mPreviewAddress;
    std::string mPreviewText;
    int mSearchPreviewTimer {-1};
    bool mPreviewShown {false};
    std::vector<AddonHost::Item> mPreviewSavedItems;
    std::string mPreviewSavedType;
    std::vector<Entry> mPreviewSavedEntries;
    int mPreviewSavedCursor {0};
    std::string mAddonRequestUrl;
    AddonRequestKind mAddonRequestKind;
    std::string mAddonRequestTitle;
    AddonHost::Item mAddonRequestItem;
    int mAddonRequestCursor;
    AddonHost::State mLastHostState;
    std::string mLastHostMessage;
    std::shared_ptr<InstallTask> mInstall;
    bool mAddonUpdating;
    std::unique_ptr<TextComponent> mBusyText;

    // Views.
    View mView;
    std::unique_ptr<TextComponent> mInfoTitle;
    std::unique_ptr<TextComponent> mInfoTagline;
    std::unique_ptr<TextComponent> mInfoDetails;
    std::unique_ptr<TextComponent> mInfoPlot;
    std::unique_ptr<ImageComponent> mInfoImage;
    std::unique_ptr<ImageComponent> mFanartImage;
    std::unique_ptr<ImageComponent> mFanartBoxImage;
    InfoData mInfo;
    // The boxes of the poster and the fanart, depending on the view.
    glm::vec4 mPosterBox;
    glm::vec4 mFanartBox;
    // The sorting and the filter of the list.
    std::string mSortKey;
    bool mSortDescending;
    int mFilter; // 0 all, 1 unwatched, 2 watched.
    std::vector<Entry> mAllEntries;
    std::string mInfoImagePath;
    std::string mFanartPath;
    int mInfoTimer;
    std::vector<Level> mLevels;
    int mSourcesCursor;
    std::vector<Entry> mEntries;
    int mClockTimer;
    std::vector<HelpPrompt> mLastHelpPrompts;

    // State of the source editor dialog.
    std::string mEditName;
    std::string mEditPath;
};

#endif // ES_APP_GUIS_GUI_MEDIA_CENTER_H
