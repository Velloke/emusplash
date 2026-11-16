//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  Dependencies.cpp
//
//  The programs EmuSPLASH keeps in ~/EmuSPLASH/dependencies, see Dependencies.h.
//

#include "media/Dependencies.h"

#include "Log.h"
#include "Window.h"
#include "media/HttpClient.h"
#include "utils/FileSystemUtil.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/document.h"

#include <archive.h>
#include <archive_entry.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

namespace Dependencies
{
    namespace
    {
        // yt-dlp is updated when it was last checked this long ago.
        constexpr int UPDATE_DAYS {7};

        // Downloads of the same program don't run at the same time.
        std::mutex sYtDlpMutex;
        std::mutex sDenoMutex;
        std::mutex sPythonMutex;

        // x86_64 or aarch64, empty for the processors there are no downloads for.
        std::string machine()
        {
            struct utsname name {};
            std::string value;
            if (uname(&name) == 0)
                value = name.machine;
            if (value == "x86_64" || value == "amd64")
                return "x86_64";
            if (value == "aarch64" || value == "arm64")
                return "aarch64";
            return "";
        }

        bool isExecutable(const std::string& path)
        {
            return !path.empty() && access(path.c_str(), X_OK) == 0 &&
                   !Utils::FileSystem::isDirectory(path);
        }

        // The program in the PATH of EmuSPLASH, empty if it's not there.
        std::string findInPath(const std::string& name)
        {
            const char* path {std::getenv("PATH")};
            for (const auto& directory : Utils::String::delimitedStringToVector(
                     path == nullptr ? "/usr/local/bin:/usr/bin:/bin" : path, ":")) {
                if (directory.empty())
                    continue;
                const std::string candidate {directory + "/" + name};
                if (isExecutable(candidate))
                    return candidate;
            }
            return "";
        }

        std::string ownPython() { return directory() + "/python/bin/python3"; }
        std::string ownDeno() { return binDirectory() + "/deno"; }
        // The time of the last update check of yt-dlp is the time of this file.
        std::string ytDlpCheckedFile() { return binDirectory() + "/.yt-dlp-checked"; }

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

        void notify(const std::string& message)
        {
            if (Window* window {Window::getInstance()}; window != nullptr)
                window->queueInfoPopup(message, 4000);
        }

        void touch(const std::string& path)
        {
            std::ofstream file {path, std::ios::trunc};
            file << std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count()
                 << "\n";
        }

        // True if the file is missing or older than the days.
        bool olderThan(const std::string& path, int days)
        {
            struct stat info {};
            if (stat(path.c_str(), &info) != 0)
                return true;
            const auto age {std::chrono::system_clock::now() -
                            std::chrono::system_clock::from_time_t(info.st_mtime)};
            return age > std::chrono::hours(24 * days);
        }

        // Unpacks the archive (zip, tar.gz) into the folder, false and the reason if it
        // couldn't be done.
        bool extract(const std::string& file, const std::string& folder, std::string& error)
        {
            archive* reader {archive_read_new()};
            archive_read_support_filter_all(reader);
            archive_read_support_format_all(reader);
            archive* writer {archive_write_disk_new()};
            archive_write_disk_set_options(
                writer, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                            ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS);
            archive_write_disk_set_standard_lookup(writer);

            bool ok {true};
            if (archive_read_open_filename(reader, file.c_str(), 65536) != ARCHIVE_OK) {
                error = std::string("Couldn't open ") + file + ": " + archive_error_string(reader);
                ok = false;
            }
            archive_entry* entry {nullptr};
            while (ok) {
                const int status {archive_read_next_header(reader, &entry)};
                if (status == ARCHIVE_EOF)
                    break;
                if (status != ARCHIVE_OK && status != ARCHIVE_WARN) {
                    error = std::string("Couldn't read ") + file + ": " +
                            archive_error_string(reader);
                    ok = false;
                    break;
                }
                const std::string name {archive_entry_pathname(entry)};
                archive_entry_set_pathname(entry, (folder + "/" + name).c_str());
                if (const char* link {archive_entry_hardlink(entry)}; link != nullptr)
                    archive_entry_set_hardlink(entry, (folder + "/" + link).c_str());
                if (archive_write_header(writer, entry) != ARCHIVE_OK) {
                    error = "Couldn't write " + name + ": " + archive_error_string(writer);
                    ok = false;
                    break;
                }
                const void* buffer {nullptr};
                size_t size {0};
                la_int64_t offset {0};
                int result {ARCHIVE_OK};
                while ((result = archive_read_data_block(reader, &buffer, &size, &offset)) ==
                       ARCHIVE_OK) {
                    if (archive_write_data_block(writer, buffer, size, offset) != ARCHIVE_OK) {
                        error = "Couldn't write " + name + ": " + archive_error_string(writer);
                        ok = false;
                        break;
                    }
                }
                if (ok && result != ARCHIVE_EOF) {
                    error = "Couldn't unpack " + name + ": " + archive_error_string(reader);
                    ok = false;
                }
                archive_write_finish_entry(writer);
            }
            archive_read_free(reader);
            archive_write_close(writer);
            archive_write_free(writer);
            return ok;
        }

