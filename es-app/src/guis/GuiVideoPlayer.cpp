//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiVideoPlayer.cpp
//
//  Fullscreen video playback with on-screen controls. The control bar has buttons for
//  previous/next, rewind/fast forward, play/pause and stop, together with media information,
//  subtitle settings and the settings menu (audio, subtitles and video).
//  Settings changed for a file are remembered and restored the next time it's played.
//  Streams of the streaming add-on are played the same way, with the state of the player
//  reported to the add-on, which also controls the resume position.
//  The dub reads the subtitles aloud using speech synthesis, see the dub module (DubSession).
//

#include "guis/GuiVideoPlayer.h"

#include "Log.h"
#include "Settings.h"
#include "Window.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "guis/GuiTextEditPopup.h"
#include "media/AddonHost.h"
#include "media/AddonSettings.h"
#include "media/HttpClient.h"
#include "media/MediaLibrary.h"
#include "media/MediaMetadata.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <SDL2/SDL_keyboard.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_video.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <regex>
#include <thread>

#define CONTROLS_TIMEOUT 3000
// Time to press back a second time to stop the playback.
#define STOP_CONFIRM_TIME 3000
#define SEEK_BAR_TIMEOUT 2500
#define VOLUME_TIMEOUT 1500
#define SEEK_APPLY_DELAY 450
#define SEEK_APPLY_DELAY_RELEASED 200
#define REPEAT_DELAY_INITIAL 400
#define REPEAT_DELAY_SEEK 150
#define REPEAT_DELAY_VOLUME 90
#define KEYBOARD_REPEAT_THROTTLE 110
#define PROGRESS_SAVE_INTERVAL 60000
#define CODEC_INFO_REFRESH_INTERVAL 500

namespace
{
    using Choices = std::vector<std::pair<std::string, std::string>>;

    // Player properties that are stored per file when changed by the user.
    const std::vector<std::string> FILE_SETTINGS {"aid",       "sid",        "sub-visibility",
                                                  "audio-delay", "sub-delay", "video-zoom",
                                                  "deinterlace"};

    const Choices VIEW_MODES {{"normal", "Normal"},
                              {"zoom", "Zoom"},
                              {"widezoom", "Wide zoom"},
                              {"stretch", "Stretch"},
                              {"original", "Original size"}};

    std::string choiceLabel(const Choices& choices, const std::string& value)
    {
        for (const auto& [key, label] : choices) {
            if (key == value)
                return label;
        }
        return value.empty() ? choices.front().second : value;
    }

    std::string cycleChoice(const Choices& choices, const std::string& value, int direction)
    {
        int index {0};
        for (size_t i {0}; i < choices.size(); ++i) {
            if (choices[i].first == value)
                index = static_cast<int>(i);
        }
        const int count {static_cast<int>(choices.size())};
        index = ((index + direction) % count + count) % count;
        return choices[index].first;
    }

    const std::vector<std::pair<std::string, std::string>>& languages()
    {
        static const std::vector<std::pair<std::string, std::string>> names {
            {"en", "English"},   {"eng", "English"},    {"et", "Estonian"},  {"est", "Estonian"},
            {"ru", "Russian"},   {"rus", "Russian"},    {"fi", "Finnish"},   {"fin", "Finnish"},
            {"de", "German"},    {"ger", "German"},     {"deu", "German"},   {"fr", "French"},
            {"fre", "French"},   {"fra", "French"},     {"es", "Spanish"},   {"spa", "Spanish"},
            {"it", "Italian"},   {"ita", "Italian"},    {"sv", "Swedish"},   {"swe", "Swedish"},
            {"no", "Norwegian"}, {"nor", "Norwegian"},  {"nb", "Norwegian"}, {"nob", "Norwegian"},
            {"da", "Danish"},    {"dan", "Danish"},     {"pl", "Polish"},    {"pol", "Polish"},
            {"lv", "Latvian"},   {"lav", "Latvian"},    {"lt", "Lithuanian"},
            {"lit", "Lithuanian"}, {"ja", "Japanese"},  {"jpn", "Japanese"}, {"zh", "Chinese"},
            {"chi", "Chinese"},  {"zho", "Chinese"},    {"ko", "Korean"},    {"kor", "Korean"},
            {"nl", "Dutch"},     {"dut", "Dutch"},      {"nld", "Dutch"},    {"pt", "Portuguese"},
            {"por", "Portuguese"}, {"uk", "Ukrainian"}, {"ukr", "Ukrainian"}};
        return names;
    }

    std::string languageName(const std::string& code)
    {
        const std::string lower {Utils::String::toLower(code)};
        if (lower == "und")
            return "";
        for (const auto& [key, name] : languages()) {
            if (key == lower)
                return name;
        }
        return Utils::String::toUpper(code);
    }

    // The two letter code used by the settings, e.g. "eng" becomes "en".
    std::string languageSettingCode(const std::string& code)
    {
        const std::string name {languageName(code)};
        for (const auto& [key, keyName] : languages()) {
            if (keyName == name && key.size() == 2)
                return key;
        }
        return Utils::String::toLower(code);
    }

    std::string channelLayout(int channels)
    {
        switch (channels) {
            case 0:
                return "";
            case 1:
                return "Mono";
            case 2:
                return "Stereo";
            case 6:
                return "5.1";
            case 8:
                return "7.1";
            default:
                return std::to_string(channels) + " ch";
        }
    }

    std::string formatOffset(double seconds)
    {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%+.3f s", seconds);
        return buffer;
    }

    std::string formatSpeed(double speed)
    {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.2f", speed);
        std::string text {buffer};
        while (text.size() > 3 && text.back() == '0')
            text.pop_back();
        return text + "x";
    }

    std::string formatDecimal(const std::string& value, int decimals)
    {
        if (value.empty())
            return "";
        char* end {nullptr};
        const double number {std::strtod(value.c_str(), &end)};
        if (end == value.c_str())
            return value;
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.*f", decimals, number);
        return buffer;
    }

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

    std::string formatDate(long long timestamp)
    {
        if (timestamp <= 0)
            return "Never";
        const std::time_t time {static_cast<std::time_t>(timestamp)};
        std::tm local {};
        localtime_r(&time, &local);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
        return buffer;
    }

    std::string parentDirectory(const std::string& path)
    {
        const std::string parent {Utils::FileSystem::getParent(path)};
        return parent.empty() ? "/" : parent;
    }

