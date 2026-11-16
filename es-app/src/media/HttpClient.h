//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  HttpClient.h
//
//  Blocking HTTP requests and downloads, to be used from background threads.
//

#ifndef ES_APP_MEDIA_HTTP_CLIENT_H
#define ES_APP_MEDIA_HTTP_CLIENT_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace HttpClient
{
    struct Options {
        std::vector<std::string> headers;
        std::string userAgent;
        bool followRedirects {true};
        long timeout {45};        // Seconds, 0 for no limit.
        long connectTimeout {15}; // Seconds.
        // Sent as the request body using POST if set.
        const std::string* postBody {nullptr};
        // The request is aborted when this becomes true.
        const std::atomic<bool>* cancel {nullptr};
        // Called with the downloaded and the total number of bytes (0 if unknown).
        std::function<void(long long, long long)> progress;
    };

    struct Response {
        long code {0};
        std::string body;
        // The target of a redirect that wasn't followed.
        std::string location;
        // The final address after the redirects that were followed.
        std::string effectiveUrl;
        std::string contentType;
        std::string error;
        bool ok() const { return error.empty() && code >= 200 && code < 300; }
    };

    Response request(const std::string& url, const Options& options = Options());
    // Downloads to a temporary file which replaces the file at path when complete.
    bool download(const std::string& url,
                  const std::string& path,
                  const Options& options,
                  std::string& error);
    std::string urlEncode(const std::string& text);
    std::string urlDecode(const std::string& text);
} // namespace HttpClient

#endif // ES_APP_MEDIA_HTTP_CLIENT_H
