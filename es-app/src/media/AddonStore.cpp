//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonStore.cpp
//
//  The installed add-ons, kept in the Elementum directory in the application data directory.
//  Add-ons are installed from their releases on GitHub (or from zip files). The streaming
//  add-on also needs its program, which is downloaded separately.
//

#include "media/AddonStore.h"

#include "Log.h"
#include "media/HttpClient.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/document.h"

#include <archive.h>
#include <archive_entry.h>
#include <pugixml.hpp>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
    const std::string GITHUB_API {"https://api.github.com/repos/"};
    const std::string BINARIES_REPOSITORY {"elgatito/elementum-binaries"};

    std::string englishText(const pugi::xml_node& parent, const char* name)
    {
        std::string fallback;
        for (pugi::xml_node node {parent.child(name)}; node; node = node.next_sibling(name)) {
            const std::string lang {node.attribute("lang").as_string()};
            if (lang.empty() || lang == "en" || lang == "en_GB" || lang == "en_US")
                return node.text().as_string();
            if (fallback.empty())
                fallback = node.text().as_string();
        }
        return fallback;
    }

    std::string platformName()
    {
        struct utsname name {};
        std::string machine {"x86_64"};
        if (uname(&name) == 0)
            machine = name.machine;
        if (machine == "x86_64" || machine == "amd64")
            return "linux_x64";
        if (machine == "aarch64" || machine == "arm64")
            return "linux_arm64";
        if (Utils::String::startsWith(machine, "armv7"))
            return "linux_armv7";
        if (Utils::String::startsWith(machine, "armv6"))
            return "linux_armv6";
        return "linux_x86";
    }

    std::string uniqueName(const std::string& prefix)
    {
        const auto now {std::chrono::steady_clock::now().time_since_epoch().count()};
        return prefix + std::to_string(now);
    }

    // Extracts an archive into an empty directory.
    bool extractArchive(const std::string& archivePath,
                        const std::string& destination,
                        std::string& error)
    {
        archive* reader {archive_read_new()};
        archive_read_support_format_all(reader);
        archive_read_support_filter_all(reader);
        archive* writer {archive_write_disk_new()};
        // The entries are written below the destination, which is an absolute path, so the
        // names of the entries are checked here instead.
        archive_write_disk_set_options(writer, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                   ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                                   ARCHIVE_EXTRACT_SECURE_SYMLINKS);
        archive_write_disk_set_standard_lookup(writer);

        bool success {true};
        if (archive_read_open_filename(reader, archivePath.c_str(), 65536) != ARCHIVE_OK) {
            error = "Couldn't open the archive";
            success = false;
        }
        archive_entry* entry {nullptr};
        while (success && archive_read_next_header(reader, &entry) == ARCHIVE_OK) {
            const std::string name {archive_entry_pathname(entry)};
            if (name.empty() || name[0] == '/' || name == ".." ||
                Utils::String::startsWith(name, "../") || name.find("/../") != std::string::npos ||
                Utils::String::endsWith(name, "/..")) {
                error = "Unsafe path in the archive: " + name;
                success = false;
                break;
            }
            archive_entry_set_pathname(entry, (destination + "/" + name).c_str());
            if (archive_write_header(writer, entry) != ARCHIVE_OK) {
                error = archive_error_string(writer) ? archive_error_string(writer) : "";
                success = false;
                break;
            }
            const void* block {nullptr};
            size_t size {0};
            la_int64_t offset {0};
            int status {ARCHIVE_OK};
            while ((status = archive_read_data_block(reader, &block, &size, &offset)) ==
                   ARCHIVE_OK) {
                if (archive_write_data_block(writer, block, size, offset) != ARCHIVE_OK) {
                    status = ARCHIVE_FATAL;
                    break;
                }
            }
            if (status != ARCHIVE_EOF) {
                error = "Couldn't extract " + name;
                success = false;
                break;
            }
            archive_write_finish_entry(writer);
        }
        archive_read_free(reader);
        archive_write_free(writer);
        return success;
    }

    // The directory with the add-on description, at the top or one level down.
    std::string findAddonDirectory(const std::string& root)
    {
        if (Utils::FileSystem::isRegularFile(root + "/addon.xml"))
            return root;
        std::error_code errorCode;
        for (const auto& entry : std::filesystem::directory_iterator(root, errorCode)) {
            if (entry.is_directory() &&
                Utils::FileSystem::isRegularFile(entry.path().string() + "/addon.xml"))
                return entry.path().string();
        }
        return "";
    }

    std::string formatAsset(std::string text, const std::map<std::string, std::string>& values)
    {
        for (const auto& [key, value] : values)
            text = Utils::String::replace(text, "{" + key + "}", value);
        return text;
    }

    std::string versionFromTag(const std::string& tag)
    {
        if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
            return tag.substr(1);
        return tag;
    }

    HttpClient::Options githubOptions(const std::atomic<bool>* cancel = nullptr)
    {
        HttpClient::Options options;
        options.headers = {"Accept: application/vnd.github+json"};
        options.cancel = cancel;
        options.timeout = 60;
        return options;
    }
} // namespace