    // Requests that are no longer needed are left to finish in the background, as waiting for
    // them (which is what destroying the future of a running request does) would block the
    // user interface.
    template <typename T> void abandonRequest(std::future<T>&& future)
    {
        static std::vector<std::future<T>> sPending;
        sPending.erase(std::remove_if(sPending.begin(), sPending.end(),
                                      [](const std::future<T>& pending) {
                                          return !pending.valid() ||
                                                 pending.wait_for(std::chrono::seconds(0)) ==
                                                     std::future_status::ready;
                                      }),
                       sPending.end());
        if (future.valid() &&
            future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            sPending.emplace_back(std::move(future));
    }

    const char* buttonIcon(int button)
    {
        switch (button) {
            case 0:
                return ":/graphics/media/previous.svg";
            case 1:
                return ":/graphics/media/rewind.svg";
            case 2:
                return ":/graphics/media/pause.svg";
            case 3:
                return ":/graphics/media/stop.svg";
            case 4:
                return ":/graphics/media/forward.svg";
            case 5:
                return ":/graphics/media/next.svg";
            case 6:
                return ":/graphics/media/dubx.svg";
            case 7:
                return ":/graphics/media/pixelratio.svg";
            case 8:
                return ":/graphics/media/volume.svg";
            case 9:
                return ":/graphics/media/subtitles.svg";
            default:
                return ":/graphics/media/settings.svg";
        }
    }

    std::unique_ptr<ImageComponent> makeIcon(const std::string& path, float size)
    {
        auto icon = std::make_unique<ImageComponent>();
        icon->setResize(size, size);
        icon->setImage(path);
        return icon;
    }
} // namespace

GuiVideoPlayer::GuiVideoPlayer(const std::vector<std::string>& playlist,
                               size_t index,
                               double startPosition,
                               const std::function<void(const std::string&)>& closedCallback,
                               const StreamInfo& stream)
    : mPlaylist {playlist}
    , mIndex {std::min(index, playlist.empty() ? size_t {0} : playlist.size() - 1)}
    , mClosedCallback {closedCallback}
    , mButtonSize {0.0f}
    , mClosing {false}
    , mInitFailed {false}
    , mScreensaverWasEnabled {SDL_IsScreenSaverEnabled() == SDL_TRUE}
    , mControlsVisible {false}
    , mOsdFocus {OsdFocus::NONE}
    , mRepeatCount {0}
    , mPixelRatio {1.0}
    , mPixelRatioPending {false}
    , mControlsTimer {0}
    , mFocusedButton {BUTTON_PLAY_PAUSE}
    , mSeekBarTimer {0}
    , mVolumeTimer {0}
    , mCodecInfoVisible {false}
    , mCodecInfoTimer {0}
    , mSaveTimer {PROGRESS_SAVE_INTERVAL}
    , mDialogRefreshTimer {0}
    , mLastPosition {0.0}
    , mLastDuration {0.0}
    , mInitialVolume {100.0}
    , mAmplification {std::clamp(Settings::getInstance()->getInt("MediaVolumeAmplification"), 0, 60)}
    , mFileSettingsChanged {false}
    , mViewMode {"normal"}
    , mSeekPending {false}
    , mSeekTarget {0.0}
    , mSeekTimer {0}
    , mSeekCount {0}
    , mRepeatAction {RepeatAction::NONE}
    , mRepeatTimer {0}
    , mLastKeyboardRepeat {0}
    , mLastVerticalInput {0}
    , mStopConfirmTimer {0}
    , mPixelRatioChanged {false}
    , mSubtitleChanged {false}
    , mTracksHandled {false}
    , mDownloadDialog {nullptr}
    , mAutomaticSearch {false}
    , mSourceAspect {0.0}
    , mHelpPromptsSet {false}
    , mStream {stream}
    , mStreamStarted {false}
    , mStreamPaused {false}
    , mStreamPosition {0.0}
    , mLastAddonSeek {0.0}
    , mLastAddonSeekTicks {0}
{
    // Where the movie or series of a stream is from, for the names of the dub. The TMDb id is
    // in the address of the stream (the watch key).
    if (mStream.active && MediaMetadata::isEnabled()) {
        static const std::regex idPattern {"^stream:(movie|show)/(\\d+)"};
        std::smatch match;
        if (std::regex_search(mStream.watchKey, match, idPattern)) {
            const bool series {match[1] == "show"};
            const int id {std::atoi(match[2].str().c_str())};
            if (mStream.originalLanguage.empty()) {
                // A detached thread, so that closing the player doesn't wait for the lookup.
                auto promise {
                    std::make_shared<std::promise<std::pair<std::string, std::string>>>()};
                mOriginFuture = promise->get_future();
                std::thread([promise, series, id] {
                    std::string country;
                    std::string language;
                    MediaMetadata::lookupOrigin(series, id, country, language);
                    promise->set_value(std::make_pair(country, language));
                }).detach();
            }
            startContextLookup(series, id, "");
        }
    }

    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    setSize(screenWidth, screenHeight);

    const float margin {std::round(screenWidth * 0.03f)};
    const float contentWidth {screenWidth - margin * 2.0f};

    mTitleText = MediaStyle::makeText(MediaStyle::font(0.040f), MediaStyle::TEXT);
    mTitleText->setSize(contentWidth * 0.72f, screenHeight * 0.06f);
    mTitleText->setPosition(margin, screenHeight * 0.022f);

    mClockText = MediaStyle::makeText(MediaStyle::font(0.052f, FONT_PATH_LIGHT), MediaStyle::TEXT,
                                      ALIGN_RIGHT);
    mClockText->setSize(contentWidth * 0.25f, screenHeight * 0.06f);
    mClockText->setPosition(margin + contentWidth * 0.75f, screenHeight * 0.008f);
    mEndsText =
        MediaStyle::makeText(MediaStyle::font(0.024f), MediaStyle::TEXT_SECONDARY, ALIGN_RIGHT);
    mEndsText->setSize(contentWidth * 0.25f, screenHeight * 0.035f);
    mEndsText->setPosition(margin + contentWidth * 0.75f, screenHeight * 0.066f);

    mTimeText =
        MediaStyle::makeText(MediaStyle::font(0.040f), MediaStyle::TEXT, ALIGN_CENTER);
    mTimeText->setSize(screenWidth * 0.36f, screenHeight * 0.06f);
    mTimeText->setPosition((screenWidth - screenWidth * 0.36f) / 2.0f, screenHeight * 0.845f);
    mStatusText =
        MediaStyle::makeText(MediaStyle::font(0.026f), MediaStyle::TEXT_SECONDARY, ALIGN_CENTER);
    mStatusText->setSize(screenWidth * 0.36f, screenHeight * 0.035f);
    mStatusText->setPosition((screenWidth - screenWidth * 0.36f) / 2.0f, screenHeight * 0.898f);

    mSeekText = MediaStyle::makeText(MediaStyle::font(0.030f, FONT_PATH_BOLD), MediaStyle::ACCENT,
                                     ALIGN_CENTER);
    mSeekText->setSize(screenWidth * 0.2f, screenHeight * 0.045f);
    mVolumeText = MediaStyle::makeText(MediaStyle::font(0.028f), MediaStyle::TEXT);
    mVolumeText->setSize(screenWidth * 0.2f, screenHeight * 0.045f);
    mLoadingText = MediaStyle::makeText(MediaStyle::font(0.036f, FONT_PATH_LIGHT),
                                        MediaStyle::TEXT_SECONDARY, ALIGN_CENTER);
    mLoadingText->setSize(screenWidth, screenHeight * 0.06f);
    mLoadingText->setPosition(0.0f, screenHeight * 0.47f);
    mPausedText = MediaStyle::makeText(MediaStyle::font(0.030f), MediaStyle::TEXT);
    mPausedText->setSize(screenWidth * 0.2f, screenHeight * 0.05f);
    mPausedText->setText("Paused");
    mCodecText = std::make_unique<TextComponent>("", MediaStyle::font(0.025f), MediaStyle::TEXT,
                                                 ALIGN_LEFT, ALIGN_TOP, glm::ivec2 {0, 0});
    mCodecText->setLineSpacing(1.35f);
    mCodecText->setSize(screenWidth * 0.40f, screenHeight * 0.30f);
    mSliderLabel = MediaStyle::makeText(MediaStyle::font(0.034f), MediaStyle::ACCENT);
    mSliderLabel->setSize(screenWidth * 0.28f, screenHeight * 0.05f);
    mSliderValue = MediaStyle::makeText(MediaStyle::font(0.034f), MediaStyle::TEXT, ALIGN_RIGHT);
    mSliderValue->setSize(screenWidth * 0.12f, screenHeight * 0.05f);

    // Control bar buttons, the transport controls to the left and the menus to the right.
    mButtonSize = std::round(screenHeight * 0.068f);
    const float iconSize {std::round(mButtonSize * 0.58f)};
    const float buttonY {std::round(screenHeight * 0.875f - mButtonSize / 2.0f)};
    const float spacing {std::round(screenWidth * 0.006f)};
    for (int i {0}; i < BUTTON_COUNT; ++i) {
        // A button that isn't shown (Dubx without Dubx) has no icon of its own to load, the
        // icon of the settings stands in for it.
        mButtonIcons.emplace_back(
            makeIcon(buttonIcon(buttonShown(i) ? i : static_cast<int>(BUTTON_SETTINGS)), iconSize));
        float x {0.0f};
        if (i <= BUTTON_NEXT)
            x = margin + i * (mButtonSize + spacing);
        else
            x = screenWidth - margin - (BUTTON_COUNT - i) * (mButtonSize + spacing) + spacing;
        mButtonPositions.emplace_back(glm::vec2 {x, buttonY});
    }
    mPlayIcon = makeIcon(":/graphics/media/play.svg", iconSize);
    mMuteIcon = makeIcon(":/graphics/media/mute.svg", iconSize);
    mPausedIcon = makeIcon(":/graphics/media/pause.svg", std::round(screenHeight * 0.035f));

    // Keep the screen from blanking during playback.
    SDL_DisableScreenSaver();

    // Questions of the streaming add-on during playback are shown here.
    if (mStream.active)
        mAddonDialogs = std::make_unique<AddonDialogs>(mDialogs, nullptr);

    mPlayer = std::make_unique<MpvPlayer>();
    if (mPlaylist.empty() || !mPlayer->initialize()) {
        mInitFailed = true;
        return;
    }
    mInitialVolume = mPlayer->getVolume();
#if defined(EMUSPLASH_DUBX)
    // The host is a private base, so it's passed from here where it's accessible.
    DubSession::Host& dubHost {*this};
    // A trailer is never dubbed, Dubx would read it from the internet a second time.
    if (!mStream.trailer)
        mDubSession = std::make_unique<DubSession>(dubHost, *mPlayer, mWindow);
#endif
    applyViewMode(Settings::getInstance()->getString("MediaViewMode"));
    startFile(mIndex, startPosition);
}

GuiVideoPlayer::~GuiVideoPlayer()
{
    abandonRequest(std::move(mSearchFuture));
    abandonRequest(std::move(mDownloadFuture));

    // The player can also be removed without being closed, e.g. when quitting the application.
    if (!mClosing && mPlayer && mPlayer->isInitialized())
        leaveCurrentFile(false);
    if (mStream.active) {
        if (mStreamStarted)
            sendStreamEvent("Player.OnStop");
        AddonHost::getInstance().setPlayerState(AddonHost::PlayerState());
    }
    mAddonDialogs.reset();
    mDialogs.clear();
    mDubSession.reset();
    mPlayer.reset();
    if (mScreensaverWasEnabled)
        SDL_EnableScreenSaver();
}

//--------------------------------------------------------------------------------------------------
// Playback and playlist.
//--------------------------------------------------------------------------------------------------

void GuiVideoPlayer::startFile(size_t index, double startPosition)
{
    mIndex = index;
    mPath = mPlaylist[index];
    mLastPosition = startPosition;
    mLastDuration = 0.0;
    mFileSettingsChanged = false;
    mPixelRatioChanged = false;
    mSubtitleChanged = false;
    mTracksHandled = false;
    // A search for the previous video is no longer needed.
    abandonRequest(std::move(mSearchFuture));
    mAutomaticSearch = false;
    mSearchTitle.clear();
    mSeekPending = false;
    // The story of the movie or series for the translation of the dub. For a video of the
    // library the TMDb id was found when its information was looked up.
    if (!mStream.active) {
        mContext.clear();
        mContextFuture = std::future<std::string>();
    }
    if (!mStream.active && MediaMetadata::isEnabled()) {
        MediaMetadata::Info info;
        if (MediaMetadata::getInstance().get(mPath, false, false, info) && info.found &&
            info.tmdbId > 0) {
            std::string episode;
            if (info.type == "episode") {
                episode = "This episode: season " + std::to_string(info.season) + ", episode " +
                          std::to_string(info.episode);
                if (!info.episodeTitle.empty())
                    episode.append(" \"" + info.episodeTitle + "\"");
                episode.append(".");
            }
            startContextLookup(info.type != "movie", info.tmdbId, episode);
        }
    }
    if (mDubSession)
        mDubSession->onFileStarted();
    MediaStyle::setText(*mTitleText, mStream.active || mStream.trailer ?
                                         streamTitle() :
                                         Utils::FileSystem::getFileName(mPath));

    resetFileSettings();
    mPlayer->loadFile(mPath, startPosition);
    showControls(OsdFocus::NONE);
}

void GuiVideoPlayer::openPlaylistEntry(size_t index)
{
    if (index >= mPlaylist.size())
        return;

    leaveCurrentFile(false);
    const std::string path {mPlaylist[index]};
    const MediaLibrary::PlayState state {MediaLibrary::getInstance().getPlayState(path)};
    const std::string resumeMode {Settings::getInstance()->getString("MediaResumeMode")};

    if (state.position <= 0.0 || resumeMode == "never") {
        startFile(index, 0.0);
        return;
    }
    if (resumeMode == "always") {
        startFile(index, state.position);
        return;
    }

    mPlayer->setPaused(true);
    MediaDialog* dialog {pushDialog(Utils::FileSystem::getStem(path), 0.42f)};
    std::vector<MediaDialog::Option> options;
    MediaDialog::Option resume;
    resume.label = "Resume from " + MediaLibrary::formatTime(state.position);
    resume.onSelect = [this, dialog, index, state] {
        dialog->close();
        startFile(index, state.position);
        mPlayer->setPaused(false);
    };
    options.emplace_back(resume);
    MediaDialog::Option beginning;
    beginning.label = "Play from beginning";
    beginning.onSelect = [this, dialog, index] {
        dialog->close();
        startFile(index, 0.0);
        mPlayer->setPaused(false);
    };
    options.emplace_back(beginning);
    dialog->setOptions(options);
    dialog->setCancelCallback([this, index] {
        startFile(index, 0.0);
        mPlayer->setPaused(false);
    });
}

void GuiVideoPlayer::leaveCurrentFile(bool reachedEnd)
{
    if (!mPlayer || !mPlayer->isInitialized() || mPath.empty() || mStream.trailer)
        return;
    // The watched state of a stream is kept here, the add-on has no library to keep it in.
    if (mStream.active) {
        if (!mStream.watchKey.empty() && mPlayer && mPlayer->isInitialized()) {
            double streamPosition {mSeekPending ? mSeekTarget : mLastPosition};
            if (reachedEnd && mLastDuration > 0.0)
                streamPosition = mLastDuration;
            if (mLastDuration > 0.0)
                MediaLibrary::getInstance().savePlaybackProgress(mStream.watchKey,
                                                                 streamPosition, mLastDuration);
        }
        return;
    }

    double position {mSeekPending ? mSeekTarget : mLastPosition};
    if (reachedEnd && mLastDuration > 0.0)
        position = mLastDuration;
    // Only store the progress if the file could be opened.
    if (mLastDuration > 0.0)
        MediaLibrary::getInstance().savePlaybackProgress(mPath, position, mLastDuration);

    saveFileSettings();
}

void GuiVideoPlayer::saveFileSettings()
{
    if (!mFileSettingsChanged || !mPlayer || !mPlayer->isInitialized() || mPath.empty())
        return;

    std::map<std::string, std::string> settings;
    for (const auto& name : FILE_SETTINGS)
        settings[name] = mPlayer->getPropertyString(name);
    // Subtitles hidden by the dub are shown again without it.
    if (mDubSession && mDubSession->hidSubtitles())
        settings["sub-visibility"] = "yes";
    settings["view-mode"] = mViewMode;
    char ratio[16];
    snprintf(ratio, sizeof(ratio), "%.2f", mPixelRatio);
    if (isSeries()) {
        // Shared by all videos in the folder.
        if (mPixelRatioChanged || mSubtitleChanged) {
            std::map<std::string, std::string> series {
                MediaLibrary::getInstance().getPlayState(seriesKey()).settings};
            if (mPixelRatioChanged)
                series["pixel-ratio"] = ratio;
            if (mSubtitleChanged)
                series["sub-choice"] = currentSubtitleChoice();
            MediaLibrary::getInstance().setFileSettings(seriesKey(), series);
            LOG(LogInfo) << "GuiVideoPlayer: Saved the settings of the series \"" << seriesKey()
                         << "\" (pixel ratio " << series["pixel-ratio"] << ", subtitle \""
                         << series["sub-choice"] << "\")";
        }
    }
    else {
        settings["pixel-ratio"] = ratio;
    }
    MediaLibrary::getInstance().setFileSettings(mPath, settings);
    mFileSettingsChanged = false;
    mPixelRatioChanged = false;
    mSubtitleChanged = false;
    LOG(LogInfo) << "GuiVideoPlayer: Saved the player settings for \"" << mPath
                 << "\" (pixel ratio " << ratio << ")";
}

void GuiVideoPlayer::resetPicture()
{
    applyViewMode("normal");
    mPlayer->setProperty("video-zoom", "0");
    mPlayer->setProperty("video-pan-y", "0");
    mPixelRatio = 1.0;
    mPixelRatioChanged = true;
    applyPixelRatio();
    settingsChanged();
}

void GuiVideoPlayer::resetFileSettings()
{
    // Options set for the previous file would otherwise carry over to the next one.
    Settings* settings {Settings::getInstance()};
    mPlayer->setProperty("aid", "auto", true);
    mPlayer->setProperty("sid", settings->getString("MediaSubtitleLanguage") == "off" ? "no" : "auto",
                         true);
    mPlayer->setProperty("sub-visibility", "yes", true);
    mPlayer->setProperty("audio-delay", "0", true);
    mPlayer->setProperty("sub-delay", "0", true);
    mPlayer->setProperty("video-zoom", "0", true);
    mPlayer->setProperty("video-pan-y", "0", true);
    mPlayer->setProperty("video-aspect-override", "no", true);
    mPixelRatio = 1.0;
    mPixelRatioPending = false;
    mSourceAspect = 0.0;
    mPlayer->setProperty("deinterlace", settings->getBool("MediaDeinterlace") ? "yes" : "no",
                         true);
    for (const char* property : {"brightness", "contrast", "saturation", "gamma", "hue"})
        mPlayer->setProperty(property, "0", true);
    mPlayer->setProperty("speed", "1", true);
    applyViewMode(settings->getString("MediaViewMode"));
}

bool GuiVideoPlayer::isSeries() const
{
    // A folder with more than three videos is treated as a series, where the videos share
    // the same pixel ratio as they normally have the same format.
    return mPlaylist.size() > 3;
}

std::string GuiVideoPlayer::seriesKey() const
{
    return Utils::FileSystem::getParent(mPath) + "/";
}

void GuiVideoPlayer::applyFileSettings()
{
    if (mStream.trailer) {
        if (!mStream.trailerAudio.empty())
            mPlayer->addAudioFile(mStream.trailerAudio);
        return;
    }
    if (mStream.active)
        return;
    const MediaLibrary::PlayState state {MediaLibrary::getInstance().getPlayState(mPath)};

    for (const auto& [name, value] : state.settings) {
        if (value.empty())
            continue;
        if (name == "view-mode")
            applyViewMode(value);
        else if (name == "pixel-ratio")
            mPixelRatio = std::clamp(std::atof(value.c_str()), 0.5, 2.0);
        else if (std::find(FILE_SETTINGS.cbegin(), FILE_SETTINGS.cend(), name) !=
                 FILE_SETTINGS.cend())
            mPlayer->setProperty(name, value);
    }

    // The pixel ratio of the series replaces the one of the individual video.
    bool seriesRatio {false};
    if (isSeries()) {
        const MediaLibrary::PlayState series {
            MediaLibrary::getInstance().getPlayState(seriesKey())};
        const auto it {series.settings.find("pixel-ratio")};
        if (it != series.settings.cend() && !it->second.empty()) {
            mPixelRatio = std::clamp(std::atof(it->second.c_str()), 0.5, 2.0);
            seriesRatio = true;
        }
    }

    if (std::abs(mPixelRatio - 1.0) > 0.001)
        applyPixelRatio();
    if (!state.settings.empty() || seriesRatio) {
        LOG(LogInfo) << "GuiVideoPlayer: Restored the player settings for \"" << mPath << "\""
                     << (seriesRatio ? " using the pixel ratio of the series" : "");
    }
}

void GuiVideoPlayer::skip(int direction)
{
    const auto& chapters {mPlayer->getChapters()};
    const int chapter {mPlayer->getChapter()};
    const double position {mPlayer->getPosition()};

    if (direction < 0) {
        // Back to the start of the current chapter first, unless just past it.
        if (!chapters.empty() && chapter >= 0 && chapter < static_cast<int>(chapters.size())) {
            if (position - chapters[chapter].time > 3.0) {
                mPlayer->seekAbsolute(chapters[chapter].time);
                return;
            }
            if (chapter > 0) {
                mPlayer->changeChapter(-1);
                return;
            }
        }
        if (position > 5.0 || mIndex == 0)
            mPlayer->seekAbsolute(0.0);
        else
            openPlaylistEntry(mIndex - 1);
    }
    else {
        if (!chapters.empty() && chapter + 1 < static_cast<int>(chapters.size())) {
            mPlayer->changeChapter(1);
            return;
        }
        if (mIndex + 1 < mPlaylist.size())
            openPlaylistEntry(mIndex + 1);
    }
}

void GuiVideoPlayer::requestStop()
{
    // Stopping by accident is avoided by asking for a second press.
    if (mStopConfirmTimer > 0) {
        mWindow->stopInfoPopup();
        close();
        return;
    }
    mStopConfirmTimer = STOP_CONFIRM_TIME;
    mWindow->queueInfoPopup("PRESS BACK ONE MORE TIME TO STOP THE VIDEO", STOP_CONFIRM_TIME);
}

void GuiVideoPlayer::close()
{
    if (mClosing)
        return;
    mClosing = true;

    if (mStream.active) {
        if (mStreamStarted) {
            sendStreamEvent("Player.OnStop",
                            mPlayer && mPlayer->getEndReason() == MpvPlayer::EndReason::END_OF_FILE);
        }
        mStreamStarted = false;
        AddonHost::getInstance().setPlayerState(AddonHost::PlayerState());
    }
    if (mPlayer && mPlayer->isInitialized()) {
        leaveCurrentFile(mPlayer->getEndReason() == MpvPlayer::EndReason::END_OF_FILE);
        const int volume {static_cast<int>(std::round(mPlayer->getVolume()))};
        if (volume != static_cast<int>(std::round(mInitialVolume))) {
            Settings::getInstance()->setInt("MediaVolume", volume);
            Settings::getInstance()->saveFile();
        }
    }

    const std::function<void(const std::string&)> callback {mClosedCallback};
    const std::string lastPath {mPath};
    delete this;
    if (callback)
        callback(lastPath);
}

//--------------------------------------------------------------------------------------------------
// Controls.
//--------------------------------------------------------------------------------------------------

void GuiVideoPlayer::showControls(OsdFocus focus)
{
    if (focus == OsdFocus::BUTTONS && mOsdFocus != OsdFocus::BUTTONS)
        mFocusedButton = BUTTON_PLAY_PAUSE;
    if (focus != OsdFocus::NONE)
        mOsdFocus = focus;
    mControlsVisible = true;
    mControlsTimer = CONTROLS_TIMEOUT;
    refreshHelpPrompts();
}

void GuiVideoPlayer::hideControls()
{
    // A seek that is still being chosen on the seek bar is applied.
    applyPendingSeek();
    mControlsVisible = false;
    mOsdFocus = OsdFocus::NONE;
    refreshHelpPrompts();
}

void GuiVideoPlayer::togglePause()
{
    if (mDubSession)
        mDubSession->onUserPause();
    mPlayer->togglePause();
    mSeekBarTimer = SEEK_BAR_TIMEOUT;
    if (mControlsVisible)
        mControlsTimer = CONTROLS_TIMEOUT;
    refreshHelpPrompts();
}

void GuiVideoPlayer::activateButton(int button)
{
    mControlsTimer = CONTROLS_TIMEOUT;
    switch (button) {
        case BUTTON_PREVIOUS: {
            skip(-1);
            break;
        }
        case BUTTON_REWIND: {
            addPendingSeek(-1, 30.0);
            break;
        }
        case BUTTON_PLAY_PAUSE: {
            togglePause();
            break;
        }
        case BUTTON_STOP: {
            close();
            break;
        }
        case BUTTON_FORWARD: {
            addPendingSeek(1, 30.0);
            break;
        }
        case BUTTON_NEXT: {
            skip(1);
            break;
        }
        case BUTTON_DUBX: {
            if (mDubSession)
                mDubSession->toggleDubx();
            break;
        }
        case BUTTON_PIXEL_RATIO: {
            openPixelRatioSlider();
            break;
        }
        case BUTTON_VOLUME: {
            openVolumeSlider();
            break;
        }
        case BUTTON_SUBTITLES: {
            openSubtitleSettings();
            break;
        }
        case BUTTON_SETTINGS: {
            openSettingsMenu();
            break;
        }
        default: {
            break;
        }
    }
}

bool GuiVideoPlayer::handleKeyboard(Input input)
{
    if (input.value == 0)
        return false;

    switch (input.id) {
        case SDLK_SPACE:
        case SDLK_p: {
            togglePause();
            return true;
        }
        case SDLK_ESCAPE: {
            if (mControlsVisible && mOsdFocus != OsdFocus::NONE)
                hideControls();
            else
                requestStop();
            return true;
        }
        case SDLK_TAB: {
            openSettingsMenu();
            return true;
        }
        case SDLK_i: {
            openInformation();
            return true;
        }
        case SDLK_o: {
            mCodecInfoVisible = !mCodecInfoVisible;
            mCodecInfoTimer = 0;
            return true;
        }
        case SDLK_t: {
            toggleSubtitles();
            return true;
        }
        case SDLK_d: {
            // Dubx, also with Shift as in earlier versions.
            if (mDubSession)
                mDubSession->toggleDubx();
            return true;
        }
        case SDLK_l: {
            cycleTrack("sub", 1);
            settingsChanged();
            mWindow->queueInfoPopup("SUBTITLES: " +
                                        Utils::String::toUpper(selectedTrackLabel("sub", true)),
                                    2000);
            return true;
        }
        case SDLK_m: {
            mPlayer->setMuted(!mPlayer->isMuted());
            mVolumeTimer = VOLUME_TIMEOUT;
            return true;
        }
        case SDLK_PLUS:
        case SDLK_EQUALS:
        case SDLK_KP_PLUS: {
            changeVolume(5.0);
            return true;
        }
        case SDLK_MINUS:
        case SDLK_KP_MINUS: {
            changeVolume(-5.0);
            return true;
        }
        case SDLK_PAGEUP: {
            changeVolume(5.0);
            return true;
        }
        case SDLK_PAGEDOWN: {
            changeVolume(-5.0);
            return true;
        }
        case SDLK_PERIOD: {
            skip(1);
            return true;
        }
        case SDLK_COMMA: {
            skip(-1);
            return true;
        }
        default: {
            return false;
        }
    }
}

bool GuiVideoPlayer::input(InputConfig* config, Input input)
{
    if (mClosing || mInitFailed)
        return true;
    // A progress dialog of the streaming add-on takes all input.
    if (mAddonDialogs && mAddonDialogs->input(config, input))
        return true;

    const bool keyboard {config->getDeviceId() == DEVICE_KEYBOARD};

    // The slider is shown on top of everything else, so it gets the input first.
    if (mSlider) {
        const bool sliderLeft {config->isMappedLike("left", input)};
        const bool sliderRight {config->isMappedLike("right", input)};
        if (sliderLeft || sliderRight) {
            if (input.value == 0) {
                mRepeatAction = RepeatAction::NONE;
            }
            else {
                adjustSlider(sliderLeft ? -1 : 1, 0);
                if (!keyboard) {
                    startRepeat(sliderLeft ? RepeatAction::SLIDER_DOWN : RepeatAction::SLIDER_UP);
                }
            }
            return true;
        }
        if (input.value != 0 &&
            (config->isMappedTo("a", input) || config->isMappedTo("b", input) ||
             (keyboard && input.id == SDLK_ESCAPE)))
            closeSlider();
        return true;
    }

    if (!mDialogs.empty()) {
        mDialogs.input(config, input);
        refreshHelpPrompts();
        return true;
    }

    if (keyboard && handleKeyboard(input))
        return true;

    // The directional pad (and the arrow keys) navigate the on-screen controls while the
    // thumbsticks change the volume.
    const bool stickUp {config->isMappedTo("leftthumbstickup", input) ||
                        config->isMappedTo("rightthumbstickup", input)};
    const bool stickDown {config->isMappedTo("leftthumbstickdown", input) ||
                          config->isMappedTo("rightthumbstickdown", input)};
    const bool stickSideways {config->isMappedTo("leftthumbstickleft", input) ||
                              config->isMappedTo("leftthumbstickright", input) ||
                              config->isMappedTo("rightthumbstickleft", input) ||
                              config->isMappedTo("rightthumbstickright", input)};
    const bool left {config->isMappedTo("left", input)};
    const bool right {config->isMappedTo("right", input)};
    const bool up {config->isMappedTo("up", input)};
    const bool down {config->isMappedTo("down", input)};

    if (up || down)
        mLastVerticalInput = SDL_GetTicks();

    if (input.value == 0) {
        // Button released.
        if (left || right || up || down || stickUp || stickDown) {
            mRepeatAction = RepeatAction::NONE;
            if (mSeekPending && (left || right) && mOsdFocus != OsdFocus::SEEK_BAR)
                mSeekTimer = std::min(mSeekTimer, SEEK_APPLY_DELAY_RELEASED);
        }
        return true;
    }

    if (stickSideways)
        return true;

    if (stickUp || stickDown) {
        changeVolume(stickUp ? 5.0 : -5.0);
        startRepeat(stickUp ? RepeatAction::VOLUME_UP : RepeatAction::VOLUME_DOWN);
        return true;
    }

    // The operating system repeats keyboard keys that are held down, so these are throttled
    // instead of using the repeat timer.
    if (keyboard && (left || right || up || down)) {
        const unsigned int now {SDL_GetTicks()};
        if (now - mLastKeyboardRepeat < KEYBOARD_REPEAT_THROTTLE &&
            !(mOsdFocus == OsdFocus::SEEK_BAR && (left || right)))
            return true;
        mLastKeyboardRepeat = now;
    }

    const OsdFocus focus {mControlsVisible ? mOsdFocus : OsdFocus::NONE};

    if (up || down) {
        if (focus == OsdFocus::NONE)
            showControls(down ? OsdFocus::BUTTONS : OsdFocus::SEEK_BAR);
        else if (focus == OsdFocus::BUTTONS && up)
            showControls(OsdFocus::SEEK_BAR);
        else if (focus == OsdFocus::SEEK_BAR && down) {
            applyPendingSeek();
            showControls(OsdFocus::BUTTONS);
        }
        else
            mControlsTimer = CONTROLS_TIMEOUT;
        return true;
    }

    if (left || right) {
        // Most likely pressed by accident while moving up or down on the controls.
        if (focus != OsdFocus::NONE && SDL_GetTicks() - mLastVerticalInput < 250)
            return true;
        if (focus == OsdFocus::BUTTONS) {
            mFocusedButton = std::clamp(mFocusedButton + (left ? -1 : 1), 0, BUTTON_COUNT - 1);
            // Without Dubx its button isn't there.
            if (!buttonShown(mFocusedButton))
                mFocusedButton = std::clamp(mFocusedButton + (left ? -1 : 1), 0, BUTTON_COUNT - 1);
            mControlsTimer = CONTROLS_TIMEOUT;
        }
        else {
            addPendingSeek(left ? -1 : 1, 10.0);
            if (!keyboard)
                startRepeat(left ? RepeatAction::SEEK_BACK : RepeatAction::SEEK_FORWARD);
        }
        return true;
    }
    if (config->isMappedTo("a", input)) {
        if (focus == OsdFocus::BUTTONS)
            activateButton(mFocusedButton);
        else if (focus == OsdFocus::SEEK_BAR && mSeekPending)
            applyPendingSeek();
        else if (focus == OsdFocus::SEEK_BAR)
            togglePause();
        else
            showControls(OsdFocus::BUTTONS);
        return true;
    }
    if (config->isMappedTo("b", input)) {
        if (focus != OsdFocus::NONE)
            hideControls();
        else
            requestStop();
        return true;
    }
    if (config->isMappedTo("x", input)) {
        togglePause();
        return true;
    }
    if (config->isMappedTo("y", input) || config->isMappedTo("start", input)) {
        openSettingsMenu();
        return true;
    }
    if (config->isMappedTo("back", input)) {
        openInformation();
        return true;
    }
    if (config->isMappedLike("leftshoulder", input) || config->isMappedLike("rightshoulder", input)) {
        addPendingSeek(config->isMappedLike("leftshoulder", input) ? -1 : 1, 60.0);
        return true;
    }
    if (config->isMappedLike("lefttrigger", input) || config->isMappedLike("righttrigger", input)) {
        skip(config->isMappedLike("lefttrigger", input) ? -1 : 1);
        mSeekBarTimer = SEEK_BAR_TIMEOUT;
        return true;
    }
    return true;
}

void GuiVideoPlayer::addPendingSeek(int direction, double step)
{
    if (!mPlayer->isFileLoaded())
        return;

    if (!mSeekPending) {
        mSeekPending = true;
        mSeekTarget = mPlayer->getPosition();
        mSeekCount = 0;
    }

    // Holding the button makes the steps longer.
    double amount {step};
    if (step <= 10.0 && mSeekCount >= 16)
        amount = 60.0;
    else if (step <= 10.0 && mSeekCount >= 6)
        amount = 30.0;

    const double duration {mPlayer->getDuration()};
    mSeekTarget += direction * amount;
    if (duration > 0.0)
        mSeekTarget = std::min(mSeekTarget, std::max(0.0, duration - 1.0));
    mSeekTarget = std::max(0.0, mSeekTarget);
    ++mSeekCount;
    mSeekTimer = SEEK_APPLY_DELAY;
    mSeekBarTimer = SEEK_BAR_TIMEOUT;
    if (mControlsVisible)
        mControlsTimer = CONTROLS_TIMEOUT;
}

void GuiVideoPlayer::applyPendingSeek()
{
    if (!mSeekPending)
        return;
    mSeekPending = false;
    mPlayer->seekAbsolute(mSeekTarget);
    mLastPosition = mSeekTarget;
    mSeekBarTimer = std::max(mSeekBarTimer, 1500);
}

void GuiVideoPlayer::changeVolume(double amount)
{
    if (mPlayer->isMuted())
        mPlayer->setMuted(false);
    mPlayer->setVolume(std::round(mPlayer->getVolume() + amount));
    mVolumeTimer = VOLUME_TIMEOUT;
}

void GuiVideoPlayer::startRepeat(RepeatAction action)
{
    mRepeatAction = action;
    mRepeatTimer = REPEAT_DELAY_INITIAL;
    mRepeatCount = 0;
}

void GuiVideoPlayer::performRepeat()
{
    switch (mRepeatAction) {
        case RepeatAction::SEEK_BACK:
        case RepeatAction::SEEK_FORWARD: {
            addPendingSeek(mRepeatAction == RepeatAction::SEEK_BACK ? -1 : 1, 10.0);
            mRepeatTimer += REPEAT_DELAY_SEEK;
            break;
        }
        case RepeatAction::VOLUME_DOWN:
        case RepeatAction::VOLUME_UP: {
            changeVolume(mRepeatAction == RepeatAction::VOLUME_DOWN ? -5.0 : 5.0);
            mRepeatTimer += REPEAT_DELAY_VOLUME;
            break;
        }
        case RepeatAction::SLIDER_DOWN:
        case RepeatAction::SLIDER_UP: {
            ++mRepeatCount;
            adjustSlider(mRepeatAction == RepeatAction::SLIDER_DOWN ? -1 : 1, mRepeatCount);
            mRepeatTimer += REPEAT_DELAY_VOLUME;
            break;
        }
        default: {
            mRepeatTimer = 0;
            break;
        }
    }
}

void GuiVideoPlayer::update(int deltaTime)
{
    if (mClosing)
        return;

    if (mInitFailed) {
        mWindow->queueInfoPopup("COULDN'T START THE VIDEO PLAYER", 4000);
        close();
        return;
    }

    mPlayer->update();

    if (mStream.active) {
        updateStreamState();
        mAddonDialogs->update(deltaTime);
    }

    if (mPlayer->takeFileLoadedEvent())
        applyFileSettings();
    // The tracks are known once the first frame has been shown.
    if (!mTracksHandled && mPlayer->hasVideoFrame()) {
        mTracksHandled = true;
        handleTracksOnStart();
    }
    pollSubtitleDownloads();
    if (mDubSession)
        mDubSession->update(deltaTime, mTracksHandled);
    if (mPixelRatioPending && mPlayer->hasVideoFrame())
        applyPixelRatio();

    if (mPlayer->isFileLoaded()) {
        if (mPlayer->getDuration() > 0.0)
            mLastDuration = mPlayer->getDuration();
        if (!mSeekPending && !mPlayer->isBuffering())
            mLastPosition = mPlayer->getPosition();
    }

    const MpvPlayer::EndReason endReason {mPlayer->getEndReason()};
    if (endReason != MpvPlayer::EndReason::NONE) {
        if (endReason == MpvPlayer::EndReason::FAILED) {
            std::string message {"COULDN'T PLAY THE FILE"};
            if (!mPlayer->getErrorMessage().empty())
                message.append(" (" + Utils::String::toUpper(mPlayer->getErrorMessage()) + ")");
            mWindow->queueInfoPopup(message, 5000);
        }
        else if (endReason == MpvPlayer::EndReason::END_OF_FILE &&
                 Settings::getInstance()->getBool("MediaPlayNext") &&
                 mIndex + 1 < mPlaylist.size()) {
            // Continue with the next video in the folder.
            leaveCurrentFile(true);
            startFile(mIndex + 1, 0.0);
            return;
        }
        close();
        return;
    }

    if (mSeekPending) {
        mSeekTimer -= deltaTime;
        if (mSeekTimer <= 0 && mRepeatAction != RepeatAction::SEEK_BACK &&
            mRepeatAction != RepeatAction::SEEK_FORWARD)
            applyPendingSeek();
        if (mControlsVisible)
            mControlsTimer = CONTROLS_TIMEOUT;
    }

    if (mRepeatAction != RepeatAction::NONE) {
        mRepeatTimer -= deltaTime;
        while (mRepeatTimer <= 0 && mRepeatAction != RepeatAction::NONE)
            performRepeat();
    }

    // The controls are hidden after a while, except when paused.
    if (mControlsVisible && !mPlayer->isPaused() && !mSeekPending && mDialogs.empty() &&
        !mSlider) {
        mControlsTimer -= deltaTime;
        if (mControlsTimer <= 0)
            hideControls();
    }

    if (mSeekBarTimer > 0)
        mSeekBarTimer -= deltaTime;
    if (mStopConfirmTimer > 0)
        mStopConfirmTimer -= deltaTime;
    if (mVolumeTimer > 0)
        mVolumeTimer -= deltaTime;

    if (mCodecInfoVisible) {
        mCodecInfoTimer -= deltaTime;
        if (mCodecInfoTimer <= 0) {
            updateCodecInfo();
            mCodecInfoTimer = CODEC_INFO_REFRESH_INTERVAL;
        }
    }

    // Store the position now and then, in case the application is closed unexpectedly.
    mSaveTimer -= deltaTime;
    if (mSaveTimer <= 0 && !mStream.active && !mStream.trailer) {
        mSaveTimer = PROGRESS_SAVE_INTERVAL;
        if (mLastDuration > 0.0 && mLastPosition < mLastDuration * 0.9)
            MediaLibrary::getInstance().savePlaybackProgress(mPath, mLastPosition, mLastDuration);
    }

    mDialogs.update(deltaTime);
    // Values such as the playback position are shown live in some dialogs.
    mDialogRefreshTimer -= deltaTime;
    if (mDialogRefreshTimer <= 0) {
        mDialogRefreshTimer = 250;
        mDialogs.refresh();
    }
}

//--------------------------------------------------------------------------------------------------
// Dub, see DubSession.
//--------------------------------------------------------------------------------------------------

MediaDialog* GuiVideoPlayer::pushDubDialog(const std::string& title, float widthFraction)
{
    return pushDialog(title, widthFraction);
}

void GuiVideoPlayer::addDubCloseOption(std::vector<MediaDialog::Option>& options,
                                       MediaDialog* dialog)
{
    addCloseOption(options, dialog);
}

void GuiVideoPlayer::refreshDubHelpPrompts() { refreshHelpPrompts(); }

const MpvPlayer::Track* GuiVideoPlayer::selectedDubSubtitle() const
{
    return selectedTrack("sub");
}

const MpvPlayer::Track* GuiVideoPlayer::selectedDubAudio() const
{
    return selectedTrack("audio");
}

std::string GuiVideoPlayer::dubTitle() const
{
    return mStream.active || mStream.trailer ? streamTitle() : Utils::FileSystem::getStem(mPath);
}

std::string GuiVideoPlayer::dubVideoPath() const { return mPath; }

void GuiVideoPlayer::startContextLookup(bool series, int id, const std::string& extra)
{
    // A detached thread, so that closing the player doesn't wait for the lookup.
    auto promise {std::make_shared<std::promise<std::string>>()};
    mContextFuture = promise->get_future();
    std::thread([promise, series, id, extra] {
        std::string context;
        MediaMetadata::lookupContext(series, id, context);
        if (!context.empty() && !extra.empty())
            context.append(" " + extra);
        promise->set_value(context);
    }).detach();
}

std::string GuiVideoPlayer::dubContext() const
{
    // The lookup normally takes well under a second, it was started with the video.
    if (mContextFuture.valid() &&
        mContextFuture.wait_for(std::chrono::milliseconds(3000)) == std::future_status::ready)
        mContext = mContextFuture.get();
    return mContext;
}

void GuiVideoPlayer::takeStreamOrigin(int maxWait) const
{
    if (!mOriginFuture.valid())
        return;
    if (mOriginFuture.wait_for(std::chrono::milliseconds(maxWait)) != std::future_status::ready)
        return;
    const auto [country, language] = mOriginFuture.get();
    // The name of the country from the add-on stays if TMDb didn't know it.
    if (!country.empty())
        mStream.originCountry = country;
    if (!language.empty())
        mStream.originalLanguage = language;
}

std::string GuiVideoPlayer::dubOriginCountry() const
{
    // The lookup normally takes well under a second, it was started with the video.
    takeStreamOrigin(3000);
    return mStream.active ? mStream.originCountry : "";
}

std::string GuiVideoPlayer::dubOriginLanguage() const
{
    takeStreamOrigin(3000);
    return mStream.active ? mStream.originalLanguage : "";
}

bool GuiVideoPlayer::isDubStream() const { return mStream.active; }

SubtitleDownloader::Query GuiVideoPlayer::dubSubtitleQuery() const
{
    SubtitleDownloader::Query query {SubtitleDownloader::parseVideoPath(subtitleVideoPath())};
    if (mStream.active) {
        const SubtitleDownloader::Query stream {streamSubtitleQuery()};
        query.title = stream.title;
        query.year = stream.year;
        query.season = stream.season;
        query.episode = stream.episode;
    }
    return query;
}

SubtitleDownloader::Query GuiVideoPlayer::streamSubtitleQuery() const
{
    // The add-on knows what is being played.
    SubtitleDownloader::Query query;
    query.title = stripAddonFormatting(mStream.showTitle.empty() ? mStream.title :
                                                                   mStream.showTitle);
    query.year = mStream.year;
    query.season = mStream.season;
    query.episode = mStream.episode;
    if (!mStream.showTitle.empty() || mStream.season > 0 || mStream.year > 0)
        return query;
    // Otherwise the title is the name of the torrent, e.g. "Tulsa King (2022) Season 1 S01
    // (1080p ...) [QxR] ... 👤 480 💾 1.69 GB", so the file name in the address is better,
    // e.g. "Tulsa King (2022) - S01E01 - Go West, Old Man (1080p BluRay x265).mkv".
    std::string address {mPath.substr(0, mPath.find('?'))};
    std::string fileName {HttpClient::urlDecode(address.substr(address.rfind('/') + 1))};
    const std::string extension {Utils::String::toLower(Utils::FileSystem::getExtension(fileName))};
    if (extension != ".mkv" && extension != ".mp4" && extension != ".avi" &&
        extension != ".m4v" && extension != ".webm" && extension != ".ts" &&
        extension != ".mov" && extension != ".wmv")
        fileName.clear();
    for (const std::string& name : {fileName, query.title + ".mkv"}) {
        if (name.empty() || name == ".mkv")
            continue;
        std::string safe;
        for (const char character : name) {
            if (character != '/' && character != '\\')
                safe.push_back(character);
        }
        const SubtitleDownloader::Query parsed {SubtitleDownloader::parseVideoPath("/" + safe)};
        if (!parsed.title.empty()) {
            query.title = parsed.title;
            query.year = parsed.year;
            query.season = parsed.season;
            query.episode = parsed.episode;
            break;
        }
    }
    return query;
}

std::string GuiVideoPlayer::dubSubtitleVideoPath() const { return subtitleVideoPath(); }

double GuiVideoPlayer::dubStartPosition() const
{
    return mSeekPending ? mSeekTarget : mPlayer->getPosition();
}

void GuiVideoPlayer::applyDubAudioFilters() { applyAudioFilters(dynamicRangeEnabled()); }

bool GuiVideoPlayer::dynamicRangeEnabled() const
{
    return mPlayer->getPropertyString("af").find("dynaudnorm") != std::string::npos;
}

void GuiVideoPlayer::applyAudioFilters(bool dynamicRange)
{
    // The dub adds a filter for lowering the volume of the video while it speaks.
    const bool ducking {mDubSession && mDubSession->wantsDuckFilter()};
    mPlayer->setProperty("af", ducking ?
                                   MpvPlayer::audioFiltersWithDucking(
                                       dynamicRange, mAmplification,
                                       mDubSession->getDuckFilterGraph()) :
                                   MpvPlayer::audioFilters(dynamicRange, mAmplification));
    if (mDubSession)
        mDubSession->onAudioFiltersApplied(ducking);
}

//--------------------------------------------------------------------------------------------------
// Streams.
//--------------------------------------------------------------------------------------------------

std::string GuiVideoPlayer::streamTitle() const
{
    std::string title {stripAddonFormatting(mStream.title)};
    if (mStream.season > 0 && mStream.episode > 0) {
        char code[32];
        snprintf(code, sizeof(code), "S%02dE%02d", mStream.season, mStream.episode);
        title = (mStream.showTitle.empty() ? "" : stripAddonFormatting(mStream.showTitle) +
                                                      " - ") +
                code + (title.empty() ? "" : " - " + title);
    }
    else if (mStream.year > 0) {
        title.append(" (" + std::to_string(mStream.year) + ")");
    }
    return title.empty() ? "Elementum" : title;
}

std::string GuiVideoPlayer::subtitleVideoPath() const
{
    if (!mStream.active)
        return mPath;
    // Named like a video file, e.g. "The Boys S03E08.mkv", in the subtitle directory.
    const SubtitleDownloader::Query stream {streamSubtitleQuery()};
    std::string name {stream.title};
    if (stream.season > 0 && stream.episode > 0) {
        char code[32];
        snprintf(code, sizeof(code), " S%02dE%02d", stream.season, stream.episode);
        name.append(code);
    }
    else if (stream.year > 0) {
        name.append(" (" + std::to_string(stream.year) + ")");
    }
    std::string safe;
    for (const char character : stripAddonFormatting(name)) {
        if (character == '/' || character == '\\' || character == ':' || character == '?' ||
            character == '*' || character == '"' || character == '<' || character == '>' ||
            character == '|')
            continue;
        safe.push_back(character);
    }
    return SubtitleDownloader::getDownloadDirectory() + "/" +
           (safe.empty() ? std::string("Stream") : safe) + ".mkv";
}

void GuiVideoPlayer::sendStreamEvent(const std::string& method, bool ended)
{
    const std::string type {mStream.mediaType == "episode" ? "episode" : "movie"};
    std::string data;
    if (method == "Player.OnStop") {
        data = std::string("{\"end\":") + (ended ? "true" : "false") +
               ",\"item\":{\"type\":\"" + type + "\"}}";
    }
    else {
        const int speed {method == "Player.OnPause" ? 0 : 1};
        data = "{\"item\":{\"type\":\"" + type + "\"},\"player\":{\"playerid\":1,\"speed\":" +
               std::to_string(speed) + "}}";
    }
    AddonHost::getInstance().sendPlayerEvent(method, data);
}

void GuiVideoPlayer::updateStreamState()
{
    AddonHost& host {AddonHost::getInstance()};
    for (const auto& command : host.takePlayerCommands()) {
        if (command.type == AddonHost::PlayerCommand::Type::SEEK) {
            // The add-on can ask for the same seek again, e.g. for resuming, if it didn't
            // notice the first one. Going back there again after playing on would repeat
            // the same part over and over.
            const unsigned int now {SDL_GetTicks()};
            if (mLastAddonSeekTicks != 0 && now - mLastAddonSeekTicks < 60000 &&
                std::abs(command.position - mLastAddonSeek) < 2.0 &&
                mPlayer->getPosition() > command.position + 2.0) {
                LOG(LogInfo) << "GuiVideoPlayer: Ignored the repeated seek to "
                             << command.position << " s asked by the streaming add-on";
                continue;
            }
            LOG(LogInfo) << "GuiVideoPlayer: Seeking to " << command.position
                         << " s as asked by the streaming add-on";
            mPlayer->seekAbsolute(command.position);
            mLastPosition = command.position;
            mLastAddonSeek = command.position;
            mLastAddonSeekTicks = now;
        }
        else {
            for (const auto& path : command.paths) {
                if (!path.empty())
                    mPlayer->addSubtitleFile(path);
            }
        }
    }

    AddonHost::PlayerState state;
    state.active = !mClosing && mPlayer->isFileLoaded();
    state.paused = mPlayer->isPaused();
    state.position = mPlayer->getPosition();
    state.duration = mPlayer->getDuration();
    state.file = mPath;
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type == "sub")
            state.subtitles.emplace_back(track.title.empty() ? track.lang : track.title);
    }
    state.labels["Title"] = mStream.title;
    state.labels["OriginalTitle"] = mStream.originalTitle;
    state.labels["TVShowTitle"] = mStream.showTitle;
    state.labels["Year"] = mStream.year > 0 ? std::to_string(mStream.year) : "";
    state.labels["Season"] = mStream.season > 0 ? std::to_string(mStream.season) : "";
    state.labels["Episode"] = mStream.episode > 0 ? std::to_string(mStream.episode) : "";
    state.labels["IMDBNumber"] = mStream.imdb;
    host.setPlayerState(state);

    // The events the add-on uses to follow the playback.
    if (state.active && !mStreamStarted && state.duration > 0.0) {
        mStreamStarted = true;
        mStreamPaused = state.paused;
        mStreamPosition = state.position;
        sendStreamEvent("Player.OnPlay");
        return;
    }
    if (!mStreamStarted)
        return;
    if (state.paused != mStreamPaused) {
        mStreamPaused = state.paused;
        sendStreamEvent(state.paused ? "Player.OnPause" : "Player.OnResume");
    }
    // The position isn't followed while seeking or buffering, so that a seek is reported once
    // the video plays at the new position, as the add-on may wait for it.
    if (!mPlayer->isBuffering()) {
        if (std::abs(state.position - mStreamPosition) > 4.0)
            sendStreamEvent("Player.OnSeek");
        mStreamPosition = state.position;
    }
}

