//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiVideoPlayer.h
//
//  Fullscreen video playback with on-screen controls. The control bar has buttons for
//  previous/next, rewind/fast forward, play/pause and stop, together with media information,
//  subtitle settings and the settings menu (audio, subtitles and video).
//  Settings changed for a file are remembered and restored the next time it's played.
//  Streams of the streaming add-on are played the same way, with the state of the player
//  reported to the add-on, which also controls the resume position.
//  The dub reads the subtitles aloud using speech synthesis, see the dub module (DubSession).
//

#ifndef ES_APP_GUIS_GUI_VIDEO_PLAYER_H
#define ES_APP_GUIS_GUI_VIDEO_PLAYER_H

#include "GuiComponent.h"
#include "components/ImageComponent.h"
#include "media/AddonDialogs.h"
#if defined(EMUSPLASH_DUBX)
#include "dub/DubSession.h"
#else
#include "media/DubSessionStub.h"
#endif
#include "media/MediaComponents.h"
#include "media/MpvPlayer.h"
#include "media/SubtitleDownloader.h"

#include <future>

#include <functional>
#include <memory>
#include <string>
#include <vector>

// A stream of the streaming add-on, with the information the add-on gave about it.
struct MediaStreamInfo {
    bool active {false};
    std::string title;
    std::string showTitle;
    std::string originalTitle;
    std::string mediaType; // "movie" or "episode".
    std::string imdb;
    int year {0};
    int season {0};
    int episode {0};
    // Where the watched state of the stream is stored, empty if it's not stored.
    std::string watchKey;
    // Where the movie or series is from, from TMDb: the country ("US", from origin_country,
    // or the English name of the country) and the original language ("en"). Empty if unknown.
    std::string originCountry;
    std::string originalLanguage;
    // A trailer from the internet: played like a file, but nothing about it is stored and
    // the title is taken from title.
    bool trailer {false};
    // The audio of a trailer if it's separate from the video, added when the video is loaded.
    std::string trailerAudio;
};

class GuiVideoPlayer : public GuiComponent, private DubSession::Host
{
public:
    using StreamInfo = MediaStreamInfo;

    // The playlist is normally the videos of the folder, starting with the one at index.
    // The callback receives the path of the last played file. For a stream, the playlist
    // is its address.
    GuiVideoPlayer(const std::vector<std::string>& playlist,
                   size_t index,
                   double startPosition,
                   const std::function<void(const std::string&)>& closedCallback,
                   const StreamInfo& stream = StreamInfo());
    ~GuiVideoPlayer();

    bool isFullscreenGui() const override { return true; }

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    enum Button {
        BUTTON_PREVIOUS,
        BUTTON_REWIND,
        BUTTON_PLAY_PAUSE,
        BUTTON_STOP,
        BUTTON_FORWARD,
        BUTTON_NEXT,
        BUTTON_DUBX,
        BUTTON_PIXEL_RATIO,
        BUTTON_VOLUME,
        BUTTON_SUBTITLES,
        BUTTON_SETTINGS,
        BUTTON_COUNT
    };
    // The Dubx button is there only when built with Dubx.
    static bool buttonShown(int button)
    {
#if defined(EMUSPLASH_DUBX)
        return button >= 0;
#else
        return button != BUTTON_DUBX;
#endif
    }

    // What the directional buttons control while the on-screen controls are shown.
    enum class OsdFocus {
        NONE,
        BUTTONS,
        SEEK_BAR
    };

    // Slider shown at the top of the screen so the effect on the picture can be seen.
    struct Slider {
        std::string label;
        double min {0.0};
        double max {1.0};
        double step {0.01};
        std::function<double()> getValue;
        std::function<void(double)> setValue;
        std::function<std::string(double)> format;
    };

    enum class RepeatAction {
        NONE,
        SEEK_BACK,
        SEEK_FORWARD,
        VOLUME_DOWN,
        VOLUME_UP,
        SLIDER_DOWN,
        SLIDER_UP
    };

    // Playback and playlist.
    void startFile(size_t index, double startPosition);
    void openPlaylistEntry(size_t index);
    void leaveCurrentFile(bool reachedEnd);
    void applyFileSettings();
    void resetFileSettings();
    void saveFileSettings();
    bool isSeries() const;
    std::string seriesKey() const;
    void resetPicture();
    void skip(int direction);
    void close();
    void requestStop();

