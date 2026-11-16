//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  SubtitleDownloader.cpp
//
//  Searching and downloading subtitles from OpenSubtitles.com and Subdl.com. The network
//  requests are made in a background thread, the results are collected by polling.
//  Downloaded subtitles are stored next to the video file, or in the application data
//  directory if that location is not writable.
//

#include "media/SubtitleDownloader.h"

#include "Log.h"
#include "Settings.h"
#include "media/MediaLibrary.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>

namespace
{
    const std::string OPENSUBTITLES_API {"https://api.opensubtitles.com/api/v1"};
    const std::string SUBDL_API {"https://api.subdl.com/api/v1/subtitles"};
    const std::string SUBDL_DOWNLOAD {"https://dl.subdl.com"};
    const std::string USER_AGENT {"EmuSPLASH Media v1.0"};

    // The account settings, copied on the main thread before each request as the settings
    // can't be accessed safely from the background threads.
    struct Credentials {
        std::string openSubtitlesKey;
        std::string openSubtitlesUser;
        std::string openSubtitlesPassword;
        std::string subdlKey;
        std::string customService;
        std::string customKey;
        std::string customUser;
        std::string customPassword;
    };
    std::mutex sCredentialsMutex;
    Credentials sCredentials;

    void storeCredentials()
    {
        Settings* settings {Settings::getInstance()};
        std::unique_lock<std::mutex> lock {sCredentialsMutex};
        sCredentials.openSubtitlesKey =
            Utils::String::trim(settings->getString("MediaOpenSubtitlesApiKey"));
        sCredentials.openSubtitlesUser =
            Utils::String::trim(settings->getString("MediaOpenSubtitlesUser"));
        sCredentials.openSubtitlesPassword = settings->getString("MediaOpenSubtitlesPassword");
        sCredentials.subdlKey = Utils::String::trim(settings->getString("MediaSubdlApiKey"));
        sCredentials.customService = settings->getString("MediaCustomSubtitleService");
        sCredentials.customKey =
            Utils::String::trim(settings->getString("MediaCustomSubtitleKey"));
        sCredentials.customUser =
            Utils::String::trim(settings->getString("MediaCustomSubtitleUser"));
        sCredentials.customPassword = settings->getString("MediaCustomSubtitlePassword");
    }

    // ISO 639-2/T codes, used by some of the services.
    std::string threeLetterCode(const std::string& code)
    {
        static const std::map<std::string, std::string> codes {
            {"en", "eng"}, {"et", "est"}, {"ru", "rus"}, {"fi", "fin"}, {"lv", "lav"},
            {"lt", "lit"}, {"sv", "swe"}, {"no", "nor"}, {"da", "dan"}, {"de", "deu"},
            {"fr", "fra"}, {"es", "spa"}, {"it", "ita"}, {"pt", "por"}, {"pt-br", "por"},
            {"nl", "nld"}, {"pl", "pol"}, {"uk", "ukr"}, {"cs", "ces"}, {"sk", "slk"},
            {"hu", "hun"}, {"ro", "ron"}, {"bg", "bul"}, {"hr", "hrv"}, {"sr", "srp"},
            {"sl", "slv"}, {"bs", "bos"}, {"mk", "mkd"}, {"sq", "sqi"}, {"el", "ell"},
            {"tr", "tur"}, {"is", "isl"}, {"ga", "gle"}, {"ca", "cat"}, {"eu", "eus"},
            {"gl", "glg"}, {"ar", "ara"}, {"he", "heb"}, {"fa", "fas"}, {"hi", "hin"},
            {"bn", "ben"}, {"ur", "urd"}, {"ta", "tam"}, {"te", "tel"}, {"ml", "mal"},
            {"th", "tha"}, {"vi", "vie"}, {"id", "ind"}, {"ms", "msa"}, {"tl", "tgl"},
            {"zh-cn", "zho"}, {"zh-tw", "zho"}, {"ja", "jpn"}, {"ko", "kor"}, {"ka", "kat"},
            {"hy", "hye"}, {"az", "aze"}, {"kk", "kaz"}, {"be", "bel"}, {"af", "afr"},
            {"sw", "swa"}, {"eo", "epo"}};
        const auto it {codes.find(code)};
        return it == codes.cend() ? code : it->second;
    }

    Credentials credentials()
    {
        std::unique_lock<std::mutex> lock {sCredentialsMutex};
        return sCredentials;
    }

    // Login token for OpenSubtitles, kept for the session.
    std::mutex sTokenMutex;
    std::string sToken;
    std::string sTokenUser;
    std::string sBaseUrl;

    struct HttpResponse {
        long code {0};
        std::string body;
        std::string error;
    };

    size_t writeCallback(void* data, size_t size, size_t count, void* userData)
    {
        static_cast<std::string*>(userData)->append(static_cast<char*>(data), size * count);
        return size * count;
    }

    HttpResponse httpRequest(const std::string& url,
                             const std::vector<std::string>& headers,
                             const std::string* postBody = nullptr)
    {
        HttpResponse response;
        CURL* curl {curl_easy_init()};
        if (curl == nullptr) {
            response.error = "Couldn't initialize the network request";
            return response;
        }

        curl_slist* headerList {nullptr};
        for (const auto& header : headers)
            headerList = curl_slist_append(headerList, header.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        if (postBody != nullptr) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(postBody->size()));
        }

