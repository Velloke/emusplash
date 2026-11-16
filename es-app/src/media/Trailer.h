//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  Trailer.h
//
//  Finds the trailer of a movie or series on TMDb and the address to play it from. It only
//  reads from TMDb and runs yt-dlp, nothing of the streaming add-on is used or changed.
//

#ifndef ES_APP_MEDIA_TRAILER_H
#define ES_APP_MEDIA_TRAILER_H

#include <atomic>
#include <memory>
#include <string>

namespace Trailer
{
    // The result of a lookup running in the background.
    struct Task {
        std::atomic<bool> done {false};
        std::atomic<bool> cancel {false};
        // What is being done, shown while waiting.
        enum Stage { LOOKING, DOWNLOADING, UPDATING };
        std::atomic<int> stage {LOOKING};
        // Set before done: the address to play, or empty and the reason.
        std::string url;
        // The audio, if YouTube gives it separately from the video.
        std::string audioUrl;
        std::string name;
        std::string error;
    };

    // Starts looking up the trailer of the movie or series. With the TMDb API key and the id
    // the trailers of TMDb are used, otherwise (or if TMDb has none) YouTube is searched for
    // the title and the year.
    std::shared_ptr<Task> start(bool series, int tmdbId, const std::string& title, int year);

    // The copy of yt-dlp kept by EmuSPLASH: ~/EmuSPLASH/dependencies/bin/yt-dlp. It's downloaded
    // at startup (Dependencies), updated once a week and when it fails.
    std::string ownYtDlp();

    // The path of yt-dlp, empty if it can't be found. The own copy comes first.
    std::string findYtDlp();
} // namespace Trailer

#endif // ES_APP_MEDIA_TRAILER_H
