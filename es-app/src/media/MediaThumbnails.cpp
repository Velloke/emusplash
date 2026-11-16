//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaThumbnails.cpp
//
//  Preview images for video files and folders. Video previews are frames taken from the
//  video in a background thread and stored in the cover-art directory in the application
//  data directory. Previews of videos that no longer exist are removed. Folders use a poster
//  image inside the folder if there is one, otherwise the preview of their first video.
//

#include "media/MediaThumbnails.h"

#include "Log.h"
#include "media/MediaLibrary.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <FreeImage.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <cstdio>
#include <fstream>

namespace
{
    // Width of the stored previews, the height follows the aspect ratio of the video.
    constexpr int PREVIEW_WIDTH {480};
    // Position of the frame, as a fraction of the duration, to avoid black frames and logos.
    constexpr double PREVIEW_POSITION {0.10};

    std::string hashPath(const std::string& path)
    {
        // 64-bit FNV-1a.
        uint64_t hash {14695981039346656037ULL};
        for (const unsigned char character : path) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        char text[17];
        snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
        return text;
    }
} // namespace

MediaThumbnails& MediaThumbnails::getInstance()
{
    static MediaThumbnails instance;
    return instance;
}

MediaThumbnails::MediaThumbnails()
    : mCleanupRequested {false}
    , mStop {false}
{
    loadIndex();
    mThread = std::thread(&MediaThumbnails::workerLoop, this);
}

MediaThumbnails::~MediaThumbnails()
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        mStop = true;
        mQueue.clear();
    }
    mCondition.notify_all();
    if (mThread.joinable())
        mThread.join();
}

std::string MediaThumbnails::getDirectory()
{
    return Utils::FileSystem::getAppDataDirectory() + "/cover-art";
}

std::string MediaThumbnails::thumbnailPath(const std::string& videoPath)
{
    return getDirectory() + "/" + hashPath(videoPath) + ".jpg";
}

std::string MediaThumbnails::getVideoPreview(const std::string& videoPath, bool urgent)
{
    const std::string path {thumbnailPath(videoPath)};
    if (Utils::FileSystem::exists(path))
        return path;
    enqueue(videoPath, urgent);
    return "";
}

std::string MediaThumbnails::getFolderPreview(const std::string& folderPath, bool urgent)
{
    // The folder is only searched once, as listing it can be slow for network shares.
    auto it {mFolders.find(folderPath)};
    if (it == mFolders.end()) {
        Folder folder;
        for (const char* name : {"poster.jpg", "poster.png", "folder.jpg", "folder.png",
                                 "cover.jpg", "cover.png", "fanart.jpg", "fanart.png"}) {
            const std::string path {folderPath + "/" + name};
            if (Utils::FileSystem::isRegularFile(path)) {
                folder.poster = path;
                break;
            }
        }
        if (folder.poster.empty()) {
            for (const auto& entry : MediaLibrary::listDirectory(folderPath)) {
                if (entry.type == MediaLibrary::EntryType::VIDEO) {
                    folder.firstVideo = entry.path;
                    break;
                }
            }
        }
        it = mFolders.emplace(folderPath, folder).first;
    }
    if (!it->second.poster.empty())
        return it->second.poster;
    if (!it->second.firstVideo.empty())
        return getVideoPreview(it->second.firstVideo, urgent);
    return "";
}

void MediaThumbnails::queueVideos(const std::vector<std::string>& videoPaths)
{
    for (const auto& path : videoPaths) {
        if (!Utils::FileSystem::exists(thumbnailPath(path)))
            enqueue(path, false);
    }
}

void MediaThumbnails::enqueue(const std::string& videoPath, bool urgent)
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        if (mFailed.count(videoPath) != 0)
            return;
        if (mQueued.count(videoPath) != 0) {
            if (!urgent)
                return;
            // Move it to the front.
            for (auto it = mQueue.begin(); it != mQueue.end(); ++it) {
                if (*it == videoPath) {
                    mQueue.erase(it);
                    break;
                }
            }
        }
        mQueued.insert(videoPath);
        if (urgent)
            mQueue.push_front(videoPath);
        else
            mQueue.push_back(videoPath);
    }
    mCondition.notify_one();
}