AddonStore& AddonStore::getInstance()
{
    static AddonStore instance;
    return instance;
}

AddonStore::AddonStore()
{
    loadState();
    scan();
}

std::string AddonStore::getHomeDirectory()
{
    return Utils::FileSystem::getAppDataDirectory() + "/Elementum";
}

std::string AddonStore::getAddonsDirectory() { return getHomeDirectory() + "/addons"; }

std::string AddonStore::getDataDirectory() { return getHomeDirectory() + "/addons-data"; }

std::string AddonStore::getTempDirectory() { return getHomeDirectory() + "/temp"; }

std::string AddonStore::getLogDirectory() { return getHomeDirectory() + "/logs"; }

std::string AddonStore::getAddonDataDirectory(const std::string& addonId)
{
    return getDataDirectory() + "/" + addonId;
}

bool AddonStore::readAddon(const std::string& directory, Addon& addon) const
{
    pugi::xml_document doc;
    const std::string file {directory + "/addon.xml"};
    if (!doc.load_file(file.c_str()))
        return false;
    const pugi::xml_node root {doc.child("addon")};
    addon.id = root.attribute("id").as_string();
    if (addon.id.empty())
        return false;
    addon.name = Utils::String::trim(stripAddonFormatting(root.attribute("name").as_string()));
    addon.version = root.attribute("version").as_string();
    addon.provider = root.attribute("provider-name").as_string();
    addon.path = directory;

    for (pugi::xml_node extension {root.child("extension")}; extension;
         extension = extension.next_sibling("extension")) {
        const std::string point {extension.attribute("point").as_string()};
        addon.extensionPoints.emplace_back(point);
        if (point == "xbmc.addon.metadata") {
            addon.summary = stripAddonFormatting(englishText(extension, "summary"));
            addon.description = stripAddonFormatting(englishText(extension, "description"));
            addon.changelog = englishText(extension, "news");
            const pugi::xml_node assets {extension.child("assets")};
            if (assets.child("icon"))
                addon.icon = directory + "/" + assets.child("icon").text().as_string();
            if (assets.child("fanart"))
                addon.fanart = directory + "/" + assets.child("fanart").text().as_string();
            continue;
        }
        if (point == "xbmc.python.module" || point == "xbmc.service" ||
            point == "xbmc.subtitle.module")
            continue;
        if (addon.type.empty()) {
            addon.type = point;
            addon.library = extension.attribute("library").as_string();
        }
    }
    if (addon.icon.empty() && Utils::FileSystem::isRegularFile(directory + "/icon.png"))
        addon.icon = directory + "/icon.png";
    if (addon.fanart.empty() && Utils::FileSystem::isRegularFile(directory + "/fanart.jpg"))
        addon.fanart = directory + "/fanart.jpg";
    return true;
}