        const CURLcode result {curl_easy_perform(curl)};
        if (result != CURLE_OK)
            response.error = curl_easy_strerror(result);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.code);

        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return response;
    }

    std::string urlEncode(const std::string& text)
    {
        std::string encoded;
        char buffer[4];
        for (const unsigned char character : text) {
            if (std::isalnum(character) || character == '-' || character == '_' ||
                character == '.' || character == '~') {
                encoded.push_back(static_cast<char>(character));
            }
            else {
                snprintf(buffer, sizeof(buffer), "%%%02X", character);
                encoded.append(buffer);
            }
        }
        return encoded;
    }

    // The message of a JSON error response, or the HTTP status.
    std::string errorMessage(const HttpResponse& response)
    {
        if (!response.error.empty())
            return response.error;
        rapidjson::Document doc;
        doc.Parse(response.body.c_str());
        if (!doc.HasParseError() && doc.IsObject()) {
            for (const char* key : {"message", "error"}) {
                if (doc.HasMember(key) && doc[key].IsString())
                    return doc[key].GetString();
            }
            if (doc.HasMember("errors") && doc["errors"].IsArray() && !doc["errors"].Empty() &&
                doc["errors"][0].IsString())
                return doc["errors"][0].GetString();
        }
        return "HTTP status " + std::to_string(response.code);
    }

    std::string jsonString(const rapidjson::Value& object, const char* key)
    {
        if (object.IsObject() && object.HasMember(key) && object[key].IsString())
            return object[key].GetString();
        return "";
    }

    int jsonInt(const rapidjson::Value& object, const char* key)
    {
        if (!object.IsObject() || !object.HasMember(key))
            return 0;
        const rapidjson::Value& value {object[key]};
        if (value.IsInt())
            return value.GetInt();
        if (value.IsInt64())
            return static_cast<int>(value.GetInt64());
        if (value.IsDouble())
            return static_cast<int>(value.GetDouble());
        if (value.IsString())
            return std::atoi(value.GetString());
        return 0;
    }

    bool jsonBool(const rapidjson::Value& object, const char* key)
    {
        return object.IsObject() && object.HasMember(key) && object[key].IsBool() &&
               object[key].GetBool();
    }

    std::vector<std::string> openSubtitlesHeaders(const std::string& token)
    {
        std::vector<std::string> headers {
            "Accept: application/json", "Content-Type: application/json",
            "Api-Key: " + credentials().openSubtitlesKey};
        if (!token.empty())
            headers.emplace_back("Authorization: Bearer " + token);
        return headers;
    }

    // Logs in if a user name has been set, returns false and sets the error on failure.
    bool openSubtitlesLogin(std::string& token, std::string& baseUrl, std::string& error)
    {
        const Credentials account {credentials()};
        const std::string& user {account.openSubtitlesUser};
        const std::string& password {account.openSubtitlesPassword};

        std::unique_lock<std::mutex> lock {sTokenMutex};
        baseUrl = sBaseUrl.empty() ? OPENSUBTITLES_API : sBaseUrl;
        if (user.empty() || password.empty()) {
            token.clear();
            return true;
        }
        if (!sToken.empty() && sTokenUser == user) {
            token = sToken;
            return true;
        }

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
        writer.StartObject();
        writer.Key("username");
        writer.String(user.c_str());
        writer.Key("password");
        writer.String(password.c_str());
        writer.EndObject();
        const std::string body {buffer.GetString()};

        const HttpResponse response {
            httpRequest(OPENSUBTITLES_API + "/login", openSubtitlesHeaders(""), &body)};
        if (response.code != 200 || !response.error.empty()) {
            error = "OpenSubtitles login failed: " + errorMessage(response);
            return false;
        }
        rapidjson::Document doc;
        doc.Parse(response.body.c_str());
        if (doc.HasParseError() || jsonString(doc, "token").empty()) {
            error = "OpenSubtitles login failed: invalid response";
            return false;
        }
        sToken = jsonString(doc, "token");
        sTokenUser = user;
        // Some accounts are served from another address.
        const std::string base {jsonString(doc, "base_url")};
        if (!base.empty())
            sBaseUrl = (base.find("://") == std::string::npos ? "https://" + base : base) +
                       "/api/v1";
        token = sToken;
        baseUrl = sBaseUrl.empty() ? OPENSUBTITLES_API : sBaseUrl;
        return true;
    }

    // Subdl uses upper case codes, with a few exceptions.
    std::string subdlLanguageCode(const std::string& code)
    {
        if (code == "pt-br")
            return "BR_PT";
        if (code == "zh-tw")
            return "ZH_BG";
        if (code == "zh-cn")
            return "ZH";
        return Utils::String::toUpper(code);
    }

    std::string subdlLanguageFromName(const std::string& name)
    {
        const std::string lower {Utils::String::toLower(name)};
        if (lower == "brazillian portuguese" || lower == "brazilian portuguese")
            return "pt-br";
        for (const auto& language : SubtitleDownloader::getLanguages()) {
            if (Utils::String::toLower(language.name) == lower ||
                Utils::String::toLower(language.code) == lower)
                return language.code;
        }
        return lower;
    }

    bool isSubtitleName(const std::string& name)
    {
        return MediaLibrary::isSubtitleFile(name);
    }

    // Takes the best subtitle file out of an archive (zip, rar and others).
    bool extractSubtitle(const std::string& archiveData,
                         std::string& content,
                         std::string& fileName,
                         std::string& error)
    {
        archive* reader {archive_read_new()};
        archive_read_support_filter_all(reader);
        archive_read_support_format_all(reader);
        if (archive_read_open_memory(reader, archiveData.data(), archiveData.size()) !=
            ARCHIVE_OK) {
            error = std::string("Couldn't open the archive: ") + archive_error_string(reader);
            archive_read_free(reader);
            return false;
        }

        archive_entry* entry {nullptr};
        bool found {false};
        while (archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            const char* entryName {archive_entry_pathname(entry)};
            if (entryName == nullptr || !isSubtitleName(entryName)) {
                archive_read_data_skip(reader);
                continue;
            }
            const std::string name {entryName};
            // Prefer .srt files over other formats.
            const bool isSrt {Utils::String::toLower(Utils::FileSystem::getExtension(name)) ==
                              ".srt"};
            if (found && !isSrt) {
                archive_read_data_skip(reader);
                continue;
            }
            std::string data;
            char buffer[16384];
            la_ssize_t size {0};
            while ((size = archive_read_data(reader, buffer, sizeof(buffer))) > 0)
                data.append(buffer, static_cast<size_t>(size));
            content = data;
            fileName = Utils::FileSystem::getFileName(name);
            found = true;
            if (isSrt)
                break;
        }
        archive_read_free(reader);
        if (!found)
            error = "No subtitle file in the downloaded archive";
        return found;
    }

    std::string cleanTitle(std::string title)
    {
        // Torrent names of add-ons end with symbols, e.g. "... 👤 480 💾 1.69 GB ⚙️ Site".
        for (size_t i {0}; i < title.size(); ++i) {
            const unsigned char byte {static_cast<unsigned char>(title[i])};
            if (byte >= 0xF0 || title.compare(i, 3, "\xE2\x9A\x99") == 0) {
                title.erase(i);
                break;
            }
        }
        for (char& character : title) {
            if (character == '.' || character == '_')
                character = ' ';
        }
        // Remove anything in brackets.
        title = std::regex_replace(title, std::regex {R"(\[[^\]]*\])"}, " ");
        title = std::regex_replace(title, std::regex {R"(\s+-\s*$)"}, "");
        title = std::regex_replace(title, std::regex {R"(\s+)"}, " ");
        return Utils::String::trim(title);
    }
} // namespace

