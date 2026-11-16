//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaLibrary.cpp
//
//  Video sources (folders added by the user) and the play state of the video files,
//  i.e. resume positions and watched status. Stored in the settings directory as
//  media_sources.xml and media_playstate.xml.
//

#include "media/MediaLibrary.h"

#include "Log.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <ctime>
#include <set>

namespace
{
    // Playback that ends after this fraction of the duration marks the video as watched.
    constexpr double WATCHED_THRESHOLD {0.92};
    // Positions closer than this to the start are not stored for resuming.
    constexpr double MIN_RESUME_POSITION {30.0};

    const std::set<std::string>& videoExtensions()
    {
        static const std::set<std::string> extensions {
            ".3g2",  ".3gp", ".asf",  ".avi", ".divx", ".dv",   ".f4v",  ".flv",
            ".m2ts", ".m2v", ".m4v",  ".mk3d", ".mkv", ".mov",  ".mp4",  ".mpe",
            ".mpeg", ".mpg", ".mts",  ".mxf", ".nsv",  ".nuv",  ".ogm",  ".ogv",
            ".qt",   ".rm",  ".rmvb", ".tp",  ".trp",  ".ts",   ".vob",  ".webm",
            ".wmv",  ".wtv", ".xvid", ".y4m"};
        return extensions;
    }
} // namespace

MediaLibrary& MediaLibrary::getInstance()
{
    static MediaLibrary instance;
    return instance;
}

MediaLibrary::MediaLibrary()
{
    loadSources();
    loadPlayStates();
}

std::string MediaLibrary::getSettingsPath(const std::string& fileName)
{
    return Utils::FileSystem::getAppDataDirectory() + "/settings/" + fileName;
}

void MediaLibrary::loadSources()
{
    mVideoSources.clear();
    const std::string path {getSettingsPath("media_sources.xml")};
    if (!Utils::FileSystem::exists(path))
        return;

    pugi::xml_document doc;
    const pugi::xml_parse_result result {doc.load_file(path.c_str())};
    if (!result) {
        LOG(LogError) << "Couldn't parse media sources file \"" << path
                      << "\": " << result.description();
        return;
    }

    for (pugi::xml_node node : doc.child("sources").child("videos").children("source")) {
        Source source;
        source.name = node.attribute("name").as_string();
        source.path = node.attribute("path").as_string();
        if (!source.path.empty())
            mVideoSources.emplace_back(source);
    }

    mFavourites.clear();
    for (pugi::xml_node node : doc.child("sources").child("favourites").children("item")) {
        Favourite favourite;
        favourite.name = node.attribute("name").as_string();
        favourite.path = node.attribute("path").as_string();
        favourite.folder = node.attribute("folder").as_bool();
        if (!favourite.path.empty())
            mFavourites.emplace_back(favourite);
    }
}

void MediaLibrary::saveSources()
{
    const std::string path {getSettingsPath("media_sources.xml")};
    Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(path));

    pugi::xml_document doc;
    pugi::xml_node root {doc.append_child("sources")};
    pugi::xml_node videos {root.append_child("videos")};
    for (const auto& source : mVideoSources) {
        pugi::xml_node node {videos.append_child("source")};
        node.append_attribute("name") = source.name.c_str();
        node.append_attribute("path") = source.path.c_str();
    }
    pugi::xml_node favourites {root.append_child("favourites")};
    for (const auto& favourite : mFavourites) {
        pugi::xml_node node {favourites.append_child("item")};
        node.append_attribute("name") = favourite.name.c_str();
        node.append_attribute("path") = favourite.path.c_str();
        node.append_attribute("folder") = favourite.folder;
    }
    if (!doc.save_file(path.c_str())) {
        LOG(LogError) << "Couldn't write media sources file \"" << path << "\"";
    }
}