void AddonStore::scan()
{
    std::map<std::string, Addon> addons;
    std::error_code errorCode;
    for (const auto& entry :
         std::filesystem::directory_iterator(getAddonsDirectory(), errorCode)) {
        if (!entry.is_directory())
            continue;
        Addon addon;
        if (readAddon(entry.path().string(), addon))
            addons[addon.id] = addon;
    }
    std::unique_lock<std::mutex> lock {mMutex};
    for (auto& [id, addon] : addons)
        addon.enabled = mDisabled.count(id) == 0;
    mAddons = addons;
    // Settings of add-ons that were removed or replaced are read again when needed.
    for (auto it = mSettings.begin(); it != mSettings.end();) {
        const auto addon {mAddons.find(it->first)};
        if (addon == mAddons.end() || addon->second.path != it->second->getAddonPath())
            it = mSettings.erase(it);
        else
            ++it;
    }
}

std::vector<AddonStore::Addon> AddonStore::getAddons() const
{
    std::unique_lock<std::mutex> lock {mMutex};
    std::vector<Addon> addons;
    for (const auto& [id, addon] : mAddons)
        addons.emplace_back(addon);
    return addons;
}

bool AddonStore::getAddon(const std::string& id, Addon& addon) const
{
    std::unique_lock<std::mutex> lock {mMutex};
    const auto it {mAddons.find(id)};
    if (it == mAddons.cend())
        return false;
    addon = it->second;
    return true;
}

bool AddonStore::isInstalled(const std::string& id) const
{
    std::unique_lock<std::mutex> lock {mMutex};
    return mAddons.count(id) != 0;
}

void AddonStore::setEnabled(const std::string& id, bool enabled)
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        if (enabled)
            mDisabled.erase(id);
        else
            mDisabled.insert(id);
        const auto it {mAddons.find(id)};
        if (it != mAddons.end())
            it->second.enabled = enabled;
    }
    saveState();
}

void AddonStore::loadState()
{
    pugi::xml_document doc;
    const std::string path {getHomeDirectory() + "/addons-state.xml"};
    if (!doc.load_file(path.c_str()))
        return;
    for (pugi::xml_node node {doc.child("addons").child("disabled")}; node;
         node = node.next_sibling("disabled"))
        mDisabled.insert(node.attribute("id").as_string());
}

void AddonStore::saveState() const
{
    pugi::xml_document doc;
    pugi::xml_node root {doc.append_child("addons")};
    {
        std::unique_lock<std::mutex> lock {mMutex};
        for (const auto& id : mDisabled)
            root.append_child("disabled").append_attribute("id") = id.c_str();
    }
    Utils::FileSystem::createDirectory(getHomeDirectory());
    doc.save_file((getHomeDirectory() + "/addons-state.xml").c_str());
}

std::shared_ptr<AddonSettings> AddonStore::getSettings(const std::string& id)
{
    std::unique_lock<std::mutex> lock {mMutex};
    const auto existing {mSettings.find(id)};
    if (existing != mSettings.end())
        return existing->second;
    const auto addon {mAddons.find(id)};
    if (addon == mAddons.end())
        return nullptr;
    auto settings =
        std::make_shared<AddonSettings>(id, addon->second.path, getAddonDataDirectory(id));
    settings->load();
    mSettings[id] = settings;
    return settings;
}