        bool downloadDeno(std::string& error)
        {
            std::unique_lock<std::mutex> lock {sDenoMutex};
            if (isExecutable(ownDeno()))
                return true;
            const std::string arch {machine()};
            if (arch.empty()) {
                error = "There is no Deno for this processor";
                return false;
            }
            const std::string url {"https://github.com/denoland/deno/releases/latest/download/"
                                   "deno-" +
                                   arch + "-unknown-linux-gnu.zip"};
            const std::string zip {binDirectory() + "/deno.zip"};
            HttpClient::Options options;
            options.timeout = 600;
            options.userAgent = "EmuSPLASH";
            LOG(LogInfo) << "Dependencies: Downloading Deno from " << url;
            notify(_("DOWNLOADING DENO FOR THE TRAILERS"));
            if (!HttpClient::download(url, zip, options, error))
                return false;
            const bool extracted {extract(zip, binDirectory(), error)};
            Utils::FileSystem::removeFile(zip);
            if (!extracted)
                return false;
            if (!isExecutable(ownDeno()) && chmod(ownDeno().c_str(), 0755) != 0) {
                error = "Couldn't make " + ownDeno() + " executable";
                return false;
            }
            return true;
        }

        // The install_only_stripped archive of the latest release of python-build-standalone,
        // Python 3.12 if it's there (then 3.13 and 3.11), otherwise the newest.
        bool findPythonArchive(std::string& url, std::string& error)
        {
            const std::string arch {machine()};
            if (arch.empty()) {
                error = "There is no standalone Python for this processor";
                return false;
            }
            HttpClient::Options options;
            options.userAgent = "EmuSPLASH";
            options.headers.emplace_back("Accept: application/vnd.github+json");
            const HttpClient::Response response {HttpClient::request(
                "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest",
                options)};
            if (!response.ok()) {
                error = "GitHub: " + (response.error.empty() ?
                                          "HTTP " + std::to_string(response.code) :
                                          response.error);
                return false;
            }
            rapidjson::Document document;
            document.Parse(response.body.c_str());
            if (document.HasParseError() || !document.IsObject() || !document.HasMember("assets") ||
                !document["assets"].IsArray()) {
                error = "GitHub gave no list of files";
                return false;
            }
            const std::regex pattern {"^cpython-3\\.([0-9]+)\\.[0-9]+\\+[0-9]+-" + arch +
                                      "-unknown-linux-gnu-install_only_stripped\\.tar\\.gz$"};
            int bestRank {-1};
            for (const auto& asset : document["assets"].GetArray()) {
                if (!asset.IsObject() || !asset.HasMember("name") || !asset["name"].IsString() ||
                    !asset.HasMember("browser_download_url") ||
                    !asset["browser_download_url"].IsString())
                    continue;
                std::smatch match;
                const std::string name {asset["name"].GetString()};
                if (!std::regex_match(name, match, pattern))
                    continue;
                const int minor {std::atoi(match[1].str().c_str())};
                const int rank {minor == 12 ? 1000 : minor == 13 ? 999 : minor == 11 ? 998 :
                                                                                     minor};
                if (rank > bestRank) {
                    bestRank = rank;
                    url = asset["browser_download_url"].GetString();
                }
            }
            if (bestRank < 0) {
                error = "The release has no Python for " + arch;
                return false;
            }
            return true;
        }

        bool downloadPython(std::string& error)
        {
            std::unique_lock<std::mutex> lock {sPythonMutex};
            if (isExecutable(ownPython()))
                return true;
            std::string url;
            if (!findPythonArchive(url, error))
                return false;
            const std::string archive {directory() + "/python.tar.gz"};
            const std::string unpacked {directory() + "/.python-new"};
            HttpClient::Options options;
            options.timeout = 600;
            options.userAgent = "EmuSPLASH";
            LOG(LogInfo) << "Dependencies: Downloading Python from " << url;
            notify(_("DOWNLOADING PYTHON 3"));
            if (!HttpClient::download(url, archive, options, error))
                return false;
            std::error_code errorCode;
            std::filesystem::remove_all(unpacked, errorCode);
            Utils::FileSystem::createDirectory(unpacked);
            const bool extracted {extract(archive, unpacked, error)};
            Utils::FileSystem::removeFile(archive);
            // The archive has the folder python.
            if (extracted) {
                std::filesystem::remove_all(directory() + "/python", errorCode);
                std::filesystem::rename(unpacked + "/python", directory() + "/python", errorCode);
                if (errorCode)
                    error = "Couldn't move Python to " + directory() + "/python: " +
                            errorCode.message();
            }
            std::filesystem::remove_all(unpacked, errorCode);
            if (!error.empty())
                return false;
            if (!isExecutable(ownPython())) {
                error = "The archive had no python/bin/python3";
                return false;
            }
            return true;
        }

