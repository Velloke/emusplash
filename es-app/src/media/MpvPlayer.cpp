//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MpvPlayer.cpp
//
//  Video playback using libmpv. The video is rendered through the application's OpenGL
//  context into a texture which is then drawn by the regular renderer, which makes it
//  possible to render menus and on-screen controls on top of it.
//

#include "media/MpvPlayer.h"

#include "media/SubtitleDownloader.h"

#include "Log.h"
#include "Settings.h"
#include "renderers/RendererOpenGL.h"
#include "utils/StringUtil.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_video.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace
{
    // Returns the native display connection of the application window, which allows the
    // player to use hardware decoding without copying the frames back to system memory.
    // Defined at the end of this file as the windowing system headers define generic macros.
    void* getNativeDisplay(mpv_render_param_type& type);

    // Language codes as used in the settings, mapped to the two and three letter variants
    // found in video files.
    std::string languageCodes(const std::string& language)
    {
        static const std::vector<std::pair<std::string, std::string>> codes {
            {"en", "en,eng"},     {"et", "et,est"},     {"ru", "ru,rus"}, {"fi", "fi,fin"},
            {"de", "de,ger,deu"}, {"fr", "fr,fre,fra"}, {"es", "es,spa"}, {"it", "it,ita"},
            {"sv", "sv,swe"},     {"no", "no,nor,nb,nob"}, {"da", "da,dan"},
            {"pl", "pl,pol"},     {"lv", "lv,lav"},     {"lt", "lt,lit"}, {"ja", "ja,jpn"}};
        for (const auto& [code, variants] : codes) {
            if (code == language)
                return variants;
        }
        return language;
    }

    const mpv_node* findMapValue(const mpv_node* map, const char* key)
    {
        if (map == nullptr || map->format != MPV_FORMAT_NODE_MAP)
            return nullptr;
        for (int i {0}; i < map->u.list->num; ++i) {
            if (std::strcmp(map->u.list->keys[i], key) == 0)
                return &map->u.list->values[i];
        }
        return nullptr;
    }

    std::string nodeString(const mpv_node* map, const char* key)
    {
        const mpv_node* value {findMapValue(map, key)};
        if (value != nullptr && value->format == MPV_FORMAT_STRING && value->u.string != nullptr)
            return value->u.string;
        return "";
    }

    int64_t nodeInt(const mpv_node* map, const char* key)
    {
        const mpv_node* value {findMapValue(map, key)};
        if (value == nullptr)
            return 0;
        if (value->format == MPV_FORMAT_INT64)
            return value->u.int64;
        if (value->format == MPV_FORMAT_DOUBLE)
            return static_cast<int64_t>(value->u.double_);
        return 0;
    }

    double nodeDouble(const mpv_node* map, const char* key)
    {
        const mpv_node* value {findMapValue(map, key)};
        if (value == nullptr)
            return 0.0;
        if (value->format == MPV_FORMAT_DOUBLE)
            return value->u.double_;
        if (value->format == MPV_FORMAT_INT64)
            return static_cast<double>(value->u.int64);
        return 0.0;
    }

    bool nodeFlag(const mpv_node* map, const char* key)
    {
        const mpv_node* value {findMapValue(map, key)};
        return value != nullptr && value->format == MPV_FORMAT_FLAG && value->u.flag != 0;
    }
} // namespace

MpvPlayer::MpvPlayer()
    : mHandle {nullptr}
    , mRenderContext {nullptr}
    , mWakeup {false}
    , mRedraw {false}
    , mTexture {0}
    , mFramebuffer {0}
    , mTextureWidth {0}
    , mTextureHeight {0}
    , mTextureFailed {false}
    , mFileLoaded {false}
    , mFileLoadedEvent {false}
    , mFileActive {false}
    , mReplacedFiles {0}
    , mHasFrame {false}
    , mSubVisible {true}
    , mEndReason {EndReason::NONE}
    , mPosition {0.0}
    , mDuration {0.0}
    , mCachedUntil {0.0}
    , mPaused {false}
    , mBuffering {false}
    , mSeeking {false}
    , mVolume {100.0}
    , mMuted {false}
    , mSpeed {1.0}
    , mAudioDelay {0.0}
    , mSubDelay {0.0}
    , mSubScale {1.0}
    , mSubPos {100}
    , mPanScan {0.0}
    , mAspectOverride {"no"}
    , mChapter {-1}
{
}

