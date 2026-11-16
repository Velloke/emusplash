//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  SubtitleDownloader.h
//
//  Searching and downloading subtitles from OpenSubtitles.com and Subdl.com. The network
//  requests are made in a background thread, the results are collected by polling.
//  Downloaded subtitles are stored next to the video file, or in the application data
//  directory if that location is not writable.
//

#ifndef ES_APP_MEDIA_SUBTITLE_DOWNLOADER_H
#define ES_APP_MEDIA_SUBTITLE_DOWNLOADER_H

#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class SubtitleDownloader
{
public:
    enum class Provider {
        OPENSUBTITLES,
        SUBDL,
        PODNAPISI,
        SUBSOURCE,
        GESTDOWN
    };

    // The services that can be chosen as the custom subtitle service, as setting value and
    // name. The fields are the ones the service uses: API key, user name and password.
    struct CustomService {
        std::string id;
        std::string name;
        bool needsKey;
        bool needsUser;
    };
    static const std::vector<CustomService>& getCustomServices();
    static std::string getProviderName(Provider provider);

    struct Language {
        std::string code; // ISO 639-1, e.g. "et", or with a region, e.g. "pt-br".
        std::string name;
    };

    struct Query {
        std::string videoPath;
        std::string title;   // Movie or series name.
        int year {0};        // 0 if unknown.
        int season {0};      // 0 if not an episode.
        int episode {0};
        std::vector<std::string> languages;
    };

    struct Result {
        Provider provider {Provider::OPENSUBTITLES};
        std::string id;          // File ID or download path, depending on the provider.
        std::string release;     // Release name shown in the list.
        std::string language;    // Language code as in Language::code.
        int downloads {0};
        bool hashMatch {false};  // Matches the video file exactly.
        bool hearingImpaired {false};
        bool episodeMatch {true};
    };

    struct SearchOutcome {
        std::vector<Result> results;
        std::vector<std::string> errors;
    };

    struct DownloadOutcome {
        std::string subtitlePath; // Empty on failure.
        std::string error;
    };

    // All languages that can be chosen for downloading.
    static const std::vector<Language>& getLanguages();
    static std::string getLanguageName(const std::string& code);
    // Converts the language of a video track (often three letters) to a language code.
    static std::string normalizeLanguage(const std::string& code);

    // Title, year, season and episode taken from the file name (and the folder name for
    // episodes without a series name), e.g. "The.Boys.S03E08.1080p" gives "The Boys" 3x08.
    static Query parseVideoPath(const std::string& path);
    static bool isEpisodeFile(const std::string& path);

    // True if at least one of the services has been configured.
    static bool isConfigured();
    static std::string getDownloadDirectory();

    // Starts the requests in the background, the results are read using the futures.
    static std::future<SearchOutcome> search(const Query& query);
    static std::future<DownloadOutcome> download(const Result& result, const std::string& videoPath);

    // Sorts the results so the most likely match comes first: exact file matches, then the
    // order of the languages in the query, then the number of downloads.
    static void sortResults(std::vector<Result>& results, const std::vector<std::string>& languages);

    // The OpenSubtitles hash of a video file, empty if it can't be calculated.
    static std::string computeHash(const std::string& path);

private:
    static SearchOutcome searchBlocking(const Query& query);
    static DownloadOutcome downloadBlocking(const Result& result, const std::string& videoPath);
    static void searchOpenSubtitles(const Query& query, SearchOutcome& outcome);
    static void searchSubdl(const Query& query, SearchOutcome& outcome);
    static void searchPodnapisi(const Query& query, SearchOutcome& outcome);
    static void searchSubSource(const Query& query, SearchOutcome& outcome);
    static void searchGestdown(const Query& query, SearchOutcome& outcome);
    static bool downloadArchiveOrFile(const std::string& url,
                                      const std::vector<std::string>& headers,
                                      const std::string& serviceName,
                                      std::string& content,
                                      std::string& fileName,
                                      std::string& error);
    static bool downloadOpenSubtitles(const Result& result,
                                      std::string& content,
                                      std::string& fileName,
                                      std::string& error);
    static bool downloadSubdl(const Result& result,
                              std::string& content,
                              std::string& fileName,
                              std::string& error);
    static std::string saveSubtitle(const std::string& videoPath,
                                    const std::string& language,
                                    const std::string& fileName,
                                    const std::string& content,
                                    std::string& error);
};

#endif // ES_APP_MEDIA_SUBTITLE_DOWNLOADER_H