AddonStore::Result AddonStore::installDirectory(const std::string& sourceDirectory)
{
    Result result;
    Addon addon;
    if (!readAddon(sourceDirectory, addon)) {
        result.error = "The archive doesn't contain an add-on";
        return result;
    }
    result.addonId = addon.id;
    result.version = addon.version;

    Utils::FileSystem::createDirectory(getAddonsDirectory());
    const std::string target {getAddonsDirectory() + "/" + addon.id};
    const std::string backup {getTempDirectory() + "/" + uniqueName(addon.id + "-old-")};
    std::error_code errorCode;
    const bool replacing {Utils::FileSystem::isDirectory(target)};
    if (replacing) {
        std::filesystem::rename(target, backup, errorCode);
        if (errorCode) {
            result.error = "Couldn't replace the installed version: " + errorCode.message();
            return result;
        }
    }
    std::filesystem::rename(sourceDirectory, target, errorCode);
    if (errorCode) {
        // Different file systems, copy instead.
        errorCode.clear();
        std::filesystem::copy(sourceDirectory, target,
                              std::filesystem::copy_options::recursive, errorCode);
    }
    if (errorCode) {
        result.error = "Couldn't install the add-on: " + errorCode.message();
        std::filesystem::remove_all(target, errorCode);
        if (replacing)
            std::filesystem::rename(backup, target, errorCode);
        return result;
    }
    if (replacing)
        std::filesystem::remove_all(backup, errorCode);

    Utils::FileSystem::createDirectory(getAddonDataDirectory(addon.id));
    scan();
    result.success = true;
    LOG(LogInfo) << "AddonStore: Installed " << addon.id << " " << addon.version;
    return result;
}

AddonStore::Result AddonStore::installFromZip(const std::string& zipPath, const Progress& progress)
{
    Result result;
    if (progress)
        progress("Extracting " + Utils::FileSystem::getFileName(zipPath), -1);
    Utils::FileSystem::createDirectory(getTempDirectory());
    const std::string extractDirectory {getTempDirectory() + "/" + uniqueName("extract-")};
    Utils::FileSystem::createDirectory(extractDirectory);

    std::string error;
    if (!extractArchive(zipPath, extractDirectory, error)) {
        result.error = "Couldn't extract the archive" + (error.empty() ? "" : ": " + error);
        std::error_code errorCode;
        std::filesystem::remove_all(extractDirectory, errorCode);
        return result;
    }
    const std::string addonDirectory {findAddonDirectory(extractDirectory)};
    if (addonDirectory.empty()) {
        result.error = "The archive doesn't contain an add-on";
    }
    else {
        if (progress)
            progress("Installing", -1);
        result = installDirectory(addonDirectory);
    }
    std::error_code errorCode;
    std::filesystem::remove_all(extractDirectory, errorCode);
    return result;
}

std::vector<AddonStore::RepositoryEntry> AddonStore::getRepositoryEntries() const
{
    // The streaming add-on and its search provider are always available, from their
    // releases on GitHub.
    std::vector<RepositoryEntry> entries;
    RepositoryEntry streaming;
    streaming.id = STREAMING_ADDON;
    streaming.username = "elgatito";
    streaming.repository = STREAMING_ADDON;
    streaming.zipAsset = "https://github.com/{username}/{repository}/releases/download/"
                         "v{version}/{id}-{version}.client.zip";
    entries.emplace_back(streaming);
    RepositoryEntry provider;
    provider.id = PROVIDER_ADDON;
    provider.username = "elgatito";
    provider.repository = PROVIDER_ADDON;
    provider.zipAsset = "https://github.com/{username}/{repository}/releases/download/"
                        "v{version}/{id}-{version}.zip";
    entries.emplace_back(provider);

    // Repository add-ons can add more.
    for (const auto& addon : getAddons()) {
        if (addon.type != "xbmc.addon.repository")
            continue;
        std::ifstream file {addon.path + "/resources/repository.json"};
        if (!file)
            continue;
        const std::string content {std::istreambuf_iterator<char>(file),
                                   std::istreambuf_iterator<char>()};
        rapidjson::Document doc;
        doc.Parse(content.c_str());
        if (doc.HasParseError() || !doc.IsArray())
            continue;
        for (const auto& item : doc.GetArray()) {
            if (!item.IsObject() || !item.HasMember("id") || !item["id"].IsString() ||
                !item.HasMember("username") || !item["username"].IsString())
                continue;
            const std::string id {item["id"].GetString()};
            if (std::find_if(entries.cbegin(), entries.cend(), [&id](const RepositoryEntry& e) {
                    return e.id == id;
                }) != entries.cend())
                continue;
            // Entries limited to other platforms are skipped.
            if (item.HasMember("platforms") && item["platforms"].IsArray()) {
                bool supported {false};
                for (const auto& platform : item["platforms"].GetArray()) {
                    if (platform.IsString() &&
                        Utils::String::startsWith(platform.GetString(), "linux"))
                        supported = true;
                }
                if (!supported)
                    continue;
            }
            RepositoryEntry entry;
            entry.id = id;
            entry.username = item["username"].GetString();
            entry.repository = entry.id;
            if (item.HasMember("repository") && item["repository"].IsString())
                entry.repository = item["repository"].GetString();
            if (item.HasMember("branch") && item["branch"].IsString())
                entry.branch = item["branch"].GetString();
            if (item.HasMember("assets") && item["assets"].IsObject() &&
                item["assets"].HasMember("zip") && item["assets"]["zip"].IsString())
                entry.zipAsset = item["assets"]["zip"].GetString();
            entry.repositoryAddon = addon.id;
            entries.emplace_back(entry);
        }
    }
    return entries;
}