MpvPlayer::~MpvPlayer()
{
    // The renderer has to be freed before the player itself, while the OpenGL context is
    // still current.
    if (mRenderContext != nullptr) {
        mpv_render_context_set_update_callback(mRenderContext, nullptr, nullptr);
        mpv_render_context_free(mRenderContext);
        mRenderContext = nullptr;
        Renderer::getInstance()->resetRenderState();
    }
    destroyTexture();
    if (mHandle != nullptr) {
        mpv_set_wakeup_callback(mHandle, nullptr, nullptr);
        if (mRemoteFile) {
            // Closing a file from the internet can wait for the network for a long time, and
            // the screen would stand still. The renderer is already freed, so the player can
            // be ended in the background.
            mpv_handle* handle {mHandle};
            std::thread([handle] {
                const unsigned int start {SDL_GetTicks()};
                mpv_terminate_destroy(handle);
                LOG(LogDebug) << "MpvPlayer: Closed in the background in "
                              << SDL_GetTicks() - start << " ms";
            }).detach();
        }
        else {
            mpv_terminate_destroy(mHandle);
        }
        mHandle = nullptr;
    }
}

bool MpvPlayer::initialize()
{
    mHandle = mpv_create();
    if (mHandle == nullptr) {
        LOG(LogError) << "MpvPlayer: Couldn't create the video player";
        return false;
    }

    Settings* settings {Settings::getInstance()};

    // Video output goes through the render API, and all the controls and on-screen
    // information are drawn by the application itself.
    setOption("vo", "libmpv");
    setOption("config", "no");
    setOption("terminal", "no");
    setOption("input-default-bindings", "no");
    setOption("input-vo-keyboard", "no");
    setOption("osc", "no");
    setOption("osd-level", "0");
    setOption("load-scripts", "no");
    setOption("ytdl", "no");
    setOption("idle", "yes");
    setOption("keep-open", "no");
    setOption("audio-client-name", "EmuSPLASH");
    setOption("volume-max", std::to_string(static_cast<int>(MAX_VOLUME)));
    setOption("volume", std::to_string(settings->getInt("MediaVolume")));
    setOption("sub-auto", "fuzzy");
    // Downloaded subtitles that couldn't be stored next to the video.
    setOption("sub-file-paths", SubtitleDownloader::getDownloadDirectory());
    setOption("sub-scale", std::to_string(settings->getInt("MediaSubtitleScale") / 100.0));
    setOption("hr-seek", "default");

    std::string hwdec {settings->getString("MediaHardwareDecoding")};
    if (hwdec != "auto-copy-safe" && hwdec != "auto-safe" && hwdec != "no")
        hwdec = "auto-copy-safe";
    setOption("hwdec", hwdec);

    if (settings->getBool("MediaAudioPassthrough"))
        setOption("audio-spdif", "ac3,eac3,dts,dts-hd,truehd");

    setOption("sub-pos", std::to_string(settings->getInt("MediaSubtitlePosition")));
    setOption("deinterlace", settings->getBool("MediaDeinterlace") ? "yes" : "no");
    const std::string filters {audioFilters(settings->getBool("MediaDynamicRange"),
                                            settings->getInt("MediaVolumeAmplification"))};
    if (!filters.empty())
        setOption("af", filters);

    const std::string audioLanguage {settings->getString("MediaAudioLanguage")};
    if (!audioLanguage.empty())
        setOption("alang", languageCodes(audioLanguage));

    const std::string subtitleLanguage {settings->getString("MediaSubtitleLanguage")};
    if (subtitleLanguage == "off")
        setOption("sid", "no");
    else if (!subtitleLanguage.empty())
        setOption("slang", languageCodes(subtitleLanguage));

    mpv_request_log_messages(mHandle, "warn");

    if (mpv_initialize(mHandle) < 0) {
        LOG(LogError) << "MpvPlayer: Couldn't initialize the video player";
        mpv_terminate_destroy(mHandle);
        mHandle = nullptr;
        return false;
    }

    mpv_opengl_init_params glInitParams {};
    glInitParams.get_proc_address = &MpvPlayer::getProcAddress;
    glInitParams.get_proc_address_ctx = nullptr;

    std::vector<mpv_render_param> params;
    params.push_back({MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)});
    params.push_back({MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInitParams});
    mpv_render_param_type displayType {MPV_RENDER_PARAM_INVALID};
    void* display {getNativeDisplay(displayType)};
    if (display != nullptr)
        params.push_back({displayType, display});
    params.push_back({MPV_RENDER_PARAM_INVALID, nullptr});

    const int result {mpv_render_context_create(&mRenderContext, mHandle, params.data())};
    // The player changes the OpenGL state when setting up its renderer.
    Renderer::getInstance()->resetRenderState();
    if (result < 0) {
        LOG(LogError) << "MpvPlayer: Couldn't create the video renderer: "
                      << mpv_error_string(result);
        mRenderContext = nullptr;
        mpv_terminate_destroy(mHandle);
        mHandle = nullptr;
        return false;
    }

    mpv_set_wakeup_callback(mHandle, &MpvPlayer::onWakeup, this);
    mpv_render_context_set_update_callback(mRenderContext, &MpvPlayer::onRenderUpdate, this);
    observeProperties();

    LOG(LogInfo) << "MpvPlayer: Initialized libmpv " << getPropertyString("mpv-version")
                 << " with hardware decoding \"" << hwdec << "\"";
    return true;
}

