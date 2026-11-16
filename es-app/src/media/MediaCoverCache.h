//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaCoverCache.h
//
//  Posters and other images of the streaming add-on, downloaded in the background and kept
//  in the cover directory of the preview images. Images older than seven days are removed
//  when the application starts.
//

#ifndef ES_APP_MEDIA_MEDIA_COVER_CACHE_H
#define ES_APP_MEDIA_MEDIA_COVER_CACHE_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class MediaCoverCache
{
public:
    static MediaCoverCache& getInstance();

    // The local file of the image, empty while it's being downloaded. Local paths are
    // returned as they are if the file exists.
    std::string get(const std::string& url, bool urgent);

    static std::string getDirectory();
    // Removes the images that are older than seven days, done in the background.
    static void removeExpiredInBackground();

private:
    MediaCoverCache();
    ~MediaCoverCache();
    void workerLoop();
    static std::string cachePath(const std::string& url);

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::deque<std::string> mQueue;
    std::set<std::string> mQueued;
    std::set<std::string> mActive;
    std::map<std::string, std::chrono::steady_clock::time_point> mFailed;
    std::vector<std::thread> mThreads;
    bool mStop;
};

#endif // ES_APP_MEDIA_MEDIA_COVER_CACHE_H