const std::vector<SubtitleDownloader::Language>& SubtitleDownloader::getLanguages()
{
    static const std::vector<Language> languages {
        {"en", "English"},     {"et", "Estonian"},   {"ru", "Russian"},
        {"fi", "Finnish"},     {"lv", "Latvian"},    {"lt", "Lithuanian"},
        {"sv", "Swedish"},     {"no", "Norwegian"},  {"da", "Danish"},
        {"de", "German"},      {"fr", "French"},     {"es", "Spanish"},
        {"it", "Italian"},     {"pt", "Portuguese"}, {"pt-br", "Portuguese (Brazil)"},
        {"nl", "Dutch"},       {"pl", "Polish"},     {"uk", "Ukrainian"},
        {"cs", "Czech"},       {"sk", "Slovak"},     {"hu", "Hungarian"},
        {"ro", "Romanian"},    {"bg", "Bulgarian"},  {"hr", "Croatian"},
        {"sr", "Serbian"},     {"sl", "Slovenian"},  {"bs", "Bosnian"},
        {"mk", "Macedonian"},  {"sq", "Albanian"},   {"el", "Greek"},
        {"tr", "Turkish"},     {"is", "Icelandic"},  {"ga", "Irish"},
        {"ca", "Catalan"},     {"eu", "Basque"},     {"gl", "Galician"},
        {"ar", "Arabic"},      {"he", "Hebrew"},     {"fa", "Persian"},
        {"hi", "Hindi"},       {"bn", "Bengali"},    {"ur", "Urdu"},
        {"ta", "Tamil"},       {"te", "Telugu"},     {"ml", "Malayalam"},
        {"th", "Thai"},        {"vi", "Vietnamese"}, {"id", "Indonesian"},
        {"ms", "Malay"},       {"tl", "Tagalog"},    {"zh-cn", "Chinese (simplified)"},
        {"zh-tw", "Chinese (traditional)"},          {"ja", "Japanese"},
        {"ko", "Korean"},      {"ka", "Georgian"},   {"hy", "Armenian"},
        {"az", "Azerbaijani"}, {"kk", "Kazakh"},     {"be", "Belarusian"},
        {"af", "Afrikaans"},   {"sw", "Swahili"},    {"eo", "Esperanto"}};
    return languages;
}

std::string SubtitleDownloader::getLanguageName(const std::string& code)
{
    for (const auto& language : getLanguages()) {
        if (language.code == code)
            return language.name;
    }
    return Utils::String::toUpper(code);
}

std::string SubtitleDownloader::normalizeLanguage(const std::string& code)
{
    static const std::map<std::string, std::string> threeLetter {
        {"eng", "en"}, {"est", "et"}, {"rus", "ru"}, {"fin", "fi"}, {"lav", "lv"},
        {"lit", "lt"}, {"swe", "sv"}, {"nor", "no"}, {"nob", "no"}, {"nno", "no"},
        {"dan", "da"}, {"ger", "de"}, {"deu", "de"}, {"fre", "fr"}, {"fra", "fr"},
        {"spa", "es"}, {"ita", "it"}, {"por", "pt"}, {"pob", "pt-br"}, {"dut", "nl"},
        {"nld", "nl"}, {"pol", "pl"}, {"ukr", "uk"}, {"cze", "cs"}, {"ces", "cs"},
        {"slo", "sk"}, {"slk", "sk"}, {"hun", "hu"}, {"rum", "ro"}, {"ron", "ro"},
        {"bul", "bg"}, {"hrv", "hr"}, {"srp", "sr"}, {"slv", "sl"}, {"gre", "el"},
        {"ell", "el"}, {"tur", "tr"}, {"ara", "ar"}, {"heb", "he"}, {"chi", "zh-cn"},
        {"zho", "zh-cn"}, {"jpn", "ja"}, {"kor", "ko"}, {"tha", "th"}, {"vie", "vi"},
        {"ind", "id"}, {"hin", "hi"}, {"per", "fa"}, {"fas", "fa"}, {"ice", "is"},
        {"isl", "is"}, {"cat", "ca"}, {"baq", "eu"}, {"eus", "eu"}, {"glg", "gl"}};
    std::string lower {Utils::String::toLower(code)};
    lower = Utils::String::replace(lower, "_", "-");
    const auto it {threeLetter.find(lower)};
    if (it != threeLetter.cend())
        return it->second;
    if (lower == "pt-pt")
        return "pt";
    if (lower == "zh" || lower == "zh-hans")
        return "zh-cn";
    if (lower == "zh-hant")
        return "zh-tw";
    return lower;
}

bool SubtitleDownloader::isEpisodeFile(const std::string& path)
{
    static const std::regex episode {R"((^|[^a-z0-9])s\d{1,2}[ ._-]?e\d{1,3}|(^|[^0-9])\d{1,2}x\d{2}([^0-9]|$))",
                                     std::regex::icase};
    return std::regex_search(Utils::FileSystem::getStem(path), episode);
}