void MpvPlayer::loadFile(const std::string& path, double startPosition)
{
    if (mHandle == nullptr)
        return;

    // Replacing a file that is still playing or loading reports the end of that file.
    if (mFileActive)
        ++mReplacedFiles;
    mFileActive = true;
    mFileLoaded = false;
    mFileLoadedEvent = false;
    mHasFrame = false;
    mEndReason = EndReason::NONE;
    mErrorMessage.clear();
    mPosition = startPosition;
    mDuration = 0.0;

    // Set synchronously so the start position is in place before the file is loaded.
    const std::string start {startPosition > 0.0 ? std::to_string(startPosition) : "none"};
    mpv_set_property_string(mHandle, "start", start.c_str());

    // Local files and the streams of the add-on (on this computer) are closed as before.
    mRemoteFile = (Utils::String::startsWith(path, "http://") ||
                   Utils::String::startsWith(path, "https://")) &&
                  path.find("://127.0.0.1") == std::string::npos &&
                  path.find("://localhost") == std::string::npos;
    LOG(LogInfo) << "MpvPlayer: Playing \"" << path << "\""
                 << (startPosition > 0.0 ? " from " + std::to_string(startPosition) + " s" : "");
    const char* args[] {"loadfile", path.c_str(), "replace", nullptr};
    const int result {mpv_command(mHandle, args)};
    if (result < 0) {
        mFileActive = false;
        mEndReason = EndReason::FAILED;
        mErrorMessage = mpv_error_string(result);
        LOG(LogWarning) << "MpvPlayer: Couldn't load the file: " << mErrorMessage;
    }
}

void MpvPlayer::stop()
{
    if (mHandle != nullptr)
        command({"stop"});
}

