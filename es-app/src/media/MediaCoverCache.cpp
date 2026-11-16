//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaCoverCache.cpp
//
//  Posters and other images of the streaming add-on, downloaded in the background and kept
//  in the cover directory of the preview images. Images older than seven days are removed
//  when the application starts.
//

#include "media/MediaCoverCache.h"

#include "Log.h"
#include "media/HttpClient.h"
#include "media/MediaThumbnails.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace
{
    constexpr int WORKER_COUNT {3};
    constexpr int MAX_AGE_DAYS {7};

    std::string hashText(const std::string& text)
    {
        // 64-bit FNV-1a.
        uint64_t hash {14695981039346656037ULL};
        for (const unsigned char character : text) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        char result[17];
        snprintf(result, sizeof(result), "%016llx", static_cast<unsigned long long>(hash));
        return result;
    }
} // namespace

MediaCoverCache& MediaCoverCache::getInstance()
{
    static MediaCoverCache instance;
    return instance;
}

MediaCoverCache::MediaCoverCache()
    : mStop {false}
{
    for (int i {0}; i < WORKER_COUNT; ++i)
        mThreads.emplace_back(&MediaCoverCache::workerLoop, this);
}

MediaCoverCache::~MediaCoverCache()
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        mStop = true;
        mQueue.clear();
    }
    mCondition.notify_all();
    for (auto& thread : mThreads) {
        if (thread.joinable())
            thread.join();
    }
}

std::string MediaCoverCache::getDirectory() { return MediaThumbnails::getDirectory() + "/cover"; }

std::string MediaCoverCache::cachePath(const std::string& url)
{
    std::string extension {".jpg"};
    const std::string lower {Utils::String::toLower(url)};
    if (lower.find(".png") != std::string::npos)
        extension = ".png";
    else if (lower.find(".webp") != std::string::npos)
        extension = ".webp";
    return getDirectory() + "/" + hashText(url) + extension;
}

std::string MediaCoverCache::get(const std::string& url, bool urgent)
{
    if (url.empty())
        return "";
    if (!Utils::String::startsWith(url, "http://") && !Utils::String::startsWith(url, "https://"))
        return Utils::FileSystem::isRegularFile(url) ? url : "";

    const std::string path {cachePath(url)};
    if (Utils::FileSystem::isRegularFile(path))
        return path;

    {
        std::unique_lock<std::mutex> lock {mMutex};
        // Failed downloads are tried again after a while, e.g. after a timeout.
        const auto failed {mFailed.find(url)};
        if (failed != mFailed.end()) {
            if (std::chrono::steady_clock::now() - failed->second < std::chrono::minutes(2))
                return "";
            mFailed.erase(failed);
        }
        // An image that is being downloaded isn't queued again, two downloads of the same
        // image would write to the same file.
        if (mActive.count(url) != 0)
            return "";
        if (mQueued.count(url) != 0) {
            if (!urgent)
                return "";
            for (auto it = mQueue.begin(); it != mQueue.end(); ++it) {
                if (*it == url) {
                    mQueue.erase(it);
                    break;
                }
            }
        }
        mQueued.insert(url);
        if (urgent)
            mQueue.push_front(url);
        else
            mQueue.push_back(url);
    }
    mCondition.notify_one();
    return "";
}

void MediaCoverCache::workerLoop()
{
    while (true) {
        std::string url;
        {
            std::unique_lock<std::mutex> lock {mMutex};
            mCondition.wait(lock, [this] { return mStop || !mQueue.empty(); });
            if (mStop)
                return;
            url = mQueue.front();
            mQueue.pop_front();
            mActive.insert(url);
        }
        Utils::FileSystem::createDirectory(getDirectory());
        HttpClient::Options options;
        options.timeout = 30;
        std::string error;
        const bool success {HttpClient::download(url, cachePath(url), options, error)};
        if (!success) {
            LOG(LogDebug) << "MediaCoverCache: Couldn't download \"" << url << "\": " << error;
        }

        std::unique_lock<std::mutex> lock {mMutex};
        mQueued.erase(url);
        mActive.erase(url);
        if (!success)
            mFailed[url] = std::chrono::steady_clock::now();
    }
}

void MediaCoverCache::removeExpiredInBackground()
{
    std::thread([] {
        namespace fs = std::filesystem;
        std::error_code errorCode;
        const auto limit {fs::file_time_type::clock::now() - std::chrono::hours(24 * MAX_AGE_DAYS)};
        int removed {0};
        for (const auto& entry : fs::directory_iterator(getDirectory(), errorCode)) {
            if (!entry.is_regular_file(errorCode))
                continue;
            if (entry.last_write_time(errorCode) < limit && fs::remove(entry.path(), errorCode))
                ++removed;
        }
        if (removed > 0) {
            LOG(LogInfo) << "MediaCoverCache: Removed " << removed << " images older than "
                         << MAX_AGE_DAYS << " days";
        }
    }).detach();
}