bool AddonStore::canInstallFromRepository(const std::string& id) const
{
    for (const auto& entry : getRepositoryEntries()) {
        if (entry.id == id)
            return true;
    }
    return false;
}

std::string AddonStore::latestReleaseTag(const RepositoryEntry& entry, std::string& error) const
{
    if (!entry.branch.empty())
        return entry.branch;

    const HttpClient::Response response {HttpClient::request(
        GITHUB_API + entry.username + "/" + entry.repository + "/releases/latest",
        githubOptions())};
    if (response.ok()) {
        rapidjson::Document doc;
        doc.Parse(response.body.c_str());
        if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("tag_name") &&
            doc["tag_name"].IsString())
            return doc["tag_name"].GetString();
    }
    // Without the API, e.g. when its request limit has been reached, the release page
    // redirects to the newest release.
    const HttpClient::Response page {HttpClient::request(
        "https://github.com/" + entry.username + "/" + entry.repository + "/releases/latest")};
    const std::string marker {"/releases/tag/"};
    const size_t position {page.effectiveUrl.find(marker)};
    if (page.error.empty() && position != std::string::npos)
        return HttpClient::urlDecode(page.effectiveUrl.substr(position + marker.size()));

    error = response.error.empty() ? "GitHub answered with code " + std::to_string(response.code) :
                                     response.error;
    return "";
}

AddonStore::Result AddonStore::getLatestVersion(const std::string& id) const
{
    Result result;
    result.addonId = id;
    const std::vector<RepositoryEntry> entries {getRepositoryEntries()};
    const auto entry {std::find_if(entries.cbegin(), entries.cend(),
                                   [&id](const RepositoryEntry& item) { return item.id == id; })};
    if (entry == entries.cend()) {
        result.error = "No installed repository has " + id;
        return result;
    }
    std::string error;
    const std::string tag {latestReleaseTag(*entry, error)};
    if (tag.empty()) {
        result.error = "Couldn't find the newest version of " + id + ": " + error;
        return result;
    }
    // The version in the add-on description of the release, the tag is used if it can't
    // be read.
    result.version = versionFromTag(tag);
    const HttpClient::Response description {
        HttpClient::request("https://raw.githubusercontent.com/" + entry->username + "/" +
                            entry->repository + "/" + tag + "/addon.xml")};
    if (description.ok()) {
        pugi::xml_document doc;
        if (doc.load_string(description.body.c_str())) {
            const std::string version {doc.child("addon").attribute("version").as_string()};
            if (!version.empty())
                result.version = version;
        }
    }
    result.success = true;
    return result;
}

