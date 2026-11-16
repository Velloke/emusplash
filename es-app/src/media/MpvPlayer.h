//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MpvPlayer.h
//
//  Video playback using libmpv. The video is rendered through the application's OpenGL
//  context into a texture which is then drawn by the regular renderer, which makes it
//  possible to render menus and on-screen controls on top of it.
//

#ifndef ES_APP_MEDIA_MPV_PLAYER_H
#define ES_APP_MEDIA_MPV_PLAYER_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct mpv_handle;
struct mpv_render_context;
struct mpv_node;

class MpvPlayer
{
public:
    struct Track {
        int64_t id {0};
        std::string type; // "video", "audio" or "sub".
        std::string title;
        std::string lang;
        std::string codec;
        bool selected {false};
        bool external {false};
        bool isDefault {false};
        bool forced {false};
        int channels {0};
        int width {0};
        int height {0};
        // The file of an external track, e.g. a subtitle file next to the video.
        std::string externalFilename;
        // The index of the stream inside the file, -1 if unknown.
        int ffIndex {-1};
    };

    struct Chapter {
        double time {0.0};
        std::string title;
    };

    enum class EndReason {
        NONE,
        END_OF_FILE,
        STOPPED,
        FAILED
    };

    MpvPlayer();
    ~MpvPlayer();

    // Creates the player and its renderer. The OpenGL context must be current.
    bool initialize();
    bool isInitialized() const { return mRenderContext != nullptr; }

    // Starts playing the file, optionally from the given position in seconds.
    void loadFile(const std::string& path, double startPosition = 0.0);
    void stop();

    // Processes the events from the player, to be called once per frame.
    void update();
    // Renders the current video frame to fill the given area (in screen coordinates).
    void render(float x, float y, float width, float height, float opacity = 1.0f);

    // Playback state.
    bool isFileLoaded() const { return mFileLoaded; }
    bool hasVideoFrame() const { return mHasFrame; }
    EndReason getEndReason() const { return mEndReason; }
    const std::string& getErrorMessage() const { return mErrorMessage; }
    double getPosition() const { return mPosition; }
    double getDuration() const { return mDuration; }
    bool isPaused() const { return mPaused; }
    bool isBuffering() const { return mBuffering || mSeeking; }
    double getCachedUntil() const { return mCachedUntil; }
    double getVolume() const { return mVolume; }
    bool isMuted() const { return mMuted; }
    double getSpeed() const { return mSpeed; }
    double getAudioDelay() const { return mAudioDelay; }
    double getSubtitleDelay() const { return mSubDelay; }
    double getSubtitleScale() const { return mSubScale; }
    int getSubtitlePosition() const { return mSubPos; }
    double getPanScan() const { return mPanScan; }
    const std::string& getAspectOverride() const { return mAspectOverride; }
    const std::string& getMediaTitle() const { return mMediaTitle; }
    const std::vector<Track>& getTracks() const { return mTracks; }
    const std::vector<Chapter>& getChapters() const { return mChapters; }
    int getChapter() const { return mChapter; }
    // Reads any property as a string, e.g. for the information overlay.
    std::string getPropertyString(const std::string& name) const;
    double getPropertyDouble(const std::string& name, double fallback = 0.0) const;
    bool isSubtitleVisible() const { return mSubVisible; }
    // Returns true once after a new file has been loaded, used to apply per-file settings.
    bool takeFileLoadedEvent();

