//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaLibrary.h
//
//  Video sources (folders added by the user) and the play state of the video files,
//  i.e. resume positions and watched status. Stored in the settings directory as
//  media_sources.xml and media_playstate.xml.
//

#ifndef ES_APP_MEDIA_MEDIA_LIBRARY_H
#define ES_APP_MEDIA_MEDIA_LIBRARY_H

#include <map>
#include <string>
#include <vector>

class MediaLibrary
{
public:
    struct Source {
        std::string name;
        std::string path;
    };

    struct PlayState {
        double position {0.0}; // Resume position in seconds, 0 if none.
        double duration {0.0}; // Last known duration in seconds.
        bool watched {false};
        int playCount {0};
        long long lastPlayed {0}; // Unix time.
        std::vector<double> bookmarks; // Positions in seconds, sorted.
        // Player settings changed for this file, e.g. the selected audio track.
        std::map<std::string, std::string> settings;
    };

    enum class EntryType {
        PARENT,
        FOLDER,
        VIDEO,
        SUBTITLE
    };

    struct Entry {
        EntryType type;
        std::string name;
        std::string path;
    };

    static MediaLibrary& getInstance();

    // Video sources.
    const std::vector<Source>& getVideoSources() const { return mVideoSources; }
    void addVideoSource(const std::string& name, const std::string& path);
    void updateVideoSource(size_t index, const std::string& name, const std::string& path);
    void removeVideoSource(size_t index);

    // Favourites, both folders and video files.
    struct Favourite {
        std::string name;
        std::string path;
        bool folder {false};
    };
    const std::vector<Favourite>& getFavourites() const { return mFavourites; }
    bool isFavourite(const std::string& path) const;
    void addFavourite(const std::string& name, const std::string& path, bool folder);
    void removeFavourite(const std::string& path);

    // Play state per video file.
    PlayState getPlayState(const std::string& path) const;
    // Called when playback stops. Decides whether the file is marked as watched or whether
    // the position is stored for resuming.
    void savePlaybackProgress(const std::string& path, double position, double duration);
    void setWatched(const std::string& path, bool watched);
    // Marks all videos in the folder and its subfolders.
    void setFolderWatched(const std::string& folder, bool watched);
    // True if the folder has videos and all of them have been watched (not subfolders).
    bool isFolderWatched(const std::string& folder) const;
    void clearResumePosition(const std::string& path);

    // Bookmarks within a video file.
    void addBookmark(const std::string& path, double position);
    void removeBookmark(const std::string& path, size_t index);
    void clearBookmarks(const std::string& path);

    // Player settings that are restored when the file is played again.
    void setFileSettings(const std::string& path,
                         const std::map<std::string, std::string>& settings);

    // Directory listing with the subfolders first followed by the video files, both sorted
    // using natural sort order. Hidden files and non-video files are skipped.
    static std::vector<Entry> listDirectory(const std::string& path);
    // Subdirectories only, used when choosing a folder for a source.
    static std::vector<Entry> listSubdirectories(const std::string& path);
    static bool isVideoFile(const std::string& path);
    // Subfolders followed by the subtitle files, used when loading external subtitles.
    static std::vector<Entry> listSubtitleDirectory(const std::string& path);
    static bool isSubtitleFile(const std::string& path);

    // Suggested starting points when browsing for a new source.
    static std::vector<Entry> getBrowseRoots();

    // Natural sort comparison, "Episode 2" comes before "Episode 10".
    static bool naturalLess(const std::string& a, const std::string& b);

    // The key of a movie or an episode of the streaming add-on in the play states, made of its
    // address (e.g. "movie/603"), empty if the address isn't one of them.
    static std::string streamKey(const std::string& address);

    // Formats seconds as H:MM:SS or M:SS.
    static std::string formatTime(double seconds, bool forceHours = false);

private:
    MediaLibrary();

    void loadSources();
    void saveSources();
    void loadPlayStates();
    void savePlayStates();

    static std::string getSettingsPath(const std::string& fileName);

    std::vector<Source> mVideoSources;
    std::vector<Favourite> mFavourites;
    std::map<std::string, PlayState> mPlayStates;
};

#endif // ES_APP_MEDIA_MEDIA_LIBRARY_H