AddonStore::Result AddonStore::installFromRepository(const std::string& id,
                                                     const Progress& progress,
                                                     const std::atomic<bool>* cancel)
{
    Result result;
    result.addonId = id;
    const std::vector<RepositoryEntry> entries {getRepositoryEntries()};
    const auto entry {std::find_if(entries.cbegin(), entries.cend(),
                                   [&id](const RepositoryEntry& item) { return item.id == id; })};
    if (entry == entries.cend()) {
        result.error = "No installed repository has " + id;
        return result;
    }

    if (progress)
        progress("Checking the newest version of " + id, -1);
    std::string error;
    const std::string tag {latestReleaseTag(*entry, error)};
    if (tag.empty()) {
        result.error = "Couldn't find the newest version of " + id + ": " + error;
        return result;
    }
    const Result latest {getLatestVersion(id)};
    const std::string version {latest.success ? latest.version : versionFromTag(tag)};

    const std::map<std::string, std::string> values {{"id", id},
                                                     {"username", entry->username},
                                                     {"repository", entry->repository},
                                                     {"ref", tag},
                                                     {"version", version},
                                                     {"system", "Linux"},
                                                     {"arch", "x64"}};
    const std::string url {entry->zipAsset.empty() ?
                               "https://github.com/" + entry->username + "/" + entry->repository +
                                   "/archive/" + tag + ".zip" :
                               formatAsset(entry->zipAsset, values)};

    Utils::FileSystem::createDirectory(getTempDirectory());
    const std::string zipPath {getTempDirectory() + "/" + id + "-" + version + ".zip"};
    HttpClient::Options options;
    options.cancel = cancel;
    options.timeout = 0;
    options.progress = [&progress, id, version](long long done, long long total) {
        if (progress && total > 0)
            progress("Downloading " + id + " " + version, static_cast<int>(done * 100 / total));
    };
    if (progress)
        progress("Downloading " + id + " " + version, 0);
    if (!HttpClient::download(url, zipPath, options, error)) {
        result.error = "Couldn't download " + id + ": " + error;
        return result;
    }

    result = installFromZip(zipPath, progress);
    Utils::FileSystem::removeFile(zipPath);
    if (result.success && id == STREAMING_ADDON) {
        const Result binary {ensureDaemonBinary(progress, cancel)};
        if (!binary.success) {
            result.success = false;
            result.error = binary.error;
        }
    }
    return result;
}

std::string AddonStore::getDaemonBinaryPath()
{
    return getAddonDataDirectory(STREAMING_ADDON) + "/bin/" + platformName() + "/elementum";
}

AddonStore::Result AddonStore::ensureDaemonBinary(const Progress& progress,
                                                  const std::atomic<bool>* cancel)
{
    Result result;
    result.addonId = STREAMING_ADDON;
    Addon addon;
    if (!getAddon(STREAMING_ADDON, addon)) {
        result.error = "Elementum is not installed";
        return result;
    }
    result.version = addon.version;

    const std::string binary {getDaemonBinaryPath()};
    const std::string directory {Utils::FileSystem::getParent(binary)};
    const std::string versionFile {directory + "/version"};
    std::string installedVersion;
    {
        std::ifstream file {versionFile};
        std::getline(file, installedVersion);
    }
    if (Utils::FileSystem::isRegularFile(binary) &&
        compareVersions(versionFromTag(Utils::String::trim(installedVersion)), addon.version) >=
            0) {
        result.success = true;
        return result;
    }

    Utils::FileSystem::createDirectory(directory);
    HttpClient::Options options;
    options.cancel = cancel;
    options.timeout = 0;
    options.progress = [&progress](long long done, long long total) {
        if (progress && total > 0)
            progress("Downloading the Elementum program", static_cast<int>(done * 100 / total));
    };
    if (progress)
        progress("Downloading the Elementum program", 0);

    std::string error;
    bool downloaded {false};
    std::string downloadedRef;
    for (const std::string& ref : {"v" + addon.version, std::string("master")}) {
        const std::string url {"https://github.com/" + BINARIES_REPOSITORY + "/raw/" + ref + "/" +
                               platformName() + "/elementum"};
        if (HttpClient::download(url, binary, options, error)) {
            downloaded = true;
            downloadedRef = ref;
            break;
        }
        if (cancel != nullptr && cancel->load())
            break;
    }
    if (!downloaded) {
        result.error = "Couldn't download the Elementum program: " + error;
        return result;
    }
    chmod(binary.c_str(), 0755);
    std::ofstream file {versionFile, std::ios::trunc};
    file << (downloadedRef == "master" ? "v" + addon.version : downloadedRef);
    LOG(LogInfo) << "AddonStore: Downloaded the Elementum program (" << downloadedRef << ")";
    result.success = true;
    return result;
}