SubtitleDownloader::Query SubtitleDownloader::parseVideoPath(const std::string& path)
{
    Query query;
    query.videoPath = path;
    const std::string stem {Utils::FileSystem::getStem(path)};
    std::smatch match;

    // Episodes, e.g. "The.Boys.S03E08.The.Instant..." or "Show - 3x08 - Title".
    static const std::regex episodeSxE {R"(^(.*?)[ ._\-\[(]*s(\d{1,2})[ ._-]?e(\d{1,3}))",
                                        std::regex::icase};
    static const std::regex episodeX {R"(^(.*?)[ ._\-\[(]*(\d{1,2})x(\d{2})(?:[^0-9]|$))",
                                      std::regex::icase};
    if (std::regex_search(stem, match, episodeSxE) || std::regex_search(stem, match, episodeX)) {
        // Release details before the name, e.g. "Show (2022) Season 1 S01 (1080p BluRay x265)
        // [QxR] Show (2022) - S01E01" of a torrent: the name after them.
        static const std::regex releaseGroup {
            R"(^.*[\[(][^\])]*(2160p|1080p|720p|576p|480p|bluray|blu-ray|web-?dl|webrip|hdtv|x264|x265|h264|h265|hevc|remux)[^\])]*[\])])",
            std::regex::icase};
        const std::string prefix {std::regex_replace(match[1].str(), releaseGroup, "")};
        query.title = cleanTitle(prefix);
        query.season = std::stoi(match[2].str());
        query.episode = std::stoi(match[3].str());
        // A year after the series name, e.g. "Doctor Who 2005".
        static const std::regex trailingYear {R"(^(.*?)\s*\(?((?:19|20)\d{2})\)?$)"};
        std::smatch yearMatch;
        if (std::regex_match(query.title, yearMatch, trailingYear) &&
            !yearMatch[1].str().empty()) {
            query.title = yearMatch[1].str();
            query.year = std::stoi(yearMatch[2].str());
        }
        // Files named only "S01E02" use the folder name, skipping season folders.
        if (query.title.empty()) {
            std::string folder {Utils::FileSystem::getParent(path)};
            static const std::regex seasonFolder {R"(^(season|series|s)[ ._-]*\d+$)",
                                                  std::regex::icase};
            if (std::regex_match(Utils::FileSystem::getFileName(folder), seasonFolder))
                folder = Utils::FileSystem::getParent(folder);
            query.title = cleanTitle(Utils::FileSystem::getFileName(folder));
        }
        return query;
    }

    // Movies, e.g. "All.Hallows.Eve.2013.1080p.BluRay" or "The Invisible Man (2000)".
    std::string name {stem};
    static const std::regex movieYear {R"(^(.*?)[ ._\-\[(]+((?:19|20)\d{2})(?:[ ._\-\])]|$))"};
    if (std::regex_search(name, match, movieYear) && !match[1].str().empty()) {
        query.title = cleanTitle(match[1].str());
        query.year = std::stoi(match[2].str());
        return query;
    }
    // No year, so cut at the first typical release tag.
    static const std::regex releaseTag {
        R"([ ._\-\[(]+(2160p|1080p|720p|576p|480p|4k|uhd|bluray|blu-ray|bdrip|brrip|web-?dl|webrip|hdtv|dvdrip|x264|x265|h264|h265|hevc|xvid|remux|proper|repack)([ ._\-\])]|$).*$)",
        std::regex::icase};
    name = std::regex_replace(name, releaseTag, "");
    query.title = cleanTitle(name);
    return query;
}

const std::vector<SubtitleDownloader::CustomService>& SubtitleDownloader::getCustomServices()
{
    static const std::vector<CustomService> services {
        {"podnapisi", "Podnapisi", false, false},
        {"subsource", "SubSource", true, false},
        {"gestdown", "Gestdown (series only)", false, false}};
    return services;
}

std::string SubtitleDownloader::getProviderName(Provider provider)
{
    switch (provider) {
        case Provider::OPENSUBTITLES:
            return "OpenSubtitles";
        case Provider::SUBDL:
            return "Subdl";
        case Provider::PODNAPISI:
            return "Podnapisi";
        case Provider::SUBSOURCE:
            return "SubSource";
        case Provider::GESTDOWN:
            return "Gestdown";
    }
    return "";
}

bool SubtitleDownloader::isConfigured()
{
    Settings* settings {Settings::getInstance()};
    if (!settings->getString("MediaOpenSubtitlesApiKey").empty() ||
        !settings->getString("MediaSubdlApiKey").empty())
        return true;
    const std::string custom {settings->getString("MediaCustomSubtitleService")};
    for (const auto& service : getCustomServices()) {
        if (service.id == custom)
            return !service.needsKey || !settings->getString("MediaCustomSubtitleKey").empty();
    }
    return false;
}

std::string SubtitleDownloader::getDownloadDirectory()
{
    return Utils::FileSystem::getAppDataDirectory() + "/Media-subtitles";
}