//--------------------------------------------------------------------------------------------------
// Tracks and settings helpers.
//--------------------------------------------------------------------------------------------------

std::string GuiVideoPlayer::trackLabel(const MpvPlayer::Track& track) const
{
    std::vector<std::string> parts;
    const std::string language {track.lang.empty() ? "" : languageName(track.lang)};
    std::string first {language};
    if (!track.title.empty() &&
        Utils::String::toLower(track.title) != Utils::String::toLower(language))
        first = first.empty() ? track.title : first + " - " + track.title;
    if (!first.empty())
        parts.emplace_back(first);
    if (!track.codec.empty())
        parts.emplace_back(Utils::String::toUpper(track.codec));
    if (track.type == "audio" && track.channels > 0)
        parts.emplace_back(channelLayout(track.channels));
    if (track.type == "video" && track.width > 0 && track.height > 0)
        parts.emplace_back(std::to_string(track.width) + "x" + std::to_string(track.height));

    std::string label;
    for (const auto& part : parts) {
        if (!label.empty())
            label.append(", ");
        label.append(part);
    }
    if (label.empty())
        label = "Track " + std::to_string(track.id);
    if (track.isDefault)
        label.append(" [default]");
    if (track.forced)
        label.append(" [forced]");
    if (track.external)
        label.append(" [external]");
    return label;
}