void MediaLibrary::loadPlayStates()
{
    mPlayStates.clear();
    const std::string path {getSettingsPath("media_playstate.xml")};
    if (!Utils::FileSystem::exists(path))
        return;

    pugi::xml_document doc;
    const pugi::xml_parse_result result {doc.load_file(path.c_str())};
    if (!result) {
        LOG(LogError) << "Couldn't parse media play state file \"" << path
                      << "\": " << result.description();
        return;
    }

    for (pugi::xml_node node : doc.child("playstate").children("file")) {
        const std::string filePath {node.attribute("path").as_string()};
        if (filePath.empty())
            continue;
        PlayState state;
        state.position = node.attribute("position").as_double();
        state.duration = node.attribute("duration").as_double();
        state.watched = node.attribute("watched").as_bool();
        state.playCount = node.attribute("playcount").as_int();
        state.lastPlayed = node.attribute("lastplayed").as_llong();
        for (pugi::xml_node bookmark : node.children("bookmark"))
            state.bookmarks.emplace_back(bookmark.attribute("position").as_double());
        std::sort(state.bookmarks.begin(), state.bookmarks.end());
        for (pugi::xml_node setting : node.children("setting")) {
            const std::string name {setting.attribute("name").as_string()};
            if (!name.empty())
                state.settings[name] = setting.attribute("value").as_string();
        }
        mPlayStates[filePath] = state;
    }
}

void MediaLibrary::savePlayStates()
{
    const std::string path {getSettingsPath("media_playstate.xml")};
    Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(path));

    pugi::xml_document doc;
    pugi::xml_node root {doc.append_child("playstate")};
    for (const auto& [filePath, state] : mPlayStates) {
        // Entries without any information are not worth keeping.
        if (state.position <= 0.0 && !state.watched && state.playCount == 0 &&
            state.bookmarks.empty() && state.settings.empty())
            continue;
        pugi::xml_node node {root.append_child("file")};
        node.append_attribute("path") = filePath.c_str();
        node.append_attribute("position") = std::round(state.position * 10.0) / 10.0;
        node.append_attribute("duration") = std::round(state.duration * 10.0) / 10.0;
        node.append_attribute("watched") = state.watched;
        node.append_attribute("playcount") = state.playCount;
        node.append_attribute("lastplayed") = state.lastPlayed;
        for (const double bookmark : state.bookmarks) {
            node.append_child("bookmark").append_attribute("position") =
                std::round(bookmark * 100.0) / 100.0;
        }
        for (const auto& [name, value] : state.settings) {
            pugi::xml_node setting {node.append_child("setting")};
            setting.append_attribute("name") = name.c_str();
            setting.append_attribute("value") = value.c_str();
        }
    }
    if (!doc.save_file(path.c_str())) {
        LOG(LogError) << "Couldn't write media play state file \"" << path << "\"";
    }
}

void MediaLibrary::addVideoSource(const std::string& name, const std::string& path)
{
    mVideoSources.emplace_back(Source {name, path});
    saveSources();
}

void MediaLibrary::updateVideoSource(size_t index, const std::string& name, const std::string& path)
{
    if (index >= mVideoSources.size())
        return;
    mVideoSources[index] = Source {name, path};
    saveSources();
}

void MediaLibrary::removeVideoSource(size_t index)
{
    if (index >= mVideoSources.size())
        return;
    mVideoSources.erase(mVideoSources.begin() + static_cast<long>(index));
    saveSources();
}

MediaLibrary::PlayState MediaLibrary::getPlayState(const std::string& path) const
{
    const auto it {mPlayStates.find(path)};
    if (it == mPlayStates.cend())
        return PlayState();
    return it->second;
}