std::string SubtitleDownloader::computeHash(const std::string& path)
{
    constexpr std::streamoff chunkSize {65536};
    std::ifstream file {path, std::ios::binary};
    if (!file)
        return "";
    file.seekg(0, std::ios::end);
    const std::streamoff size {file.tellg()};
    if (size < chunkSize * 2)
        return "";

    uint64_t hash {static_cast<uint64_t>(size)};
    auto addChunk = [&](std::streamoff offset) {
        std::vector<unsigned char> buffer(static_cast<size_t>(chunkSize));
        file.seekg(offset, std::ios::beg);
        file.read(reinterpret_cast<char*>(buffer.data()), chunkSize);
        if (file.gcount() != chunkSize)
            return false;
        for (size_t i {0}; i < buffer.size(); i += 8) {
            uint64_t value {0};
            for (int byte {7}; byte >= 0; --byte)
                value = (value << 8) | buffer[i + static_cast<size_t>(byte)];
            hash += value;
        }
        return true;
    };
    if (!addChunk(0) || !addChunk(size - chunkSize))
        return "";

    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

void SubtitleDownloader::sortResults(std::vector<Result>& results,
                                     const std::vector<std::string>& languages)
{
    auto languageRank = [&languages](const std::string& code) {
        const auto it {std::find(languages.cbegin(), languages.cend(), code)};
        return it == languages.cend() ? static_cast<int>(languages.size()) :
                                        static_cast<int>(it - languages.cbegin());
    };
    std::stable_sort(results.begin(), results.end(), [&](const Result& a, const Result& b) {
        if (a.episodeMatch != b.episodeMatch)
            return a.episodeMatch;
        const int rankA {languageRank(a.language)};
        const int rankB {languageRank(b.language)};
        if (rankA != rankB)
            return rankA < rankB;
        if (a.hashMatch != b.hashMatch)
            return a.hashMatch;
        // Hearing impaired subtitles last as they contain sound descriptions.
        if (a.hearingImpaired != b.hearingImpaired)
            return !a.hearingImpaired;
        return a.downloads > b.downloads;
    });
}

std::future<SubtitleDownloader::SearchOutcome> SubtitleDownloader::search(const Query& query)
{
    storeCredentials();
    return std::async(std::launch::async, [query] { return searchBlocking(query); });
}

std::future<SubtitleDownloader::DownloadOutcome> SubtitleDownloader::download(
    const Result& result, const std::string& videoPath)
{
    storeCredentials();
    return std::async(std::launch::async,
                      [result, videoPath] { return downloadBlocking(result, videoPath); });
}

SubtitleDownloader::SearchOutcome SubtitleDownloader::searchBlocking(const Query& query)
{
    SearchOutcome outcome;
    const Credentials account {credentials()};
    if (!account.openSubtitlesKey.empty())
        searchOpenSubtitles(query, outcome);
    if (!account.subdlKey.empty())
        searchSubdl(query, outcome);
    if (account.customService == "podnapisi")
        searchPodnapisi(query, outcome);
    else if (account.customService == "subsource")
        searchSubSource(query, outcome);
    else if (account.customService == "gestdown")
        searchGestdown(query, outcome);
    sortResults(outcome.results, query.languages);
    LOG(LogInfo) << "SubtitleDownloader: Found " << outcome.results.size()
                 << " subtitles for \"" << query.title << "\""
                 << (query.season > 0 ? " season " + std::to_string(query.season) + " episode " +
                                            std::to_string(query.episode) :
                                        "");
    for (const auto& error : outcome.errors)
        LOG(LogWarning) << "SubtitleDownloader: " << error;
    return outcome;
}

void SubtitleDownloader::searchOpenSubtitles(const Query& query, SearchOutcome& outcome)
{
    std::string token;
    std::string baseUrl;
    std::string error;
    // The search itself works without logging in, the token is only needed for downloads.
    openSubtitlesLogin(token, baseUrl, error);

    // The parameters are sorted alphabetically with lower case values, otherwise the
    // service answers with a redirect.
    std::map<std::string, std::string> parameters;
    if (query.episode > 0)
        parameters["episode_number"] = std::to_string(query.episode);
    std::string languages;
    for (const auto& language : query.languages)
        languages.append((languages.empty() ? "" : ",") + language);
    if (!languages.empty())
        parameters["languages"] = languages;
    const std::string hash {computeHash(query.videoPath)};
    if (!hash.empty())
        parameters["moviehash"] = hash;
    parameters["query"] = Utils::String::toLower(query.title);
    if (query.season > 0) {
        parameters["season_number"] = std::to_string(query.season);
        parameters["type"] = "episode";
    }
    else {
        parameters["type"] = "movie";
        if (query.year > 0)
            parameters["year"] = std::to_string(query.year);
    }

    std::string url {baseUrl + "/subtitles?"};
    bool first {true};
    for (const auto& [name, value] : parameters) {
        url.append((first ? "" : "&") + name + "=" + urlEncode(value));
        first = false;
    }

    const HttpResponse response {httpRequest(url, openSubtitlesHeaders(token))};
    if (response.code != 200 || !response.error.empty()) {
        outcome.errors.emplace_back("OpenSubtitles search failed: " + errorMessage(response));
        return;
    }
    rapidjson::Document doc;
    doc.Parse(response.body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("data") ||
        !doc["data"].IsArray()) {
        outcome.errors.emplace_back("OpenSubtitles search failed: invalid response");
        return;
    }

    for (const auto& item : doc["data"].GetArray()) {
        if (!item.IsObject() || !item.HasMember("attributes"))
            continue;
        const rapidjson::Value& attributes {item["attributes"]};
        if (!attributes.HasMember("files") || !attributes["files"].IsArray() ||
            attributes["files"].Empty())
            continue;
        const rapidjson::Value& file {attributes["files"][0]};
        Result result;
        result.provider = Provider::OPENSUBTITLES;
        result.id = std::to_string(jsonInt(file, "file_id"));
        if (result.id == "0")
            continue;
        result.release = jsonString(attributes, "release");
        if (result.release.empty())
            result.release = jsonString(file, "file_name");
        result.language = normalizeLanguage(jsonString(attributes, "language"));
        result.downloads = jsonInt(attributes, "download_count");
        result.hashMatch = jsonBool(attributes, "moviehash_match");
        result.hearingImpaired = jsonBool(attributes, "hearing_impaired");
        if (query.season > 0 && attributes.HasMember("feature_details")) {
            const rapidjson::Value& details {attributes["feature_details"]};
            const int season {jsonInt(details, "season_number")};
            const int episode {jsonInt(details, "episode_number")};
            if ((season != 0 && season != query.season) ||
                (episode != 0 && episode != query.episode))
                result.episodeMatch = false;
        }
        outcome.results.emplace_back(result);
    }
}

void SubtitleDownloader::searchSubdl(const Query& query, SearchOutcome& outcome)
{
    std::string languages;
    for (const auto& language : query.languages)
        languages.append((languages.empty() ? "" : ",") + subdlLanguageCode(language));

    std::string url {SUBDL_API +
                     "?api_key=" + urlEncode(credentials().subdlKey) +
                     "&film_name=" + urlEncode(query.title) + "&subs_per_page=30"};
    if (!languages.empty())
        url.append("&languages=" + languages);
    if (query.season > 0) {
        url.append("&type=tv&season_number=" + std::to_string(query.season) +
                   "&episode_number=" + std::to_string(query.episode));
    }
    else {
        url.append("&type=movie");
        if (query.year > 0)
            url.append("&year=" + std::to_string(query.year));
    }

    const HttpResponse response {httpRequest(url, {"Accept: application/json"})};
    if (response.code != 200 || !response.error.empty()) {
        outcome.errors.emplace_back("Subdl search failed: " + errorMessage(response));
        return;
    }
    rapidjson::Document doc;
    doc.Parse(response.body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        outcome.errors.emplace_back("Subdl search failed: invalid response");
        return;
    }
    if (doc.HasMember("status") && doc["status"].IsBool() && !doc["status"].GetBool()) {
        const std::string message {jsonString(doc, "error")};
        // Nothing found is reported as an error by this service.
        if (!message.empty() && Utils::String::toLower(message).find("not found") ==
                                    std::string::npos)
            outcome.errors.emplace_back("Subdl search failed: " + message);
        return;
    }
    if (!doc.HasMember("subtitles") || !doc["subtitles"].IsArray())
        return;

    const std::string videoName {Utils::String::toLower(
        Utils::FileSystem::getStem(query.videoPath))};
    for (const auto& item : doc["subtitles"].GetArray()) {
        Result result;
        result.provider = Provider::SUBDL;
        result.id = jsonString(item, "url");
        if (result.id.empty())
            continue;
        result.release = jsonString(item, "release_name");
        if (result.release.empty())
            result.release = jsonString(item, "name");
        result.language = subdlLanguageFromName(jsonString(item, "language").empty() ?
                                                    jsonString(item, "lang") :
                                                    jsonString(item, "language"));
        result.hearingImpaired = jsonBool(item, "hi");
        // There is no hash, but an identical release name is just as good.
        result.hashMatch = !result.release.empty() &&
                           Utils::String::toLower(result.release) == videoName;
        if (query.season > 0) {
            const int season {jsonInt(item, "season")};
            const int episode {jsonInt(item, "episode")};
            if ((season != 0 && season != query.season) ||
                (episode != 0 && episode != query.episode))
                result.episodeMatch = false;
        }
        outcome.results.emplace_back(result);
    }
}

void SubtitleDownloader::searchPodnapisi(const Query& query, SearchOutcome& outcome)
{
    // One language per request.
    for (const auto& language : query.languages) {
        std::string code {language};
        if (code == "pt-br")
            code = "pt-BR";
        else if (code == "zh-cn" || code == "zh-tw")
            code = "zh";
        std::string url {"https://www.podnapisi.net/subtitles/search/advanced?keywords=" +
                         urlEncode(query.title) + "&language=" + urlEncode(code)};
        if (query.season > 0) {
            url.append("&seasons=" + std::to_string(query.season) + "&episodes=" +
                       std::to_string(query.episode) +
                       "&movie_type=tv-series&movie_type=mini-series");
        }
        else {
            url.append("&movie_type=movie");
            if (query.year > 0)
                url.append("&year=" + std::to_string(query.year));
        }

        const HttpResponse response {httpRequest(url, {"Accept: application/json"})};
        if (response.code != 200 || !response.error.empty()) {
            outcome.errors.emplace_back("Podnapisi search failed: " + errorMessage(response));
            return;
        }
        rapidjson::Document doc;
        doc.Parse(response.body.c_str());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("data") ||
            !doc["data"].IsArray()) {
            outcome.errors.emplace_back("Podnapisi search failed: invalid response");
            return;
        }
        const std::string videoName {
            Utils::String::toLower(Utils::FileSystem::getStem(query.videoPath))};
        for (const auto& item : doc["data"].GetArray()) {
            Result result;
            result.provider = Provider::PODNAPISI;
            result.id = jsonString(item, "id");
            if (result.id.empty() && jsonInt(item, "id") != 0)
                result.id = std::to_string(jsonInt(item, "id"));
            if (result.id.empty())
                continue;
            std::vector<std::string> releases;
            for (const char* key : {"releases", "custom_releases"}) {
                if (item.HasMember(key) && item[key].IsArray()) {
                    for (const auto& release : item[key].GetArray()) {
                        if (release.IsString())
                            releases.emplace_back(release.GetString());
                    }
                }
            }
            if (!releases.empty())
                result.release = releases.front();
            if (item.HasMember("movie") && item["movie"].IsObject()) {
                const rapidjson::Value& movie {item["movie"]};
                if (result.release.empty())
                    result.release = jsonString(movie, "title");
                if (query.season > 0 && movie.HasMember("episode_info") &&
                    movie["episode_info"].IsObject()) {
                    const int season {jsonInt(movie["episode_info"], "season")};
                    const int episode {jsonInt(movie["episode_info"], "episode")};
                    if ((season != 0 && season != query.season) ||
                        (episode != 0 && episode != query.episode))
                        result.episodeMatch = false;
                }
            }
            result.language = normalizeLanguage(jsonString(item, "language"));
            if (item.HasMember("flags") && item["flags"].IsArray()) {
                for (const auto& flag : item["flags"].GetArray()) {
                    if (flag.IsString() && std::string(flag.GetString()) == "hearing_impaired")
                        result.hearingImpaired = true;
                }
            }
            if (item.HasMember("stats") && item["stats"].IsObject())
                result.downloads = jsonInt(item["stats"], "downloads");
            for (const auto& release : releases) {
                if (Utils::String::toLower(release) == videoName)
                    result.hashMatch = true;
            }
            outcome.results.emplace_back(result);
        }
    }
}