const MpvPlayer::Track* GuiVideoPlayer::selectedTrack(const std::string& type) const
{
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type == type && track.selected)
            return &track;
    }
    return nullptr;
}

int GuiVideoPlayer::trackCount(const std::string& type) const
{
    int count {0};
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type == type)
            ++count;
    }
    return count;
}

std::string GuiVideoPlayer::selectedTrackLabel(const std::string& type, bool withIndex) const
{
    // Dubx is shown as the last audio streams while it's on.
    const int dubxTracks {type == "audio" && mDubSession ? mDubSession->getAudioTrackCount() : 0};
    if (dubxTracks > 0) {
        const int selected {mDubSession->getSelectedAudioTrack()};
        std::string label {mDubSession->getAudioTrackLabel(selected)};
        const int count {trackCount(type) + dubxTracks};
        if (withIndex)
            label.append(" (" + std::to_string(trackCount(type) + selected + 1) + "/" +
                         std::to_string(count) + ")");
        return label;
    }
    const int count {trackCount(type)};
    if (count == 0)
        return "None";
    int index {0};
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type != type)
            continue;
        ++index;
        if (track.selected) {
            std::string label {trackLabel(track)};
            if (withIndex)
                label.append(" (" + std::to_string(index) + "/" + std::to_string(count) + ")");
            return label;
        }
    }
    return "Off";
}

void GuiVideoPlayer::cycleTrack(const std::string& type, int direction)
{
    // Disabled (0) is included for subtitles only.
    std::vector<int64_t> ids;
    if (type == "sub")
        ids.emplace_back(0);
    int64_t current {0};
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type != type)
            continue;
        ids.emplace_back(track.id);
        if (track.selected)
            current = track.id;
    }
    // Dubx (-1, -2) is the last audio streams while it's on, choosing another one turns it off.
    const int dubxTracks {type == "audio" && mDubSession ? mDubSession->getAudioTrackCount() : 0};
    const bool dubx {dubxTracks > 0};
    for (int track {0}; track < dubxTracks; ++track)
        ids.emplace_back(-1 - track);
    if (dubx)
        current = -1 - mDubSession->getSelectedAudioTrack();
    if (ids.size() < 2)
        return;

    int index {0};
    for (size_t i {0}; i < ids.size(); ++i) {
        if (ids[i] == current)
            index = static_cast<int>(i);
    }
    const int count {static_cast<int>(ids.size())};
    index = ((index + direction) % count + count) % count;
    if (ids[index] < 0) {
        mDubSession->selectAudioTrack(static_cast<int>(-1 - ids[index]));
        return;
    }
    if (dubx)
        mDubSession->toggle(DubSession::Mode::DUBX);
    mPlayer->selectTrack(type, ids[index]);
    if (type == "sub") {
        if (ids[index] != 0)
            mPlayer->setSubtitleVisible(true);
        mSubtitleChanged = true;
    }
}

bool GuiVideoPlayer::subtitlesEnabled() const
{
    return selectedTrack("sub") != nullptr && mPlayer->isSubtitleVisible();
}

void GuiVideoPlayer::toggleSubtitles()
{
    if (subtitlesEnabled()) {
        mPlayer->setSubtitleVisible(false);
    }
    else if (selectedTrack("sub") != nullptr) {
        mPlayer->setSubtitleVisible(true);
    }
    else {
        // No track selected yet, so pick the first one.
        for (const auto& track : mPlayer->getTracks()) {
            if (track.type == "sub") {
                mPlayer->selectTrack("sub", track.id);
                mPlayer->setSubtitleVisible(true);
                break;
            }
        }
    }
    settingsChanged();
    mSubtitleChanged = true;
    const bool enabled {selectedTrack("sub") != nullptr && mPlayer->isSubtitleVisible()};
    if (trackCount("sub") == 0)
        mWindow->queueInfoPopup("NO SUBTITLES AVAILABLE", 2000);
    else
        mWindow->queueInfoPopup(enabled ? "SUBTITLES ON" : "SUBTITLES OFF", 1500);
}