void MpvPlayer::update()
{
    if (mHandle == nullptr)
        return;

    mWakeup = false;
    while (true) {
        mpv_event* event {mpv_wait_event(mHandle, 0.0)};
        if (event == nullptr || event->event_id == MPV_EVENT_NONE)
            break;

        switch (event->event_id) {
            case MPV_EVENT_PROPERTY_CHANGE: {
                const mpv_event_property* property {
                    static_cast<mpv_event_property*>(event->data)};
                handlePropertyChange(property->name, property->format, property->data);
                break;
            }
            case MPV_EVENT_FILE_LOADED: {
                mFileLoaded = true;
                mFileLoadedEvent = true;
                break;
            }
            case MPV_EVENT_END_FILE: {
                const mpv_event_end_file* endFile {static_cast<mpv_event_end_file*>(event->data)};
                if (mReplacedFiles > 0) {
                    // The end of a file that has already been replaced by a new one.
                    --mReplacedFiles;
                    break;
                }
                mFileActive = false;
                if (endFile->reason == MPV_END_FILE_REASON_EOF) {
                    mEndReason = EndReason::END_OF_FILE;
                }
                else if (endFile->reason == MPV_END_FILE_REASON_ERROR) {
                    mEndReason = EndReason::FAILED;
                    mErrorMessage = mpv_error_string(endFile->error);
                    LOG(LogWarning) << "MpvPlayer: Playback failed: " << mErrorMessage;
                }
                else if (endFile->reason != MPV_END_FILE_REASON_REDIRECT) {
                    mEndReason = EndReason::STOPPED;
                }
                mFileLoaded = false;
                break;
            }
            case MPV_EVENT_LOG_MESSAGE: {
                const mpv_event_log_message* message {
                    static_cast<mpv_event_log_message*>(event->data)};
                std::string text {Utils::String::trim(message->text)};
                if (text.empty())
                    break;
                text = "MpvPlayer: [" + std::string(message->prefix) + "] " + text;
                if (message->log_level <= MPV_LOG_LEVEL_ERROR) {
                    LOG(LogError) << text;
                }
                else {
                    LOG(LogWarning) << text;
                }
                break;
            }
            case MPV_EVENT_SHUTDOWN: {
                mFileLoaded = false;
                if (mEndReason == EndReason::NONE)
                    mEndReason = EndReason::STOPPED;
                break;
            }
            default: {
                break;
            }
        }
    }
}

