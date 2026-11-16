//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaMetadata.h
//
//  Titles, descriptions and posters of the videos and folders in the video sources, looked up
//  on TheMovieDB (TMDb) using the API key of the user. The movie or series is identified from
//  the file or folder name, and can be chosen manually if that fails. The information and the
//  images are stored in the preview directory and removed together with the previews.
//

#ifndef ES_APP_MEDIA_MEDIA_METADATA_H
#define ES_APP_MEDIA_MEDIA_METADATA_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class MediaMetadata
{
public:
    struct Info {
        bool found {false};
        std::string type; // "movie", "tvshow" or "episode".
        std::string title;
        std::string originalTitle;
        std::string showTitle;
        std::string episodeTitle;
        int year {0};
        int season {0};
        int episode {0};
        std::string plot;
        double rating {0.0};
        int runtime {0}; // Minutes.
        std::string genres;
        std::string poster; // Local files.
        std::string fanart;
        int tmdbId {0};
    };

    // A possible match when choosing the movie or series manually.
    struct Candidate {
        std::string type; // "movie" or "tv".
        int id {0};
        std::string title;
        int year {0};
        std::string overview;
    };

    static MediaMetadata& getInstance();

    // True if an API key has been set.
    static bool isEnabled();

    // The information of a video or folder. Returns false if it hasn't been looked up yet,
    // in which case it's queued.
    bool get(const std::string& path, bool folder, bool urgent, Info& info);
    void queue(const std::vector<std::pair<std::string, bool>>& entries);

    // Searches for movies and series, the callback is called from a background thread.
    void search(const std::string& query,
                const std::function<void(std::vector<Candidate>, std::string)>& callback);
    // Uses the given movie or series for the video or folder, done in the background.
    void identify(const std::string& path, bool folder, const Candidate& candidate);
    // Looks up the information again.
    void refresh(const std::string& path, bool folder);

    // The country (ISO 3166-1 code, e.g. "US") and the original language (ISO 639-1 code,
    // e.g. "en") of a movie or series on TMDb. Blocks, so to be called from a background
    // thread. Returns false if it couldn't be looked up.
    static bool lookupOrigin(bool series,
                             int tmdbId,
                             std::string& country,
                             std::string& language);

    // A short description of a movie or series on TMDb for the translation of the dub: title,
    // year, genres, summary and the main characters (with male or female). Blocks, so to be
    // called from a background thread. Returns false if it couldn't be looked up.
    static bool lookupContext(bool series, int tmdbId, std::string& context);

    // A video of a movie or series on TMDb, here only the ones on YouTube.
    struct Video {
        std::string name;
        std::string key; // The YouTube id.
    };

    // The trailers of a movie or series, the best first (official trailers, then others, then
    // teasers). Blocks, so to be called from a background thread. False if TMDb couldn't be
    // reached, true with an empty list if there are none.
    static bool lookupVideos(bool series,
                             int tmdbId,
                             std::vector<Video>& videos,
                             std::string& error);

private:
    MediaMetadata();
    ~MediaMetadata();

    struct Job {
        std::string path;
        bool folder {false};
        Candidate forced;
    };

    void workerLoop();
    Info lookup(const Job& job);
    bool loadInfo(const std::string& path, Info& info);
    void saveInfo(const std::string& path, const Info& info);
    static std::string infoFile(const std::string& path);
    std::string downloadImage(const std::string& tmdbPath,
                              const std::string& size,
                              const std::string& sourcePath,
                              const std::string& suffix);

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::deque<Job> mQueue;
    std::set<std::string> mQueued;
    std::map<std::string, Info> mCache;
    // Series looked up by name, so the episodes of a series don't search again.
    std::map<std::string, int> mShowIds;
    std::thread mThread;
    bool mStop;
};

#endif // ES_APP_MEDIA_MEDIA_METADATA_H
