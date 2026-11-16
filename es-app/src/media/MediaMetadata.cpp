//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaMetadata.cpp
//
//  Titles, descriptions and posters of the videos and folders in the video sources, looked up
//  on TheMovieDB (TMDb) using the API key of the user. The movie or series is identified from
//  the file or folder name, and can be chosen manually if that fails. The information and the
//  images are stored in the preview directory and removed together with the previews.
//

#include "media/MediaMetadata.h"

#include "Log.h"
#include "Settings.h"
#include "media/HttpClient.h"
#include "media/MediaLibrary.h"
#include "media/MediaThumbnails.h"
#include "media/SubtitleDownloader.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <regex>

namespace
{
    const std::string API_URL {"https://api.themoviedb.org/3/"};
    const std::string IMAGE_URL {"https://image.tmdb.org/t/p/"};
    const std::string INFO_DIRECTORY {"metadata"};

    std::string hashText(const std::string& text)
    {
        uint64_t hash {14695981039346656037ULL};
        for (const unsigned char character : text) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
        char result[17];
        snprintf(result, sizeof(result), "%016llx", static_cast<unsigned long long>(hash));
        return result;
    }

    std::string text(const rapidjson::Value& object, const char* key)
    {
        if (!object.IsObject() || !object.HasMember(key) || !object[key].IsString())
            return "";
        return object[key].GetString();
    }

    double number(const rapidjson::Value& object, const char* key)
    {
        if (!object.IsObject() || !object.HasMember(key) || !object[key].IsNumber())
            return 0.0;
        return object[key].GetDouble();
    }

    int yearOf(const std::string& date)
    {
        return date.size() >= 4 ? std::atoi(date.substr(0, 4).c_str()) : 0;
    }

    // Performs a request to the API, false if it couldn't be completed (as opposed to having
    // no results).
    bool apiRequest(const std::string& path,
                    const std::map<std::string, std::string>& parameters,
                    rapidjson::Document& doc,
                    std::string& error)
    {
        const std::string key {Utils::String::trim(
            Settings::getInstance()->getString("MediaTmdbApiKey"))};
        std::string language {Settings::getInstance()->getString("MediaTmdbLanguage")};
        if (language.empty())
            language = "en-US";

        std::string url {API_URL + path + "?language=" + HttpClient::urlEncode(language)};
        for (const auto& [name, value] : parameters)
            url.append("&" + name + "=" + HttpClient::urlEncode(value));
        HttpClient::Options options;
        options.headers = {"Accept: application/json"};
        // The read access token is used as a bearer token, the older API key as a parameter.
        if (Utils::String::startsWith(key, "eyJ"))
            options.headers.emplace_back("Authorization: Bearer " + key);
        else
            url.append("&api_key=" + HttpClient::urlEncode(key));
        options.timeout = 20;

        for (int attempt {0}; attempt < 3; ++attempt) {
            const HttpClient::Response response {HttpClient::request(url, options)};
            if (response.code == 429) {
                std::this_thread::sleep_for(std::chrono::seconds(1 + attempt));
                continue;
            }
            if (!response.error.empty()) {
                error = response.error;
                return false;
            }
            if (response.code == 401) {
                error = "The TMDb API key was not accepted";
                return false;
            }
            if (response.code == 404) {
                doc.SetObject();
                return true;
            }
            doc.Parse(response.body.c_str());
            if (doc.HasParseError() || !doc.IsObject()) {
                error = "Invalid answer from TMDb (code " + std::to_string(response.code) + ")";
                return false;
            }
            return true;
        }
        error = "TMDb is busy";
        return false;
    }

    // Title and year taken from a folder or file name.
    SubtitleDownloader::Query parseName(const std::string& name)
    {
        return SubtitleDownloader::parseVideoPath("/" + name + ".mkv");
    }
} // namespace

MediaMetadata& MediaMetadata::getInstance()
{
    static MediaMetadata instance;
    return instance;
}

MediaMetadata::MediaMetadata()
    : mStop {false}
{
    mThread = std::thread(&MediaMetadata::workerLoop, this);
}

