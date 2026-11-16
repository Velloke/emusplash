//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  Dependencies.h
//
//  The programs EmuSPLASH keeps in ~/EmuSPLASH/dependencies, the same for every Linux
//  distribution: yt-dlp and Deno for the trailers, and Python 3 when the system has none.
//
//  ~/EmuSPLASH/dependencies/
//      bin/yt-dlp    the standalone yt-dlp of GitHub, updated once a week (yt-dlp -U)
//      bin/deno      the JavaScript runtime yt-dlp needs for YouTube, if the system has none
//      python/       a standalone Python 3 (python-build-standalone), if the system has none
//

#ifndef ES_APP_MEDIA_DEPENDENCIES_H
#define ES_APP_MEDIA_DEPENDENCIES_H

#include <atomic>
#include <string>

namespace Dependencies
{
    // ~/EmuSPLASH/dependencies and its bin folder.
    std::string directory();
    std::string binDirectory();

    // The yt-dlp kept in the bin folder (it may not be there yet).
    std::string ownYtDlp();

    // Python 3 of the system if there is one, otherwise the own copy, empty if neither.
    std::string python();

    // The PATH for the programs started by EmuSPLASH: the bin folder first (for Deno), then
    // the PATH of EmuSPLASH.
    std::string searchPath();

    // Downloads yt-dlp to the bin folder unless it's already there, false and the reason if it
    // couldn't be done. Also called by the trailers when the check at startup hasn't finished.
    bool downloadYtDlp(const std::atomic<bool>* cancel, std::string& error);

    // The check at startup, in the background: yt-dlp is downloaded or updated once a week,
    // Deno and Python are downloaded only if the system has none. A short notification is
    // shown while something is downloaded, the result is written to the log.
    void checkInBackground();
} // namespace Dependencies

#endif // ES_APP_MEDIA_DEPENDENCIES_H