void GuiVideoPlayer::applyViewMode(const std::string& mode)
{
    mViewMode = mode;
    if (mode == "zoom") {
        mPlayer->setProperty("keepaspect", "yes");
        mPlayer->setProperty("video-unscaled", "no");
        mPlayer->setProperty("panscan", "1.0");
    }
    else if (mode == "widezoom") {
        mPlayer->setProperty("keepaspect", "yes");
        mPlayer->setProperty("video-unscaled", "no");
        mPlayer->setProperty("panscan", "0.5");
    }
    else if (mode == "stretch") {
        mPlayer->setProperty("keepaspect", "no");
        mPlayer->setProperty("video-unscaled", "no");
        mPlayer->setProperty("panscan", "0.0");
    }
    else if (mode == "original") {
        mPlayer->setProperty("keepaspect", "yes");
        mPlayer->setProperty("video-unscaled", "yes");
        mPlayer->setProperty("panscan", "0.0");
    }
    else {
        mViewMode = "normal";
        mPlayer->setProperty("keepaspect", "yes");
        mPlayer->setProperty("video-unscaled", "no");
        mPlayer->setProperty("panscan", "0.0");
    }
}

//--------------------------------------------------------------------------------------------------
// Dialogs.
//--------------------------------------------------------------------------------------------------

MediaDialog* GuiVideoPlayer::pushDialog(const std::string& title, float widthFraction)
{
    // Pending seeks would otherwise be applied with a delay while the dialog is open.
    applyPendingSeek();
    mRepeatAction = RepeatAction::NONE;
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(title, widthFraction))};
    dialog->setCancelCallback([this] { refreshHelpPrompts(); });
    return dialog;
}

void GuiVideoPlayer::addCloseOption(std::vector<MediaDialog::Option>& options, MediaDialog* dialog)
{
    MediaDialog::Option close;
    close.label = "Close";
    close.dimmed = true;
    close.onSelect = [dialog] { dialog->close(); };
    options.emplace_back(close);
}