void SubtitleDownloader::searchSubSource(const Query& query, SearchOutcome& outcome)
{
    const std::string key {credentials().customKey};
    if (key.empty()) {
        outcome.errors.emplace_back("SubSource needs an API key");
        return;
    }
    const std::vector<std::string> headers {"Accept: application/json", "X-API-Key: " + key};
    const std::string base {"https://api.subsource.net/api/v1"};

    // First the movie or series, then its subtitles.
    std::string url {base + "/movies/search?searchType=text&q=" +
                     urlEncode(Utils::String::toLower(query.title))};
    if (query.season > 0)
        url.append("&season=" + std::to_string(query.season));
    HttpResponse response {httpRequest(url, headers)};
    if (response.code != 200 || !response.error.empty()) {
        outcome.errors.emplace_back("SubSource search failed: " + errorMessage(response));
        return;
    }
    rapidjson::Document doc;
    doc.Parse(response.body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("data") ||
        !doc["data"].IsArray()) {
        outcome.errors.emplace_back("SubSource search failed: invalid response");
        return;
    }
    std::string movieId;
    const std::string wanted {Utils::String::toLower(query.title)};
    for (const auto& item : doc["data"].GetArray()) {
        const std::string title {Utils::String::toLower(jsonString(item, "title"))};
        const int year {jsonInt(item, "releaseYear")};
        if (query.year > 0 && year > 0 && year != query.year)
            continue;
        if (title.find(wanted) != std::string::npos || wanted.find(title) != std::string::npos) {
            movieId = std::to_string(jsonInt(item, "movieId"));
            if (movieId == "0")
                movieId = jsonString(item, "movieId");
            break;
        }
    }
    if (movieId.empty() || movieId == "0")
        return;

    const std::string videoName {Utils::String::toLower(Utils::FileSystem::getStem(query.videoPath))};
    for (const auto& language : query.languages) {
        std::string name {Utils::String::toLower(getLanguageName(language))};
        if (language == "pt-br")
            name = "brazillian portuguese";
        std::string subtitlesUrl {base + "/subtitles?language=" + urlEncode(name) +
                                  "&limit=100&movieId=" + urlEncode(movieId)};
        if (query.season > 0) {
            subtitlesUrl.append("&seasonNumber=" + std::to_string(query.season) +
                                "&episodeNumber=" + std::to_string(query.episode));
        }
        response = httpRequest(subtitlesUrl, headers);
        if (response.code != 200 || !response.error.empty()) {
            outcome.errors.emplace_back("SubSource search failed: " + errorMessage(response));
            return;
        }
        rapidjson::Document subtitles;
        subtitles.Parse(response.body.c_str());
        if (subtitles.HasParseError() || !subtitles.IsObject() || !subtitles.HasMember("data") ||
            !subtitles["data"].IsArray())
            continue;
        for (const auto& item : subtitles["data"].GetArray()) {
            Result result;
            result.provider = Provider::SUBSOURCE;
            result.id = std::to_string(jsonInt(item, "subtitleId"));
            if (result.id == "0")
                result.id = jsonString(item, "subtitleId");
            if (result.id.empty() || result.id == "0")
                continue;
            if (item.HasMember("releaseInfo") && item["releaseInfo"].IsArray() &&
                !item["releaseInfo"].Empty() && item["releaseInfo"][0].IsString())
                result.release = item["releaseInfo"][0].GetString();
            else
                result.release = jsonString(item, "releaseInfo");
            result.language = language;
            result.hearingImpaired = jsonBool(item, "hearingImpaired");
            result.downloads = jsonInt(item, "downloads");
            result.hashMatch = !result.release.empty() &&
                               Utils::String::toLower(result.release) == videoName;
            if (query.season > 0 && !result.release.empty()) {
                const Query parsed {parseVideoPath("/" + result.release + ".mkv")};
                if (parsed.season > 0 &&
                    (parsed.season != query.season ||
                     (parsed.episode > 0 && parsed.episode != query.episode)))
                    result.episodeMatch = false;
            }
            outcome.results.emplace_back(result);
        }
    }
}