MediaMetadata::~MediaMetadata()
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

bool MediaMetadata::isEnabled()
{
    return !Utils::String::trim(Settings::getInstance()->getString("MediaTmdbApiKey")).empty();
}

std::string MediaMetadata::infoFile(const std::string& path)
{
    return INFO_DIRECTORY + "/" + hashText(path) + ".json";
}

bool MediaMetadata::get(const std::string& path, bool folder, bool urgent, Info& info)
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        const auto it {mCache.find(path)};
        if (it != mCache.cend()) {
            info = it->second;
            return true;
        }
    }
    if (loadInfo(path, info)) {
        std::unique_lock<std::mutex> lock {mMutex};
        mCache[path] = info;
        return true;
    }
    if (!isEnabled())
        return false;

    {
        std::unique_lock<std::mutex> lock {mMutex};
        if (mQueued.count(path) != 0) {
            if (!urgent)
                return false;
            for (auto it = mQueue.begin(); it != mQueue.end(); ++it) {
                if (it->path == path) {
                    Job job {*it};
                    mQueue.erase(it);
                    mQueue.push_front(job);
                    break;
                }
            }
            return false;
        }
        mQueued.insert(path);
        Job job;
        job.path = path;
        job.folder = folder;
        if (urgent)
            mQueue.push_front(job);
        else
            mQueue.push_back(job);
    }
    mCondition.notify_one();
    return false;
}

void MediaMetadata::queue(const std::vector<std::pair<std::string, bool>>& entries)
{
    if (!isEnabled())
        return;
    for (const auto& [path, folder] : entries) {
        Info info;
        get(path, folder, false, info);
    }
}

void MediaMetadata::refresh(const std::string& path, bool folder)
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        mCache.erase(path);
    }
    Utils::FileSystem::removeFile(MediaThumbnails::getDirectory() + "/" + infoFile(path));
    Info info;
    get(path, folder, true, info);
}

void MediaMetadata::identify(const std::string& path, bool folder, const Candidate& candidate)
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        mCache.erase(path);
        for (auto it = mQueue.begin(); it != mQueue.end(); ++it) {
            if (it->path == path) {
                mQueue.erase(it);
                break;
            }
        }
        Job job;
        job.path = path;
        job.folder = folder;
        job.forced = candidate;
        mQueued.insert(path);
        mQueue.push_front(job);
    }
    Utils::FileSystem::removeFile(MediaThumbnails::getDirectory() + "/" + infoFile(path));
    mCondition.notify_one();
}

void MediaMetadata::search(const std::string& query,
                           const std::function<void(std::vector<Candidate>, std::string)>& callback)
{
    std::thread([query, callback] {
        std::vector<Candidate> candidates;
        rapidjson::Document doc;
        std::string error;
        if (apiRequest("search/multi", {{"query", query}}, doc, error) &&
            doc.HasMember("results") && doc["results"].IsArray()) {
            for (const auto& result : doc["results"].GetArray()) {
                const std::string type {text(result, "media_type")};
                if (type != "movie" && type != "tv")
                    continue;
                Candidate candidate;
                candidate.type = type;
                candidate.id = static_cast<int>(number(result, "id"));
                candidate.title = type == "movie" ? text(result, "title") : text(result, "name");
                candidate.year = yearOf(type == "movie" ? text(result, "release_date") :
                                                          text(result, "first_air_date"));
                candidate.overview = text(result, "overview");
                candidates.emplace_back(candidate);
            }
        }
        callback(candidates, error);
    }).detach();
}

void MediaMetadata::workerLoop()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock {mMutex};
            mCondition.wait(lock, [this] { return mStop || !mQueue.empty(); });
            if (mStop)
                return;
            job = mQueue.front();
            mQueue.pop_front();
        }
        const Info info {lookup(job)};
        std::unique_lock<std::mutex> lock {mMutex};
        mQueued.erase(job.path);
        mCache[job.path] = info;
    }
}