void MediaThumbnails::startCleanup()
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        mCleanupRequested = true;
    }
    mCondition.notify_one();
}

void MediaThumbnails::registerFile(const std::string& fileName, const std::string& sourcePath)
{
    std::unique_lock<std::mutex> lock {mMutex};
    const auto it {mIndex.find(fileName)};
    if (it != mIndex.cend() && it->second == sourcePath)
        return;
    mIndex[fileName] = sourcePath;
    saveIndex();
}

void MediaThumbnails::workerLoop()
{
    while (true) {
        std::string videoPath;
        bool cleanupNow {false};
        {
            std::unique_lock<std::mutex> lock {mMutex};
            mCondition.wait(lock,
                            [this] { return mStop || mCleanupRequested || !mQueue.empty(); });
            if (mStop)
                return;
            if (mCleanupRequested) {
                mCleanupRequested = false;
                cleanupNow = true;
            }
            else {
                videoPath = mQueue.front();
                mQueue.pop_front();
            }
        }

        if (cleanupNow) {
            cleanup();
            continue;
        }

        const std::string output {thumbnailPath(videoPath)};
        bool success {Utils::FileSystem::exists(output)};
        if (!success) {
            Utils::FileSystem::createDirectory(getDirectory());
            success = generate(videoPath, output);
        }

        std::unique_lock<std::mutex> lock {mMutex};
        mQueued.erase(videoPath);
        if (success) {
            mIndex[Utils::FileSystem::getFileName(output)] = videoPath;
            saveIndex();
        }
        else {
            mFailed.insert(videoPath);
        }
    }
}

void MediaThumbnails::loadIndex()
{
    std::ifstream file {getDirectory() + "/index.txt"};
    std::string line;
    while (std::getline(file, line)) {
        const size_t tab {line.find('\t')};
        if (tab != std::string::npos)
            mIndex[line.substr(0, tab)] = line.substr(tab + 1);
    }
}

void MediaThumbnails::saveIndex()
{
    Utils::FileSystem::createDirectory(getDirectory());
    std::ofstream file {getDirectory() + "/index.txt", std::ios::trunc};
    for (const auto& [thumbnail, video] : mIndex)
        file << thumbnail << '\t' << video << '\n';
}

void MediaThumbnails::cleanup()
{
    std::map<std::string, std::string> index;
    {
        std::unique_lock<std::mutex> lock {mMutex};
        index = mIndex;
    }
    std::vector<std::string> removed;
    for (const auto& [thumbnail, video] : index) {
        // Videos on drives that aren't mounted are kept, only removed files are cleaned up.
        const std::string folder {Utils::FileSystem::getParent(video)};
        if (Utils::FileSystem::isDirectory(folder) && !Utils::FileSystem::exists(video))
            removed.emplace_back(thumbnail);
    }
    if (removed.empty())
        return;

    std::unique_lock<std::mutex> lock {mMutex};
    for (const auto& thumbnail : removed) {
        Utils::FileSystem::removeFile(getDirectory() + "/" + thumbnail);
        mIndex.erase(thumbnail);
    }
    saveIndex();
    LOG(LogInfo) << "MediaThumbnails: Removed " << removed.size()
                 << " previews of videos that no longer exist";
}