    // Controls.
    bool handleKeyboard(Input input);
    void showControls(OsdFocus focus);
    void hideControls();
    void activateButton(int button);
    void addPendingSeek(int direction, double step);
    void applyPendingSeek();
    void changeVolume(double amount);
    void togglePause();
    void startRepeat(RepeatAction action);
    void performRepeat();
    void refreshHelpPrompts();

    // Dialogs.
    MediaDialog* pushDialog(const std::string& title, float widthFraction = 0.50f);
    void addCloseOption(std::vector<MediaDialog::Option>& options, MediaDialog* dialog);
    void openSettingsMenu();
    void openAudioSettings();
    void openSubtitleSettings();
    void openVideoSettings();
    void openInformation();
    void openSlider(const Slider& slider);
    void openVolumeSlider();
    void openPixelRatioSlider();
    void adjustSlider(int direction, int repeatCount);
    void closeSlider();
    double getDisplayAspect();
    void applyPixelRatio();
    void openTrackDialog(const std::string& type);
    void openSubtitleBrowser();

    // Subtitle downloading.
    void openSubtitleDownload();
    void startSubtitleSearch(bool automatic);
    void rebuildDownloadDialog(const std::string& status);
    void downloadSubtitle(const SubtitleDownloader::Result& result);
    void pollSubtitleDownloads();
    void openLanguagePicker(bool defaultOnly, const std::function<void()>& doneCallback);
    std::vector<std::string> downloadLanguages() const;
    // Applied when the tracks of a new file are known.
    void handleTracksOnStart();
    // Shows a subtitle that exists for the video if none is shown.
    void selectAvailableSubtitle();
    std::string currentSubtitleChoice() const;
    bool applySubtitleChoice(const std::string& choice);
    void rebuildSubtitleBrowser(MediaDialog* dialog,
                                const std::shared_ptr<std::string>& directory,
                                int cursor);

    // Tracks and settings helpers.
    std::string trackLabel(const MpvPlayer::Track& track) const;
    std::string selectedTrackLabel(const std::string& type, bool withIndex) const;
    const MpvPlayer::Track* selectedTrack(const std::string& type) const;
    int trackCount(const std::string& type) const;
    void cycleTrack(const std::string& type, int direction);
    bool subtitlesEnabled() const;
    void toggleSubtitles();
    void applyViewMode(const std::string& mode);
    void settingsChanged() { mFileSettingsChanged = true; }

    // Dub, what DubSession needs from the player.
    MediaDialog* pushDubDialog(const std::string& title, float widthFraction) override;
    void addDubCloseOption(std::vector<MediaDialog::Option>& options, MediaDialog* dialog) override;
    void refreshDubHelpPrompts() override;
    const MpvPlayer::Track* selectedDubSubtitle() const override;
    const MpvPlayer::Track* selectedDubAudio() const override;
    std::string dubTitle() const override;
    std::string dubVideoPath() const override;
    bool isDubStream() const override;
    SubtitleDownloader::Query dubSubtitleQuery() const override;
    std::string dubSubtitleVideoPath() const override;
    double dubStartPosition() const override;
    std::string dubOriginCountry() const override;
    std::string dubOriginLanguage() const override;
    std::string dubContext() const override;
    // Looks up the story of the movie or series on TMDb in the background, for the translation.
    void startContextLookup(bool series, int id, const std::string& extra);
    // Takes the result of the TMDb lookup of the origin of the stream, if it has finished.
    // Waits at most maxWait milliseconds for it.
    void takeStreamOrigin(int maxWait) const;
    void applyDubAudioFilters() override;
    bool dynamicRangeEnabled() const;
    // Sets the audio filters, including the one that lowers the volume while the dub speaks.
    void applyAudioFilters(bool dynamicRange);

    // Streams.
    std::string streamTitle() const;
    // The path used for naming downloaded subtitles, as a stream has no file.
    std::string subtitleVideoPath() const;
    // What a stream is, for searching subtitles: from the add-on, or from the file name when
    // the add-on doesn't say (e.g. a torrent played from a list of files).
    SubtitleDownloader::Query streamSubtitleQuery() const;
    void updateStreamState();
    void sendStreamEvent(const std::string& method, bool ended = false);