void GuiVideoPlayer::openSettingsMenu()
{
    MediaDialog* dialog {pushDialog("Settings", 0.38f)};
    std::vector<MediaDialog::Option> options;

    MediaDialog::Option audio;
    audio.label = "Audio settings";
    audio.onSelect = [this] { openAudioSettings(); };
    options.emplace_back(audio);

    MediaDialog::Option subtitles;
    subtitles.label = "Subtitle settings";
    subtitles.onSelect = [this] { openSubtitleSettings(); };
    options.emplace_back(subtitles);

    MediaDialog::Option video;
    video.label = "Video settings";
    video.onSelect = [this] { openVideoSettings(); };
    options.emplace_back(video);

    MediaDialog::Option information;
    information.label = "Information";
    information.onSelect = [this] { openInformation(); };
    options.emplace_back(information);

    MediaDialog::Option toggle;
    toggle.label = "Toggle subtitles";
    toggle.value = [this] {
        const MpvPlayer::Track* track {selectedTrack("sub")};
        if (track == nullptr || !mPlayer->isSubtitleVisible())
            return std::string("Off");
        return track->lang.empty() ? std::string("On") : Utils::String::toLower(track->lang);
    };
    toggle.onSelect = [this] { toggleSubtitles(); };
    options.emplace_back(toggle);

    addCloseOption(options, dialog);
    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiVideoPlayer::openAudioSettings()
{
    MediaDialog* dialog {pushDialog("Audio settings", 0.56f)};
    MpvPlayer* player {mPlayer.get()};
    Settings* settings {Settings::getInstance()};
    std::vector<MediaDialog::Option> options;

    MediaDialog::Option volume;
    volume.label = "Volume";
    volume.value = [player] {
        if (player->isMuted())
            return std::string("Muted");
        return std::to_string(static_cast<int>(std::round(player->getVolume()))) + "%";
    };
    volume.onSelect = [player] { player->setMuted(!player->isMuted()); };
    volume.onAdjust = [this](int direction) { changeVolume(direction * 5.0); };
    options.emplace_back(volume);

    // Makes quiet videos louder, up to 60 dB.
    MediaDialog::Option amplification;
    amplification.label = "Volume amplification";
    amplification.value = [this] { return std::to_string(mAmplification) + ".0 dB"; };
    amplification.onSelect = [this] {
        mAmplification = 0;
        applyAudioFilters(dynamicRangeEnabled());
    };
    amplification.onAdjust = [this](int direction) {
        mAmplification = std::clamp(mAmplification + direction, 0, 60);
        applyAudioFilters(dynamicRangeEnabled());
    };
    options.emplace_back(amplification);

    MediaDialog::Option delay;
    delay.label = "Audio offset";
    delay.value = [player] { return formatOffset(player->getAudioDelay()); };
    delay.onSelect = [this, player] {
        player->setAudioDelay(0.0);
        settingsChanged();
    };
    delay.onAdjust = [this, player](int direction) {
        player->setAudioDelay(std::round((player->getAudioDelay() + direction * 0.025) * 1000.0) /
                              1000.0);
        settingsChanged();
    };
    options.emplace_back(delay);

    MediaDialog::Option stream;
    stream.label = "Audio stream";
    stream.value = [this] { return selectedTrackLabel("audio", true); };
    stream.onSelect = [this] { openTrackDialog("audio"); };
    stream.onAdjust = [this](int direction) {
        cycleTrack("audio", direction);
        settingsChanged();
    };
    options.emplace_back(stream);

    // Reading the subtitles aloud.
    if (mDubSession)
        mDubSession->addAudioOptions(options);

    MediaDialog::Option passthrough;
    passthrough.label = "Passthrough to amplifier";
    passthrough.value = [settings] {
        return settings->getBool("MediaAudioPassthrough") ? "On" : "Off";
    };
    passthrough.onAdjust = [player, settings](int) {
        const bool enabled {!settings->getBool("MediaAudioPassthrough")};
        settings->setBool("MediaAudioPassthrough", enabled);
        settings->saveFile();
        player->setProperty("audio-spdif", enabled ? "ac3,eac3,dts,dts-hd,truehd" : "");
    };
    options.emplace_back(passthrough);

    MediaDialog::Option compression;
    compression.label = "Dynamic range compression";
    compression.value = [player] {
        return player->getPropertyString("af").find("dynaudnorm") != std::string::npos ? "On" :
                                                                                         "Off";
    };
    compression.onAdjust = [this](int) { applyAudioFilters(!dynamicRangeEnabled()); };
    options.emplace_back(compression);

    MediaDialog::Option defaults;
    defaults.label = "Set as default for all media";
    defaults.highlight = true;
    defaults.onSelect = [this, player, settings] {
        const MpvPlayer::Track* track {selectedTrack("audio")};
        if (track != nullptr && !track->lang.empty() && Utils::String::toLower(track->lang) != "und")
            settings->setString("MediaAudioLanguage", languageSettingCode(track->lang));
        settings->setBool("MediaDynamicRange",
                          player->getPropertyString("af").find("dynaudnorm") != std::string::npos);
        settings->setInt("MediaVolumeAmplification", mAmplification);
        settings->saveFile();
        mWindow->queueInfoPopup("AUDIO SETTINGS SAVED AS DEFAULT", 2500);
    };
    options.emplace_back(defaults);

    addCloseOption(options, dialog);
    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiVideoPlayer::openSubtitleSettings()
{
    MediaDialog* dialog {pushDialog("Subtitle settings", 0.56f)};
    MpvPlayer* player {mPlayer.get()};
    Settings* settings {Settings::getInstance()};
    std::vector<MediaDialog::Option> options;

    MediaDialog::Option enable;
    enable.label = "Enable subtitles";
    enable.value = [this] { return subtitlesEnabled() ? "On" : "Off"; };
    enable.onAdjust = [this](int) { toggleSubtitles(); };
    options.emplace_back(enable);

    MediaDialog::Option delay;
    delay.label = "Subtitle offset";
    delay.value = [player] { return formatOffset(player->getSubtitleDelay()); };
    delay.onSelect = [this, player] {
        player->setSubtitleDelay(0.0);
        settingsChanged();
    };
    delay.onAdjust = [this, player](int direction) {
        player->setSubtitleDelay(
            std::round((player->getSubtitleDelay() + direction * 0.1) * 1000.0) / 1000.0);
        settingsChanged();
    };
    options.emplace_back(delay);

    MediaDialog::Option stream;
    stream.label = "Subtitle";
    stream.value = [this] { return selectedTrackLabel("sub", true); };
    stream.onSelect = [this] { openTrackDialog("sub"); };
    stream.onAdjust = [this](int direction) {
        cycleTrack("sub", direction);
        settingsChanged();
    };
    options.emplace_back(stream);

    MediaDialog::Option browse;
    browse.label = "Browse for subtitle...";
    browse.onSelect = [this] { openSubtitleBrowser(); };
    options.emplace_back(browse);

    MediaDialog::Option download;
    download.label = "Download subtitle...";
    download.onSelect = [this] { openSubtitleDownload(); };
    options.emplace_back(download);

    MediaDialog::Option size;
    size.label = "Subtitle size";
    size.value = [player] {
        return std::to_string(static_cast<int>(std::round(player->getSubtitleScale() * 100.0))) +
               "%";
    };
    size.onSelect = [player, settings] {
        player->setSubtitleScale(settings->getInt("MediaSubtitleScale") / 100.0);
    };
    size.onAdjust = [player](int direction) {
        player->setSubtitleScale(std::round((player->getSubtitleScale() + direction * 0.1) * 10.0) /
                                 10.0);
    };
    options.emplace_back(size);

    MediaDialog::Option position;
    position.label = "Subtitle position";
    position.value = [player] {
        const int value {player->getSubtitlePosition()};
        return value == 100 ? std::string("Bottom") : std::to_string(value);
    };
    position.onSelect = [player, settings] {
        player->setSubtitlePosition(settings->getInt("MediaSubtitlePosition"));
    };
    position.onAdjust = [player](int direction) {
        // Lower values move the subtitles up.
        player->setSubtitlePosition(player->getSubtitlePosition() - direction * 2);
    };
    options.emplace_back(position);

    MediaDialog::Option defaults;
    defaults.label = "Set as default for all media";
    defaults.highlight = true;
    defaults.onSelect = [this, player, settings] {
        const MpvPlayer::Track* track {selectedTrack("sub")};
        if (!subtitlesEnabled())
            settings->setString("MediaSubtitleLanguage", "off");
        else if (track != nullptr && !track->lang.empty() &&
                 Utils::String::toLower(track->lang) != "und")
            settings->setString("MediaSubtitleLanguage", languageSettingCode(track->lang));
        else
            settings->setString("MediaSubtitleLanguage", "");
        settings->setInt("MediaSubtitleScale",
                         static_cast<int>(std::round(player->getSubtitleScale() * 100.0)));
        settings->setInt("MediaSubtitlePosition", player->getSubtitlePosition());
        settings->saveFile();
        mWindow->queueInfoPopup("SUBTITLE SETTINGS SAVED AS DEFAULT", 2500);
    };
    options.emplace_back(defaults);

    addCloseOption(options, dialog);
    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiVideoPlayer::openVideoSettings()
{
    MediaDialog* dialog {pushDialog("Video settings", 0.56f)};
    MpvPlayer* player {mPlayer.get()};
    Settings* settings {Settings::getInstance()};
    std::vector<MediaDialog::Option> options;

    if (trackCount("video") > 1) {
        MediaDialog::Option stream;
        stream.label = "Video stream";
        stream.value = [this] { return selectedTrackLabel("video", true); };
        stream.onAdjust = [this](int direction) {
            cycleTrack("video", direction);
            settingsChanged();
        };
        options.emplace_back(stream);
    }

    MediaDialog::Option viewMode;
    viewMode.label = "View mode";
    viewMode.value = [this] { return choiceLabel(VIEW_MODES, mViewMode); };
    viewMode.onAdjust = [this](int direction) {
        applyViewMode(cycleChoice(VIEW_MODES, mViewMode, direction));
        settingsChanged();
    };
    options.emplace_back(viewMode);

    MediaDialog::Option zoom;
    zoom.label = "Zoom amount";
    zoom.value = [player] {
        const double zoomValue {player->getPropertyDouble("video-zoom")};
        return std::to_string(static_cast<int>(std::round(std::pow(2.0, zoomValue) * 100.0))) +
               "%";
    };
    zoom.onSelect = [this, player] {
        Slider slider;
        slider.label = "Zoom amount";
        slider.min = 50.0;
        slider.max = 300.0;
        slider.step = 1.0;
        slider.getValue = [player] {
            return std::round(std::pow(2.0, player->getPropertyDouble("video-zoom")) * 100.0);
        };
        slider.setValue = [this, player](double value) {
            player->setProperty("video-zoom", std::to_string(std::log2(value / 100.0)));
            settingsChanged();
        };
        slider.format = [](double value) {
            return std::to_string(static_cast<int>(std::round(value))) + "%";
        };
        openSlider(slider);
    };
    zoom.onAdjust = [this, player](int direction) {
        const double percent {std::round(std::pow(2.0, player->getPropertyDouble("video-zoom")) *
                                         100.0)};
        const double newPercent {std::clamp(percent + direction * 5.0, 50.0, 300.0)};
        player->setProperty("video-zoom", std::to_string(std::log2(newPercent / 100.0)));
        settingsChanged();
    };
    options.emplace_back(zoom);

    MediaDialog::Option pixelRatio;
    pixelRatio.label = "Pixel ratio";
    pixelRatio.value = [this] {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%.2f", mPixelRatio);
        return std::string(buffer);
    };
    pixelRatio.onSelect = [this] { openPixelRatioSlider(); };
    pixelRatio.onAdjust = [this](int direction) {
        mPixelRatio = std::clamp(std::round((mPixelRatio + direction * 0.01) * 100.0) / 100.0,
                                 0.5, 2.0);
        mPixelRatioChanged = true;
        applyPixelRatio();
        settingsChanged();
    };
    options.emplace_back(pixelRatio);

    MediaDialog::Option deinterlace;
    deinterlace.label = "Deinterlace video";
    deinterlace.value = [player] {
        return player->getPropertyString("deinterlace") == "yes" ? "On" : "Off";
    };
    deinterlace.onAdjust = [this, player](int) {
        const bool enabled {player->getPropertyString("deinterlace") != "yes"};
        player->setProperty("deinterlace", enabled ? "yes" : "no");
        settingsChanged();
    };
    options.emplace_back(deinterlace);

    MediaDialog::Option speed;
    speed.label = "Playback speed";
    speed.value = [player] { return formatSpeed(player->getSpeed()); };
    speed.onSelect = [player] { player->setSpeed(1.0); };
    speed.onAdjust = [player](int direction) {
        static const std::vector<double> speeds {0.5, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 2.0};
        int index {3};
        for (size_t i {0}; i < speeds.size(); ++i) {
            if (std::abs(speeds[i] - player->getSpeed()) < 0.01)
                index = static_cast<int>(i);
        }
        index = std::clamp(index + direction, 0, static_cast<int>(speeds.size()) - 1);
        player->setSpeed(speeds[index]);
    };
    options.emplace_back(speed);

    MediaDialog::Option reset;
    reset.label = "Reset picture size and position";
    reset.onSelect = [this] {
        resetPicture();
        mWindow->queueInfoPopup("PICTURE SIZE AND POSITION RESET", 2000);
    };
    options.emplace_back(reset);

    MediaDialog::Option defaults;
    defaults.label = "Set as default for all media";
    defaults.highlight = true;
    defaults.onSelect = [this, player, settings] {
        settings->setString("MediaViewMode", mViewMode);
        settings->setBool("MediaDeinterlace", player->getPropertyString("deinterlace") == "yes");
        settings->saveFile();
        mWindow->queueInfoPopup("VIDEO SETTINGS SAVED AS DEFAULT", 2500);
    };
    options.emplace_back(defaults);

    addCloseOption(options, dialog);
    dialog->setOptions(options);
    refreshHelpPrompts();
}

void GuiVideoPlayer::openInformation()
{
    MediaDialog* dialog {pushDialog("Information", 0.62f)};
    MpvPlayer* player {mPlayer.get()};
    const std::string path {mPath};
    std::vector<MediaDialog::Option> options;

    auto addRow = [&options](const std::string& label, const std::function<std::string()>& value) {
        MediaDialog::Option option;
        option.label = label;
        option.value = value;
        options.emplace_back(option);
    };

    addRow("File", [path] { return Utils::FileSystem::getFileName(path); });
    addRow("Folder", [path] { return Utils::FileSystem::getParent(path); });
    const std::string size {formatFileSize(Utils::FileSystem::getFileSize(path))};
    addRow("Size", [size] { return size; });
    addRow("Duration", [player] {
        return player->getDuration() > 0.0 ? MediaLibrary::formatTime(player->getDuration()) : "";
    });
    addRow("Video", [player] {
        std::string video;
        const std::string width {player->getPropertyString("video-params/w")};
        const std::string height {player->getPropertyString("video-params/h")};
        if (!width.empty() && !height.empty())
            video.append(width + "x" + height + "  ");
        std::string codec {player->getPropertyString("video-codec-name")};
        if (codec.empty())
            codec = player->getPropertyString("video-format");
        video.append(Utils::String::toUpper(codec));
        const std::string fps {formatDecimal(player->getPropertyString("container-fps"), 3)};
        if (!fps.empty())
            video.append("  " + fps + " fps");
        const std::string transfer {player->getPropertyString("video-params/gamma")};
        if (transfer == "pq")
            video.append("  HDR10");
        else if (transfer == "hlg")
            video.append("  HLG");
        return video;
    });
    addRow("Audio", [this] { return selectedTrackLabel("audio", true); });
    addRow("Subtitles", [this] { return selectedTrackLabel("sub", true); });
    addRow("Chapters", [player] { return std::to_string(player->getChapters().size()); });
    const MediaLibrary::PlayState state {MediaLibrary::getInstance().getPlayState(path)};
    addRow("Times played", [state] { return std::to_string(state.playCount); });
    addRow("Last played", [state] { return formatDate(state.lastPlayed); });

    MediaDialog::Option codec;
    codec.label = "Show codec information";
    codec.highlight = true;
    codec.value = [this] { return mCodecInfoVisible ? "On" : "Off"; };
    codec.onAdjust = [this](int) {
        mCodecInfoVisible = !mCodecInfoVisible;
        mCodecInfoTimer = 0;
    };
    options.emplace_back(codec);

    addCloseOption(options, dialog);
    dialog->setOptions(options, static_cast<int>(options.size()) - 2);
    refreshHelpPrompts();
}

void GuiVideoPlayer::openTrackDialog(const std::string& type)
{
    MediaDialog* dialog {pushDialog(type == "audio" ? "Audio stream" : "Subtitle", 0.56f)};
    std::vector<MediaDialog::Option> options;
    int cursor {0};

    if (type == "sub") {
        MediaDialog::Option off;
        off.label = "Off";
        off.onSelect = [this, dialog] {
            mPlayer->selectTrack("sub", 0);
            settingsChanged();
            mSubtitleChanged = true;
            dialog->close();
        };
        options.emplace_back(off);
    }
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type != type)
            continue;
        if (track.selected)
            cursor = static_cast<int>(options.size());
        MediaDialog::Option option;
        option.label = trackLabel(track);
        const bool dubx {type == "audio" && mDubSession && mDubSession->getAudioTrackCount() > 0};
        option.value = [selected = track.selected, dubx] {
            return selected ? (dubx ? "Under Dubx" : "Active") : "";
        };
        const int64_t id {track.id};
        option.onSelect = [this, dialog, type, id] {
            // Under Dubx another audio stream stays under it: the dub is made again over that
            // stream (e.g. Japanese music without the Japanese voices). Dubx is turned off with
            // its own button.
            mPlayer->selectTrack(type, id);
            if (type == "sub") {
                mPlayer->setSubtitleVisible(true);
                mSubtitleChanged = true;
            }
            settingsChanged();
            dialog->close();
        };
        options.emplace_back(option);
    }
    const int dubxTracks {type == "audio" && mDubSession ? mDubSession->getAudioTrackCount() : 0};
    for (int track {0}; track < dubxTracks; ++track) {
        const bool selected {track == mDubSession->getSelectedAudioTrack()};
        if (selected)
            cursor = static_cast<int>(options.size());
        MediaDialog::Option dubx;
        dubx.label = mDubSession->getAudioTrackLabel(track);
        dubx.value = [selected] { return selected ? std::string("Active") : std::string(""); };
        dubx.onSelect = [this, dialog, track] {
            mDubSession->selectAudioTrack(track);
            dialog->close();
        };
        options.emplace_back(dubx);
    }
    if (options.empty()) {
        MediaDialog::Option none;
        none.label = "No tracks available";
        none.dimmed = true;
        options.emplace_back(none);
    }
    dialog->setOptions(options, cursor);
    refreshHelpPrompts();
}

void GuiVideoPlayer::openSubtitleBrowser()
{
    MediaDialog* dialog {pushDialog("Browse for subtitle", 0.56f)};
    auto directory = std::make_shared<std::string>(
        mStream.active ? SubtitleDownloader::getDownloadDirectory() : parentDirectory(mPath));
    dialog->setBackHandler([this, dialog, directory] {
        if (*directory == "/")
            return false;
        *directory = parentDirectory(*directory);
        rebuildSubtitleBrowser(dialog, directory, 0);
        return true;
    });
    rebuildSubtitleBrowser(dialog, directory, 0);
    refreshHelpPrompts();
}

void GuiVideoPlayer::rebuildSubtitleBrowser(MediaDialog* dialog,
                                            const std::shared_ptr<std::string>& directory,
                                            int cursor)
{
    std::vector<MediaDialog::Option> options;
    const std::string current {*directory};

    if (current != "/") {
        MediaDialog::Option parent;
        parent.label = "..";
        parent.onSelect = [this, dialog, directory] {
            *directory = parentDirectory(*directory);
            rebuildSubtitleBrowser(dialog, directory, 0);
        };
        options.emplace_back(parent);
    }

    bool hasFiles {false};
    for (const auto& entry : MediaLibrary::listSubtitleDirectory(current)) {
        MediaDialog::Option option;
        option.label = entry.name;
        const std::string path {entry.path};
        if (entry.type == MediaLibrary::EntryType::FOLDER) {
            option.value = [] { return std::string("Folder"); };
            option.onSelect = [this, dialog, directory, path] {
                *directory = path;
                rebuildSubtitleBrowser(dialog, directory, 0);
            };
        }
        else {
            hasFiles = true;
            option.highlight = true;
            option.onSelect = [this, dialog, path] {
                mPlayer->addSubtitleFile(path);
                mPlayer->setSubtitleVisible(true);
                dialog->close();
                mWindow->queueInfoPopup(
                    "LOADED '" + Utils::String::toUpper(Utils::FileSystem::getFileName(path)) +
                        "'",
                    2500);
            };
        }
        options.emplace_back(option);
    }

    if (options.empty()) {
        MediaDialog::Option none;
        none.label = "No subtitle files in this folder";
        none.dimmed = true;
        options.emplace_back(none);
    }

    dialog->setSubtitle(current + (hasFiles ? "" : "  (no subtitle files)"));
    // Start on the first subtitle file if there is one.
    int firstFile {cursor};
    if (cursor == 0) {
        for (size_t i {0}; i < options.size(); ++i) {
            if (options[i].highlight) {
                firstFile = static_cast<int>(i);
                break;
            }
        }
    }
    dialog->setOptions(options, firstFile);
}

void GuiVideoPlayer::openSlider(const Slider& slider)
{
    applyPendingSeek();
    mRepeatAction = RepeatAction::NONE;
    mSlider = std::make_unique<Slider>(slider);
    refreshHelpPrompts();
}

void GuiVideoPlayer::closeSlider()
{
    mSlider.reset();
    mRepeatAction = RepeatAction::NONE;
    refreshHelpPrompts();
}

void GuiVideoPlayer::adjustSlider(int direction, int repeatCount)
{
    if (!mSlider)
        return;
    // Holding the button makes the steps larger.
    double step {mSlider->step};
    if (repeatCount > 30)
        step *= 5.0;
    else if (repeatCount > 10)
        step *= 2.0;
    const double value {mSlider->getValue()};
    const double newValue {std::clamp(std::round((value + direction * step) / mSlider->step) *
                                          mSlider->step,
                                      mSlider->min, mSlider->max)};
    if (newValue != value)
        mSlider->setValue(newValue);
}

void GuiVideoPlayer::openVolumeSlider()
{
    MpvPlayer* player {mPlayer.get()};
    Slider slider;
    slider.label = "Volume";
    slider.min = 0.0;
    slider.max = MpvPlayer::MAX_VOLUME;
    slider.step = 1.0;
    slider.getValue = [player] { return std::round(player->getVolume()); };
    slider.setValue = [player](double value) {
        if (player->isMuted())
            player->setMuted(false);
        player->setVolume(value);
    };
    slider.format = [player](double value) {
        if (player->isMuted())
            return std::string("Muted");
        return std::to_string(static_cast<int>(std::round(value))) + "%";
    };
    openSlider(slider);
}

double GuiVideoPlayer::getDisplayAspect()
{
    // The aspect ratio of the video as decoded, before any override. The regular video
    // parameters already include the override, so these can't be used as the pixel ratio
    // would otherwise be applied on top of itself each time it's changed.
    if (mSourceAspect > 0.0)
        return mSourceAspect;
    const double width {mPlayer->getPropertyDouble("video-dec-params/w")};
    const double height {mPlayer->getPropertyDouble("video-dec-params/h")};
    double pixelAspect {mPlayer->getPropertyDouble("video-dec-params/par", 1.0)};
    if (pixelAspect <= 0.0)
        pixelAspect = 1.0;
    if (width <= 0.0 || height <= 0.0)
        return 0.0;
    mSourceAspect = width / height * pixelAspect;
    return mSourceAspect;
}

void GuiVideoPlayer::applyPixelRatio()
{
    if (std::abs(mPixelRatio - 1.0) < 0.001) {
        mPixelRatioPending = false;
        mPlayer->setProperty("video-aspect-override", "no");
        return;
    }
    const double aspect {getDisplayAspect()};
    // Not known before the first frame has been decoded, so it's applied later in that case.
    if (aspect <= 0.0) {
        mPixelRatioPending = true;
        return;
    }
    mPixelRatioPending = false;
    mPlayer->setProperty("video-aspect-override", std::to_string(aspect * mPixelRatio));
}

void GuiVideoPlayer::openPixelRatioSlider()
{
    Slider slider;
    slider.label = "Pixel ratio";
    slider.min = 0.5;
    slider.max = 2.0;
    slider.step = 0.01;
    slider.getValue = [this] { return mPixelRatio; };
    slider.setValue = [this](double value) {
        mPixelRatio = value;
        mPixelRatioChanged = true;
        applyPixelRatio();
        settingsChanged();
    };
    slider.format = [](double value) {
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%.2f", value);
        return std::string(buffer);
    };
    openSlider(slider);
}

void GuiVideoPlayer::renderSlider(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float width {std::round(screenWidth * 0.42f)};
    const float height {std::round(screenHeight * 0.115f)};
    const float x {std::round((screenWidth - width) / 2.0f)};
    const float y {std::round(screenHeight * 0.01f)};
    const float padding {std::round(screenWidth * 0.021f)};

    MediaStyle::drawRect(trans, x, y, width, height, 0x0A0A0AC8);

    const double value {mSlider->getValue()};
    MediaStyle::setText(*mSliderLabel, mSlider->label);
    MediaStyle::setText(*mSliderValue, mSlider->format(value));
    mSliderLabel->setPosition(x + padding, y + height * 0.06f);
    mSliderLabel->render(trans);
    mSliderValue->setPosition(x + width - padding - mSliderValue->getSize().x, y + height * 0.06f);
    mSliderValue->render(trans);

    // Track with arrows on both sides.
    const float trackHeight {std::round(screenHeight * 0.022f)};
    const float trackY {std::round(y + height * 0.66f - trackHeight / 2.0f)};
    const float trackX {x + padding};
    const float trackWidth {width - padding * 2.0f};
    const float outline {std::max(2.0f, std::round(screenHeight * 0.002f))};
    MediaStyle::drawRect(trans, trackX, trackY, trackWidth, trackHeight, 0xFFFFFFFF,
                         trackHeight / 2.0f);
    MediaStyle::drawRect(trans, trackX + outline, trackY + outline, trackWidth - outline * 2.0f,
                         trackHeight - outline * 2.0f, 0x0A0A0AFF,
                         (trackHeight - outline * 2.0f) / 2.0f);

    const float fraction {static_cast<float>(
        std::clamp((value - mSlider->min) / (mSlider->max - mSlider->min), 0.0, 1.0))};
    const float knobSize {std::round(trackHeight * 0.8f)};
    const float knobX {trackX + trackHeight / 2.0f +
                       (trackWidth - trackHeight) * fraction - knobSize / 2.0f};
    MediaStyle::drawRect(trans, knobX, trackY + (trackHeight - knobSize) / 2.0f, knobSize,
                         knobSize, MediaStyle::ACCENT, knobSize / 2.0f);

    const float arrowWidth {std::round(padding * 0.35f)};
    const float arrowHeight {std::round(trackHeight * 1.1f)};
    const float arrowY {trackY + (trackHeight - arrowHeight) / 2.0f};
    const float arrowStroke {std::max(2.0f, std::round(screenHeight * 0.003f))};
    // Simple chevrons made of two short bars each.
    for (int side {0}; side < 2; ++side) {
        const float centerX {side == 0 ? x + padding * 0.5f : x + width - padding * 0.5f};
        const float direction {side == 0 ? -1.0f : 1.0f};
        for (int part {0}; part < 2; ++part) {
            const float rotation {(part == 0 ? -1.0f : 1.0f) * direction * 0.79f};
            glm::mat4 armTrans {glm::translate(
                trans, glm::vec3 {centerX, arrowY + arrowHeight / 2.0f, 0.0f})};
            armTrans = glm::rotate(armTrans, rotation, glm::vec3 {0.0f, 0.0f, 1.0f});
            MediaStyle::drawRect(armTrans, -arrowStroke / 2.0f, part == 0 ? -arrowWidth : 0.0f,
                                 arrowStroke, arrowWidth, 0xFFFFFFFF);
        }
    }
}

//--------------------------------------------------------------------------------------------------
// Subtitle choice of a series and subtitle downloading.
//--------------------------------------------------------------------------------------------------

std::string GuiVideoPlayer::currentSubtitleChoice() const
{
    // Stored as "language|number|title", where the number counts the tracks with the same
    // language, e.g. the second English track. This works for the episodes of a series as
    // these normally have the same tracks in the same order.
    const MpvPlayer::Track* track {selectedTrack("sub")};
    if (track == nullptr || !mPlayer->isSubtitleVisible())
        return "off";
    const std::string language {
        track->lang.empty() ? "und" : SubtitleDownloader::normalizeLanguage(track->lang)};
    int number {0};
    for (const auto& other : mPlayer->getTracks()) {
        if (other.type != "sub" || other.external)
            continue;
        const std::string otherLanguage {
            other.lang.empty() ? "und" : SubtitleDownloader::normalizeLanguage(other.lang)};
        if (otherLanguage == language)
            ++number;
        if (other.id == track->id)
            break;
    }
    // Downloaded and other external files only remember the language.
    if (track->external)
        number = 0;
    return language + "|" + std::to_string(number) + "|" + track->title;
}

bool GuiVideoPlayer::applySubtitleChoice(const std::string& choice)
{
    if (choice.empty())
        return false;
    if (choice == "off") {
        mPlayer->setSubtitleVisible(false);
        return true;
    }
    const std::vector<std::string> parts {Utils::String::delimitedStringToVector(choice, "|")};
    if (parts.empty())
        return false;
    const std::string language {parts[0]};
    const int number {parts.size() > 1 ? std::atoi(parts[1].c_str()) : 0};
    const std::string title {parts.size() > 2 ? parts[2] : ""};

    const MpvPlayer::Track* byTitle {nullptr};
    const MpvPlayer::Track* byNumber {nullptr};
    const MpvPlayer::Track* first {nullptr};
    int count {0};
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type != "sub")
            continue;
        const std::string trackLanguage {
            track.lang.empty() ? "und" : SubtitleDownloader::normalizeLanguage(track.lang)};
        if (trackLanguage != language)
            continue;
        if (first == nullptr)
            first = &track;
        if (!track.external)
            ++count;
        if (!track.external && count == number && byNumber == nullptr)
            byNumber = &track;
        if (!title.empty() && track.title == title && byTitle == nullptr)
            byTitle = &track;
    }
    const MpvPlayer::Track* chosen {byTitle != nullptr ? byTitle :
                                    (byNumber != nullptr ? byNumber : first)};
    if (chosen == nullptr)
        return false;
    mPlayer->selectTrack("sub", chosen->id);
    mPlayer->setSubtitleVisible(true);
    return true;
}