void MediaLibrary::savePlaybackProgress(const std::string& path, double position, double duration)
{
    PlayState& state {mPlayStates[path]};
    if (duration > 0.0)
        state.duration = duration;
    state.lastPlayed = static_cast<long long>(std::time(nullptr));

    if (duration > 0.0 && position >= duration * WATCHED_THRESHOLD) {
        state.watched = true;
        state.position = 0.0;
        ++state.playCount;
    }
    else if (position >= MIN_RESUME_POSITION) {
        state.position = position;
    }
    else {
        state.position = 0.0;
    }
    savePlayStates();
}

void MediaLibrary::setWatched(const std::string& path, bool watched)
{
    PlayState& state {mPlayStates[path]};
    state.watched = watched;
    state.position = 0.0;
    if (watched && state.playCount == 0)
        state.playCount = 1;
    else if (!watched)
        state.playCount = 0;
    savePlayStates();
}

bool MediaLibrary::isFavourite(const std::string& path) const
{
    for (const auto& favourite : mFavourites) {
        if (favourite.path == path)
            return true;
    }
    return false;
}

void MediaLibrary::addFavourite(const std::string& name, const std::string& path, bool folder)
{
    if (isFavourite(path))
        return;
    mFavourites.emplace_back(Favourite {name, path, folder});
    std::stable_sort(mFavourites.begin(), mFavourites.end(),
                     [](const Favourite& a, const Favourite& b) {
                         if (a.folder != b.folder)
                             return a.folder;
                         return naturalLess(a.name, b.name);
                     });
    saveSources();
}

void MediaLibrary::removeFavourite(const std::string& path)
{
    const size_t count {mFavourites.size()};
    mFavourites.erase(std::remove_if(mFavourites.begin(), mFavourites.end(),
                                     [&path](const Favourite& favourite) {
                                         return favourite.path == path;
                                     }),
                      mFavourites.end());
    if (mFavourites.size() != count)
        saveSources();
}

void MediaLibrary::setFolderWatched(const std::string& folder, bool watched)
{
    int count {0};
    for (const auto& path : Utils::FileSystem::getDirContent(folder, true)) {
        const std::string name {Utils::FileSystem::getFileName(path)};
        if (name.empty() || name.front() == '.' || !isVideoFile(path) ||
            Utils::FileSystem::isDirectory(path))
            continue;
        PlayState& state {mPlayStates[path]};
        state.watched = watched;
        state.position = 0.0;
        if (watched && state.playCount == 0)
            state.playCount = 1;
        else if (!watched)
            state.playCount = 0;
        ++count;
    }
    if (count > 0)
        savePlayStates();
}

bool MediaLibrary::isFolderWatched(const std::string& folder) const
{
    bool hasVideos {false};
    for (const auto& path : Utils::FileSystem::getDirContent(folder)) {
        const std::string name {Utils::FileSystem::getFileName(path)};
        if (name.empty() || name.front() == '.' || !isVideoFile(path) ||
            Utils::FileSystem::isDirectory(path))
            continue;
        hasVideos = true;
        const auto it {mPlayStates.find(path)};
        if (it == mPlayStates.cend() || !it->second.watched || it->second.position > 0.0)
            return false;
    }
    return hasVideos;
}

void MediaLibrary::clearResumePosition(const std::string& path)
{
    const auto it {mPlayStates.find(path)};
    if (it == mPlayStates.end() || it->second.position == 0.0)
        return;
    it->second.position = 0.0;
    savePlayStates();
}

void MediaLibrary::addBookmark(const std::string& path, double position)
{
    PlayState& state {mPlayStates[path]};
    // Positions within a second of an existing bookmark are considered the same.
    for (const double bookmark : state.bookmarks) {
        if (std::abs(bookmark - position) < 1.0)
            return;
    }
    state.bookmarks.emplace_back(position);
    std::sort(state.bookmarks.begin(), state.bookmarks.end());
    savePlayStates();
}

void MediaLibrary::removeBookmark(const std::string& path, size_t index)
{
    const auto it {mPlayStates.find(path)};
    if (it == mPlayStates.end() || index >= it->second.bookmarks.size())
        return;
    it->second.bookmarks.erase(it->second.bookmarks.begin() + static_cast<long>(index));
    savePlayStates();
}