bool MediaMetadata::loadInfo(const std::string& path, Info& info)
{
    std::ifstream file {MediaThumbnails::getDirectory() + "/" + infoFile(path)};
    if (!file)
        return false;
    const std::string content {std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>()};
    rapidjson::Document doc;
    doc.Parse(content.c_str());
    if (doc.HasParseError() || !doc.IsObject())
        return false;
    info.found = doc.HasMember("found") && doc["found"].IsBool() && doc["found"].GetBool();
    info.type = text(doc, "type");
    info.title = text(doc, "title");
    info.originalTitle = text(doc, "originalTitle");
    info.showTitle = text(doc, "showTitle");
    info.episodeTitle = text(doc, "episodeTitle");
    info.year = static_cast<int>(number(doc, "year"));
    info.season = static_cast<int>(number(doc, "season"));
    info.episode = static_cast<int>(number(doc, "episode"));
    info.plot = text(doc, "plot");
    info.rating = number(doc, "rating");
    info.runtime = static_cast<int>(number(doc, "runtime"));
    info.genres = text(doc, "genres");
    info.tmdbId = static_cast<int>(number(doc, "tmdbId"));
    const std::string directory {MediaThumbnails::getDirectory() + "/"};
    if (!text(doc, "poster").empty())
        info.poster = directory + text(doc, "poster");
    if (!text(doc, "fanart").empty())
        info.fanart = directory + text(doc, "fanart");
    return true;
}

void MediaMetadata::saveInfo(const std::string& path, const Info& info)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
    const std::string directory {MediaThumbnails::getDirectory() + "/"};
    auto relative = [&directory](const std::string& file) {
        return Utils::String::startsWith(file, directory) ? file.substr(directory.size()) : file;
    };
    writer.StartObject();
    writer.Key("source");
    writer.String(path.c_str());
    writer.Key("found");
    writer.Bool(info.found);
    const std::vector<std::pair<const char*, std::string>> texts {
        {"type", info.type},
        {"title", info.title},
        {"originalTitle", info.originalTitle},
        {"showTitle", info.showTitle},
        {"episodeTitle", info.episodeTitle},
        {"plot", info.plot},
        {"genres", info.genres},
        {"poster", relative(info.poster)},
        {"fanart", relative(info.fanart)}};
    for (const auto& [key, value] : texts) {
        writer.Key(key);
        writer.String(value.c_str());
    }
    writer.Key("year");
    writer.Int(info.year);
    writer.Key("season");
    writer.Int(info.season);
    writer.Key("episode");
    writer.Int(info.episode);
    writer.Key("rating");
    writer.Double(info.rating);
    writer.Key("runtime");
    writer.Int(info.runtime);
    writer.Key("tmdbId");
    writer.Int(info.tmdbId);
    writer.EndObject();

    Utils::FileSystem::createDirectory(directory + INFO_DIRECTORY);
    const std::string name {infoFile(path)};
    std::ofstream file {directory + name, std::ios::trunc};
    file << buffer.GetString();
    file.close();
    MediaThumbnails::getInstance().registerFile(name, path);
}

std::string MediaMetadata::downloadImage(const std::string& tmdbPath,
                                         const std::string& size,
                                         const std::string& sourcePath,
                                         const std::string& suffix)
{
    if (tmdbPath.empty())
        return "";
    const std::string name {INFO_DIRECTORY + "/" + hashText(sourcePath) + "-" + suffix + ".jpg"};
    const std::string file {MediaThumbnails::getDirectory() + "/" + name};
    HttpClient::Options options;
    options.timeout = 30;
    std::string error;
    if (!HttpClient::download(IMAGE_URL + size + tmdbPath, file, options, error)) {
        LOG(LogDebug) << "MediaMetadata: Couldn't download " << tmdbPath << ": " << error;
        return "";
    }
    MediaThumbnails::getInstance().registerFile(name, sourcePath);
    return file;
}