std::vector<std::string> GuiVideoPlayer::downloadLanguages() const
{
    Settings* settings {Settings::getInstance()};
    std::vector<std::string> languages;
    const std::string defaultLanguage {settings->getString("MediaSubtitleDownloadLanguage")};
    if (!defaultLanguage.empty())
        languages.emplace_back(defaultLanguage);
    for (const auto& language : Utils::String::delimitedStringToVector(
             settings->getString("MediaSubtitleExtraLanguages"), ",")) {
        if (!language.empty() &&
            std::find(languages.cbegin(), languages.cend(), language) == languages.cend())
            languages.emplace_back(language);
    }
    return languages;
}

void GuiVideoPlayer::handleTracksOnStart()
{
    if (mDubSession)
        mDubSession->onTracksKnown();

    const MediaLibrary::PlayState state {
        mStream.active ? MediaLibrary::PlayState() :
                         MediaLibrary::getInstance().getPlayState(mPath)};
    const bool fileChoice {state.settings.count("sid") != 0};
    std::string seriesChoice;

    // The subtitle chosen for the other episodes, unless this video has its own setting.
    if (isSeries() && !fileChoice) {
        const MediaLibrary::PlayState series {
            MediaLibrary::getInstance().getPlayState(seriesKey())};
        const auto it {series.settings.find("sub-choice")};
        if (it != series.settings.cend()) {
            seriesChoice = it->second;
            if (applySubtitleChoice(seriesChoice)) {
                LOG(LogInfo) << "GuiVideoPlayer: Selected the subtitle \"" << seriesChoice
                             << "\" used for the series";
            }
        }
    }

    Settings* settings {Settings::getInstance()};
    // A subtitle that exists for the video is shown, unless subtitles have been turned off or
    // a choice has been made for the video or the series.
    if (settings->getBool("MediaSubtitleAlwaysLoad") && !fileChoice && seriesChoice.empty() &&
        settings->getString("MediaSubtitleLanguage") != "off")
        selectAvailableSubtitle();

    // Episodes get subtitles in the default language automatically if there are none.
    const std::string language {settings->getString("MediaSubtitleDownloadLanguage")};
    if (!settings->getBool("MediaSubtitleAutoDownload") || language.empty() ||
        !SubtitleDownloader::isConfigured() || seriesChoice == "off")
        return;
    const bool episode {mStream.active ? mStream.season > 0 && mStream.episode > 0 :
                                         isSeries() || SubtitleDownloader::isEpisodeFile(mPath)};
    if (!episode)
        return;
    for (const auto& track : mPlayer->getTracks()) {
        if (track.type == "sub" && SubtitleDownloader::normalizeLanguage(track.lang) == language)
            return;
    }
    // A subtitle downloaded earlier is named after the video and the language.
    const std::string videoPath {subtitleVideoPath()};
    const std::string stem {Utils::FileSystem::getStem(videoPath)};
    for (const auto& directory :
         {Utils::FileSystem::getParent(videoPath), SubtitleDownloader::getDownloadDirectory()}) {
        for (const auto& file : MediaLibrary::listSubtitleDirectory(directory)) {
            if (Utils::String::startsWith(file.name, stem + "." + language + "."))
                return;
        }
    }
    startSubtitleSearch(true);
}

void GuiVideoPlayer::selectAvailableSubtitle()
{
    const std::vector<MpvPlayer::Track> tracks {mPlayer->getTracks()};
    for (const auto& track : tracks) {
        if (track.type == "sub" && track.selected)
            return;
    }
    // The preferred languages: the subtitle language and the download languages.
    Settings* settings {Settings::getInstance()};
    std::vector<std::string> languages;
    for (const std::string& code :
         {settings->getString("MediaSubtitleLanguage"),
          settings->getString("MediaSubtitleDownloadLanguage")}) {
        if (!code.empty())
            languages.emplace_back(SubtitleDownloader::normalizeLanguage(code));
    }
    for (const auto& code : Utils::String::delimitedStringToVector(
             settings->getString("MediaSubtitleExtraLanguages"), ",")) {
        if (!code.empty())
            languages.emplace_back(SubtitleDownloader::normalizeLanguage(code));
    }
    auto preferred = [&languages](const MpvPlayer::Track& track) {
        const std::string language {SubtitleDownloader::normalizeLanguage(track.lang)};
        return !language.empty() &&
               std::find(languages.cbegin(), languages.cend(), language) != languages.cend();
    };
    // Subtitle files next to the video first, then the subtitles inside it in a preferred
    // language. Other subtitles inside the video aren't shown, they are often in a language
    // that isn't wanted.
    const MpvPlayer::Track* chosen {nullptr};
    for (int pass {0}; pass < 3 && chosen == nullptr; ++pass) {
        for (const auto& track : tracks) {
            if (track.type != "sub")
                continue;
            if ((pass == 0 && track.external && preferred(track)) ||
                (pass == 1 && track.external) || (pass == 2 && preferred(track))) {
                chosen = &track;
                break;
            }
        }
    }
    if (chosen == nullptr)
        return;
    mPlayer->selectTrack("sub", chosen->id);
    LOG(LogInfo) << "GuiVideoPlayer: Showing the available subtitle \""
                 << (chosen->title.empty() ? chosen->lang : chosen->title) << "\"";
}

void GuiVideoPlayer::startSubtitleSearch(bool automatic)
{
    SubtitleDownloader::Query query {SubtitleDownloader::parseVideoPath(subtitleVideoPath())};
    if (mStream.active) {
        const SubtitleDownloader::Query stream {streamSubtitleQuery()};
        query.title = stream.title;
        query.year = stream.year;
        query.season = stream.season;
        query.episode = stream.episode;
    }
    if (!mSearchTitle.empty()) {
        query.title = mSearchTitle;
        query.year = 0;
    }
    if (automatic) {
        query.languages = {Settings::getInstance()->getString("MediaSubtitleDownloadLanguage")};
    }
    else {
        if (mSearchLanguages.empty())
            mSearchLanguages = downloadLanguages();
        query.languages = mSearchLanguages;
    }
    mAutomaticSearch = automatic;
    mSearchResults.clear();
    abandonRequest(std::move(mSearchFuture));
    mSearchFuture = SubtitleDownloader::search(query);
    LOG(LogInfo) << "GuiVideoPlayer: Searching subtitles for \"" << query.title << "\""
                 << (automatic ? " automatically" : "");
}

void GuiVideoPlayer::openSubtitleDownload()
{
    if (!SubtitleDownloader::isConfigured()) {
        MediaDialog* dialog {pushDialog("Download subtitle", 0.56f)};
        dialog->setSubtitle("No subtitle service has been set up");
        std::vector<MediaDialog::Option> options;
        MediaDialog::Option info;
        info.label = "Add an API key in the media settings";
        info.dimmed = true;
        options.emplace_back(info);
        MediaDialog::Option info2;
        info2.label = "(Select button in the media screen)";
        info2.dimmed = true;
        options.emplace_back(info2);
        addCloseOption(options, dialog);
        dialog->setOptions(options, 2);
        refreshHelpPrompts();
        return;
    }
    // The default language is chosen first, the first time.
    if (Settings::getInstance()->getString("MediaSubtitleDownloadLanguage").empty()) {
        openLanguagePicker(true, [this] { openSubtitleDownload(); });
        return;
    }

    mDownloadDialog = pushDialog("Download subtitle", 0.66f);
    mDownloadDialog->setCancelCallback([this] {
        mDownloadDialog = nullptr;
        refreshHelpPrompts();
    });
    mSearchLanguages = downloadLanguages();
    if (!mSearchFuture.valid() || mAutomaticSearch)
        startSubtitleSearch(false);
    rebuildDownloadDialog("Searching...");
    refreshHelpPrompts();
}

void GuiVideoPlayer::rebuildDownloadDialog(const std::string& status)
{
    if (mDownloadDialog == nullptr || !mDialogs.contains(mDownloadDialog))
        return;
    MediaDialog* dialog {mDownloadDialog};
    std::vector<MediaDialog::Option> options;

    std::string languageNames;
    for (const auto& language : mSearchLanguages) {
        languageNames.append((languageNames.empty() ? "" : ", ") +
                             SubtitleDownloader::getLanguageName(language));
    }
    MediaDialog::Option languages;
    languages.label = "Languages";
    languages.value = [languageNames] { return languageNames; };
    languages.onSelect = [this] {
        openLanguagePicker(false, [this] {
            mSearchLanguages = downloadLanguages();
            startSubtitleSearch(false);
            rebuildDownloadDialog("Searching...");
        });
    };
    options.emplace_back(languages);

    const SubtitleDownloader::Query parsed {SubtitleDownloader::parseVideoPath(mPath)};
    const std::string title {mSearchTitle.empty() ? parsed.title : mSearchTitle};
    MediaDialog::Option manual;
    manual.label = "Manual search...";
    manual.value = [title] { return title; };
    manual.onSelect = [this, title] {
        auto callback = [this](const std::string& text) {
            const std::string trimmed {Utils::String::trim(text)};
            if (trimmed.empty())
                return;
            mSearchTitle = trimmed;
            startSubtitleSearch(false);
            rebuildDownloadDialog("Searching...");
        };
        if (Settings::getInstance()->getBool("VirtualKeyboard")) {
            mWindow->pushGui(new GuiTextEditKeyboardPopup(Renderer::getScreenHeight() * 0.2f,
                                                          "SEARCH SUBTITLES", title, callback,
                                                          false, "SEARCH", "SEARCH?"));
        }
        else {
            mWindow->pushGui(
                new GuiTextEditPopup("SEARCH SUBTITLES", title, callback, false, "SEARCH",
                                     "SEARCH?"));
        }
    };
    options.emplace_back(manual);

    int firstResult {static_cast<int>(options.size())};
    if (status.empty()) {
        for (const auto& result : mSearchResults) {
            MediaDialog::Option option;
            option.label = "[" + Utils::String::toUpper(result.language) + "]  " +
                           (result.release.empty() ? std::string("Unnamed") : result.release);
            std::string value;
            if (result.hashMatch)
                value = "Exact match  ";
            if (result.hearingImpaired)
                value.append("HI  ");
            if (!result.episodeMatch)
                value.append("Other episode  ");
            if (result.downloads > 0)
                value.append(std::to_string(result.downloads) + "  ");
            value.append(SubtitleDownloader::getProviderName(result.provider));
            option.value = [value] { return value; };
            option.highlight = result.hashMatch;
            option.dimmed = !result.episodeMatch;
            const SubtitleDownloader::Result chosen {result};
            option.onSelect = [this, chosen] { downloadSubtitle(chosen); };
            options.emplace_back(option);
        }
        if (mSearchResults.empty()) {
            MediaDialog::Option none;
            none.label = "No subtitles found";
            none.dimmed = true;
            options.emplace_back(none);
            firstResult = 0;
        }
        dialog->setSubtitle(std::to_string(mSearchResults.size()) + " subtitles found for \"" +
                            title + "\"");
    }
    else {
        MediaDialog::Option busy;
        busy.label = status;
        busy.dimmed = true;
        options.emplace_back(busy);
        dialog->setSubtitle(title);
        firstResult = std::min(dialog->getCursor(), firstResult);
    }
    dialog->setOptions(options, firstResult);
    refreshHelpPrompts();
}

void GuiVideoPlayer::downloadSubtitle(const SubtitleDownloader::Result& result)
{
    if (mDownloadFuture.valid())
        return;
    mDownloadingRelease = result.release;
    mDownloadFuture = SubtitleDownloader::download(result, subtitleVideoPath());
    mDownloadPath = mPath;
    if (!mAutomaticSearch)
        rebuildDownloadDialog("Downloading...");
}

void GuiVideoPlayer::pollSubtitleDownloads()
{
    if (mSearchFuture.valid() &&
        mSearchFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const SubtitleDownloader::SearchOutcome outcome {mSearchFuture.get()};
        mSearchResults = outcome.results;

        if (mAutomaticSearch) {
            const std::string language {
                Settings::getInstance()->getString("MediaSubtitleDownloadLanguage")};
            const SubtitleDownloader::Result* best {nullptr};
            for (const auto& result : mSearchResults) {
                if (result.episodeMatch && result.language == language) {
                    best = &result;
                    break;
                }
            }
            if (best != nullptr) {
                downloadSubtitle(*best);
            }
            else {
                mAutomaticSearch = false;
                mWindow->queueInfoPopup(
                    "NO " +
                        Utils::String::toUpper(SubtitleDownloader::getLanguageName(language)) +
                        " SUBTITLES FOUND" +
                        (outcome.errors.empty() ? "" :
                                                  " (" + Utils::String::toUpper(
                                                             outcome.errors.front()) + ")"),
                    4000);
            }
        }
        else {
            if (!outcome.errors.empty() && mSearchResults.empty())
                mWindow->queueInfoPopup(Utils::String::toUpper(outcome.errors.front()), 5000);
            rebuildDownloadDialog("");
        }
    }

    if (mDownloadFuture.valid() &&
        mDownloadFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const SubtitleDownloader::DownloadOutcome outcome {mDownloadFuture.get()};
        const bool automatic {mAutomaticSearch};
        mAutomaticSearch = false;
        if (outcome.subtitlePath.empty()) {
            mWindow->queueInfoPopup("SUBTITLE DOWNLOAD FAILED: " +
                                        Utils::String::toUpper(outcome.error),
                                    5000);
            if (!automatic)
                rebuildDownloadDialog("");
            return;
        }
        // The video may have been changed in the meantime.
        if (mDownloadPath == mPath) {
            mPlayer->addSubtitleFile(outcome.subtitlePath);
            mPlayer->setSubtitleVisible(true);
        }
        mWindow->queueInfoPopup("DOWNLOADED SUBTITLE: " + mDownloadingRelease, 4000);
        if (!automatic && mDownloadDialog != nullptr && mDialogs.contains(mDownloadDialog)) {
            // Back to the video.
            mDialogs.clear();
            mDownloadDialog = nullptr;
            refreshHelpPrompts();
        }
    }
}

void GuiVideoPlayer::openLanguagePicker(bool defaultOnly,
                                        const std::function<void()>& doneCallback)
{
    MediaDialog* dialog {pushDialog(defaultOnly ? "Default subtitle language" :
                                                  "Subtitle languages",
                                    0.50f)};
    buildSubtitleLanguagePicker(dialog, defaultOnly, doneCallback);
    refreshHelpPrompts();
}

//--------------------------------------------------------------------------------------------------
// Rendering.
//--------------------------------------------------------------------------------------------------