    // Rendering.
    void updateCodecInfo();
    void renderTopBar(const glm::mat4& trans);
    void renderBottomBar(const glm::mat4& trans, bool withButtons);
    void renderVolume(const glm::mat4& trans);
    void renderCodecInfo(const glm::mat4& trans);
    void renderPausedIndicator(const glm::mat4& trans);
    void renderSlider(const glm::mat4& trans);

    std::unique_ptr<MpvPlayer> mPlayer;
    std::vector<std::string> mPlaylist;
    size_t mIndex;
    std::string mPath;
    std::function<void(const std::string&)> mClosedCallback;
    MediaDialogStack mDialogs;

    std::vector<std::unique_ptr<ImageComponent>> mButtonIcons;
    std::unique_ptr<ImageComponent> mPlayIcon;
    std::unique_ptr<ImageComponent> mMuteIcon;
    std::unique_ptr<ImageComponent> mPausedIcon;
    std::vector<glm::vec2> mButtonPositions;
    float mButtonSize;

    std::unique_ptr<TextComponent> mTitleText;
    std::unique_ptr<TextComponent> mClockText;
    std::unique_ptr<TextComponent> mEndsText;
    std::unique_ptr<TextComponent> mTimeText;
    std::unique_ptr<TextComponent> mStatusText;
    std::unique_ptr<TextComponent> mSeekText;
    std::unique_ptr<TextComponent> mVolumeText;
    std::unique_ptr<TextComponent> mLoadingText;
    std::unique_ptr<TextComponent> mPausedText;
    std::unique_ptr<TextComponent> mCodecText;
    std::unique_ptr<TextComponent> mSliderLabel;
    std::unique_ptr<TextComponent> mSliderValue;

    bool mClosing;
    bool mInitFailed;
    bool mScreensaverWasEnabled;

    bool mControlsVisible;
    OsdFocus mOsdFocus;
    std::unique_ptr<Slider> mSlider;
    int mRepeatCount;
    double mPixelRatio;
    bool mPixelRatioPending;
    int mControlsTimer;
    int mFocusedButton;
    int mSeekBarTimer;
    int mVolumeTimer;
    bool mCodecInfoVisible;
    int mCodecInfoTimer;
    int mSaveTimer;
    int mDialogRefreshTimer;

    double mLastPosition;
    double mLastDuration;
    double mInitialVolume;
    int mAmplification; // Volume amplification in decibels.
    bool mFileSettingsChanged;
    std::string mViewMode;

    bool mSeekPending;
    double mSeekTarget;
    int mSeekTimer;
    int mSeekCount;

    RepeatAction mRepeatAction;
    int mRepeatTimer;
    unsigned int mLastKeyboardRepeat;
    unsigned int mLastVerticalInput;
    int mStopConfirmTimer;
    // The pixel ratio is shared by the videos of a series, see saveFileSettings().
    bool mPixelRatioChanged;
    // The subtitle track has been changed by the user, shared by the videos of a series.
    bool mSubtitleChanged;
    bool mTracksHandled;

    std::future<SubtitleDownloader::SearchOutcome> mSearchFuture;
    std::future<SubtitleDownloader::DownloadOutcome> mDownloadFuture;
    MediaDialog* mDownloadDialog;
    bool mAutomaticSearch;
    std::string mSearchTitle;
    std::vector<std::string> mSearchLanguages;
    std::vector<SubtitleDownloader::Result> mSearchResults;
    std::string mDownloadingRelease;
    std::string mDownloadPath;
    double mSourceAspect;
    std::vector<HelpPrompt> mLastHelpPrompts;
    bool mHelpPromptsSet;

    // mutable as the origin of the stream is filled in when the dub asks for it.
    mutable StreamInfo mStream;
    // The country and the original language of the stream, looked up on TMDb.
    mutable std::future<std::pair<std::string, std::string>> mOriginFuture;
    // The story of the movie or series (title, genres, summary, characters), from TMDb.
    mutable std::future<std::string> mContextFuture;
    mutable std::string mContext;
    std::unique_ptr<AddonDialogs> mAddonDialogs;
    bool mStreamStarted;
    bool mStreamPaused;
    double mStreamPosition;
    // The last seek asked by the add-on, to ignore it if it's asked again.
    double mLastAddonSeek;
    unsigned int mLastAddonSeekTicks;

    std::unique_ptr<DubSession> mDubSession;
};

#endif // ES_APP_GUIS_GUI_VIDEO_PLAYER_H