        // Once a week the own yt-dlp updates itself, YouTube changes often.
        void updateYtDlp()
        {
            if (!isExecutable(ownYtDlp()) || !olderThan(ytDlpCheckedFile(), UPDATE_DAYS))
                return;
            std::unique_lock<std::mutex> lock {sYtDlpMutex};
            const std::string output {readCommand("PATH=" + quote(searchPath()) +
                                                  " timeout 180 " + quote(ownYtDlp()) + " -U")};
            touch(ytDlpCheckedFile());
            LOG(LogInfo) << "Dependencies: yt-dlp -U: "
                         << Utils::String::replace(Utils::String::trim(output), "\n", " | ");
        }
    } // namespace

    std::string directory() { return Utils::FileSystem::getAppDataDirectory() + "/dependencies"; }

    std::string binDirectory() { return directory() + "/bin"; }

    std::string ownYtDlp() { return binDirectory() + "/yt-dlp"; }

    std::string python()
    {
        const std::string system {findInPath("python3")};
        if (!system.empty())
            return system;
        return isExecutable(ownPython()) ? ownPython() : "";
    }

    std::string searchPath()
    {
        const char* path {std::getenv("PATH")};
        return binDirectory() + ":" +
               (path == nullptr ? std::string("/usr/local/bin:/usr/bin:/bin") : path);
    }

    bool downloadYtDlp(const std::atomic<bool>* cancel, std::string& error)
    {
        std::unique_lock<std::mutex> lock {sYtDlpMutex};
        if (isExecutable(ownYtDlp()))
            return true;
        const std::string arch {machine()};
        if (arch.empty()) {
            error = "There is no standalone yt-dlp for this processor";
            return false;
        }
        Utils::FileSystem::createDirectory(binDirectory());
        // The standalone programs of yt-dlp need no Python.
        const std::string url {"https://github.com/yt-dlp/yt-dlp/releases/latest/download/" +
                               std::string(arch == "x86_64" ? "yt-dlp_linux" :
                                                              "yt-dlp_linux_aarch64")};
        HttpClient::Options options;
        options.timeout = 600;
        options.cancel = cancel;
        options.userAgent = "EmuSPLASH";
        LOG(LogInfo) << "Dependencies: Downloading yt-dlp from " << url;
        notify(_("DOWNLOADING YT-DLP FOR THE TRAILERS"));
        if (!HttpClient::download(url, ownYtDlp(), options, error))
            return false;
        if (chmod(ownYtDlp().c_str(), 0755) != 0) {
            error = "Couldn't make " + ownYtDlp() + " executable";
            return false;
        }
        touch(ytDlpCheckedFile());
        return true;
    }

    void checkInBackground()
    {
        std::thread([] {
            Utils::FileSystem::createDirectory(binDirectory());

            // The yt-dlp of the earlier versions (~/EmuSPLASH/yt-dlp) needed Python, the
            // standalone one in the bin folder replaces it.
            const std::string earlier {Utils::FileSystem::getAppDataDirectory() + "/yt-dlp"};
            if (Utils::FileSystem::isDirectory(earlier)) {
                std::error_code errorCode;
                std::filesystem::remove_all(earlier, errorCode);
                LOG(LogInfo) << "Dependencies: Removed the earlier yt-dlp in " << earlier;
            }

            std::string error;
            if (!downloadYtDlp(nullptr, error))
                LOG(LogWarning) << "Dependencies: Couldn't download yt-dlp: " << error;
            else
                updateYtDlp();

            std::string deno {findInPath("deno")};
            if (deno.empty()) {
                error.clear();
                if (downloadDeno(error))
                    deno = ownDeno();
                else
                    LOG(LogWarning) << "Dependencies: Couldn't download Deno: " << error;
            }

            std::string python {findInPath("python3")};
            if (python.empty()) {
                error.clear();
                if (downloadPython(error))
                    python = ownPython();
                else
                    LOG(LogWarning) << "Dependencies: Couldn't download Python 3: " << error;
            }

            LOG(LogInfo) << "Dependencies: yt-dlp "
                         << (isExecutable(ownYtDlp()) ? ownYtDlp() : "missing") << ", Deno "
                         << (deno.empty() ? "missing" : deno) << ", Python 3 "
                         << (python.empty() ? "missing" : python);
        }).detach();
    }
} // namespace Dependencies