void SubtitleDownloader::searchGestdown(const Query& query, SearchOutcome& outcome)
{
    // Only series are available.
    if (query.season <= 0)
        return;
    const std::string base {"https://api.gestdown.info"};
    const std::vector<std::string> headers {"Accept: application/json"};

    HttpResponse response {httpRequest(base + "/shows/search/" + urlEncode(query.title), headers)};
    if (response.code == 404)
        return;
    if (response.code != 200 || !response.error.empty()) {
        outcome.errors.emplace_back("Gestdown search failed: " + errorMessage(response));
        return;
    }
    rapidjson::Document doc;
    doc.Parse(response.body.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("shows") ||
        !doc["shows"].IsArray() || doc["shows"].Empty())
        return;
    std::string showId;
    const std::string wanted {Utils::String::toLower(query.title)};
    for (const auto& show : doc["shows"].GetArray()) {
        if (Utils::String::toLower(jsonString(show, "name")) == wanted) {
            showId = jsonString(show, "id");
            break;
        }
    }
    if (showId.empty())
        showId = jsonString(doc["shows"][0], "id");
    if (showId.empty())
        return;

    for (const auto& language : query.languages) {
        response = httpRequest(base + "/subtitles/get/" + showId + "/" +
                                   std::to_string(query.season) + "/" +
                                   std::to_string(query.episode) + "/" + threeLetterCode(language),
                               headers);
        if (response.code != 200 || !response.error.empty())
            continue;
        rapidjson::Document subtitles;
        subtitles.Parse(response.body.c_str());
        if (subtitles.HasParseError() || !subtitles.IsObject() ||
            !subtitles.HasMember("matchingSubtitles") || !subtitles["matchingSubtitles"].IsArray())
            continue;
        for (const auto& item : subtitles["matchingSubtitles"].GetArray()) {
            Result result;
            result.provider = Provider::GESTDOWN;
            result.id = jsonString(item, "downloadUri");
            if (result.id.empty())
                continue;
            result.release = query.title + " S" + std::to_string(query.season) + "E" +
                             std::to_string(query.episode) + " " + jsonString(item, "version");
            result.language = language;
            result.hearingImpaired = jsonBool(item, "hearingImpaired");
            result.downloads = jsonInt(item, "downloadCount");
            outcome.results.emplace_back(result);
        }
    }
}

bool SubtitleDownloader::downloadArchiveOrFile(const std::string& url,
                                               const std::vector<std::string>& headers,
                                               const std::string& serviceName,
                                               std::string& content,
                                               std::string& fileName,
                                               std::string& error)
{
    const HttpResponse response {httpRequest(url, headers)};
    if (response.code != 200 || !response.error.empty() || response.body.empty()) {
        error = serviceName + " download failed: " + errorMessage(response);
        return false;
    }
    const bool archive {response.body.size() > 4 &&
                        (response.body.compare(0, 2, "PK") == 0 ||
                         response.body.compare(0, 4, "Rar!") == 0 ||
                         response.body.compare(0, 2, "7z") == 0)};
    if (!archive) {
        // An error page instead of a subtitle.
        const std::string start {Utils::String::toLower(response.body.substr(0, 200))};
        if (start.find("<html") != std::string::npos || start.find("<!doctype") != std::string::npos) {
            error = serviceName + " download failed: the service returned a web page";
            return false;
        }
        content = response.body;
        fileName = "subtitle.srt";
        return true;
    }
    if (!extractSubtitle(response.body, content, fileName, error)) {
        error = serviceName + " download failed: " + error;
        return false;
    }
    return true;
}