void MpvPlayer::render(float x, float y, float width, float height, float opacity)
{
    if (mRenderContext == nullptr)
        return;

    Renderer* renderer {Renderer::getInstance()};

    // The video is always rendered at the screen resolution, the player itself takes care
    // of scaling and letterboxing the video to fit.
    const int textureWidth {static_cast<int>(Renderer::getScreenWidth())};
    const int textureHeight {static_cast<int>(Renderer::getScreenHeight())};
    bool forceRender {false};
    if (mTextureFailed)
        return;
    if (mTexture == 0 || textureWidth != mTextureWidth || textureHeight != mTextureHeight) {
        updateTexture(textureWidth, textureHeight);
        forceRender = true;
    }
    if (mTexture == 0)
        return;

    bool renderFrame {forceRender};
    if (mRedraw.exchange(false)) {
        if (mpv_render_context_update(mRenderContext) & MPV_RENDER_UPDATE_FRAME)
            renderFrame = true;
    }

    if (renderFrame) {
        mpv_opengl_fbo fbo {};
        fbo.fbo = static_cast<int>(mFramebuffer);
        fbo.w = mTextureWidth;
        fbo.h = mTextureHeight;
        fbo.internal_format = 0;
        // Not flipped, the first row of the texture is the top of the video frame which is
        // the same layout as the decoded frames used by the other video components.
        int flipY {0};
        // Don't wait for the frame's display time, the application controls the frame pacing.
        int blockForTargetTime {0};
        mpv_render_param params[] {{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
                                   {MPV_RENDER_PARAM_FLIP_Y, &flipY},
                                   {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
                                   {MPV_RENDER_PARAM_INVALID, nullptr}};
        mpv_render_context_render(mRenderContext, params);
        renderer->resetRenderState();
        if (mFileLoaded)
            mHasFrame = true;
    }

    Renderer::Vertex vertices[4];
    // clang-format off
    vertices[0] = {{x,         y         }, {0.0f, 0.0f}, 0xFFFFFFFF};
    vertices[1] = {{x,         y + height}, {0.0f, 1.0f}, 0xFFFFFFFF};
    vertices[2] = {{x + width, y         }, {1.0f, 0.0f}, 0xFFFFFFFF};
    vertices[3] = {{x + width, y + height}, {1.0f, 1.0f}, 0xFFFFFFFF};
    // clang-format on
    for (auto& vertex : vertices)
        vertex.opacity = opacity;

    renderer->setMatrix(Renderer::getIdentity());
    renderer->bindTexture(mTexture, 0);
    renderer->drawTriangleStrips(&vertices[0], 4);
    renderer->bindTexture(0, 0);
}

std::string MpvPlayer::getPropertyString(const std::string& name) const
{
    if (mHandle == nullptr)
        return "";
    char* value {mpv_get_property_string(mHandle, name.c_str())};
    if (value == nullptr)
        return "";
    std::string result {value};
    mpv_free(value);
    return result;
}

void MpvPlayer::setPaused(bool paused)
{
    setPropertyFlag("pause", paused);
    mPaused = paused;
}

void MpvPlayer::seekRelative(double seconds)
{
    if (!mFileLoaded)
        return;
    command({"seek", std::to_string(seconds), "relative"});
}

void MpvPlayer::seekAbsolute(double seconds)
{
    if (!mFileLoaded)
        return;
    command({"seek", std::to_string(std::max(0.0, seconds)), "absolute"});
}

void MpvPlayer::seekPercent(double percent)
{
    if (!mFileLoaded)
        return;
    command({"seek", std::to_string(percent), "absolute-percent"});
}

void MpvPlayer::changeChapter(int amount)
{
    if (!mFileLoaded || mChapters.empty())
        return;
    command({"add", "chapter", std::to_string(amount)});
}

void MpvPlayer::setVolume(double volume)
{
    mVolume = std::clamp(volume, 0.0, MAX_VOLUME);
    setPropertyDouble("volume", mVolume);
}

void MpvPlayer::setMuted(bool muted)
{
    mMuted = muted;
    setPropertyFlag("mute", muted);
}

void MpvPlayer::setSpeed(double speed)
{
    mSpeed = std::clamp(speed, 0.25, 4.0);
    setPropertyDouble("speed", mSpeed);
}

void MpvPlayer::setAudioDelay(double seconds)
{
    mAudioDelay = seconds;
    setPropertyDouble("audio-delay", seconds);
}

void MpvPlayer::setSubtitleDelay(double seconds)
{
    mSubDelay = seconds;
    setPropertyDouble("sub-delay", seconds);
}

void MpvPlayer::setSubtitleScale(double scale)
{
    mSubScale = std::clamp(scale, 0.3, 3.0);
    setPropertyDouble("sub-scale", mSubScale);
}

void MpvPlayer::setSubtitlePosition(int position)
{
    mSubPos = std::clamp(position, 0, 150);
    setPropertyString("sub-pos", std::to_string(mSubPos));
}

void MpvPlayer::setAspectOverride(const std::string& aspect)
{
    mAspectOverride = aspect;
    setPropertyString("video-aspect-override", aspect);
}

void MpvPlayer::setPanScan(double value)
{
    mPanScan = std::clamp(value, 0.0, 1.0);
    setPropertyDouble("panscan", mPanScan);
}

void MpvPlayer::selectTrack(const std::string& type, int64_t id)
{
    const char* property {type == "audio" ? "aid" : (type == "sub" ? "sid" : "vid")};
    setPropertyString(property, id <= 0 ? "no" : std::to_string(id));
    for (auto& track : mTracks) {
        if (track.type == type)
            track.selected = (track.id == id);
    }
}

double MpvPlayer::getPropertyDouble(const std::string& name, double fallback) const
{
    if (mHandle == nullptr)
        return fallback;
    double value {0.0};
    if (mpv_get_property(mHandle, name.c_str(), MPV_FORMAT_DOUBLE, &value) < 0)
        return fallback;
    return value;
}

bool MpvPlayer::takeFileLoadedEvent()
{
    const bool event {mFileLoadedEvent};
    mFileLoadedEvent = false;
    return event;
}

void MpvPlayer::setSubtitleVisible(bool visible)
{
    mSubVisible = visible;
    setPropertyFlag("sub-visibility", visible);
}

void MpvPlayer::addSubtitleFile(const std::string& path)
{
    if (mFileLoaded)
        command({"sub-add", path, "select"});
}

void MpvPlayer::addAudioFile(const std::string& path)
{
    if (mFileLoaded)
        command({"audio-add", path, "select"});
}

void MpvPlayer::setProperty(const std::string& name, const std::string& value, bool synchronous)
{
    if (mHandle == nullptr)
        return;
    if (synchronous)
        mpv_set_property_string(mHandle, name.c_str(), value.c_str());
    else
        setPropertyString(name.c_str(), value);
}

void MpvPlayer::audioFilterCommand(const std::string& label,
                                   const std::string& name,
                                   const std::string& argument)
{
    if (mHandle == nullptr)
        return;
    const char* args[] {"af-command", label.c_str(), name.c_str(), argument.c_str(), nullptr};
    // Not logged as a failure, the filter doesn't exist while audio is passed through.
    mpv_command_async(mHandle, 0, args);
}

void MpvPlayer::onWakeup(void* context)
{
    static_cast<MpvPlayer*>(context)->mWakeup = true;
}

void MpvPlayer::onRenderUpdate(void* context)
{
    static_cast<MpvPlayer*>(context)->mRedraw = true;
}

void* MpvPlayer::getProcAddress(void* context, const char* name)
{
    return SDL_GL_GetProcAddress(name);
}

void MpvPlayer::setOption(const char* name, const std::string& value)
{
    const int result {mpv_set_option_string(mHandle, name, value.c_str())};
    if (result < 0) {
        LOG(LogWarning) << "MpvPlayer: Couldn't set option \"" << name << "\" to \"" << value
                        << "\": " << mpv_error_string(result);
    }
}

void MpvPlayer::setPropertyDouble(const char* name, double value)
{
    if (mHandle == nullptr)
        return;
    mpv_set_property_async(mHandle, 0, name, MPV_FORMAT_DOUBLE, &value);
}

void MpvPlayer::setPropertyFlag(const char* name, bool value)
{
    if (mHandle == nullptr)
        return;
    int flag {value ? 1 : 0};
    mpv_set_property_async(mHandle, 0, name, MPV_FORMAT_FLAG, &flag);
}

void MpvPlayer::setPropertyString(const char* name, const std::string& value)
{
    if (mHandle == nullptr)
        return;
    const char* string {value.c_str()};
    mpv_set_property_async(mHandle, 0, name, MPV_FORMAT_STRING, &string);
}

void MpvPlayer::command(const std::vector<std::string>& args)
{
    if (mHandle == nullptr)
        return;
    std::vector<const char*> argv;
    for (const auto& arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const int result {mpv_command_async(mHandle, 0, argv.data())};
    if (result < 0) {
        LOG(LogWarning) << "MpvPlayer: Command \"" << args.front()
                        << "\" failed: " << mpv_error_string(result);
    }
}

void MpvPlayer::observeProperties()
{
    mpv_observe_property(mHandle, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(mHandle, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(mHandle, 0, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(mHandle, 0, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(mHandle, 0, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "audio-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "sub-delay", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "sub-scale", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "sub-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "panscan", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mHandle, 0, "video-aspect-override", MPV_FORMAT_STRING);
    mpv_observe_property(mHandle, 0, "media-title", MPV_FORMAT_STRING);
    mpv_observe_property(mHandle, 0, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(mHandle, 0, "chapter-list", MPV_FORMAT_NODE);
    mpv_observe_property(mHandle, 0, "chapter", MPV_FORMAT_INT64);
    mpv_observe_property(mHandle, 0, "sub-visibility", MPV_FORMAT_FLAG);
}

void MpvPlayer::handlePropertyChange(const char* name, int format, void* data)
{
    const std::string property {name};
    const bool available {format != MPV_FORMAT_NONE && data != nullptr};

    auto getDouble = [&](double fallback) {
        return available && format == MPV_FORMAT_DOUBLE ? *static_cast<double*>(data) : fallback;
    };
    auto getFlag = [&]() {
        return available && format == MPV_FORMAT_FLAG && *static_cast<int*>(data) != 0;
    };
    auto getString = [&]() -> std::string {
        if (available && format == MPV_FORMAT_STRING && *static_cast<char**>(data) != nullptr)
            return *static_cast<char**>(data);
        return "";
    };

    if (property == "time-pos") {
        if (available)
            mPosition = getDouble(mPosition);
    }
    else if (property == "duration") {
        mDuration = getDouble(0.0);
    }
    else if (property == "demuxer-cache-time") {
        mCachedUntil = getDouble(0.0);
    }
    else if (property == "pause") {
        mPaused = getFlag();
    }
    else if (property == "paused-for-cache") {
        mBuffering = getFlag();
    }
    else if (property == "seeking") {
        mSeeking = getFlag();
    }
    else if (property == "volume") {
        mVolume = getDouble(mVolume);
    }
    else if (property == "mute") {
        mMuted = getFlag();
    }
    else if (property == "speed") {
        mSpeed = getDouble(1.0);
    }
    else if (property == "audio-delay") {
        mAudioDelay = getDouble(0.0);
    }
    else if (property == "sub-delay") {
        mSubDelay = getDouble(0.0);
    }
    else if (property == "sub-scale") {
        mSubScale = getDouble(1.0);
    }
    else if (property == "sub-pos") {
        mSubPos = static_cast<int>(std::round(getDouble(100.0)));
    }
    else if (property == "panscan") {
        mPanScan = getDouble(0.0);
    }
    else if (property == "video-aspect-override") {
        mAspectOverride = getString();
        if (mAspectOverride.empty() || mAspectOverride == "-1")
            mAspectOverride = "no";
    }
    else if (property == "media-title") {
        mMediaTitle = getString();
    }
    else if (property == "track-list") {
        parseTrackList(available && format == MPV_FORMAT_NODE ? static_cast<mpv_node*>(data) :
                                                                 nullptr);
    }
    else if (property == "chapter-list") {
        parseChapterList(available && format == MPV_FORMAT_NODE ? static_cast<mpv_node*>(data) :
                                                                   nullptr);
    }
    else if (property == "sub-visibility") {
        mSubVisible = !available || getFlag();
    }
    else if (property == "chapter") {
        mChapter = available && format == MPV_FORMAT_INT64 ?
                       static_cast<int>(*static_cast<int64_t*>(data)) :
                       -1;
    }
}

void MpvPlayer::parseTrackList(const mpv_node* node)
{
    mTracks.clear();
    if (node == nullptr || node->format != MPV_FORMAT_NODE_ARRAY)
        return;

    for (int i {0}; i < node->u.list->num; ++i) {
        const mpv_node* entry {&node->u.list->values[i]};
        Track track;
        track.id = nodeInt(entry, "id");
        track.type = nodeString(entry, "type");
        track.title = nodeString(entry, "title");
        track.lang = nodeString(entry, "lang");
        track.codec = nodeString(entry, "codec");
        track.selected = nodeFlag(entry, "selected");
        track.external = nodeFlag(entry, "external");
        track.isDefault = nodeFlag(entry, "default");
        track.forced = nodeFlag(entry, "forced");
        track.channels = static_cast<int>(nodeInt(entry, "demux-channel-count"));
        track.width = static_cast<int>(nodeInt(entry, "demux-w"));
        track.height = static_cast<int>(nodeInt(entry, "demux-h"));
        track.externalFilename = nodeString(entry, "external-filename");
        track.ffIndex = findMapValue(entry, "ff-index") != nullptr ?
                            static_cast<int>(nodeInt(entry, "ff-index")) :
                            -1;
        mTracks.emplace_back(track);
    }
}

void MpvPlayer::parseChapterList(const mpv_node* node)
{
    mChapters.clear();
    if (node == nullptr || node->format != MPV_FORMAT_NODE_ARRAY)
        return;

    for (int i {0}; i < node->u.list->num; ++i) {
        const mpv_node* entry {&node->u.list->values[i]};
        Chapter chapter;
        chapter.time = nodeDouble(entry, "time");
        chapter.title = nodeString(entry, "title");
        mChapters.emplace_back(chapter);
    }
}

void MpvPlayer::updateTexture(int width, int height)
{
    destroyTexture();
    if (width <= 0 || height <= 0)
        return;

    Renderer* renderer {Renderer::getInstance()};
    mTexture = renderer->createTexture(0, Renderer::TextureType::RGBA, true, true, false, false,
                                       static_cast<unsigned int>(width),
                                       static_cast<unsigned int>(height), nullptr);
    if (mTexture == 0)
        return;

    GLuint framebuffer {0};
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexture, 0);
    const GLenum status {glCheckFramebufferStatus(GL_FRAMEBUFFER)};
    // Start with a black frame.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    mFramebuffer = framebuffer;
    renderer->resetRenderState();

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG(LogError) << "MpvPlayer: Couldn't set up the video framebuffer (status " << status
                      << ")";
        destroyTexture();
        mTextureFailed = true;
        return;
    }

    mTextureWidth = width;
    mTextureHeight = height;
}

void MpvPlayer::destroyTexture()
{
    if (mFramebuffer != 0) {
        GLuint framebuffer {mFramebuffer};
        glDeleteFramebuffers(1, &framebuffer);
        mFramebuffer = 0;
    }
    if (mTexture != 0) {
        Renderer::getInstance()->destroyTexture(mTexture);
        mTexture = 0;
    }
    mTextureWidth = 0;
    mTextureHeight = 0;
}

// The windowing system headers are included last as they define a number of generic macros.
#include <SDL2/SDL_syswm.h>

namespace
{
    void* getNativeDisplay(mpv_render_param_type& type)
    {
        type = MPV_RENDER_PARAM_INVALID;
        SDL_SysWMinfo info {};
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(Renderer::getInstance()->getSDLWindow(), &info))
            return nullptr;
#if defined(SDL_VIDEO_DRIVER_X11)
        if (info.subsystem == SDL_SYSWM_X11) {
            type = MPV_RENDER_PARAM_X11_DISPLAY;
            return info.info.x11.display;
        }
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
        if (info.subsystem == SDL_SYSWM_WAYLAND) {
            type = MPV_RENDER_PARAM_WL_DISPLAY;
            return info.info.wl.display;
        }
#endif
        return nullptr;
    }
} // namespace

std::string MpvPlayer::audioFilters(bool dynamicRange, int amplification)
{
    std::string filters;
    if (dynamicRange)
        filters = DYNAMIC_RANGE_FILTER;
    if (amplification > 0) {
        filters.append((filters.empty() ? "" : ",") + std::string("lavfi=[volume=") +
                       std::to_string(amplification) + "dB]");
    }
    return filters;
}

std::string MpvPlayer::audioFiltersWithDucking(bool dynamicRange,
                                               int amplification,
                                               const std::string& graph)
{
    std::string filters {audioFilters(dynamicRange, amplification)};
    filters.append((filters.empty() ? "" : ",") + std::string("@") + DUCK_FILTER_LABEL +
                   ":lavfi=[" + graph + "]");
    return filters;
}
