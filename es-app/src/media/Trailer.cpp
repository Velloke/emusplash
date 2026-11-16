//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  Trailer.cpp
//
//  Finds the trailer of a movie or series on TMDb and the address to play it from.
//

#include "media/Trailer.h"

#include "Log.h"
#include "media/Dependencies.h"
#include "media/HttpClient.h"
#include "media/MediaMetadata.h"
#include "utils/FileSystemUtil.h"

#include <cstdio>
#include <cstdlib>
#include <regex>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace Trailer
{
    namespace
    {
        std::string readCommand(const std::string& command)
        {
            std::string output;
            FILE* pipe {popen((command + " 2>&1").c_str(), "r")};
            if (pipe == nullptr)
                return output;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
                output.append(buffer);
            pclose(pipe);
            return output;
        }

        // In single quotes for the shell.
        std::string quote(const std::string& text)
        {
            std::string quoted {"'"};
            for (const char character : text) {
                if (character == '\'')
                    quoted.append("'\\''");
                else
                    quoted.push_back(character);
            }
            return quoted + "'";
        }

        std::vector<std::string> splitLines(const std::string& text)
        {
            std::vector<std::string> lines;
            std::string line;
            for (const char character : text) {
                if (character == '\n' || character == '\r') {
                    if (!line.empty())
                        lines.emplace_back(line);
                    line.clear();
                }
                else {
                    line.push_back(character);
                }
            }
            if (!line.empty())
                lines.emplace_back(line);
            return lines;
        }
    } // namespace

    std::string ownYtDlp() { return Dependencies::ownYtDlp(); }

    namespace
    {
        const std::string DOWNLOAD_URL {
            "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux"};

        // Downloads yt-dlp to the own copy (if the check at startup hasn't done it), false and
        // the reason if it couldn't be done.
        bool downloadYtDlp(const std::shared_ptr<Task>& task, std::string& error)
        {
            return Dependencies::downloadYtDlp(&task->cancel, error);
        }

        // yt-dlp finds Deno (for YouTube) in the bin folder of the dependencies.
        std::string withSearchPath(const std::string& command)
        {
            return "PATH=" + quote(Dependencies::searchPath()) + " " + command;
        }
    } // namespace

    std::string findYtDlp()
    {
        std::vector<std::string> candidates;
        // ESDE_YTDLP is the name of the earlier versions.
        for (const char* name : {"EMUSPLASH_YTDLP", "ESDE_YTDLP"}) {
            if (const char* custom {std::getenv(name)}; custom != nullptr && *custom != 0) {
                candidates.emplace_back(custom);
                break;
            }
        }
        candidates.emplace_back(ownYtDlp());
        const std::string home {Utils::FileSystem::getHomePath()};
        candidates.emplace_back(home + "/.local/bin/yt-dlp");
        if (const char* path {std::getenv("PATH")}; path != nullptr) {
            std::string directory;
            for (const char* c {path};; ++c) {
                if (*c == ':' || *c == 0) {
                    if (!directory.empty())
                        candidates.emplace_back(directory + "/yt-dlp");
                    directory.clear();
                    if (*c == 0)
                        break;
                }
                else {
                    directory.push_back(*c);
                }
            }
        }
        candidates.emplace_back("/usr/local/bin/yt-dlp");
        candidates.emplace_back("/usr/bin/yt-dlp");
        for (const std::string& candidate : candidates) {
            if (access(candidate.c_str(), X_OK) == 0 && !Utils::FileSystem::isDirectory(candidate))
                return candidate;
        }
        return "";
    }

    std::shared_ptr<Task> start(bool series, int tmdbId, const std::string& title, int year)
    {
        auto task {std::make_shared<Task>()};
        std::thread([task, series, tmdbId, title, year] {
            // The addresses given to yt-dlp, the best first.
            std::vector<std::pair<std::string, std::string>> sources;
            if (MediaMetadata::isEnabled() && tmdbId > 0) {
                std::vector<MediaMetadata::Video> videos;
                std::string error;
                if (!MediaMetadata::lookupVideos(series, tmdbId, videos, error)) {
                    LOG(LogWarning) << "Trailer: TMDb: " << error;
                }
                static const std::regex keyPattern {"^[A-Za-z0-9_-]{6,20}$"};
                for (const auto& video : videos) {
                    if (std::regex_match(video.key, keyPattern) && sources.size() < 3)
                        sources.emplace_back("https://www.youtube.com/watch?v=" + video.key,
                                             video.name);
                }
            }
            // Without TMDb, or if it has nothing: the first result of a YouTube search.
            if (sources.empty() && !title.empty()) {
                std::string query {title};
                if (year > 0)
                    query.append(" " + std::to_string(year));
                query.append(series ? " series official trailer" : " official trailer");
                sources.emplace_back("ytsearch1:" + query, "YouTube: " + query);
            }
            if (sources.empty()) {
                task->error = "No trailer was found for this title.";
                task->done = true;
                return;
            }

            // Without a yt-dlp, EmuSPLASH downloads its own copy (the standalone one, no Python).
            std::string ytdlp {findYtDlp()};
            if (ytdlp.empty()) {
                task->stage = Task::DOWNLOADING;
                std::string error;
                if (!downloadYtDlp(task, error)) {
                    if (!task->cancel) {
                        task->error = "yt-dlp is needed to play trailers. It could not be "
                                      "downloaded to " +
                                      ownYtDlp() + ":\n" + error +
                                      "\n\nIt can also be put there by hand:\n"
                                      "curl -L " +
                                      DOWNLOAD_URL + " -o " + ownYtDlp() + "\nchmod a+rx " +
                                      ownYtDlp();
                    }
                    task->done = true;
                    return;
                }
                ytdlp = ownYtDlp();
                task->stage = Task::LOOKING;
            }

            std::string failure;
            auto resolve = [&]() {
                for (const auto& [address, name] : sources) {
                    if (task->cancel)
                        break;
                    const std::string output {readCommand(withSearchPath(
                        "timeout 40 " + quote(ytdlp) +
                        " --no-playlist --no-cache-dir -g "
                        "-f \"bv*[height<=1080]+ba/b[height<=1080]/b\" " +
                        quote(address)))};
                    // The video and the audio on separate lines, or one line with both.
                    std::vector<std::string> addresses;
                    for (const std::string& line : splitLines(output)) {
                        if (line.rfind("http", 0) == 0)
                            addresses.emplace_back(line);
                        else if (line.rfind("WARNING", 0) != 0 || failure.empty())
                            failure = line;
                    }
                    if (!addresses.empty()) {
                        task->url = addresses[0];
                        task->audioUrl = addresses.size() > 1 ? addresses[1] : "";
                        task->name = name;
                    }
                    if (!task->url.empty())
                        break;
                    LOG(LogWarning) << "Trailer: yt-dlp failed for " << address << ":\n"
                                    << output;
                }
            };
            resolve();
            // YouTube changes often and an old yt-dlp stops working, the own copy is updated
            // and tried again.
            if (task->url.empty() && !task->cancel && ytdlp == ownYtDlp()) {
                task->stage = Task::UPDATING;
                LOG(LogInfo) << "Trailer: Updating yt-dlp: "
                             << readCommand(withSearchPath("timeout 120 " + quote(ytdlp) + " -U"));
                task->stage = Task::LOOKING;
                resolve();
            }
            if (task->url.empty() && !task->cancel) {
                task->error = "The trailer could not be opened.\n\n" +
                              (failure.empty() ? std::string("yt-dlp gave no address.") :
                                                 failure) +
                              (ytdlp == ownYtDlp() ?
                                   std::string() :
                                   "\n\nAn old yt-dlp is the usual reason, update it (" +
                                       ytdlp + " -U, or the package of the system). Or delete "
                                       "it, then EmuSPLASH downloads its own copy to " +
                                       ownYtDlp() + ".");
            }
            LOG(LogInfo) << "Trailer: " << (task->url.empty() ? "Nothing to play" :
                                                               "Playing \"" + task->name + "\"");
            task->done = true;
        }).detach();
        return task;
    }
} // namespace Trailer