void MediaLibrary::clearBookmarks(const std::string& path)
{
    const auto it {mPlayStates.find(path)};
    if (it == mPlayStates.end() || it->second.bookmarks.empty())
        return;
    it->second.bookmarks.clear();
    savePlayStates();
}

void MediaLibrary::setFileSettings(const std::string& path,
                                   const std::map<std::string, std::string>& settings)
{
    mPlayStates[path].settings = settings;
    savePlayStates();
}

bool MediaLibrary::isVideoFile(const std::string& path)
{
    return videoExtensions().count(
               Utils::String::toLower(Utils::FileSystem::getExtension(path))) != 0;
}

std::vector<MediaLibrary::Entry> MediaLibrary::listDirectory(const std::string& path)
{
    std::vector<Entry> folders;
    std::vector<Entry> videos;

    for (const auto& entryPath : Utils::FileSystem::getDirContent(path)) {
        const std::string name {Utils::FileSystem::getFileName(entryPath)};
        if (name.empty() || name.front() == '.')
            continue;
        if (Utils::FileSystem::isDirectory(entryPath))
            folders.emplace_back(Entry {EntryType::FOLDER, name, entryPath});
        else if (isVideoFile(entryPath))
            videos.emplace_back(
                Entry {EntryType::VIDEO, Utils::FileSystem::getStem(entryPath), entryPath});
    }

    auto sortFunc = [](const Entry& a, const Entry& b) { return naturalLess(a.name, b.name); };
    std::sort(folders.begin(), folders.end(), sortFunc);
    std::sort(videos.begin(), videos.end(), sortFunc);

    folders.insert(folders.end(), videos.cbegin(), videos.cend());
    return folders;
}

bool MediaLibrary::isSubtitleFile(const std::string& path)
{
    static const std::set<std::string> extensions {".ass", ".idx", ".smi", ".srt", ".ssa",
                                                   ".sub", ".sup", ".vtt"};
    return extensions.count(Utils::String::toLower(Utils::FileSystem::getExtension(path))) != 0;
}

std::vector<MediaLibrary::Entry> MediaLibrary::listSubtitleDirectory(const std::string& path)
{
    std::vector<Entry> folders;
    std::vector<Entry> files;
    for (const auto& entryPath : Utils::FileSystem::getDirContent(path)) {
        const std::string name {Utils::FileSystem::getFileName(entryPath)};
        if (name.empty() || name.front() == '.')
            continue;
        if (Utils::FileSystem::isDirectory(entryPath))
            folders.emplace_back(Entry {EntryType::FOLDER, name, entryPath});
        else if (isSubtitleFile(entryPath))
            files.emplace_back(Entry {EntryType::SUBTITLE, name, entryPath});
    }
    auto sortFunc = [](const Entry& a, const Entry& b) { return naturalLess(a.name, b.name); };
    std::sort(folders.begin(), folders.end(), sortFunc);
    std::sort(files.begin(), files.end(), sortFunc);
    folders.insert(folders.end(), files.cbegin(), files.cend());
    return folders;
}

std::vector<MediaLibrary::Entry> MediaLibrary::listSubdirectories(const std::string& path)
{
    std::vector<Entry> folders;
    for (const auto& entryPath : Utils::FileSystem::getDirContent(path)) {
        const std::string name {Utils::FileSystem::getFileName(entryPath)};
        if (name.empty() || name.front() == '.')
            continue;
        if (Utils::FileSystem::isDirectory(entryPath))
            folders.emplace_back(Entry {EntryType::FOLDER, name, entryPath});
    }
    std::sort(folders.begin(), folders.end(),
              [](const Entry& a, const Entry& b) { return naturalLess(a.name, b.name); });
    return folders;
}