SubtitleDownloader::DownloadOutcome SubtitleDownloader::downloadBlocking(
    const Result& result, const std::string& videoPath)
{
    DownloadOutcome outcome;
    std::string content;
    std::string fileName;
    bool success {false};
    switch (result.provider) {
        case Provider::OPENSUBTITLES: {
            success = downloadOpenSubtitles(result, content, fileName, outcome.error);
            break;
        }
        case Provider::SUBDL: {
            success = downloadSubdl(result, content, fileName, outcome.error);
            break;
        }
        case Provider::PODNAPISI: {
            success = downloadArchiveOrFile(
                "https://www.podnapisi.net/subtitles/" + result.id + "/download?container=zip",
                {"Accept: application/zip"}, "Podnapisi", content, fileName, outcome.error);
            break;
        }
        case Provider::SUBSOURCE: {
            success = downloadArchiveOrFile(
                "https://api.subsource.net/api/v1/subtitles/" + result.id + "/download",
                {"X-API-Key: " + credentials().customKey}, "SubSource", content, fileName,
                outcome.error);
            break;
        }
        case Provider::GESTDOWN: {
            success = downloadArchiveOrFile("https://api.gestdown.info" + result.id, {},
                                            "Gestdown", content, fileName, outcome.error);
            break;
        }
    }
    if (!success) {
        LOG(LogWarning) << "SubtitleDownloader: " << outcome.error;
        return outcome;
    }
    outcome.subtitlePath = saveSubtitle(videoPath, result.language, fileName, content,
                                        outcome.error);
    if (!outcome.subtitlePath.empty())
        LOG(LogInfo) << "SubtitleDownloader: Saved \"" << outcome.subtitlePath << "\"";
    else
        LOG(LogWarning) << "SubtitleDownloader: " << outcome.error;
    return outcome;
}

bool SubtitleDownloader::downloadOpenSubtitles(const Result& result,
                                               std::string& content,
                                               std::string& fileName,
                                               std::string& error)
{
    std::string token;
    std::string baseUrl;
    if (!openSubtitlesLogin(token, baseUrl, error))
        return false;

    const std::string body {"{\"file_id\":" + result.id + "}"};
    HttpResponse response {httpRequest(baseUrl + "/download", openSubtitlesHeaders(token), &body)};
    if (response.code == 401 && !token.empty()) {
        // The login has expired, so log in again once.
        {
            std::unique_lock<std::mutex> lock {sTokenMutex};
            sToken.clear();
        }
        if (!openSubtitlesLogin(token, baseUrl, error))
            return false;
        response = httpRequest(baseUrl + "/download", openSubtitlesHeaders(token), &body);
    }
    if (response.code != 200 || !response.error.empty()) {
        error = "OpenSubtitles download failed: " + errorMessage(response);
        return false;
    }
    rapidjson::Document doc;
    doc.Parse(response.body.c_str());
    const std::string link {doc.HasParseError() ? "" : jsonString(doc, "link")};
    if (link.empty()) {
        error = "OpenSubtitles download failed: " +
                (doc.HasParseError() ? std::string("invalid response") : errorMessage(response));
        return false;
    }
    fileName = jsonString(doc, "file_name");
    if (doc.HasMember("remaining") && doc["remaining"].IsInt()) {
        LOG(LogInfo) << "SubtitleDownloader: OpenSubtitles downloads remaining today: "
                     << doc["remaining"].GetInt();
    }

    const HttpResponse file {httpRequest(link, {})};
    if (file.code != 200 || !file.error.empty() || file.body.empty()) {
        error = "OpenSubtitles download failed: " + errorMessage(file);
        return false;
    }
    content = file.body;
    return true;
}

bool SubtitleDownloader::downloadSubdl(const Result& result,
                                       std::string& content,
                                       std::string& fileName,
                                       std::string& error)
{
    const std::string url {Utils::String::startsWith(result.id, "http") ?
                               result.id :
                               SUBDL_DOWNLOAD + (result.id.front() == '/' ? "" : "/") +
                                   result.id};
    const HttpResponse response {httpRequest(url, {})};
    if (response.code != 200 || !response.error.empty() || response.body.empty()) {
        error = "Subdl download failed: " + errorMessage(response);
        return false;
    }
    // Normally a zip archive, but plain subtitle files are accepted as well.
    if (response.body.size() > 4 && response.body.compare(0, 2, "PK") != 0 &&
        response.body.compare(0, 4, "Rar!") != 0) {
        content = response.body;
        fileName = Utils::FileSystem::getFileName(url);
        return true;
    }
    if (!extractSubtitle(response.body, content, fileName, error)) {
        error = "Subdl download failed: " + error;
        return false;
    }
    return true;
}

std::string SubtitleDownloader::saveSubtitle(const std::string& videoPath,
                                             const std::string& language,
                                             const std::string& fileName,
                                             const std::string& content,
                                             std::string& error)
{
    std::string extension {Utils::String::toLower(Utils::FileSystem::getExtension(fileName))};
    if (!MediaLibrary::isSubtitleFile("x" + extension))
        extension = ".srt";
    // Named after the video so the player finds it automatically the next time.
    const std::string name {Utils::FileSystem::getStem(videoPath) + "." +
                            (language.empty() ? "und" : language) + extension};

    const std::vector<std::string> directories {Utils::FileSystem::getParent(videoPath),
                                                getDownloadDirectory()};
    for (const auto& directory : directories) {
        Utils::FileSystem::createDirectory(directory);
        std::string path {directory + "/" + name};
        // Keep earlier downloads, e.g. a different release of the same language.
        for (int i {2}; Utils::FileSystem::exists(path) && i < 100; ++i) {
            path = directory + "/" + Utils::FileSystem::getStem(videoPath) + "." +
                   (language.empty() ? "und" : language) + "." + std::to_string(i) + extension;
        }
        std::ofstream file {path, std::ios::binary};
        if (!file)
            continue;
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        file.close();
        if (file)
            return path;
        Utils::FileSystem::removeFile(path);
    }
    error = "Couldn't save the subtitle file";
    return "";
}
