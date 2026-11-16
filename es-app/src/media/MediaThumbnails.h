//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaThumbnails.h
//
//  Preview images for video files and folders. Video previews are frames taken from the
//  video in a background thread and stored in the cover-art directory in the application
//  data directory. Previews of videos that no longer exist are removed. Folders use a poster
//  image inside the folder if there is one, otherwise the preview of their first video.
//

#ifndef ES_APP_MEDIA_MEDIA_THUMBNAILS_H
#define ES_APP_MEDIA_MEDIA_THUMBNAILS_H

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class MediaThumbnails
{
public:
    static MediaThumbnails& getInstance();

    // Returns the preview image of the video if it has been created, otherwise an empty
    // string and the preview is queued. Urgent requests are handled before the others.
    std::string getVideoPreview(const std::string& videoPath, bool urgent);
    // The poster in the folder, or the preview of the first video in it.
    std::string getFolderPreview(const std::string& folderPath, bool urgent);
    // Queues the previews of all videos in the list that don't have one yet.
    void queueVideos(const std::vector<std::string>& videoPaths);

    // Removes the previews of videos that no longer exist, done in the background.
    void startCleanup();
    // Adds another file in the directory (relative path) that belongs to a video or folder,
    // so it's removed together with the preview.
    void registerFile(const std::string& fileName, const std::string& sourcePath);

    static std::string getDirectory();

private:
    MediaThumbnails();
    ~MediaThumbnails();

    void workerLoop();
    void enqueue(const std::string& videoPath, bool urgent);
    void cleanup();
    void loadIndex();
    void saveIndex();
    static std::string thumbnailPath(const std::string& videoPath);
    static bool generate(const std::string& videoPath, const std::string& outputPath);

    std::mutex mMutex;
    std::condition_variable mCondition;
    std::deque<std::string> mQueue;
    std::set<std::string> mQueued;
    std::set<std::string> mFailed;
    // Thumbnail file name to video path, used for removing old previews.
    std::map<std::string, std::string> mIndex;
    // Poster or first video of the folders, only used from the main thread.
    struct Folder {
        std::string poster;
        std::string firstVideo;
    };
    std::map<std::string, Folder> mFolders;
    bool mCleanupRequested;
    bool mStop;
    std::thread mThread;
};

#endif // ES_APP_MEDIA_MEDIA_THUMBNAILS_H
