//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  HttpClient.cpp
//
//  Blocking HTTP requests and downloads, to be used from background threads.
//

#include "media/HttpClient.h"

#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <curl/curl.h>

#include <cstdio>

namespace
{
    const std::string DEFAULT_USER_AGENT {"EmuSPLASH"};

    size_t writeToString(char* data, size_t size, size_t count, void* userData)
    {
        static_cast<std::string*>(userData)->append(data, size * count);
        return size * count;
    }

    size_t writeToFile(char* data, size_t size, size_t count, void* userData)
    {
        return fwrite(data, size, count, static_cast<FILE*>(userData)) * size;
    }

    struct ProgressData {
        const HttpClient::Options* options;
    };

    int onProgress(void* userData,
                   curl_off_t downloadTotal,
                   curl_off_t downloaded,
                   curl_off_t,
                   curl_off_t)
    {
        const HttpClient::Options* options {static_cast<ProgressData*>(userData)->options};
        if (options->cancel != nullptr && options->cancel->load())
            return 1;
        if (options->progress)
            options->progress(static_cast<long long>(downloaded),
                              static_cast<long long>(downloadTotal));
        return 0;
    }

    CURL* createHandle(const std::string& url,
                       const HttpClient::Options& options,
                       curl_slist*& headerList,
                       ProgressData& progress)
    {
        CURL* curl {curl_easy_init()};
        if (curl == nullptr)
            return nullptr;
        headerList = nullptr;
        for (const auto& header : options.headers)
            headerList = curl_slist_append(headerList, header.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (headerList != nullptr)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, options.userAgent.empty() ?
                                                      DEFAULT_USER_AGENT.c_str() :
                                                      options.userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, options.followRedirects ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, options.connectTimeout);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.timeout);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        // Requests to the local host never go through a proxy.
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "127.0.0.1,localhost");
        progress.options = &options;
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        if (options.postBody != nullptr) {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.postBody->c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(options.postBody->size()));
        }
        return curl;
    }
} // namespace

HttpClient::Response HttpClient::request(const std::string& url, const Options& options)
{
    Response response;
    curl_slist* headerList {nullptr};
    ProgressData progress;
    CURL* curl {createHandle(url, options, headerList, progress)};
    if (curl == nullptr) {
        response.error = "Couldn't create the request";
        return response;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    const CURLcode result {curl_easy_perform(curl)};
    if (result != CURLE_OK)
        response.error = curl_easy_strerror(result);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.code);
    char* text {nullptr};
    if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &text) == CURLE_OK && text != nullptr)
        response.location = text;
    text = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &text) == CURLE_OK && text != nullptr)
        response.effectiveUrl = text;
    text = nullptr;
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &text) == CURLE_OK && text != nullptr)
        response.contentType = text;

    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    return response;
}

bool HttpClient::download(const std::string& url,
                          const std::string& path,
                          const Options& options,
                          std::string& error)
{
    const std::string temporary {path + ".part"};
    FILE* file {fopen(temporary.c_str(), "wb")};
    if (file == nullptr) {
        error = "Couldn't write to " + temporary;
        return false;
    }

    curl_slist* headerList {nullptr};
    ProgressData progress;
    CURL* curl {createHandle(url, options, headerList, progress)};
    if (curl == nullptr) {
        fclose(file);
        Utils::FileSystem::removeFile(temporary);
        error = "Couldn't create the request";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    const CURLcode result {curl_easy_perform(curl)};
    long code {0};
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
    const bool written {fclose(file) == 0};

    if (result != CURLE_OK || !written) {
        error = result != CURLE_OK ? curl_easy_strerror(result) : "Couldn't write the file";
        if (code >= 400)
            error = "HTTP error " + std::to_string(code);
        Utils::FileSystem::removeFile(temporary);
        return false;
    }
    // Note that renameFile() returns true on failure.
    if (Utils::FileSystem::renameFile(temporary, path, true)) {
        error = "Couldn't replace " + path;
        Utils::FileSystem::removeFile(temporary);
        return false;
    }
    return true;
}

std::string HttpClient::urlEncode(const std::string& text)
{
    std::string result;
    char buffer[4];
    for (const unsigned char character : text) {
        if (isalnum(character) || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            result.push_back(static_cast<char>(character));
        }
        else {
            snprintf(buffer, sizeof(buffer), "%%%02X", character);
            result.append(buffer);
        }
    }
    return result;
}

std::string HttpClient::urlDecode(const std::string& text)
{
    std::string result;
    for (size_t i {0}; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && isxdigit(text[i + 1]) &&
            isxdigit(text[i + 2])) {
            result.push_back(static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        }
        else if (text[i] == '+') {
            result.push_back(' ');
        }
        else {
            result.push_back(text[i]);
        }
    }
    return result;
}