bool MediaMetadata::lookupOrigin(bool series,
                                 int tmdbId,
                                 std::string& country,
                                 std::string& language)
{
    country.clear();
    language.clear();
    if (!isEnabled() || tmdbId <= 0)
        return false;
    rapidjson::Document doc;
    std::string error;
    if (!apiRequest((series ? "tv/" : "movie/") + std::to_string(tmdbId), {}, doc, error)) {
        LOG(LogWarning) << "MediaMetadata: Couldn't look up the origin of "
                        << (series ? "series " : "movie ") << tmdbId << ": " << error;
        return false;
    }
    language = text(doc, "original_language");
    if (doc.HasMember("origin_country") && doc["origin_country"].IsArray()) {
        for (const auto& value : doc["origin_country"].GetArray()) {
            if (value.IsString() && value.GetStringLength() > 0) {
                country = value.GetString();
                break;
            }
        }
    }
    // Older answers for movies only have the production countries.
    if (country.empty() && doc.HasMember("production_countries") &&
        doc["production_countries"].IsArray()) {
        for (const auto& value : doc["production_countries"].GetArray()) {
            country = text(value, "iso_3166_1");
            if (!country.empty())
                break;
        }
    }
    LOG(LogInfo) << "MediaMetadata: The origin of " << (series ? "series " : "movie ")
                 << tmdbId << " is \"" << country << "\", language \"" << language << "\"";
    return true;
}

bool MediaMetadata::lookupContext(bool series, int tmdbId, std::string& context)
{
    context.clear();
    if (!isEnabled() || tmdbId <= 0)
        return false;
    rapidjson::Document doc;
    std::string error;
    if (!apiRequest((series ? "tv/" : "movie/") + std::to_string(tmdbId),
                    {{"append_to_response", "credits"}}, doc, error)) {
        LOG(LogWarning) << "MediaMetadata: Couldn't look up the story of "
                        << (series ? "series " : "movie ") << tmdbId << ": " << error;
        return false;
    }

    // Cuts the text at a word, not in the middle of a character.
    const auto shorten = [](std::string value, size_t limit) {
        if (value.size() <= limit)
            return value;
        value.resize(limit);
        while (!value.empty() && (static_cast<unsigned char>(value.back()) & 0xC0) == 0x80)
            value.pop_back();
        if (!value.empty() && (static_cast<unsigned char>(value.back()) & 0x80))
            value.pop_back();
        const size_t space {value.find_last_of(' ')};
        if (space != std::string::npos && space > limit / 2)
            value.resize(space);
        return value + "...";
    };

    const std::string title {series ? text(doc, "name") : text(doc, "title")};
    const int year {yearOf(series ? text(doc, "first_air_date") : text(doc, "release_date"))};
    context = std::string {series ? "TV series" : "Movie"} + ": " + title;
    if (year > 0)
        context.append(" (" + std::to_string(year) + ")");
    context.append(".");

    std::string genres;
    if (doc.HasMember("genres") && doc["genres"].IsArray()) {
        for (const auto& genre : doc["genres"].GetArray())
            genres.append((genres.empty() ? "" : ", ") + text(genre, "name"));
    }
    if (!genres.empty())
        context.append(" Genres: " + genres + ".");
    const std::string overview {text(doc, "overview")};
    if (!overview.empty())
        context.append(" Summary: " + shorten(overview, 700));

    // The characters, with the gender of the actor, which tells him from her in the sentences
    // that don't say it.
    std::string characters;
    int count {0};
    if (doc.HasMember("credits") && doc["credits"].IsObject() &&
        doc["credits"].HasMember("cast") && doc["credits"]["cast"].IsArray()) {
        for (const auto& member : doc["credits"]["cast"].GetArray()) {
            std::string character {text(member, "character")};
            const size_t paren {character.find(" (")};
            if (paren != std::string::npos)
                character.resize(paren);
            const size_t slash {character.find(" / ")};
            if (slash != std::string::npos)
                character.resize(slash);
            if (character.empty() || character == "Self" || character == "Himself" ||
                character == "Herself")
                continue;
            const int gender {static_cast<int>(number(member, "gender"))};
            characters.append((characters.empty() ? "" : ", ") + character +
                              (gender == 1 ? " (female)" : gender == 2 ? " (male)" : ""));
            if (++count >= 12)
                break;
        }
    }
    if (!characters.empty())
        context.append(" Main characters: " + characters + ".");
    LOG(LogInfo) << "MediaMetadata: The story of " << (series ? "series " : "movie ") << tmdbId
                 << " is " << context.size() << " characters";
    return true;
}

