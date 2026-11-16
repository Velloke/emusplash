//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonStore.h
//
//  The installed add-ons, kept in the Elementum directory in the application data directory.
//  Add-ons are installed from their releases on GitHub (or from zip files). The streaming
//  add-on also needs its program, which is downloaded separately.
//

#ifndef ES_APP_MEDIA_ADDON_STORE_H
#define ES_APP_MEDIA_ADDON_STORE_H

#include "media/AddonSettings.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class AddonStore
{
public:
    struct Addon {
        std::string id;
        std::string name; // Without formatting.
        std::string version;
        std::string path;
        std::string provider;
        std::string summary;
        std::string description;
        std::string icon;
        std::string fanart;
        std::string changelog;
        std::string type;    // The main extension point, e.g. "xbmc.python.script".
        std::string library; // Script file run for the extension point.
        std::vector<std::string> extensionPoints;
        bool enabled {true};
    };

    // An add-on that can be installed from a repository.
    struct RepositoryEntry {
        std::string id;
        std::string username;
        std::string repository;
        std::string branch;
        std::string zipAsset;
        std::string repositoryAddon;
    };

    struct Result {
        bool success {false};
        std::string error;
        std::string addonId;
        std::string version;
    };

    // Reports the progress of an installation, as a message and a percentage (-1 if unknown).
    using Progress = std::function<void(const std::string&, int)>;

    static AddonStore& getInstance();

    static std::string getHomeDirectory();
    static std::string getAddonsDirectory();
    static std::string getDataDirectory();
    static std::string getTempDirectory();
    static std::string getLogDirectory();
    static std::string getAddonDataDirectory(const std::string& addonId);

    static constexpr const char* STREAMING_ADDON {"plugin.video.elementum"};
    static constexpr const char* PROVIDER_ADDON {"script.elementum.burst"};
    static constexpr const char* REPOSITORY_ADDON {"repository.elementumorg"};

    // Reads the installed add-ons again.
    void scan();
    std::vector<Addon> getAddons() const;
    bool getAddon(const std::string& id, Addon& addon) const;
    bool isInstalled(const std::string& id) const;
    void setEnabled(const std::string& id, bool enabled);

    // The settings of an add-on, loaded the first time they're needed. Null if not installed.
    std::shared_ptr<AddonSettings> getSettings(const std::string& id);

    // Installation and updates, these block and are meant for background threads.
    Result installFromZip(const std::string& zipPath, const Progress& progress = Progress());
    // The add-ons that can be installed: the streaming add-on and its search provider, and
    // the ones listed by installed repository add-ons.
    std::vector<RepositoryEntry> getRepositoryEntries() const;
    bool canInstallFromRepository(const std::string& id) const;
    // Finds the newest version available in the repository.
    Result getLatestVersion(const std::string& id) const;
    Result installFromRepository(const std::string& id,
                                 const Progress& progress = Progress(),
                                 const std::atomic<bool>* cancel = nullptr);
    // The program of the streaming add-on, downloaded if it's missing or outdated.
    Result ensureDaemonBinary(const Progress& progress = Progress(),
                              const std::atomic<bool>* cancel = nullptr);
    static std::string getDaemonBinaryPath();

    // Negative if a is older than b, positive if newer.
    static int compareVersions(const std::string& a, const std::string& b);

    // Updates the installed add-ons in the background, done when the application starts.
    // The callback is called from that thread with the names of the updated add-ons, or an
    // error message.
    static void startAutoUpdate(
        const std::function<void(const std::vector<std::string>&, const std::string&)>& done);
    // Cancels the update and waits for it, done before quitting.
    static void stopAutoUpdate();
    static bool isAutoUpdating();

private:
    AddonStore();
    bool readAddon(const std::string& directory, Addon& addon) const;
    void loadState();
    void saveState() const;
    Result installDirectory(const std::string& sourceDirectory);
    std::string latestReleaseTag(const RepositoryEntry& entry, std::string& error) const;

    mutable std::mutex mMutex;
    std::map<std::string, Addon> mAddons;
    std::set<std::string> mDisabled;
    std::map<std::string, std::shared_ptr<AddonSettings>> mSettings;
};

#endif // ES_APP_MEDIA_ADDON_STORE_H