std::vector<MediaLibrary::Entry> MediaLibrary::getBrowseRoots()
{
    std::vector<Entry> roots;
    const std::string home {Utils::FileSystem::getSystemHomeDirectory()};
    roots.emplace_back(Entry {EntryType::FOLDER, "Home folder", home});

    const std::string videosDir {home + "/Videos"};
    if (Utils::FileSystem::isDirectory(videosDir))
        roots.emplace_back(Entry {EntryType::FOLDER, "Videos", videosDir});

    // Mounted drives.
    std::string user;
    if (const char* envUser {getenv("USER")}; envUser != nullptr)
        user = envUser;
    std::vector<std::string> mountDirs {"/media", "/mnt"};
    if (!user.empty()) {
        mountDirs.insert(mountDirs.begin(), "/run/media/" + user);
        mountDirs.insert(mountDirs.begin(), "/media/" + user);
    }
    std::set<std::string> added;
    for (const auto& mountDir : mountDirs) {
        if (!Utils::FileSystem::isDirectory(mountDir))
            continue;
        for (const auto& drive : listSubdirectories(mountDir)) {
            // Skip the per-user directory itself when listing /media.
            if (!user.empty() && drive.path == "/media/" + user)
                continue;
            if (added.insert(drive.path).second)
                roots.emplace_back(Entry {EntryType::FOLDER, drive.name, drive.path});
        }
    }

    roots.emplace_back(Entry {EntryType::FOLDER, "Root filesystem", "/"});
    return roots;
}

bool MediaLibrary::naturalLess(const std::string& a, const std::string& b)
{
    size_t i {0};
    size_t j {0};
    while (i < a.size() && j < b.size()) {
        const unsigned char ca {static_cast<unsigned char>(a[i])};
        const unsigned char cb {static_cast<unsigned char>(b[j])};
        if (std::isdigit(ca) && std::isdigit(cb)) {
            // Compare the complete numbers, ignoring leading zeros.
            size_t endA {i};
            size_t endB {j};
            while (endA < a.size() && std::isdigit(static_cast<unsigned char>(a[endA])))
                ++endA;
            while (endB < b.size() && std::isdigit(static_cast<unsigned char>(b[endB])))
                ++endB;
            size_t startA {i};
            size_t startB {j};
            while (startA + 1 < endA && a[startA] == '0')
                ++startA;
            while (startB + 1 < endB && b[startB] == '0')
                ++startB;
            const size_t lenA {endA - startA};
            const size_t lenB {endB - startB};
            if (lenA != lenB)
                return lenA < lenB;
            const int cmp {a.compare(startA, lenA, b, startB, lenB)};
            if (cmp != 0)
                return cmp < 0;
            i = endA;
            j = endB;
            continue;
        }
        const int la {std::tolower(ca)};
        const int lb {std::tolower(cb)};
        if (la != lb)
            return la < lb;
        ++i;
        ++j;
    }
    if ((a.size() - i) != (b.size() - j))
        return (a.size() - i) < (b.size() - j);
    return a < b;
}

std::string MediaLibrary::formatTime(double seconds, bool forceHours)
{
    if (seconds < 0.0 || std::isnan(seconds))
        seconds = 0.0;
    const long long total {static_cast<long long>(seconds)};
    const long long hours {total / 3600};
    const long long minutes {(total % 3600) / 60};
    const long long secs {total % 60};

    char buffer[32];
    if (hours > 0 || forceHours)
        snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", hours, minutes, secs);
    else
        snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, secs);
    return buffer;
}

std::string MediaLibrary::streamKey(const std::string& address)
{
    static const std::regex pattern {
        R"(/(movie/\d+|show/\d+/season/\d+/episode/\d+)(/|\?|$))"};
    std::smatch match;
    if (address.find("plugin.video.elementum") == std::string::npos ||
        !std::regex_search(address, match, pattern))
        return "";
    return "stream:" + match[1].str();
}