bool MediaMetadata::lookupVideos(bool series,
                                 int tmdbId,
                                 std::vector<Video>& videos,
                                 std::string& error)
{
    videos.clear();
    if (!isEnabled() || tmdbId <= 0) {
        error = "The TMDb API key is not set (Media settings).";
        return false;
    }
    // The videos in the language of the setting and in English, and the ones without a language.
    std::string language {Settings::getInstance()->getString("MediaTmdbLanguage")};
    language = language.substr(0, language.find('-'));
    std::string include {"en,null"};
    if (!language.empty() && language != "en")
        include = language + "," + include;
    rapidjson::Document doc;
    if (!apiRequest((series ? "tv/" : "movie/") + std::to_string(tmdbId) + "/videos",
                    {{"include_video_language", include}}, doc, error))
        return false;
    if (!doc.HasMember("results") || !doc["results"].IsArray())
        return true;

    struct Candidate {
        Video video;
        int rank;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry : doc["results"].GetArray()) {
        if (!entry.IsObject() || text(entry, "site") != "YouTube")
            continue;
        const std::string type {text(entry, "type")};
        if (type != "Trailer" && type != "Teaser")
            continue;
        Candidate candidate;
        candidate.video.name = text(entry, "name");
        candidate.video.key = text(entry, "key");
        if (candidate.video.key.empty())
            continue;
        const bool official {entry.HasMember("official") && entry["official"].IsBool() &&
                             entry["official"].GetBool()};
        const std::string videoLanguage {text(entry, "iso_639_1")};
        // Lower is better: trailers before teasers, official ones first, then the wanted language.
        candidate.rank = (type == "Trailer" ? 0 : 4) + (official ? 0 : 2) +
                         (videoLanguage == language ? 0 : 1);
        candidates.emplace_back(candidate);
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.rank < b.rank; });
    for (const auto& candidate : candidates)
        videos.emplace_back(candidate.video);
    LOG(LogInfo) << "MediaMetadata: " << videos.size() << " trailers for "
                 << (series ? "series " : "movie ") << tmdbId;
    return true;
}