int AddonStore::compareVersions(const std::string& a, const std::string& b)
{
    auto parts = [](const std::string& version) {
        std::vector<long> numbers;
        std::string current;
        for (const char character : versionFromTag(version)) {
            if (character >= '0' && character <= '9') {
                current.push_back(character);
            }
            else {
                if (!current.empty())
                    numbers.emplace_back(std::atol(current.c_str()));
                current.clear();
                if (character == '~' || character == '+')
                    break;
            }
        }
        if (!current.empty())
            numbers.emplace_back(std::atol(current.c_str()));
        return numbers;
    };
    const std::vector<long> first {parts(a)};
    const std::vector<long> second {parts(b)};
    for (size_t i {0}; i < std::max(first.size(), second.size()); ++i) {
        const long left {i < first.size() ? first[i] : 0};
        const long right {i < second.size() ? second[i] : 0};
        if (left != right)
            return left < right ? -1 : 1;
    }
    return 0;
}

namespace
{
    std::mutex sAutoUpdateMutex;
    std::thread sAutoUpdateThread;
    std::atomic<bool> sAutoUpdating {false};
    std::atomic<bool> sAutoUpdateCancel {false};
} // namespace

void AddonStore::startAutoUpdate(
    const std::function<void(const std::vector<std::string>&, const std::string&)>& done)
{
    std::unique_lock<std::mutex> lock {sAutoUpdateMutex};
    if (sAutoUpdating || !getInstance().isInstalled(STREAMING_ADDON))
        return;
    if (sAutoUpdateThread.joinable())
        sAutoUpdateThread.join();
    sAutoUpdating = true;
    sAutoUpdateCancel = false;
    sAutoUpdateThread = std::thread([done] {
        AddonStore& store {getInstance()};
        std::vector<std::string> updated;
        std::string error;
        for (const auto& entry : store.getRepositoryEntries()) {
            if (sAutoUpdateCancel)
                break;
            Addon addon;
            if (!store.getAddon(entry.id, addon))
                continue;
            const Result latest {store.getLatestVersion(entry.id)};
            if (!latest.success) {
                error = latest.error;
                continue;
            }
            if (compareVersions(latest.version, addon.version) <= 0)
                continue;
            LOG(LogInfo) << "AddonStore: Updating " << addon.id << " from " << addon.version
                         << " to " << latest.version;
            const Result result {
                store.installFromRepository(entry.id, Progress(), &sAutoUpdateCancel)};
            if (result.success)
                updated.emplace_back(addon.name + " " + latest.version);
            else if (!sAutoUpdateCancel)
                error = result.error;
        }
        // The program matching the new version is downloaded now, so starting isn't delayed.
        if (!updated.empty() && !sAutoUpdateCancel)
            store.ensureDaemonBinary(Progress(), &sAutoUpdateCancel);
        if (!error.empty()) {
            LOG(LogWarning) << "AddonStore: Couldn't check for updates: " << error;
        }
        sAutoUpdating = false;
        if (done && !sAutoUpdateCancel)
            done(updated, error);
    });
}

void AddonStore::stopAutoUpdate()
{
    std::unique_lock<std::mutex> lock {sAutoUpdateMutex};
    sAutoUpdateCancel = true;
    if (sAutoUpdateThread.joinable())
        sAutoUpdateThread.join();
    sAutoUpdating = false;
}

bool AddonStore::isAutoUpdating() { return sAutoUpdating; }