bool MediaThumbnails::generate(const std::string& videoPath, const std::string& outputPath)
{
    AVFormatContext* format {nullptr};
    if (avformat_open_input(&format, videoPath.c_str(), nullptr, nullptr) != 0) {
        LOG(LogWarning) << "MediaThumbnails: Couldn't open \"" << videoPath << "\"";
        return false;
    }
    bool success {false};
    AVCodecContext* codecContext {nullptr};
    AVFrame* frame {av_frame_alloc()};
    AVPacket* packet {av_packet_alloc()};
    SwsContext* scaler {nullptr};
    std::vector<uint8_t> pixels;
    int width {0};
    int height {0};

    do {
        if (avformat_find_stream_info(format, nullptr) < 0)
            break;
        // FFmpeg 4 (libavutil 56, Ubuntu 22.04 of the AppImage) gives a codec that isn't const.
#if LIBAVUTIL_VERSION_MAJOR > 56
        const AVCodec* codec {nullptr};
#else
        AVCodec* codec {nullptr};
#endif
        const int stream {av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)};
        if (stream < 0 || codec == nullptr)
            break;
        codecContext = avcodec_alloc_context3(codec);
        if (codecContext == nullptr ||
            avcodec_parameters_to_context(codecContext, format->streams[stream]->codecpar) < 0)
            break;
        codecContext->thread_count = 2;
        if (avcodec_open2(codecContext, codec, nullptr) < 0)
            break;

        // Seek to the preview position.
        if (format->duration > 0) {
            const int64_t target {
                static_cast<int64_t>(static_cast<double>(format->duration) * PREVIEW_POSITION)};
            if (av_seek_frame(format, -1, target, AVSEEK_FLAG_BACKWARD) >= 0)
                avcodec_flush_buffers(codecContext);
        }

        bool gotFrame {false};
        for (int packets {0}; packets < 3000 && !gotFrame; ++packets) {
            if (av_read_frame(format, packet) < 0) {
                // Drain the decoder at the end of the file.
                avcodec_send_packet(codecContext, nullptr);
                gotFrame = avcodec_receive_frame(codecContext, frame) == 0;
                break;
            }
            if (packet->stream_index == stream && avcodec_send_packet(codecContext, packet) >= 0)
                gotFrame = avcodec_receive_frame(codecContext, frame) == 0;
            av_packet_unref(packet);
        }
        if (!gotFrame || frame->width <= 0 || frame->height <= 0)
            break;

        // Keep the display aspect ratio of the video.
        double aspect {static_cast<double>(frame->width) / frame->height};
        const AVRational sar {av_guess_sample_aspect_ratio(format, format->streams[stream], frame)};
        if (sar.num > 0 && sar.den > 0)
            aspect *= av_q2d(sar);
        width = PREVIEW_WIDTH;
        height = std::max(2, static_cast<int>(PREVIEW_WIDTH / aspect) & ~1);

        scaler = sws_getContext(frame->width, frame->height,
                                static_cast<AVPixelFormat>(frame->format), width, height,
                                AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler == nullptr)
            break;
        const int stride {width * 3};
        pixels.resize(static_cast<size_t>(stride) * static_cast<size_t>(height));
        uint8_t* destination[4] {pixels.data(), nullptr, nullptr, nullptr};
        int destinationStride[4] {stride, 0, 0, 0};
        sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, destination,
                  destinationStride);

        FIBITMAP* bitmap {FreeImage_ConvertFromRawBits(pixels.data(), width, height, stride, 24,
                                                       FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK,
                                                       FI_RGBA_BLUE_MASK, TRUE)};
        if (bitmap == nullptr)
            break;
        // Written to a temporary file first so a partial file is never shown.
        const std::string temporary {outputPath + ".tmp"};
        success = FreeImage_Save(FIF_JPEG, bitmap, temporary.c_str(), JPEG_QUALITYGOOD) != 0;
        FreeImage_Unload(bitmap);
        // Note that renameFile() returns true on failure.
        if (success)
            success = !Utils::FileSystem::renameFile(temporary, outputPath, true);
        if (!success)
            Utils::FileSystem::removeFile(temporary);
    } while (false);

    if (scaler != nullptr)
        sws_freeContext(scaler);
    av_packet_free(&packet);
    av_frame_free(&frame);
    if (codecContext != nullptr)
        avcodec_free_context(&codecContext);
    avformat_close_input(&format);

    if (!success) {
        LOG(LogWarning) << "MediaThumbnails: Couldn't create a preview of \"" << videoPath << "\"";
    }
    return success;
}