MediaMetadata::Info MediaMetadata::lookup(const Job& job)
{
    Info info;
    std::string error;

    // What is looked for: a movie, a series (folder) or an episode (file).
    std::string title;
    int year {0};
    int season {0};
    int episode {0};
    bool series {false};
    if (job.folder) {
        std::string name {Utils::FileSystem::getFileName(job.path)};
        static const std::regex seasonFolder {R"(^(season|series|s)[ ._-]*(\d+)$)",
                                              std::regex::icase};
        std::smatch match;
        if (std::regex_match(name, match, seasonFolder)) {
            season = std::atoi(match[2].str().c_str());
            name = Utils::FileSystem::getFileName(Utils::FileSystem::getParent(job.path));
            series = true;
        }
        const SubtitleDownloader::Query query {parseName(name)};
        title = query.title;
        year = query.year;
        if (!series) {
            for (const auto& entry : MediaLibrary::listDirectory(job.path)) {
                if (entry.type == MediaLibrary::EntryType::FOLDER ||
                    SubtitleDownloader::isEpisodeFile(entry.path)) {
                    series = true;
                    break;
                }
            }
        }
    }
    else {
        const SubtitleDownloader::Query query {SubtitleDownloader::parseVideoPath(job.path)};
        title = query.title;
        year = query.year;
        season = query.season;
        episode = query.episode;
        series = season > 0 && episode > 0;
    }

    std::string type {series ? "tv" : "movie"};
    int id {0};
    if (job.forced.id != 0) {
        type = job.forced.type;
        id = job.forced.id;
    }
    else if (!title.empty()) {
        const std::string cacheKey {Utils::String::toLower(title) + "|" + std::to_string(year)};
        {
            std::unique_lock<std::mutex> lock {mMutex};
            const auto it {mShowIds.find(cacheKey)};
            if (series && it != mShowIds.cend())
                id = it->second;
        }
        for (int attempt {0}; id == 0 && attempt < 2; ++attempt) {
            std::map<std::string, std::string> parameters {{"query", title}};
            if (year > 0 && attempt == 0)
                parameters[series ? "first_air_date_year" : "year"] = std::to_string(year);
            else if (attempt == 1 && year == 0)
                break;
            rapidjson::Document doc;
            if (!apiRequest(series ? "search/tv" : "search/movie", parameters, doc, error)) {
                LOG(LogWarning) << "MediaMetadata: " << error;
                // Not stored, so it's tried again later.
                info.found = false;
                return info;
            }
            if (doc.HasMember("results") && doc["results"].IsArray() &&
                !doc["results"].Empty())
                id = static_cast<int>(number(doc["results"][0], "id"));
        }
        if (series && id != 0) {
            std::unique_lock<std::mutex> lock {mMutex};
            mShowIds[cacheKey] = id;
        }
    }

    if (id == 0) {
        LOG(LogDebug) << "MediaMetadata: Nothing found for \"" << job.path << "\"";
        saveInfo(job.path, info);
        return info;
    }

    rapidjson::Document details;
    if (!apiRequest(std::string(type == "movie" ? "movie/" : "tv/") + std::to_string(id), {},
                    details, error)) {
        LOG(LogWarning) << "MediaMetadata: " << error;
        return info;
    }
    info.found = true;
    info.tmdbId = id;
    std::string genres;
    if (details.HasMember("genres") && details["genres"].IsArray()) {
        for (const auto& genre : details["genres"].GetArray())
            genres.append((genres.empty() ? "" : " / ") + text(genre, "name"));
    }
    info.genres = genres;
    info.rating = number(details, "vote_average");
    if (type == "movie") {
        info.type = "movie";
        info.title = text(details, "title");
        info.originalTitle = text(details, "original_title");
        info.year = yearOf(text(details, "release_date"));
        info.runtime = static_cast<int>(number(details, "runtime"));
    }
    else {
        info.type = "tvshow";
        info.title = text(details, "name");
        info.showTitle = info.title;
        info.originalTitle = text(details, "original_name");
        info.year = yearOf(text(details, "first_air_date"));
        if (details.HasMember("episode_run_time") && details["episode_run_time"].IsArray() &&
            !details["episode_run_time"].Empty() && details["episode_run_time"][0].IsNumber())
            info.runtime = details["episode_run_time"][0].GetInt();
    }
    info.plot = text(details, "overview");
    info.poster = downloadImage(text(details, "poster_path"), "w500", job.path, "poster");
    info.fanart = downloadImage(text(details, "backdrop_path"), "w1280", job.path, "fanart");

    // An episode has its own title, description and picture.
    if (type == "tv" && !job.folder && season > 0 && episode > 0) {
        rapidjson::Document episodeDoc;
        if (apiRequest("tv/" + std::to_string(id) + "/season/" + std::to_string(season) +
                           "/episode/" + std::to_string(episode),
                       {}, episodeDoc, error) &&
            episodeDoc.HasMember("name")) {
            info.type = "episode";
            info.season = season;
            info.episode = episode;
            info.episodeTitle = text(episodeDoc, "name");
            const std::string plot {text(episodeDoc, "overview")};
            if (!plot.empty())
                info.plot = plot;
            const int aired {yearOf(text(episodeDoc, "air_date"))};
            if (aired > 0)
                info.year = aired;
            if (number(episodeDoc, "vote_average") > 0.0)
                info.rating = number(episodeDoc, "vote_average");
            if (number(episodeDoc, "runtime") > 0.0)
                info.runtime = static_cast<int>(number(episodeDoc, "runtime"));
            const std::string still {
                downloadImage(text(episodeDoc, "still_path"), "w780", job.path, "still")};
            if (!still.empty())
                info.fanart = still;
        }
    }
    else if (type == "tv" && season > 0) {
        info.season = season;
    }

    saveInfo(job.path, info);
    return info;
}