void GuiVideoPlayer::updateCodecInfo()
{
    auto property = [this](const std::string& name) { return mPlayer->getPropertyString(name); };

    std::vector<std::string> lines;
    std::string videoCodec {property("video-codec-name")};
    if (videoCodec.empty())
        videoCodec = property("video-format");
    std::string video {"Video: "};
    const std::string width {property("video-params/w")};
    const std::string height {property("video-params/h")};
    if (!width.empty() && !height.empty())
        video.append(width + "x" + height + "  ");
    video.append(Utils::String::toUpper(videoCodec));
    const std::string pixelFormat {property("video-params/pixelformat")};
    if (!pixelFormat.empty())
        video.append("  " + pixelFormat);
    lines.emplace_back(video);

    const std::string fps {formatDecimal(property("container-fps"), 3)};
    const std::string displayFps {formatDecimal(property("estimated-vf-fps"), 3)};
    lines.emplace_back("Frame rate: " + (fps.empty() ? "-" : fps) + " fps (output " +
                       (displayFps.empty() ? "-" : displayFps) + ")");

    std::string hwdec {property("hwdec-current")};
    if (hwdec.empty() || hwdec == "no")
        hwdec = "software";
    lines.emplace_back("Decoder: " + hwdec);

    const std::string colour {property("video-params/colormatrix")};
    const std::string primaries {property("video-params/primaries")};
    const std::string gamma {property("video-params/gamma")};
    if (!colour.empty())
        lines.emplace_back("Colour: " + colour + ", " + primaries + ", " + gamma);

    std::string audio {"Audio: " + Utils::String::toUpper(property("audio-codec-name"))};
    const std::string channels {property("audio-params/channel-count")};
    if (!channels.empty())
        audio.append("  " + channelLayout(std::atoi(channels.c_str())));
    const std::string sampleRate {property("audio-params/samplerate")};
    if (!sampleRate.empty())
        audio.append("  " +
                     formatDecimal(std::to_string(std::atof(sampleRate.c_str()) / 1000.0), 1) +
                     " kHz");
    if (Utils::String::startsWith(property("audio-params/format"), "spdif"))
        audio.append("  (passthrough)");
    lines.emplace_back(audio);

    const std::string dropped {property("frame-drop-count")};
    const std::string decoderDropped {property("decoder-frame-drop-count")};
    lines.emplace_back("Dropped frames: " + (dropped.empty() ? "0" : dropped) + " output, " +
                       (decoderDropped.empty() ? "0" : decoderDropped) + " decoder");

    const std::string cache {formatDecimal(property("demuxer-cache-duration"), 1)};
    if (!cache.empty())
        lines.emplace_back("Cache: " + cache + " s");

    std::string text;
    for (const auto& line : lines) {
        if (!text.empty())
            text.append("\n");
        text.append(line);
    }
    MediaStyle::setText(*mCodecText, text);
}

void GuiVideoPlayer::render(const glm::mat4& parentTrans)
{
    const glm::mat4 trans {parentTrans * getTransform()};
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};

    MediaStyle::drawRect(trans, 0.0f, 0.0f, screenWidth, screenHeight, 0x000000FF);
    if (mClosing || mInitFailed)
        return;

    mPlayer->render(0.0f, 0.0f, screenWidth, screenHeight);

    if (!mPlayer->hasVideoFrame()) {
        MediaStyle::setText(*mLoadingText, "Loading...");
        mLoadingText->render(trans);
    }
    else if (mPlayer->isBuffering() && !mPlayer->isPaused() && !mSeekPending) {
        MediaStyle::setText(*mLoadingText, "Buffering...");
        mLoadingText->render(trans);
    }
    else if (mDubSession && mDubSession->isPreparing() && mDialogs.empty() &&
             !mSlider) {
        MediaStyle::setText(*mLoadingText,
                            "Preparing the dub... " + std::to_string(mDubSession->getProgress()) + "%");
        mLoadingText->render(trans);
    }

    if (mSlider) {
        // Only the slider so the whole picture can be seen while adjusting it.
    }
    else if (mControlsVisible) {
        renderTopBar(trans);
        renderBottomBar(trans, true);
    }
    else {
        if (mSeekPending || mSeekBarTimer > 0)
            renderBottomBar(trans, false);
        if (mPlayer->isPaused() && mPlayer->hasVideoFrame())
            renderPausedIndicator(trans);
    }

    if (mVolumeTimer > 0 && !mSlider)
        renderVolume(trans);
    if (mCodecInfoVisible)
        renderCodecInfo(trans);

    // The status of the streaming add-on while paused.
    if (mAddonDialogs && mPlayer->isPaused() && !mSlider && mDialogs.empty())
        mAddonDialogs->renderOverlay(trans);

    // The dialogs are hidden while a slider is shown so the picture can be seen.
    if (mSlider)
        renderSlider(trans);
    else
        mDialogs.render(trans);
    if (mAddonDialogs)
        mAddonDialogs->render(trans, false);
}

void GuiVideoPlayer::renderTopBar(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};

    MediaStyle::drawRect(trans, 0.0f, 0.0f, screenWidth, screenHeight * 0.105f, 0x000000C0);
    mTitleText->render(trans);

    MediaStyle::setText(*mClockText, MediaStyle::clockText());
    mClockText->render(trans);

    const double duration {mPlayer->getDuration()};
    if (duration > 0.0) {
        const double position {mSeekPending ? mSeekTarget : mPlayer->getPosition()};
        const double speed {mPlayer->getSpeed() > 0.0 ? mPlayer->getSpeed() : 1.0};
        MediaStyle::setText(*mEndsText, "Ends at: " + MediaStyle::clockTextAfter(
                                                          (duration - position) / speed));
        mEndsText->render(trans);
    }
}

void GuiVideoPlayer::renderBottomBar(const glm::mat4& trans, bool withButtons)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float margin {std::round(screenWidth * 0.03f)};
    const float barWidth {screenWidth - margin * 2.0f};

    const float panelY {std::round(screenHeight * (withButtons ? 0.795f : 0.80f))};
    MediaStyle::drawRect(trans, 0.0f, panelY, screenWidth, screenHeight - panelY, 0x000000C8);

    const double duration {mPlayer->getDuration()};
    const double position {mSeekPending ? mSeekTarget : mPlayer->getPosition()};
    const bool showHours {duration >= 3600.0};

    // Progress bar with chapter marks.
    const float barY {std::round(screenHeight * 0.815f)};
    const bool seekBarFocused {withButtons && mOsdFocus == OsdFocus::SEEK_BAR};
    const float barHeight {
        std::max(4.0f, std::round(screenHeight * (seekBarFocused ? 0.010f : 0.007f)))};
    const float fraction {
        duration > 0.0 ? static_cast<float>(std::clamp(position / duration, 0.0, 1.0)) : 0.0f};
    if (seekBarFocused) {
        const float outline {std::max(2.0f, std::round(screenHeight * 0.003f))};
        MediaStyle::drawRect(trans, margin - outline, barY - outline, barWidth + outline * 2.0f,
                             barHeight + outline * 2.0f, MediaStyle::ACCENT,
                             (barHeight + outline * 2.0f) / 2.0f);
        MediaStyle::drawRect(trans, margin, barY, barWidth, barHeight, 0x202020FF,
                             barHeight / 2.0f);
    }
    MediaStyle::drawRect(trans, margin, barY, barWidth, barHeight, 0xFFFFFF30, barHeight / 2.0f);
    if (duration > 0.0) {
        const double cached {std::clamp(mPlayer->getCachedUntil() / duration, 0.0, 1.0)};
        if (cached > fraction) {
            MediaStyle::drawRect(trans, margin + barWidth * fraction, barY,
                                 barWidth * static_cast<float>(cached - fraction), barHeight,
                                 0xFFFFFF40);
        }
    }
    MediaStyle::drawRect(trans, margin, barY, barWidth * fraction, barHeight, MediaStyle::ACCENT,
                         barHeight / 2.0f);
    if (duration > 0.0) {
        for (const auto& chapter : mPlayer->getChapters()) {
            if (chapter.time <= 0.0 || chapter.time >= duration)
                continue;
            const float x {margin + barWidth * static_cast<float>(chapter.time / duration)};
            MediaStyle::drawRect(trans, std::round(x - 1.0f), barY - barHeight * 0.6f, 2.0f,
                                 barHeight * 2.2f, 0xFFFFFFA0);
        }
    }
    const float knobSize {barHeight * (seekBarFocused ? 2.4f : 2.6f)};
    const float knobX {margin + barWidth * fraction};
    MediaStyle::drawRect(trans, knobX - knobSize / 2.0f, barY + barHeight / 2.0f - knobSize / 2.0f,
                         knobSize, knobSize, seekBarFocused ? MediaStyle::ACCENT : 0xFFFFFFFF,
                         knobSize / 2.0f);

    if (mSeekPending) {
        const double difference {mSeekTarget - mPlayer->getPosition()};
        std::string seekText {MediaLibrary::formatTime(mSeekTarget, showHours)};
        seekText.append(difference >= 0.0 ? "  (+" : "  (-");
        seekText.append(MediaLibrary::formatTime(std::abs(difference)) + ")");
        MediaStyle::setText(*mSeekText, seekText);
        const float textWidth {mSeekText->getSize().x};
        mSeekText->setPosition(
            std::clamp(knobX - textWidth / 2.0f, margin, screenWidth - margin - textWidth),
            barY - screenHeight * 0.055f);
        mSeekText->render(trans);
    }

    // Elapsed and total time in the middle.
    std::string timeText {MediaLibrary::formatTime(position, true)};
    if (duration > 0.0)
        timeText.append(" / " + MediaLibrary::formatTime(duration, true));
    else if (!showHours)
        timeText = MediaLibrary::formatTime(position, true);
    MediaStyle::setText(*mTimeText, timeText);
    mTimeText->render(trans);

    std::string status;
    const auto& chapters {mPlayer->getChapters()};
    const int chapter {mPlayer->getChapter()};
    if (!chapters.empty() && chapter >= 0 && chapter < static_cast<int>(chapters.size())) {
        status = "Chapter " + std::to_string(chapter + 1) + "/" + std::to_string(chapters.size());
        if (!chapters[chapter].title.empty())
            status.append(": " + chapters[chapter].title);
    }
    if (std::abs(mPlayer->getSpeed() - 1.0) > 0.001)
        status.append((status.empty() ? "" : "   |   ") + std::string("Speed ") +
                      formatSpeed(mPlayer->getSpeed()));
    if (mPlaylist.size() > 1)
        status.append((status.empty() ? "" : "   |   ") + std::to_string(mIndex + 1) + "/" +
                      std::to_string(mPlaylist.size()));
    const std::string dub {mDubSession ? mDubSession->getStatus() : ""};
    if (!dub.empty())
        status.append((status.empty() ? "" : "   |   ") + mDubSession->getLabel() + ": " + dub);
    MediaStyle::setText(*mStatusText, status);
    if (!status.empty())
        mStatusText->render(trans);

    if (!withButtons)
        return;

    for (int i {0}; i < BUTTON_COUNT; ++i) {
        if (!buttonShown(i))
            continue;
        const glm::vec2 pos {mButtonPositions[i]};
        const bool focused {mOsdFocus == OsdFocus::BUTTONS && i == mFocusedButton};
        if (focused) {
            MediaStyle::drawRect(trans, pos.x, pos.y, mButtonSize, mButtonSize, MediaStyle::ACCENT,
                                 mButtonSize / 2.0f);
        }
        ImageComponent* icon {mButtonIcons[i].get()};
        if (i == BUTTON_PLAY_PAUSE && mPlayer->isPaused())
            icon = mPlayIcon.get();
        else if (i == BUTTON_VOLUME && mPlayer->isMuted())
            icon = mMuteIcon.get();
        // Previous and next are dimmed when there is nowhere to go.
        bool available {true};
        if (i == BUTTON_NEXT)
            available = mIndex + 1 < mPlaylist.size() ||
                        (!chapters.empty() && chapter + 1 < static_cast<int>(chapters.size()));
        unsigned int color {focused ? 0xFFFFFFFF : (available ? 0xE6E6E6FF : 0xFFFFFF60)};
        // The dub button is shown in the accent color while the dub is on, and blinks while
        // it's being prepared or waits for subtitles.
        if (i == BUTTON_DUBX && mDubSession && mDubSession->isWanted() && !focused) {
            const bool active {mDubSession->isActive()};
            color = active || (SDL_GetTicks() / 500) % 2 == 0 ? MediaStyle::ACCENT : 0xFFFFFF60;
        }
        icon->setColorShift(color);
        const glm::vec2 iconSize {icon->getSize()};
        icon->setPosition(std::round(pos.x + (mButtonSize - iconSize.x) / 2.0f),
                          std::round(pos.y + (mButtonSize - iconSize.y) / 2.0f));
        icon->render(trans);
    }
}

void GuiVideoPlayer::renderPausedIndicator(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float x {std::round(screenWidth * 0.03f)};
    const float y {std::round(screenHeight * 0.04f)};
    const float height {std::round(screenHeight * 0.06f)};
    const float iconSize {mPausedIcon->getSize().y};

    MediaStyle::drawRect(trans, x, y, screenWidth * 0.12f, height, 0x000000B0, height * 0.2f);
    mPausedIcon->setPosition(x + height * 0.3f, y + (height - iconSize) / 2.0f);
    mPausedIcon->render(trans);
    mPausedText->setPosition(x + height * 0.4f + iconSize + screenWidth * 0.008f,
                             y + (height - mPausedText->getSize().y) / 2.0f);
    mPausedText->render(trans);
}

void GuiVideoPlayer::renderVolume(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float width {std::round(screenWidth * 0.22f)};
    const float height {std::round(screenHeight * 0.09f)};
    const float x {screenWidth - std::round(screenWidth * 0.03f) - width};
    const float y {std::round(screenHeight * 0.13f)};
    const float padding {std::round(screenHeight * 0.018f)};

    MediaStyle::drawRect(trans, x, y, width, height, 0x000000C8, screenHeight * 0.01f);

    const bool muted {mPlayer->isMuted()};
    const double volume {mPlayer->getVolume()};
    MediaStyle::setText(*mVolumeText,
                        muted ? "Muted" :
                                "Volume " + std::to_string(static_cast<int>(std::round(volume))) +
                                    "%");
    mVolumeText->setPosition(x + padding, y + padding * 0.3f);
    mVolumeText->render(trans);

    const float barX {x + padding};
    const float barWidth {width - padding * 2.0f};
    const float barHeight {std::max(4.0f, std::round(screenHeight * 0.007f))};
    const float barY {y + height - padding - barHeight};
    const float fraction {
        muted ? 0.0f : static_cast<float>(std::clamp(volume / MpvPlayer::MAX_VOLUME, 0.0, 1.0))};
    MediaStyle::drawRect(trans, barX, barY, barWidth, barHeight, 0xFFFFFF30, barHeight / 2.0f);
    MediaStyle::drawRect(trans, barX, barY, barWidth * fraction, barHeight,
                         volume > 100.0 ? 0xE0A030FF : MediaStyle::ACCENT, barHeight / 2.0f);
    // Mark the 100% level as values above it amplify the audio.
    const float normalX {barX + barWidth * static_cast<float>(100.0 / MpvPlayer::MAX_VOLUME)};
    MediaStyle::drawRect(trans, std::round(normalX - 1.0f), barY - barHeight * 0.5f, 2.0f,
                         barHeight * 2.0f, 0xFFFFFFA0);
}

void GuiVideoPlayer::renderCodecInfo(const glm::mat4& trans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float padding {std::round(screenHeight * 0.022f)};
    const float x {std::round(screenWidth * 0.03f)};
    const float y {std::round(screenHeight * 0.13f)};
    const float width {mCodecText->getSize().x + padding * 2.0f};
    const float height {mCodecText->getSize().y + padding * 2.0f};

    MediaStyle::drawRect(trans, x, y, width, height, 0x000000C8, screenHeight * 0.01f);
    mCodecText->setPosition(x + padding, y + padding);
    mCodecText->render(trans);
}

std::vector<HelpPrompt> GuiVideoPlayer::getHelpPrompts()
{
    // Nothing is shown during playback, only for the menus.
    if (!mDialogs.empty() && !mSlider)
        return mDialogs.getHelpPrompts();
    return std::vector<HelpPrompt>();
}

void GuiVideoPlayer::refreshHelpPrompts()
{
    if (mWindow->peekGui() != this)
        return;
    const std::vector<HelpPrompt> prompts {getHelpPrompts()};
    if (prompts == mLastHelpPrompts && mHelpPromptsSet)
        return;
    mHelpPromptsSet = true;
    mLastHelpPrompts = prompts;
    mWindow->setHelpPrompts(prompts);
}