    // Controls.
    void setPaused(bool paused);
    void togglePause() { setPaused(!mPaused); }
    void seekRelative(double seconds);
    void seekAbsolute(double seconds);
    void seekPercent(double percent);
    void changeChapter(int amount);
    void setVolume(double volume);
    void setMuted(bool muted);
    void setSpeed(double speed);
    void setAudioDelay(double seconds);
    void setSubtitleDelay(double seconds);
    void setSubtitleScale(double scale);
    void setSubtitlePosition(int position);
    // The aspect ratio override, e.g. "16:9", or "no" to use the video's own aspect ratio.
    void setAspectOverride(const std::string& aspect);
    // Zooms in to fill the screen by cropping the video, 0.0 (off) to 1.0 (fully cropped).
    void setPanScan(double value);
    // A track ID of 0 disables the track type, e.g. no subtitles.
    void selectTrack(const std::string& type, int64_t id);
    void setSubtitleVisible(bool visible);
    // Loads an external subtitle file and selects it.
    void addSubtitleFile(const std::string& path);
    // Adds an audio file or address to the loaded file and selects it.
    void addAudioFile(const std::string& path);
    // Sets any property using its string representation. Synchronous calls are completed
    // before returning, e.g. to make sure an option is in place before loading a file.
    void setProperty(const std::string& name, const std::string& value, bool synchronous = false);
    // Sends a command to the audio filter with the given label, e.g. to change its volume.
    void audioFilterCommand(const std::string& label,
                            const std::string& name,
                            const std::string& argument);

    static constexpr double MAX_VOLUME {130.0};
    // Audio filter used for the dynamic range compression setting.
    static constexpr const char* DYNAMIC_RANGE_FILTER {"lavfi=[dynaudnorm=f=250:g=31:p=0.5]"};
    // The audio filters for the dynamic range compression and the volume amplification in
    // decibels, empty if neither is used.
    static std::string audioFilters(bool dynamicRange, int amplification);
    // Label of the audio filter that lowers the volume of the video while the dub speaks.
    static constexpr const char* DUCK_FILTER_LABEL {"dubduck"};
    // The audio filters with the filter used for lowering the volume while the dub speaks,
    // graph is the lavfi filter graph of that filter.
    static std::string audioFiltersWithDucking(bool dynamicRange,
                                               int amplification,
                                               const std::string& graph = "volume=volume=1.0");

private:
    static void onWakeup(void* context);
    static void onRenderUpdate(void* context);
    static void* getProcAddress(void* context, const char* name);

    void setOption(const char* name, const std::string& value);
    void setPropertyDouble(const char* name, double value);
    void setPropertyFlag(const char* name, bool value);
    void setPropertyString(const char* name, const std::string& value);
    void command(const std::vector<std::string>& args);
    void observeProperties();
    void handlePropertyChange(const char* name, int format, void* data);
    void parseTrackList(const mpv_node* node);
    void parseChapterList(const mpv_node* node);
    void updateTexture(int width, int height);
    void destroyTexture();

    mpv_handle* mHandle;
    mpv_render_context* mRenderContext;

    std::atomic<bool> mWakeup;
    std::atomic<bool> mRedraw;

    unsigned int mTexture;
    unsigned int mFramebuffer;
    int mTextureWidth;
    int mTextureHeight;
    bool mTextureFailed;

    bool mFileLoaded;
    bool mFileLoadedEvent;
    // A file has been requested and its end hasn't been reported yet.
    bool mFileActive;
    // A file from the internet (not from this computer), closing it may wait for the network.
    bool mRemoteFile {false};
    // End events still to come for files that were replaced by a new one.
    int mReplacedFiles;
    bool mHasFrame;
    bool mSubVisible;
    EndReason mEndReason;
    std::string mErrorMessage;

    double mPosition;
    double mDuration;
    double mCachedUntil;
    bool mPaused;
    bool mBuffering;
    bool mSeeking;
    double mVolume;
    bool mMuted;
    double mSpeed;
    double mAudioDelay;
    double mSubDelay;
    double mSubScale;
    int mSubPos;
    double mPanScan;
    std::string mAspectOverride;
    std::string mMediaTitle;
    std::vector<Track> mTracks;
    std::vector<Chapter> mChapters;
    int mChapter;
};

#endif // ES_APP_MEDIA_MPV_PLAYER_H
