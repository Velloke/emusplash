//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  FileData.cpp
//
//  Provides game file data structures and functions to access and sort this information.
//  Also provides functions to look up paths to media files and for launching games
//  (launching initiated in ViewController).
//

#include "FileData.h"

#include "AudioManager.h"
#include "CollectionSystemsManager.h"
#include "EmulatorConfig.h"
#include "FileFilterIndex.h"
#include "FileSorts.h"
#include "Log.h"
#include "MameNames.h"
#include "Scripting.h"
#include "SystemData.h"
#include "SystemStatus.h"
#include "UIModeController.h"
#include "emulators/EmulatorProfiles.h"
#include "emulators/GameHotkeys.h"
#include "Window.h"
#include "utils/FileSystemUtil.h"
#include "utils/PlatformUtil.h"
#include "utils/TimeUtil.h"
#include "views/GamelistView.h"
#include "views/ViewController.h"

#if defined(__ANDROID__)
#include "utils/PlatformUtilAndroid.h"
#endif

#include <assert.h>
#include <regex>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>

namespace
{
    // Helpers for the per-game "load always recent state" option.
    // Supported emulators: PCSX2 (PS2), PPSSPP (PSP) and RPCS3 (PS3).
    //
    // EmuSPLASH can't work out the game serial that the emulators use when naming their save
    // states, so after each session we check which state file was written and remember its
    // prefix for the game. On the next launch the most recently written state with that
    // prefix is passed to the emulator on the command line.

    enum class StateEmulator {
        NONE,
        PCSX2,
        PPSSPP,
        RPCS3
    };

    const char* stateEmulatorName(StateEmulator emu)
    {
        switch (emu) {
            case StateEmulator::PCSX2:
                return "PCSX2";
            case StateEmulator::PPSSPP:
                return "PPSSPP";
            case StateEmulator::RPCS3:
                return "RPCS3";
            default:
                return "";
        }
    }

    StateEmulator detectStateEmulator(const std::string& commandRaw, const std::string& command)
    {
        if (commandRaw.find("%EMULATOR_PCSX2%") != std::string::npos)
            return StateEmulator::PCSX2;
        if (commandRaw.find("%EMULATOR_PPSSPP%") != std::string::npos)
            return StateEmulator::PPSSPP;
        if (commandRaw.find("%EMULATOR_RPCS3%") != std::string::npos)
            return StateEmulator::RPCS3;

        const std::string lower {Utils::String::toLower(command)};
        if (lower.find("libretro") != std::string::npos)
            return StateEmulator::NONE;
        if (lower.find("pcsx2") != std::string::npos &&
            lower.find("pcsx2x6") == std::string::npos &&
            lower.find("legacy") == std::string::npos)
            return StateEmulator::PCSX2;
        if (lower.find("ppsspp") != std::string::npos)
            return StateEmulator::PPSSPP;
        if (lower.find("rpcs3") != std::string::npos)
            return StateEmulator::RPCS3;
        return StateEmulator::NONE;
    }

    std::vector<std::string> stateDirs(StateEmulator emu)
    {
        std::vector<std::string> dirs;
        const char* settingName {emu == StateEmulator::PCSX2  ? "PCSX2SaveStatesDirectory" :
                                 emu == StateEmulator::PPSSPP ? "PPSSPPSaveStatesDirectory" :
                                                                "RPCS3SaveStatesDirectory"};
        const std::string configured {Settings::getInstance()->getString(settingName)};
        if (!configured.empty()) {
            dirs.emplace_back(Utils::FileSystem::expandHomePath(configured));
            return dirs;
        }

        const std::string home {Utils::FileSystem::getSystemHomeDirectory()};
        std::vector<std::string> configDirs;
        if (const char* xdg {getenv("XDG_CONFIG_HOME")}; xdg != nullptr && xdg[0] != '\0')
            configDirs.emplace_back(xdg);
        configDirs.emplace_back(home + "/.config");

        std::vector<std::string> candidates;
        if (emu == StateEmulator::PCSX2) {
            for (const auto& config : configDirs)
                candidates.emplace_back(config + "/PCSX2/sstates");
            candidates.emplace_back(home + "/.var/app/net.pcsx2.PCSX2/config/PCSX2/sstates");
#if defined(_WIN64)
            candidates.emplace_back(home + "/Documents/PCSX2/sstates");
#elif defined(__APPLE__)
            candidates.emplace_back(home + "/Library/Application Support/PCSX2/sstates");
#endif
        }
        else if (emu == StateEmulator::PPSSPP) {
            for (const auto& config : configDirs)
                candidates.emplace_back(config + "/ppsspp/PSP/PPSSPP_STATE");
            candidates.emplace_back(home +
                                    "/.var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP/PPSSPP_STATE");
            candidates.emplace_back(home + "/Documents/PPSSPP/PSP/PPSSPP_STATE");
        }
        else if (emu == StateEmulator::RPCS3) {
            for (const auto& config : configDirs)
                candidates.emplace_back(config + "/rpcs3/savestates");
            candidates.emplace_back(home + "/.var/app/net.rpcs3.RPCS3/config/rpcs3/savestates");
        }

        for (const auto& dir : candidates) {
            if (Utils::FileSystem::isDirectory(dir) &&
                std::find(dirs.cbegin(), dirs.cend(), dir) == dirs.cend())
                dirs.emplace_back(dir);
        }
        return dirs;
    }

    // Returns the game-specific prefix for a save state file, or an empty string if the
    // file is not a save state of the given emulator.
    std::string statePrefix(StateEmulator emu, const std::filesystem::path& path)
    {
        const std::string fileName {path.filename().string()};
        std::smatch match;

        if (emu == StateEmulator::PCSX2) {
            // "<SERIAL> (<CRC>).resume.p2s" and "<SERIAL> (<CRC>).<NN>.p2s"
            static const std::regex regex {R"(^(.+ \([0-9A-Fa-f]{8}\))\.(resume|\d{2})\.p2s$)"};
            if (std::regex_match(fileName, match, regex))
                return match[1].str();
        }
        else if (emu == StateEmulator::PPSSPP) {
            // "<DISCID>_<VERSION>_<SLOT>.ppst" (undo states are skipped).
            static const std::regex regex {R"(^(.+_\d+\.\d+)_\d+\.ppst$)"};
            if (std::regex_match(fileName, match, regex))
                return match[1].str();
        }
        else if (emu == StateEmulator::RPCS3) {
            // "savestates/<TITLE>/<TITLE>_<...>_<ID>.SAVESTAT[.zst|.gz]"
            static const std::regex regex {R"(^.+\.SAVESTAT(\.zst|\.gz)?$)"};
            if (std::regex_match(fileName, match, regex))
                return path.parent_path().filename().string();
        }
        return "";
    }

    using StateSnapshot = std::map<std::string, std::filesystem::file_time_type>;

    StateSnapshot stateSnapshot(StateEmulator emu)
    {
        StateSnapshot snapshot;
        for (const auto& dir : stateDirs(emu)) {
            std::error_code ec;
            // RPCS3 keeps the states in one subdirectory per game, the others use a flat
            // directory so a depth of one level is enough.
            for (auto it {std::filesystem::recursive_directory_iterator(dir, ec)};
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (it.depth() > 1) {
                    it.disable_recursion_pending();
                    continue;
                }
                std::error_code fileEc;
                if (!it->is_regular_file(fileEc) || statePrefix(emu, it->path()).empty())
                    continue;
                const auto time {it->last_write_time(fileEc)};
                if (!fileEc)
                    snapshot[it->path().string()] = time;
            }
        }
        return snapshot;
    }

    std::string stateMappingFile(StateEmulator emu)
    {
        return Utils::FileSystem::getAppDataDirectory() + "/settings/" +
               Utils::String::toLower(stateEmulatorName(emu)) + "_recent_states.txt";
    }

    // Maps the game file path to the state prefix. One "<prefix>\t<game path>" per line.
    std::map<std::string, std::string> readStateMapping(StateEmulator emu)
    {
        std::map<std::string, std::string> mapping;
        std::ifstream file {stateMappingFile(emu)};
        std::string line;
        while (std::getline(file, line)) {
            const size_t tab {line.find('\t')};
            if (tab != std::string::npos && tab > 0)
                mapping[line.substr(tab + 1)] = line.substr(0, tab);
        }
        return mapping;
    }

    void writeStateMapping(StateEmulator emu, const std::map<std::string, std::string>& mapping)
    {
        Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(stateMappingFile(emu)));
        std::ofstream file {stateMappingFile(emu), std::ios::trunc};
        for (const auto& [gamePath, prefix] : mapping)
            file << prefix << '\t' << gamePath << '\n';
    }

    // The newest state file (any slot or the resume state) for the given prefix.
    std::string mostRecentState(StateEmulator emu, const std::string& prefix)
    {
        std::string newestPath;
        std::filesystem::file_time_type newestTime {};
        for (const auto& [path, time] : stateSnapshot(emu)) {
            if (statePrefix(emu, path) != prefix)
                continue;
            if (newestPath.empty() || time > newestTime) {
                newestPath = path;
                newestTime = time;
            }
        }
        return newestPath;
    }

    // After a session, find the state file that was written and remember its prefix.
    void learnStatePrefix(StateEmulator emu,
                          const std::string& gamePath,
                          const StateSnapshot& before)
    {
        std::string newestPath;
        std::filesystem::file_time_type newestTime {};
        for (const auto& [path, time] : stateSnapshot(emu)) {
            const auto it {before.find(path)};
            if (it != before.cend() && it->second == time)
                continue;
            if (newestPath.empty() || time > newestTime) {
                newestPath = path;
                newestTime = time;
            }
        }
        if (newestPath.empty())
            return;
        const std::string prefix {statePrefix(emu, newestPath)};
        auto mapping {readStateMapping(emu)};
        if (mapping[gamePath] != prefix) {
            mapping[gamePath] = prefix;
            writeStateMapping(emu, mapping);
            LOG(LogInfo) << stateEmulatorName(emu) << " save states for this game use \""
                         << prefix << "\"";
        }
    }

    // Inserts the state argument in front of %ROM%, also stepping in front of an opening
    // quotation mark and in front of a "--" separator (everything after "--" is treated
    // as the game filename by PCSX2).
    void insertStateArgument(std::string& command, const std::string& argument)
    {
        size_t insertPos {command.find("%ROM%")};
        if (insertPos == std::string::npos)
            return;
        if (insertPos > 0 && (command[insertPos - 1] == '"' || command[insertPos - 1] == '\''))
            --insertPos;
        size_t end {insertPos};
        while (end > 0 && command[end - 1] == ' ')
            --end;
        if (end >= 3 && command.compare(end - 3, 3, " --") == 0)
            insertPos = end - 2;
        command.insert(insertPos, argument + " ");
    }

    std::string stateArgument(StateEmulator emu, const std::string& stateFile)
    {
        const std::string path {Utils::FileSystem::getEscapedPath(stateFile)};
        switch (emu) {
            case StateEmulator::PCSX2:
                return "-statefile " + path;
            case StateEmulator::PPSSPP:
                return "--state=" + path;
            case StateEmulator::RPCS3:
                return "--savestate " + path;
            default:
                return "";
        }
    }
    // The arguments of the program in the Exec line of a .desktop file, without the program
    // itself, e.g. '--no-gui "%RPCS3_GAMEID%:BLES01807"' of a shortcut made by RPCS3. Used when
    // a shortcut is started with an emulator that can't open .desktop files, so the emulator
    // of the command gets the game of the shortcut. "flatpak run <id>" and "env VAR=x" in front
    // of the program are skipped too. Empty if the file has no usable Exec line.
    std::string desktopExecArguments(const std::string& path)
    {
        std::ifstream stream {path};
        std::string exec;
        bool inEntry {false};
        for (std::string line; std::getline(stream, line);) {
            line = Utils::String::trim(line);
            if (!line.empty() && line.front() == '[')
                inEntry = (line == "[Desktop Entry]");
            else if (inEntry && line.rfind("Exec=", 0) == 0)
                exec = Utils::String::trim(line.substr(5));
        }
        if (exec.empty())
            return "";
        // The field codes (%f, %U and others) are left out, "%%" is a percent sign.
        static const std::regex fieldCodes {"(^|[^%])%[fFuUdDnNickvm]"};
        exec = std::regex_replace(exec, fieldCodes, "$1");
        exec = Utils::String::replace(exec, "%%", "%");

        // Split in words, keeping the quotes as they are given to the shell.
        std::vector<std::pair<size_t, std::string>> words;
        for (size_t position {0}; position < exec.size();) {
            while (position < exec.size() && std::isspace(static_cast<unsigned char>(exec[position])))
                ++position;
            if (position >= exec.size())
                break;
            const size_t start {position};
            char quote {0};
            std::string word;
            for (; position < exec.size(); ++position) {
                const char character {exec[position]};
                if (quote != 0) {
                    if (character == quote)
                        quote = 0;
                    else
                        word.push_back(character);
                }
                else if (character == '"' || character == '\'') {
                    quote = character;
                }
                else if (std::isspace(static_cast<unsigned char>(character))) {
                    break;
                }
                else {
                    word.push_back(character);
                }
            }
            words.emplace_back(start, word);
        }
        size_t program {0};
        if (program < words.size() && Utils::FileSystem::getFileName(words[program].second) == "env") {
            ++program;
            while (program < words.size() && words[program].second.find('=') != std::string::npos)
                ++program;
        }
        if (program < words.size() &&
            Utils::FileSystem::getFileName(words[program].second) == "flatpak") {
            ++program;
            if (program < words.size() && words[program].second == "run")
                ++program;
            while (program < words.size() && words[program].second.rfind("-", 0) == 0)
                ++program;
        }
        // The word after the program is where the arguments start.
        if (program + 1 >= words.size())
            return "";
        return Utils::String::trim(exec.substr(words[program + 1].first));
    }
} // namespace

FileData::FileData(FileType type,
                   const std::string& path,
                   SystemEnvironmentData* envData,
                   SystemData* system)
    : metadata {type == GAME ? GAME_METADATA : FOLDER_METADATA}
    , mSourceFileData {nullptr}
    , mParent {nullptr}
    , mType {type}
    , mPath {path}
    , mEnvData {envData}
    , mSystem {system}
    , mOnlyFolders {false}
    , mHasFolders {false}
    , mUpdateChildrenLastPlayed {false}
    , mUpdateChildrenMostPlayed {false}
    , mDeletionFlag {false}
    , mNoLoad {false}
{
    // Metadata needs at least a name field (since that's what getName() will return).
    if ((system->hasPlatformId(PlatformIds::ARCADE) ||
         system->hasPlatformId(PlatformIds::SNK_NEO_GEO)) &&
        metadata.getType() != FOLDER_METADATA) {
        // If it's a MAME or Neo Geo game, expand the game name accordingly.
        metadata.set("name", MameNames::getInstance().getCleanName(getCleanName()));
    }
    else {
        if (metadata.getType() == FOLDER_METADATA && Utils::FileSystem::isHidden(mPath))
            metadata.set("name", Utils::FileSystem::getFileName(mPath));
        else
            metadata.set("name", getDisplayName());
    }

    mSystemName = system->getName();
    metadata.resetChangedFlag();
}

FileData::~FileData()
{
    while (mChildren.size() > 0)
        delete (mChildren.front());

    if (mParent)
        mParent->removeChild(this);
}

const std::string& FileData::getSortName()
{
    if (mSystem->isCustomCollection() && mType == GAME) {
        if (!metadata.get("collectionsortname").empty())
            return metadata.get("collectionsortname");
        else if (!metadata.get("sortname").empty())
            return metadata.get("sortname");
        else
            return metadata.get("name");
    }

    if (metadata.get("sortname").empty())
        return metadata.get("name");
    else
        return metadata.get("sortname");
}

const bool FileData::getFavorite()
{
    if (metadata.get("favorite") == "true")
        return true;
    else
        return false;
}

const bool FileData::getKidgame()
{
    if (metadata.get("kidgame") == "true")
        return true;
    else
        return false;
}

const bool FileData::getHidden()
{
    if (metadata.get("hidden") == "true")
        return true;
    else
        return false;
}

const bool FileData::getCountAsGame()
{
    if (metadata.get("nogamecount") == "true")
        return false;
    else
        return true;
}

const bool FileData::getExcludeFromScraper()
{
    if (metadata.get("nomultiscrape") == "true")
        return true;
    else
        return false;
}

void FileData::setPlayMetadata(const bool writeMetadata)
{
    FileData* gameToUpdate {getSourceFileData()};
    gameToUpdate->metadata.set("lastplayed", Utils::Time::DateTime(Utils::Time::now()));

    // If the cursor is on a folder then a folder link must have been configured, so set the
    // lastplayed timestamp for this folder to the same as the launched game.
    FileData* cursor {
        ViewController::getInstance()->getGamelistView(gameToUpdate->getSystem())->getCursor()};
    if (cursor->getType() == FOLDER)
        cursor->metadata.set("lastplayed", gameToUpdate->metadata.get("lastplayed"));

    // If the parent is a folder and it's not the root of the system, then update its lastplayed
    // timestamp to the same time as the game that was just launched.
    if (gameToUpdate->getParent()->getType() == FOLDER &&
        gameToUpdate->getParent()->getName() != gameToUpdate->getSystem()->getFullName()) {
        gameToUpdate->getParent()->metadata.set("lastplayed",
                                                gameToUpdate->metadata.get("lastplayed"));
    }

    const int startTime {Settings::getInstance()->getInt("GameLaunchTime")};

    if (startTime != 0 && Settings::getInstance()->getInt("MaxPlayTimeTracking") != 0) {
        Settings::getInstance()->setInt("GameLaunchTime", 0);
        int prevPlayTime {gameToUpdate->metadata.getInt("playtime")};

        if (prevPlayTime < 0)
            prevPlayTime = 0;

        const int endTime {static_cast<int>(Utils::Time::DateTime(Utils::Time::now()).getTime())};
        int playTime {endTime - startTime};

        if (playTime < 0)
            playTime = 0;

        const int maxPlayTimeTracking {
            glm::clamp(Settings::getInstance()->getInt("MaxPlayTimeTracking"), 0, 24) * 3600};

        if (maxPlayTimeTracking != 24 * 3600 && playTime > maxPlayTimeTracking) {
            LOG(LogDebug) << "FileData::setPlayMetadata(): Play time was " << playTime
                          << " seconds which exceeds the MaxPlayTimeTracking value of "
                          << maxPlayTimeTracking << ", not updating game metadata";
        }
        else {
            LOG(LogDebug) << "FileData::setPlayMetadata(): Play time was " << playTime
                          << " seconds";

            gameToUpdate->metadata.set(
                "playtime", std::to_string(static_cast<long long>(prevPlayTime + playTime)));
        }
    }
    else {
        LOG(LogDebug) << "FileData::setPlayMetadata(): Play time tracking disabled, not updating "
                         "game metadata";
    }

    if (writeMetadata) {
        CollectionSystemsManager::getInstance()->refreshCollectionSystems(gameToUpdate);
        gameToUpdate->mSystem->onMetaDataSavePoint();
    }
}

const std::string FileData::getPlayTimeString(const std::string& playTimeSeconds)
{
    int playTimeValue {0};

    try {
        if (playTimeSeconds != "") {
            playTimeValue = std::stoi(playTimeSeconds);

            if (playTimeValue < 0)
                playTimeValue = 0;
        }
    }
    catch (...) {
        playTimeValue = 0;
    }

    std::string timeString;
    std::tm time {};

    time.tm_hour = playTimeValue / 3600;
    time.tm_min = (playTimeValue % 3600) / 60;
    time.tm_sec = playTimeValue % 60;

    // The logic is that anything from 1 to 119 seconds will be displayed as "1 minute" and
    // everything from 2 to 119 minutes will be displayed as the number of minutes, such as
    // "94 minutes". Everything from 120 minutes and upwards will be displayed as hours, with
    // fractional values if applicable, such as "12.7 hours". The first six minutes of each
    // hour will lead to the decimal being omitted, such as "79 hours". This way of presenting
    // the play time is mimicking exactly how Steam does it.

    if (time.tm_hour == 0 && time.tm_min == 0 && time.tm_sec == 0) {
        timeString = _p("theme", "unknown");
    }
    else if (time.tm_hour == 0 && time.tm_min == 0 && time.tm_sec > 0) {
        timeString = Utils::String::format(_np("theme", "%i minute", "%i minutes", 1), 1);
    }
    else if (time.tm_hour < 2) {
        int minutes {(time.tm_hour * 60) + time.tm_min};
        timeString =
            Utils::String::format(_np("theme", "%i minute", "%i minutes", minutes), minutes);
    }
    else {
        const std::string hours {std::to_string(time.tm_hour)};
        timeString =
            Utils::String::format(_np("theme", "%i hour", "%i hours", time.tm_hour), time.tm_hour);
        if (time.tm_min >= 6) {
            int fractional {time.tm_min / 6};
            timeString =
                Utils::String::replace(timeString, hours, hours + "." + std::to_string(fractional));
        }
    }

    return timeString;
}

const std::vector<FileData*> FileData::getChildrenRecursive() const
{
    std::vector<FileData*> childrenRecursive;

    for (auto it = mChildrenByFilename.cbegin(); it != mChildrenByFilename.cend(); ++it) {
        childrenRecursive.emplace_back((*it).second);
        // Recurse through any subdirectories.
        if ((*it).second->getType() == FOLDER) {
            std::vector<FileData*> childrenSubdirectory {(*it).second->getChildrenRecursive()};
            childrenRecursive.insert(childrenRecursive.end(), childrenSubdirectory.begin(),
                                     childrenSubdirectory.end());
        }
    }

    return childrenRecursive;
}

const std::string FileData::getROMDirectory()
{
#if defined(__ANDROID__)
    return AndroidVariables::sROMDirectory;
#elif defined(__IOS__)
    std::string dir {Utils::FileSystem::getHomePath() + "/Documents/ROMs"};
    return dir;
#endif

    const std::string& romDirSetting {Settings::getInstance()->getString("ROMDirectory")};
    std::string romDirPath;

    if (romDirSetting == "") {
        romDirPath = Utils::FileSystem::getHomePath() + "/ROMs/";
    }
    else {
        romDirPath = romDirSetting;
        // Expand home path if ~ is used.
        romDirPath = Utils::FileSystem::expandHomePath(romDirPath);

#if defined(_WIN64)
        if (romDirPath.back() != '\\')
            romDirPath = romDirPath + "\\";
#else
        if (romDirPath.back() != '/')
            romDirPath = romDirPath + "/";
#endif
    }

    // If %ESPATH% is used for the ROM path configuration, then expand it to the binary
    // directory of EmuSPLASH.
    romDirPath = Utils::String::replace(romDirPath, "%ESPATH%", Utils::FileSystem::getExePath());

    return romDirPath;
}

const std::vector<std::string> FileData::getExtraROMDirectories()
{
    std::vector<std::string> directories;
#if !defined(__ANDROID__) && !defined(__IOS__)
    const std::string main {getROMDirectory()};
    for (const char* setting : {"ROMDirectory2", "ROMDirectory3"}) {
        std::string directory {Utils::String::trim(Settings::getInstance()->getString(setting))};
        if (directory.empty())
            continue;
        directory = Utils::FileSystem::expandHomePath(directory);
#if defined(_WIN64)
        if (directory.back() != '\\')
            directory += "\\";
#else
        if (directory.back() != '/')
            directory += "/";
#endif
        if (directory != main &&
            std::find(directories.cbegin(), directories.cend(), directory) == directories.cend())
            directories.emplace_back(directory);
    }
#endif
    return directories;
}

const std::string FileData::getMediaDirectory()
{
#if defined(__IOS__)
    return Utils::FileSystem::getAppDataDirectory() + "/downloaded_media/";
#endif

    const std::string& mediaDirSetting {Settings::getInstance()->getString("MediaDirectory")};
    std::string mediaDirPath;

    if (mediaDirSetting.empty()) {
        mediaDirPath = Utils::FileSystem::getAppDataDirectory() + "/downloaded_media/";
    }
    else {
        mediaDirPath = mediaDirSetting;
        // Expand home path if ~ is used.
        mediaDirPath = Utils::FileSystem::expandHomePath(mediaDirPath);

        // If %ESPATH% is used for the media directory configuration, then expand it to the
        // binary directory of EmuSPLASH.
        mediaDirPath =
            Utils::String::replace(mediaDirPath, "%ESPATH%", Utils::FileSystem::getExePath());

        if (mediaDirPath.back() != '/')
            mediaDirPath = mediaDirPath + "/";
    }

    return mediaDirPath;
}

const std::string FileData::getMediafilePath(const std::string& subdirectory) const
{
    std::string subFolders;

    // Extract possible subfolders from the path.
    if (mEnvData->mStartPath != "")
        subFolders =
            mEnvData->removeStartPath(Utils::FileSystem::getParent(mPath));

    const std::string tempPath {getMediaDirectory() + mSystemName + "/" + subdirectory +
                                subFolders + "/" + getDisplayName()};

    // Look for an image file in the media directory.
    for (auto& extension : sImageExtensions) {
        const std::string mediaPath {tempPath + extension};
        if (Utils::FileSystem::exists(mediaPath))
            return mediaPath;
    }

    return "";
}

const std::string FileData::getImagePath() const
{
    // Look for a mix image (a combination of screenshot, 2D/3D box and marquee).
    std::string image {getMediafilePath("miximages")};

    if (image != "")
        return image;

    // If no mix image was found, try screenshot instead.
    image = getMediafilePath("screenshots");
    if (image != "")
        return image;

    // If no screenshot image was found, try title screen instead.
    image = getMediafilePath("titlescreens");
    if (image != "")
        return image;

    // If no screenshot was found either, try cover.
    return getMediafilePath("covers");
}

const std::string FileData::get3DBoxPath() const
{
    // Return path to the 3D box image.
    return getMediafilePath("3dboxes");
}

const std::string FileData::getBackCoverPath() const
{
    // Return path to the box back cover image.
    return getMediafilePath("backcovers");
}

const std::string FileData::getCoverPath() const
{
    // Return path to the box cover image.
    return getMediafilePath("covers");
}

const std::string FileData::getFanArtPath() const
{
    // Return path to the fan art image.
    return getMediafilePath("fanart");
}

const std::string FileData::getMarqueePath() const
{
    // Return path to the marquee image.
    return getMediafilePath("marquees");
}

const std::string FileData::getPhysicalMediaPath() const
{
    // Return path to the physical media image.
    return getMediafilePath("physicalmedia");
}

const std::string FileData::getMiximagePath() const
{
    // Return path to the miximage.
    return getMediafilePath("miximages");
}

const std::string FileData::getScreenshotPath() const
{
    // Return path to the screenshot image.
    return getMediafilePath("screenshots");
}

const std::string FileData::getTitleScreenPath() const
{
    // Return path to the title screen image.
    return getMediafilePath("titlescreens");
}

const std::string FileData::getCustomImagePath() const
{
    // Return path to the custom image.
    return getMediafilePath("custom");
}

const std::string FileData::getVideoPath() const
{
    std::string subFolders;

    // Extract possible subfolders from the path.
    if (mEnvData->mStartPath != "")
        subFolders =
            mEnvData->removeStartPath(Utils::FileSystem::getParent(mPath));

    const std::string tempPath {getMediaDirectory() + mSystemName + "/videos" + subFolders + "/" +
                                getDisplayName()};

    // Look for media in the media directory.
    for (auto& extension : sVideoExtensions) {
        const std::string mediaPath {tempPath + extension};
        if (Utils::FileSystem::exists(mediaPath))
            return mediaPath;
    }

    return "";
}

const std::string FileData::getManualPath() const
{
    const std::vector<std::string> extList {".pdf"};
    std::string subFolders;

    // Extract possible subfolders from the path.
    if (mEnvData->mStartPath != "")
        subFolders =
            mEnvData->removeStartPath(Utils::FileSystem::getParent(mPath));

    const std::string tempPath {getMediaDirectory() + mSystemName + "/manuals" + subFolders + "/" +
                                getDisplayName()};

    // Look for manuals in the media directory.
    for (size_t i {0}; i < extList.size(); ++i) {
        std::string mediaPath {tempPath + extList[i]};
        if (Utils::FileSystem::exists(mediaPath))
            return mediaPath;
    }

    return "";
}

const std::vector<FileData*>& FileData::getChildrenListToDisplay()
{
    FileFilterIndex* idx {mSystem->getIndex()};
    if (idx->isFiltered() || UIModeController::getInstance()->isUIModeKid()) {
        mFilteredChildren.clear();
        for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
            if (idx->showFile((*it))) {
                mFilteredChildren.emplace_back(*it);
            }
        }
        return mFilteredChildren;
    }
    else {
        return mChildren;
    }
}

std::vector<FileData*> FileData::getFilesRecursive(unsigned int typeMask,
                                                   bool displayedOnly,
                                                   bool countAllGames) const
{
    std::vector<FileData*> out;
    FileFilterIndex* idx {mSystem->getIndex()};

    for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
        if ((*it)->getType() & typeMask) {
            if (!displayedOnly || !idx->isFiltered() || idx->showFile(*it)) {
                if (countAllGames)
                    out.emplace_back(*it);
                else if ((*it)->getCountAsGame())
                    out.emplace_back(*it);
            }
        }
        if ((*it)->getChildren().size() > 0) {
            std::vector<FileData*> subChildren {(*it)->getFilesRecursive(typeMask, displayedOnly)};
            if (countAllGames) {
                out.insert(out.cend(), subChildren.cbegin(), subChildren.cend());
            }
            else {
                for (auto it2 = subChildren.cbegin(); it2 != subChildren.cend(); ++it2) {
                    if ((*it2)->getCountAsGame())
                        out.emplace_back(*it2);
                }
            }
        }
    }

    return out;
}

std::vector<FileData*> FileData::getScrapeFilesRecursive(bool includeFolders,
                                                         bool excludeRecursively,
                                                         bool respectExclusions) const
{
    std::vector<FileData*> out;

    for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
        if (includeFolders && (*it)->getType() == FOLDER) {
            if (!(respectExclusions && (*it)->getExcludeFromScraper()))
                out.emplace_back(*it);
        }
        else if ((*it)->getType() == GAME) {
            if (!(respectExclusions && (*it)->getExcludeFromScraper()))
                out.emplace_back(*it);
        }

        // If the flag has been passed to exclude directories recursively, then skip the entire
        // folder at this point if the folder is marked for scrape exclusion.
        if (excludeRecursively && (*it)->getType() == FOLDER && (*it)->getExcludeFromScraper())
            continue;

        if ((*it)->getChildren().size() > 0) {
            std::vector<FileData*> subChildren {(*it)->getScrapeFilesRecursive(
                includeFolders, excludeRecursively, respectExclusions)};
            out.insert(out.cend(), subChildren.cbegin(), subChildren.cend());
        }
    }

    return out;
}

const bool FileData::isArcadeAsset() const
{
    const std::string& stem {Utils::FileSystem::getStem(mPath)};
    return ((mSystem && (mSystem->hasPlatformId(PlatformIds::ARCADE) ||
                         mSystem->hasPlatformId(PlatformIds::SNK_NEO_GEO))) &&
            (MameNames::getInstance().isBios(stem) || MameNames::getInstance().isDevice(stem)));
}

const bool FileData::isArcadeGame() const
{
    const std::string& stem {Utils::FileSystem::getStem(mPath)};
    return ((mSystem && (mSystem->hasPlatformId(PlatformIds::ARCADE) ||
                         mSystem->hasPlatformId(PlatformIds::SNK_NEO_GEO))) &&
            (!MameNames::getInstance().isBios(stem) && !MameNames::getInstance().isDevice(stem)));
}

void FileData::addChild(FileData* file)
{
    assert(mType == FOLDER);
    if (!mSystem->getFlattenFolders())
        assert(file->getParent() == nullptr);

    const std::string& key {file->getKey()};
    if (mChildrenByFilename.find(key) == mChildrenByFilename.cend()) {
        mChildrenByFilename[key] = file;
        mChildren.emplace_back(file);
        file->mParent = this;
    }
}

void FileData::removeChild(FileData* file)
{
    assert(mType == FOLDER);
    assert(file->getParent() == this);
    mChildrenByFilename.erase(file->getKey());
    for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
        if (*it == file) {
            file->mParent = nullptr;
            mChildren.erase(it);
            return;
        }
    }

    // File somehow wasn't in our children.
    assert(false);
}

void FileData::sort(ComparisonFunction& comparator,
                    std::pair<unsigned int, unsigned int>& gameCount)
{
    mOnlyFolders = true;
    mHasFolders = false;
    bool foldersOnTop {Settings::getInstance()->getBool("FoldersOnTop")};
    bool showHiddenGames {Settings::getInstance()->getBool("ShowHiddenGames")};
    bool isKidMode {UIModeController::getInstance()->isUIModeKid()};
    std::vector<FileData*> mChildrenFolders;
    std::vector<FileData*> mChildrenOthers;

    if (mSystem->isGroupedCustomCollection())
        gameCount = {0, 0};

    if (!showHiddenGames) {
        for (auto it = mChildren.begin(); it != mChildren.end();) {
            // If the option to hide hidden games has been set and the game is hidden,
            // then skip it. Normally games are hidden during loading of the gamelists in
            // Gamelist::parseGamelist() and this code should only run when a user has marked
            // an entry manually as hidden. So upon the next application startup, this game
            // should be filtered already at that earlier point.
            if ((*it)->getHidden())
                it = mChildren.erase(it);
            // Also hide folders where all its entries have been hidden, unless it's a
            // grouped custom collection.
            else if ((*it)->getType() == FOLDER && (*it)->getChildren().size() == 0 &&
                     !(*it)->getSystem()->isGroupedCustomCollection())
                it = mChildren.erase(it);
            else
                ++it;
        }
    }

    // The main custom collections view is sorted during startup in CollectionSystemsManager.
    // The individual collections are however sorted as any normal systems/folders.
    if (mSystem->isCollection() && mSystem->getFullName() == "collections") {
        std::pair<unsigned int, unsigned int> tempGameCount {0, 0};
        for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
            if ((*it)->getChildren().size() > 0)
                (*it)->sort(comparator, gameCount);
            tempGameCount.first += gameCount.first;
            tempGameCount.second += gameCount.second;
            gameCount = {0, 0};
        }
        gameCount = tempGameCount;
        return;
    }

    if (foldersOnTop) {
        for (unsigned int i {0}; i < mChildren.size(); ++i) {
            if (mChildren[i]->getType() == FOLDER) {
                mChildrenFolders.emplace_back(mChildren[i]);
            }
            else {
                mChildrenOthers.emplace_back(mChildren[i]);
                mOnlyFolders = false;
            }
        }

        // If the requested sorting is not by name, then sort in ascending name order as a first
        // step, in order to get a correct secondary sorting.
        if (getSortTypeFromString("name, ascending").comparisonFunction != comparator &&
            getSortTypeFromString("name, descending").comparisonFunction != comparator) {
            std::stable_sort(mChildrenFolders.begin(), mChildrenFolders.end(),
                             getSortTypeFromString("name, ascending").comparisonFunction);
            std::stable_sort(mChildrenOthers.begin(), mChildrenOthers.end(),
                             getSortTypeFromString("name, ascending").comparisonFunction);
        }

        if (foldersOnTop)
            std::stable_sort(mChildrenFolders.begin(), mChildrenFolders.end(), comparator);

        std::stable_sort(mChildrenOthers.begin(), mChildrenOthers.end(), comparator);

        mChildren.erase(mChildren.begin(), mChildren.end());
        mChildren.reserve(mChildrenFolders.size() + mChildrenOthers.size());
        mChildren.insert(mChildren.end(), mChildrenFolders.begin(), mChildrenFolders.end());
        mChildren.insert(mChildren.end(), mChildrenOthers.begin(), mChildrenOthers.end());
    }
    else {
        // If the requested sorting is not by name, then sort in ascending name order as a first
        // step, in order to get a correct secondary sorting.
        if (getSortTypeFromString("name, ascending").comparisonFunction != comparator &&
            getSortTypeFromString("name, descending").comparisonFunction != comparator)
            std::stable_sort(mChildren.begin(), mChildren.end(),
                             getSortTypeFromString("name, ascending").comparisonFunction);

        std::stable_sort(mChildren.begin(), mChildren.end(), comparator);
    }

    for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
        // Game count, which will be displayed in the system view.
        if ((*it)->getType() == GAME && (*it)->getCountAsGame()) {
            if (!isKidMode || (isKidMode && (*it)->getKidgame())) {
                ++gameCount.first;
                if ((*it)->getFavorite())
                    ++gameCount.second;
            }
        }

        if ((*it)->getType() != FOLDER)
            mOnlyFolders = false;
        else
            mHasFolders = true;

        // Iterate through any child folders.
        if ((*it)->getChildren().size() > 0)
            (*it)->sort(comparator, gameCount);
    }

    if (mSystem->isGroupedCustomCollection())
        mGameCount = gameCount;
}

void FileData::sortFavoritesOnTop(ComparisonFunction& comparator,
                                  std::pair<unsigned int, unsigned int>& gameCount)
{
    mOnlyFolders = true;
    mHasFolders = false;
    bool foldersOnTop {Settings::getInstance()->getBool("FoldersOnTop")};
    bool showHiddenGames {Settings::getInstance()->getBool("ShowHiddenGames")};
    bool isKidMode {UIModeController::getInstance()->isUIModeKid()};
    std::vector<FileData*> mChildrenFolders;
    std::vector<FileData*> mChildrenFavoritesFolders;
    std::vector<FileData*> mChildrenFavorites;
    std::vector<FileData*> mChildrenOthers;

    if (mSystem->isGroupedCustomCollection())
        gameCount = {0, 0};

    // The main custom collections view is sorted during startup in CollectionSystemsManager.
    // The individual collections are however sorted as any normal systems/folders.
    if (mSystem->isCollection() && mSystem->getFullName() == "collections") {
        std::pair<unsigned int, unsigned int> tempGameCount = {0, 0};
        for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
            if ((*it)->getChildren().size() > 0)
                (*it)->sortFavoritesOnTop(comparator, gameCount);
            tempGameCount.first += gameCount.first;
            tempGameCount.second += gameCount.second;
            gameCount = {0, 0};
        }
        gameCount = tempGameCount;
        return;
    }

    for (unsigned int i {0}; i < mChildren.size(); ++i) {
        // If the option to hide hidden games has been set and the game is hidden,
        // then skip it. Normally games are hidden during loading of the gamelists in
        // Gamelist::parseGamelist() and this code should only run when a user has marked
        // an entry manually as hidden. So upon the next application startup, this game
        // should be filtered already at that earlier point.
        if (!showHiddenGames && mChildren[i]->getHidden())
            continue;
        // Also hide folders where all its entries have been hidden.
        else if (mChildren[i]->getType() == FOLDER && mChildren[i]->getChildren().size() == 0)
            continue;

        // Game count, which will be displayed in the system view.
        if (mChildren[i]->getType() == GAME && mChildren[i]->getCountAsGame()) {
            if (!isKidMode || (isKidMode && mChildren[i]->getKidgame())) {
                ++gameCount.first;
                if (mChildren[i]->getFavorite())
                    ++gameCount.second;
            }
        }

        if (foldersOnTop && mChildren[i]->getType() == FOLDER) {
            if (!mChildren[i]->getFavorite())
                mChildrenFolders.emplace_back(mChildren[i]);
            else
                mChildrenFavoritesFolders.emplace_back(mChildren[i]);
        }
        else if (mChildren[i]->getFavorite()) {
            mChildrenFavorites.emplace_back(mChildren[i]);
        }
        else {
            mChildrenOthers.emplace_back(mChildren[i]);
        }

        if (mChildren[i]->getType() != FOLDER)
            mOnlyFolders = false;
        else
            mHasFolders = true;
    }

    if (mSystem->isGroupedCustomCollection())
        mGameCount = gameCount;

    // If there are favorite folders and this is a mixed list, then don't handle these
    // separately but instead merge them into the same vector. This is a quite wasteful
    // approach but the scenario where a user has a mixed folder and files list and marks
    // some folders as favorites is probably a rare situation.
    if (!mOnlyFolders && mChildrenFavoritesFolders.size() > 0) {
        mChildrenFolders.insert(mChildrenFolders.end(), mChildrenFavoritesFolders.begin(),
                                mChildrenFavoritesFolders.end());
        mChildrenFavoritesFolders.erase(mChildrenFavoritesFolders.begin(),
                                        mChildrenFavoritesFolders.end());
        std::stable_sort(mChildrenFolders.begin(), mChildrenFolders.end(),
                         getSortTypeFromString("name, ascending").comparisonFunction);
    }

    // If the requested sorting is not by name, then sort in ascending name order as a first
    // step, in order to get a correct secondary sorting.
    if (getSortTypeFromString("name, ascending").comparisonFunction != comparator &&
        getSortTypeFromString("name, descending").comparisonFunction != comparator) {
        std::stable_sort(mChildrenFolders.begin(), mChildrenFolders.end(),
                         getSortTypeFromString("name, ascending").comparisonFunction);
        std::stable_sort(mChildrenFavoritesFolders.begin(), mChildrenFavoritesFolders.end(),
                         getSortTypeFromString("name, ascending").comparisonFunction);
        std::stable_sort(mChildrenFavorites.begin(), mChildrenFavorites.end(),
                         getSortTypeFromString("name, ascending").comparisonFunction);
        std::stable_sort(mChildrenOthers.begin(), mChildrenOthers.end(),
                         getSortTypeFromString("name, ascending").comparisonFunction);
    }

    // Sort favorite games and the other games separately.
    if (foldersOnTop) {
        std::stable_sort(mChildrenFavoritesFolders.begin(), mChildrenFavoritesFolders.end(),
                         comparator);
        std::stable_sort(mChildrenFolders.begin(), mChildrenFolders.end(), comparator);
    }
    std::stable_sort(mChildrenFavorites.begin(), mChildrenFavorites.end(), comparator);
    std::stable_sort(mChildrenOthers.begin(), mChildrenOthers.end(), comparator);

    // Iterate through any child favorite folders.
    for (auto it = mChildrenFavoritesFolders.cbegin(); // Line break.
         it != mChildrenFavoritesFolders.cend(); ++it) {
        if ((*it)->getChildren().size() > 0)
            (*it)->sortFavoritesOnTop(comparator, gameCount);
    }

    // Iterate through any child folders.
    for (auto it = mChildrenFolders.cbegin(); it != mChildrenFolders.cend(); ++it) {
        if ((*it)->getChildren().size() > 0)
            (*it)->sortFavoritesOnTop(comparator, gameCount);
    }

    // If folders are not sorted on top, mChildrenFavoritesFolders and mChildrenFolders
    // could be empty. So due to this, step through all mChildren and see if there are
    // any folders that we need to iterate.
    if (mChildrenFavoritesFolders.size() == 0 && mChildrenFolders.size() == 0) {
        for (auto it = mChildren.cbegin(); it != mChildren.cend(); ++it) {
            if ((*it)->getChildren().size() > 0)
                (*it)->sortFavoritesOnTop(comparator, gameCount);
        }
    }

    // Combine the individually sorted favorite games and other games vectors.
    mChildren.erase(mChildren.begin(), mChildren.end());
    mChildren.reserve(mChildrenFavoritesFolders.size() + mChildrenFolders.size() +
                      mChildrenFavorites.size() + mChildrenOthers.size());
    mChildren.insert(mChildren.end(), mChildrenFavoritesFolders.begin(),
                     mChildrenFavoritesFolders.end());
    mChildren.insert(mChildren.end(), mChildrenFolders.begin(), mChildrenFolders.end());
    mChildren.insert(mChildren.end(), mChildrenFavorites.begin(), mChildrenFavorites.end());
    mChildren.insert(mChildren.end(), mChildrenOthers.begin(), mChildrenOthers.end());
}

void FileData::sort(const SortType& type, bool mFavoritesOnTop)
{
    mGameCount = std::make_pair(0, 0);

    if (mFavoritesOnTop)
        sortFavoritesOnTop(*type.comparisonFunction, mGameCount);
    else
        sort(*type.comparisonFunction, mGameCount);

    updateLastPlayedList();
    updateMostPlayedList();
}

void FileData::countGames(std::pair<unsigned int, unsigned int>& gameCount)
{
    bool isKidMode {(Settings::getInstance()->getString("UIMode") == "kid" ||
                     Settings::getInstance()->getBool("ForceKid"))};

    for (unsigned int i {0}; i < mChildren.size(); ++i) {
        if (mChildren[i]->getType() == GAME && mChildren[i]->getCountAsGame()) {
            if (!isKidMode || (isKidMode && mChildren[i]->getKidgame())) {
                ++gameCount.first;
                if (mChildren[i]->getFavorite())
                    ++gameCount.second;
            }
        }
        // Iterate through any folders.
        else if (mChildren[i]->getType() == FOLDER)
            mChildren[i]->countGames(gameCount);
    }
    mGameCount = gameCount;
}

void FileData::updateLastPlayedList()
{
    if (mUpdateListCallback)
        mUpdateListCallback();

    if (!mUpdateChildrenLastPlayed)
        return;

    mChildrenLastPlayed.clear();
    mChildrenLastPlayed = getChildrenRecursive();

    std::stable_sort(mChildrenLastPlayed.begin(), mChildrenLastPlayed.end());
    std::sort(std::begin(mChildrenLastPlayed), std::end(mChildrenLastPlayed),
              [](FileData* a, FileData* b) {
                  return a->metadata.get("lastplayed") > b->metadata.get("lastplayed");
              });
}

void FileData::updateMostPlayedList()
{
    if (mUpdateListCallback)
        mUpdateListCallback();

    if (!mUpdateChildrenMostPlayed)
        return;

    mChildrenMostPlayed.clear();
    mChildrenMostPlayed = getChildrenRecursive();

    std::stable_sort(mChildrenMostPlayed.begin(), mChildrenMostPlayed.end());
    std::sort(std::begin(mChildrenMostPlayed), std::end(mChildrenMostPlayed),
              [](FileData* a, FileData* b) {
                  return a->metadata.getInt("playcount") > b->metadata.getInt("playcount");
              });
}

const FileData::SortType& FileData::getSortTypeFromString(const std::string& desc) const
{
    std::vector<FileData::SortType> SortTypes {FileSorts::SortTypes};

    for (unsigned int i {0}; i < FileSorts::SortTypes.size(); ++i) {
        const FileData::SortType& sort {FileSorts::SortTypes.at(i)};
        if (sort.description == desc)
            return sort;
    }
    // If no type was found then default to "name, ascending".
    return FileSorts::SortTypes.at(0);
}

void FileData::launchGame(const std::vector<std::string>& triedLabels)
{
    Window* window {Window::getInstance()};

    LOG(LogInfo) << "Launching game \"" << this->metadata.get("name") << "\" from system \""
                 << getSourceFileData()->getSystem()->getFullName() << " ("
                 << getSourceFileData()->getSystem()->getName() << ")\"...";

    SystemData* gameSystem {nullptr};
    std::string command;
    std::string alternativeEmulator;
#if defined(__ANDROID__)
    bool launchOnOtherScreen {false};
#endif

    if (mSystem->isCollection())
        gameSystem = SystemData::getSystemByName(mSystemName);
    else
        gameSystem = mSystem;

    // This is just a precaution as getSystemByName() should always return a valid result.
    if (gameSystem == nullptr)
        gameSystem = mSystem;

    alternativeEmulator = gameSystem->getAlternativeEmulator();

    // A MAME game that didn't start is tried with the next emulator of the system.
    std::string nextMameLabel;
    if (!triedLabels.empty() && gameSystem->getName() == "mame") {
        for (const auto& entry : gameSystem->getSystemEnvData()->mLaunchCommands) {
            if (std::find(triedLabels.cbegin(), triedLabels.cend(), entry.second) !=
                triedLabels.cend())
                continue;
            if (!entry.second.empty() && isEmulatorInstalled(entry.first)) {
                nextMameLabel = entry.second;
                break;
            }
        }
    }

    if (!nextMameLabel.empty()) {
        command = gameSystem->getLaunchCommandFromLabel(nextMameLabel);
        LOG(LogInfo) << "FileData::launchGame(): The game didn't start, trying the emulator \""
                     << nextMameLabel << "\"";
    }
    // Check if there is a game-specific alternative emulator configured.
    // This takes precedence over any system-wide alternative emulator configuration.
    else if (Settings::getInstance()->getBool("AlternativeEmulatorPerGame") &&
             !metadata.get("altemulator").empty()) {
        command = gameSystem->getLaunchCommandFromLabel(metadata.get("altemulator"));
        if (command == "") {
            LOG(LogWarning) << "Invalid alternative emulator \"" << metadata.get("altemulator")
                            << "\" configured for game";
        }
        else {
            LOG(LogDebug) << "FileData::launchGame(): Using alternative emulator \""
                          << metadata.get("altemulator")
                          << "\" as configured for the specific game";
        }
    }
    // Check if there is a system-wide alternative emulator configured.
    else if (command == "" && alternativeEmulator != "") {
        command = gameSystem->getLaunchCommandFromLabel(alternativeEmulator);
        if (command == "") {
            LOG(LogWarning) << "Invalid alternative emulator \""
                            << alternativeEmulator.substr(9, alternativeEmulator.length() - 9)
                            << "\" configured for system \"" << gameSystem->getName() << "\"";
        }
        else {
            LOG(LogDebug) << "FileData::launchGame(): Using alternative emulator \""
                          << gameSystem->getAlternativeEmulator() << "\""
                          << " as configured for system \"" << gameSystem->getName() << "\"";
        }
    }
    else {
        if (!mEnvData->mLaunchCommands.front().second.empty()) {
            LOG(LogDebug) << "FileData::launchGame(): Using default emulator \""
                          << mEnvData->mLaunchCommands.front().second << "\"";
        }
        else {
            LOG(LogDebug) << "FileData::launchGame(): Using default emulator";
        }
    }

#if defined(__ANDROID__)
    if (Settings::getInstance()->getBool("LaunchOnOtherScreen")) {
        launchOnOtherScreen = gameSystem->getLaunchOnOtherScreen();
        std::string screenValue {metadata.get("screen")};

        if (screenValue != "" && screenValue != "other" && screenValue != "primary")
            screenValue = "";

        if (screenValue == "") {
            if (launchOnOtherScreen) {
                LOG(LogDebug)
                    << "FileData::launchGame(): Launching game on other screen as configured "
                       "for system \""
                    << gameSystem->getName() << "\"";
            }
            else {
                LOG(LogDebug)
                    << "FileData::launchGame(): Launching game on primary screen as configured "
                       "for system \""
                    << gameSystem->getName() << "\"";
            }
        }
        else if (screenValue == "other") {
            LOG(LogDebug) << "FileData::launchGame(): Launching on other screen as configured "
                             "for the specific game";
            launchOnOtherScreen = true;
        }
        else if (screenValue == "primary") {
            LOG(LogDebug) << "FileData::launchGame(): Launching on primary screen as configured "
                             "for the specific game";
            launchOnOtherScreen = false;
        }
    }
#endif

    if (command.empty())
        command = mEnvData->mLaunchCommands.front().first;

    // The label of the emulator used, to try the next one if a MAME game doesn't start.
    std::string usedLabel;
    for (const auto& entry : gameSystem->getSystemEnvData()->mLaunchCommands) {
        if (entry.first == command) {
            usedLabel = entry.second;
            break;
        }
    }
    const bool mameFallback {gameSystem->getName() == "mame" && !usedLabel.empty()};
    std::vector<std::string> triedNow {triedLabels};
    triedNow.emplace_back(usedLabel);

    std::string commandRaw {command};
#if defined(__linux__) && !defined(__ANDROID__)
    // An added emulator can be made fullscreen once its window has opened.
    bool forceFullscreen {false};
    for (const auto& entry : mEnvData->mLaunchCommands) {
        if (entry.first == commandRaw) {
            forceFullscreen = EmulatorConfig::isForceFullscreen(gameSystem->getName(), entry.second);
            break;
        }
    }
#endif
    std::string romPath {Utils::FileSystem::getEscapedPath(mPath)};
    std::string baseName {Utils::FileSystem::getStem(mPath)};
    std::string romRaw {Utils::FileSystem::getPreferredPath(mPath)};
#if !defined(_WIN64)
    std::string romRawWindows {
        Utils::String::replace(Utils::FileSystem::getPreferredPath(mPath), "/", "\\")};
#endif

    // For the special case where a directory has a supported file extension and is therefore
    // interpreted as a file, check if there is a matching filename inside the directory.
    // This is used as a shortcut to be able to launch games directly inside folders.
    if (mType == GAME && Utils::FileSystem::isDirectory(mPath)) {
        for (std::string& file : Utils::FileSystem::getDirContent(mPath)) {
            if (Utils::FileSystem::getFileName(file) == Utils::FileSystem::getFileName(mPath) &&
                (Utils::FileSystem::isRegularFile(file) || Utils::FileSystem::isSymlink(file))) {
#if defined(__ANDROID__)
                romRaw = file;
#else
                romPath = Utils::FileSystem::getEscapedPath(file);
#endif
                baseName = baseName.substr(0, baseName.find("."));
                break;
            }
        }
    }

    const std::string fileName {baseName + Utils::FileSystem::getExtension(romPath)};
    const std::string esPath {Utils::FileSystem::getExePath()};

#if defined(__ANDROID__)
    // On Android we always run in the background, although the logic is a bit different
    // as we don't need to wake up the application manually.
    bool runInBackground {true};
#else
    bool runInBackground {false};

    // In addition to the global RunInBackground setting it's possible to define this flag
    // per launch command in es_systems.xml.
    size_t inBackgroundPos {command.find("%RUNINBACKGROUND%")};

    if (inBackgroundPos != std::string::npos) {
        runInBackground = true;
        command = Utils::String::replace(command, "%RUNINBACKGROUND%", "");
        // Trim any leading whitespaces as they could cause the script execution to fail.
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](char c) {
                          return !std::isspace(static_cast<unsigned char>(c));
                      }));
    }

    // The global setting RunInBackground isn't used (its menu row is gone): EmuSPLASH waits for the
    // game, which the hotkeys need. A launch command can still ask for it.
    if (Settings::getInstance()->getBool("RunInBackground")) {
        LOG(LogDebug) << "FileData::launchGame(): RunInBackground is ignored, the game is waited "
                         "for";
    }
#endif

#if !defined(_WIN64)
    // Whether to parse .desktop files on Unix or open apps or alias files on macOS.
    bool isShortcut {false};
    size_t enableShortcutsPos {command.find("%ENABLESHORTCUTS%")};

    if (enableShortcutsPos != std::string::npos) {
#if defined(__APPLE__)
        if (Utils::FileSystem::getExtension(romRaw) == ".app")
#else
        if (Utils::FileSystem::getExtension(romRaw) == ".desktop")
#endif
            isShortcut = true;

        command = Utils::String::replace(command, "%ENABLESHORTCUTS%", "");
        // Trim any leading whitespaces as they could cause the script execution to fail.
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](char c) {
                          return !std::isspace(static_cast<unsigned char>(c));
                      }));
    }
#endif

    std::string coreEntry;
    std::string coreName;
    size_t coreEntryPos {0};
    size_t coreFilePos {0};
    bool foundCoreFile {false};
    std::vector<std::string> emulatorCorePaths;
    bool isAndroidApp {false};

#if defined(__ANDROID__)
    std::string androidPackage;
    std::string androidActivity;
    std::string androidAction;
    std::string androidCategory;
    std::string androidMimeType;
    std::string androidData;
    std::map<std::string, std::string> androidExtrasString;
    std::map<std::string, std::string> androidExtrasStringArray;
    std::map<std::string, std::string> androidExtrasInteger;
    std::map<std::string, std::string> androidExtrasBool;
    std::vector<std::string> androidActivityFlags;
#endif

#if defined(_WIN64)
    bool hideWindow {false};

    // If the %HIDEWINDOW% variable is defined, we pass a flag to launchGameWindows() to
    // hide the window. This is intended primarily for hiding console windows when launching
    // scripts (used for example by Steam games and source ports).
    if (command.find("%HIDEWINDOW%") != std::string::npos) {
        hideWindow = true;
        command = Utils::String::replace(command, "%HIDEWINDOW%", "");
        // Trim any leading whitespaces as they could cause the script execution to fail.
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](char c) {
                          return !std::isspace(static_cast<unsigned char>(c));
                      }));
    }

    bool escapeSpecials {false};

    // If calling scripts and links using some binaries like cmd.exe then the special characters
    // &()^=;, must be escaped.
    if (command.find("%ESCAPESPECIALS%") != std::string::npos) {
        escapeSpecials = true;
        command = Utils::String::replace(command, "%ESCAPESPECIALS%", "");
        // Trim any leading whitespaces as they could cause the script execution to fail.
        command.erase(command.begin(), std::find_if(command.begin(), command.end(), [](char c) {
                          return !std::isspace(static_cast<unsigned char>(c));
                      }));
    }
#endif

    // If there's a quotation mark before the %CORE_ variable, then remove it.
    // The closing quotation mark will be removed later below.
    bool hasCoreQuotation {false};
    if (command.find("\"%CORE_") != std::string::npos) {
        command = Utils::String::replace(command, "\"%CORE_", "%CORE_");
        hasCoreQuotation = true;
    }

    coreEntryPos = command.find("%CORE_");
    if (coreEntryPos != std::string::npos) {
        coreFilePos = command.find("%", coreEntryPos + 6);
        if (coreFilePos != std::string::npos)
            coreEntry = command.substr(coreEntryPos + 6, coreFilePos - (coreEntryPos + 6));
    }

    if (coreEntry != "")
        emulatorCorePaths = SystemData::sFindRules.get()->mCores[coreEntry].corePaths;

    // Expand home path if ~ is used.
    command = Utils::FileSystem::expandHomePath(command);

    // Check for any pre-command entry, and if it exists then expand it using the find rules.
    if (command.find("%PRECOMMAND_") != std::string::npos) {
        const std::pair<std::string, FileData::findEmulatorResult> preCommand {
            findEmulator(command, true)};
        // Show an error message if there was no matching emulator entry in es_find_rules.xml.
        if (preCommand.second == FileData::findEmulatorResult::NO_RULES) {
            LOG(LogError)
                << "Couldn't launch game, either there is no emulator entry for pre-command \""
                << preCommand.first << "\" in es_find_rules.xml or there are no rules defined";
            LOG(LogError) << "Raw emulator launch command:";
            LOG(LogError) << commandRaw;

            window->queueInfoPopup(
                Utils::String::format(
                    _("ERROR: MISSING PRE-COMMAND FIND RULES CONFIGURATION FOR '%s'"),
                    preCommand.first.c_str()),
                6000);
            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }
        else if (preCommand.first.empty()) {
            LOG(LogError) << "Couldn't launch game, pre-command not found";
            LOG(LogError) << "Raw emulator launch command:";
            LOG(LogError) << commandRaw;

            std::string emulatorName;
            size_t startPos {0};
            size_t endPos {0};

            if ((startPos = command.find("%PRECOMMAND_")) != std::string::npos) {
                endPos = command.find("%", startPos + 1);
                if (endPos != std::string::npos)
                    emulatorName = command.substr(startPos + 12, endPos - startPos - 12);
            }

            if (emulatorName == "")
                window->queueInfoPopup(
                    _("ERROR: COULDN'T FIND PRE-COMMAND, HAS IT BEEN PROPERLY INSTALLED?"), 6000);
            else
                window->queueInfoPopup(
                    Utils::String::format(
                        _("ERROR: COULDN'T FIND PRE-COMMAND '%s', HAS IT BEEN PROPERLY INSTALLED?"),
                        emulatorName.c_str()),
                    6000);

            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }
        else {
            LOG(LogDebug) << "FileData::launchGame(): Pre-command set to \"" << preCommand.first
                          << "\"";
        }
    }

    // Check that the emulator actually exists, and if so, get its path.
    std::pair<std::string, FileData::findEmulatorResult> emulator;

#if defined(__ANDROID__)
    // Native Android apps and games.
    if (command.find("%ANDROIDAPP%=") != std::string::npos) {
        std::string packageName;
        size_t startPos {command.find("%ANDROIDAPP%=")};
        size_t endPos {command.find(" ", startPos)};
        if (endPos == std::string::npos)
            endPos = command.length();

        packageName = command.substr(startPos + 13, endPos - startPos - 13);
        isAndroidApp = true;

        if (packageName == "%FILEINJECT%") {
            LOG(LogDebug) << "Injecting app info from file \"" + fileName + "\"";
            std::string appString;
            std::ifstream injectFileStream;

            injectFileStream.open(romRaw);
            for (std::string line; getline(injectFileStream, line);) {
                // Remove Windows carriage return characters.
                line = Utils::String::replace(line, "\r", "");
                appString += line;
                if (appString.size() > 4096)
                    break;
            }
            injectFileStream.close();

            if (appString.empty()) {
                LOG(LogDebug) << "FileData::launchGame(): File empty or insufficient permissions, "
                                 "nothing to inject";
                packageName = "";
            }
            else if (appString.size() > 4096) {
                LOG(LogWarning) << "FileData::launchGame(): Injection file exceeding maximum "
                                   "allowed size of 4096 bytes, skipping \""
                                << fileName << "\"";
                packageName = "";
            }
            else {
                packageName = appString;
            }
        }

        if (packageName != "" && packageName != "%FILEINJECT%") {
            LOG(LogInfo) << "Game entry is an Android app: " << packageName;

            size_t separatorPos {packageName.find('/')};

            if (separatorPos != std::string::npos) {
                androidActivity = packageName.substr(separatorPos + 1);
                packageName = packageName.substr(0, separatorPos);
            }

            if (Utils::Platform::Android::checkEmulatorInstalled(packageName, androidActivity)) {
                emulator = std::make_pair(packageName,
                                          FileData::findEmulatorResult::FOUND_ANDROID_PACKAGE);
            }
            else {
                emulator = std::make_pair(packageName, FileData::findEmulatorResult::NOT_FOUND);
            }
        }
        else {
            emulator = std::make_pair(packageName, FileData::findEmulatorResult::NOT_FOUND);
        }
    }
    else {
        emulator = findEmulator(command, false);
    }
#else
    emulator = findEmulator(command, false);
#endif

    // Show an error message if there was no matching emulator entry in es_find_rules.xml.
    if (emulator.second == FileData::findEmulatorResult::NO_RULES) {
        LOG(LogError) << "Couldn't launch game, either there is no emulator entry for \""
                      << emulator.first << "\" in es_find_rules.xml or there are no rules defined";
        LOG(LogError) << "Raw emulator launch command:";
        LOG(LogError) << commandRaw;

        window->queueInfoPopup(
            Utils::String::format(_("ERROR: MISSING EMULATOR FIND RULES CONFIGURATION FOR '%s'"),
                                  emulator.first.c_str()),
            6000);
        window->setAllowTextScrolling(true);
        window->setAllowFileAnimation(true);
        return;
    }
    else if (emulator.second == FileData::findEmulatorResult::NOT_FOUND) {
        if (isAndroidApp) {
            LOG(LogError) << "Couldn't launch app as it does not seem to be installed";
        }
        else {
            LOG(LogError) << "Couldn't launch game, emulator not found";
        }
        LOG(LogError) << "Raw emulator launch command:";
        LOG(LogError) << commandRaw;

        std::string emulatorName;
        size_t startPos {0};
        size_t endPos {0};

        if ((startPos = command.find("%EMULATOR_")) != std::string::npos) {
            endPos = command.find("%", startPos + 1);
            if (endPos != std::string::npos)
                emulatorName = command.substr(startPos + 10, endPos - startPos - 10);
        }
#if defined(__ANDROID__)
        else if ((startPos = command.find("%ANDROIDAPP%=")) != std::string::npos) {
            endPos = command.find(" ", startPos);
            if (endPos == std::string::npos)
                endPos = command.length();

            emulatorName = command.substr(startPos + 13, endPos - startPos - 13);
        }
#endif
        if (isAndroidApp) {
            if (emulatorName == "" || emulatorName == "%FILEINJECT%") {
                window->queueInfoPopup(
                    _("ERROR: COULDN'T FIND APP, HAS IT BEEN PROPERLY INSTALLED?"), 6000);
            }
            else {
                window->queueInfoPopup(
                    Utils::String::format(
                        _("ERROR: COULDN'T FIND APP '%s', HAS IT BEEN PROPERLY INSTALLED?"),
                        emulatorName.c_str()),
                    6000);
            }
        }
        else {
            if (emulatorName == "") {
                window->queueInfoPopup(
                    _("ERROR: COULDN'T FIND EMULATOR, HAS IT BEEN PROPERLY INSTALLED?"), 6000);
            }
            else {
                window->queueInfoPopup(
                    Utils::String::format(
                        _("ERROR: COULDN'T FIND EMULATOR '%s', HAS IT BEEN PROPERLY INSTALLED?"),
                        emulatorName.c_str()),
                    6000);
            }
        }

        window->setAllowTextScrolling(true);
        window->setAllowFileAnimation(true);
        return;
    }
#if defined(_WIN64)
    else {
        std::string emulatorLogPath {Utils::String::replace(
            Utils::String::replace(emulator.first, "%ESPATH%", esPath), "/", "\\")};
        if (emulatorLogPath.front() != '\"' && emulatorLogPath.back() != '\"')
            emulatorLogPath = "\"" + emulatorLogPath + "\"";
        LOG(LogDebug) << "FileData::launchGame(): Emulator set to " << emulatorLogPath;
#else
#if defined(__ANDROID__)
    else if (emulator.second == FileData::findEmulatorResult::FOUND_ANDROID_PACKAGE) {
        androidPackage = emulator.first;
        size_t separatorPos {androidPackage.find('/')};

        if (separatorPos != std::string::npos) {
            androidActivity = androidPackage.substr(separatorPos + 1);
            androidPackage = androidPackage.substr(0, separatorPos);
        }

        LOG(LogDebug) << "FileData::launchGame(): Found emulator package \"" << androidPackage
                      << "\"";
    }
#endif
    else if (!isShortcut) {
        LOG(LogDebug) << "FileData::launchGame(): Emulator set to \""
                      << Utils::String::replace(emulator.first, "%ESPATH%", esPath) << "\"";
#endif
    }

    // If %EMUPATH% is used in es_systems.xml for this system, then check that the core
    // file actually exists.
    size_t emuPathPos {command.find("%EMUPATH%")};
    if (emuPathPos != std::string::npos) {
        bool hasQuotationMark {false};
        unsigned int quotationMarkPos {0};
        if (command.find("\"%EMUPATH%", emuPathPos - 1) != std::string::npos) {
            hasQuotationMark = true;
            quotationMarkPos =
                static_cast<unsigned int>(command.find("\"", emuPathPos + 9) - emuPathPos);
        }
        const size_t spacePos {command.find(" ", emuPathPos + quotationMarkPos)};
        std::string coreRaw;
        std::string coreFile;
        if (spacePos != std::string::npos) {
            coreRaw = command.substr(emuPathPos, spacePos - emuPathPos);
#if defined(_WIN64)
            coreFile =
                Utils::FileSystem::getParent(Utils::String::replace(emulator.first, "\"", "")) +
                command.substr(emuPathPos + 9, spacePos - emuPathPos - 9);
            coreFile = Utils::String::replace(coreFile, "/", "\\");
#else
            coreFile = Utils::FileSystem::getParent(emulator.first) +
                       command.substr(emuPathPos + 9, spacePos - emuPathPos - 9);
#endif
            if (hasQuotationMark) {
                coreRaw.pop_back();
                coreFile.pop_back();
            }
            if (!Utils::FileSystem::isRegularFile(coreFile) &&
                !Utils::FileSystem::isSymlink(coreFile)) {
                LOG(LogError) << "Couldn't launch game, emulator core file \""
                              << Utils::FileSystem::getFileName(coreFile) << "\" not found";
                LOG(LogError) << "Raw emulator launch command:";
                LOG(LogError) << commandRaw;

                window->queueInfoPopup(
                    Utils::String::format(
                        _("ERROR: COULDN'T FIND EMULATOR CORE FILE '%s'"),
                        Utils::String::toUpper(Utils::FileSystem::getFileName(coreFile)).c_str()),
                    6000);
                window->setAllowTextScrolling(true);
                window->setAllowFileAnimation(true);
                return;
            }
            else {
                if (hasQuotationMark) {
                    command = command.replace(emuPathPos + quotationMarkPos, 1, "");
                    --emuPathPos;
                    command = command.replace(emuPathPos, 1, "");
                }
                coreFile = Utils::FileSystem::getEscapedPath(coreFile);
                command = command.replace(emuPathPos, coreRaw.size(), coreFile);
            }
        }
        else {
            LOG(LogError) << "Invalid entry in systems configuration file es_systems.xml";
            LOG(LogError) << "Raw emulator launch command:";
            LOG(LogError) << commandRaw;

            window->queueInfoPopup(_("ERROR: INVALID ENTRY IN SYSTEMS CONFIGURATION FILE"), 6000);
            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }
    }

    // Error handling in case of no core find rule.
    if (coreEntry != "" && emulatorCorePaths.empty()) {
        LOG(LogError) << "Couldn't launch game, either there is no core entry for \"" << coreEntry
                      << "\" in es_find_rules.xml or there are no corepath rules defined";
        LOG(LogError) << "Raw emulator launch command:";
        LOG(LogError) << commandRaw;

        window->queueInfoPopup(
            Utils::String::format(_("ERROR: MISSING CORE CONFIGURATION FOR '%s'"),
                                  coreEntry.c_str()),
            6000);
        window->setAllowTextScrolling(true);
        window->setAllowFileAnimation(true);
        return;
    }

    // If a %CORE_ find rule entry is used in es_systems.xml for this system, then try to find
    // the emulator core using the rules defined in es_find_rules.xml.
    for (std::string& path : emulatorCorePaths) {
        // The position of the %CORE_ variable could have changed as there may have been an
        // %EMULATOR_ variable that was substituted for the actual emulator.
        coreEntryPos = command.find("%CORE_");
        coreFilePos = command.find("%", coreEntryPos + 6);

        size_t separatorPos;
        size_t quotePos {hasCoreQuotation ? command.find("\"", coreFilePos) : std::string::npos};
        if (quotePos == std::string::npos)
            separatorPos = command.find(" ", coreFilePos);
        else
            separatorPos = quotePos;

        if (separatorPos != std::string::npos) {
            coreName = command.substr(coreFilePos + 2, separatorPos - (coreFilePos + 2));

#if defined(_WIN64)
            std::string coreFile {Utils::FileSystem::expandHomePath(path + "\\" + coreName)};
#else
            std::string coreFile {Utils::FileSystem::expandHomePath(path + "/" + coreName)};
#endif

            // Expand %EMUPATH% if it has been used in the %CORE_ variable.
            size_t stringPos {coreFile.find("%EMUPATH%")};
            if (stringPos != std::string::npos) {
#if defined(_WIN64)
                coreFile = coreFile.replace(
                    stringPos, 9,
                    Utils::FileSystem::getParent(Utils::String::replace(emulator.first, "\"", "")));
                coreFile = Utils::String::replace(coreFile, "/", "\\");
#else
                coreFile =
                    coreFile.replace(stringPos, 9, Utils::FileSystem::getParent(emulator.first));
#endif
            }

            // Expand %ESPATH% if it has been used in the %CORE_ variable.
            stringPos = coreFile.find("%ESPATH%");
            if (stringPos != std::string::npos) {
                coreFile = coreFile.replace(stringPos, 8, esPath);
#if defined(_WIN64)
                coreFile = Utils::String::replace(coreFile, "/", "\\");
#endif
            }

            if (Utils::FileSystem::isRegularFile(coreFile) ||
                Utils::FileSystem::isSymlink(coreFile)) {
                foundCoreFile = true;
                // Escape any blankspaces.
                if (coreFile.find(" ") != std::string::npos)
                    coreFile = Utils::FileSystem::getEscapedPath(coreFile);
                command.replace(coreEntryPos,
                                separatorPos - coreEntryPos + (hasCoreQuotation ? 1 : 0), coreFile);
                break;
            }
        }
        else {
            LOG(LogError) << "Invalid entry in systems configuration file es_systems.xml";
            LOG(LogError) << "Raw emulator launch command:";
            LOG(LogError) << commandRaw;

            window->queueInfoPopup(_("ERROR: INVALID ENTRY IN SYSTEMS CONFIGURATION FILE"), 6000);
            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }
    }
    if (!foundCoreFile && coreName.size() > 0) {
        LOG(LogError) << "Couldn't launch game, emulator core file \""
                      << coreName.substr(0, coreName.size()) << "\" not found";
        LOG(LogError) << "Raw emulator launch command:";
        LOG(LogError) << commandRaw;
        LOG(LogError)
            << "Tried to find the core file using these paths as defined by es_find_rules.xml:";
        LOG(LogError) << Utils::String::vectorToDelimitedString(emulatorCorePaths, ", ");

        window->queueInfoPopup(
            Utils::String::format(
                _("ERROR: COULDN'T FIND EMULATOR CORE FILE '%s'"),
                Utils::String::toUpper(coreName.substr(0, coreName.size())).c_str()),
            6000);
        window->setAllowTextScrolling(true);
        window->setAllowFileAnimation(true);
        return;
    }

    std::string startDirectory;
    const size_t startDirPos {command.find("%STARTDIR%")};

    if (startDirPos != std::string::npos) {
        bool invalidEntry {false};

        if (startDirPos + 12 >= command.size())
            invalidEntry = true;
        else if (command[startDirPos + 10] != '=')
            invalidEntry = true;

        if (!invalidEntry && command[startDirPos + 11] == '\"') {
            size_t closingQuotation {command.find("\"", startDirPos + 12)};

            if (closingQuotation != std::string::npos) {
                startDirectory =
                    command.substr(startDirPos + 12, closingQuotation - startDirPos - 12);
                command = command.replace(startDirPos, closingQuotation - startDirPos + 2, "");
            }
            else {
                invalidEntry = true;
            }
        }
        else if (!invalidEntry) {
            const size_t spacePos {command.find(" ", startDirPos)};
            if (spacePos != std::string::npos) {
                startDirectory = command.substr(startDirPos + 11, spacePos - startDirPos - 11);
                command = command.replace(startDirPos, spacePos - startDirPos + 1, "");
            }
            else {
                startDirectory = command.substr(startDirPos + 11, command.size() - startDirPos);
                command = command.replace(startDirPos, command.size() - startDirPos, "");
            }
        }

        if (invalidEntry) {
            LOG(LogError) << "Couldn't launch game, invalid %STARTDIR% entry";
            LOG(LogError) << "Raw emulator launch command:";
            LOG(LogError) << commandRaw;

            window->queueInfoPopup(
                Utils::String::format(_("ERROR: INVALID %s VARIABLE ENTRY"), "%STARTDIR%"), 6000);
            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }

        if (startDirectory != "") {
            startDirectory = Utils::FileSystem::expandHomePath(startDirectory);
#if defined(_WIN64)
            startDirectory = Utils::String::replace(
                startDirectory, "%EMUDIR%",
                Utils::FileSystem::getParent(Utils::String::replace(emulator.first, "\"", "")));

            startDirectory = Utils::String::replace(
                startDirectory, "%GAMEDIR%",
                Utils::FileSystem::getParent(Utils::String::replace(romPath, "\"", "")));

            startDirectory = Utils::String::replace(startDirectory, "%GAMEENTRYDIR%",
                                                    Utils::String::replace(romPath, "\"", ""));
#else
            startDirectory = Utils::String::replace(
                startDirectory, "%EMUDIR%",
                Utils::FileSystem::getParent(Utils::String::replace(emulator.first, "\\", "")));

            startDirectory = Utils::String::replace(
                startDirectory, "%GAMEDIR%",
                Utils::FileSystem::getParent(Utils::String::replace(romPath, "\\", "")));

            startDirectory = Utils::String::replace(startDirectory, "%GAMEENTRYDIR%",
                                                    Utils::String::replace(romPath, "\\", ""));
#endif
            if (!Utils::FileSystem::isDirectory(startDirectory)) {
                Utils::FileSystem::createDirectory(startDirectory);

                if (!Utils::FileSystem::isDirectory(startDirectory)) {
                    LOG(LogError) << "Couldn't launch game, directory \"" << startDirectory
                                  << "\" defined by %STARTDIR% could not be created, "
                                     "permission problems?";
                    LOG(LogError) << "Raw emulator launch command:";
                    LOG(LogError) << commandRaw;

                    window->queueInfoPopup(
                        Utils::String::format(_("ERROR: DIRECTORY DEFINED BY %s COULD NOT BE "
                                                "CREATED, PERMISSION PROBLEMS?"),
                                              "%STARTDIR%"),
                        6000);
                    window->setAllowTextScrolling(true);
                    window->setAllowFileAnimation(true);
                    return;
                }
            }
#if defined(_WIN64)
            startDirectory = Utils::String::replace(startDirectory, "/", "\\");
#else
            startDirectory = Utils::FileSystem::getEscapedPath(startDirectory);
#endif
            LOG(LogDebug) << "FileData::launchGame(): Setting start directory to \""
                          << startDirectory << "\"";
        }
    }

    std::string injectFile;
    size_t injectPos {command.find("%INJECT%")};

    while (injectPos != std::string::npos) {
        if (injectPos != std::string::npos) {
            bool invalidEntry {false};

            if (injectPos + 10 >= command.size())
                invalidEntry = true;
            else if (command[injectPos + 8] != '=')
                invalidEntry = true;

            if (!invalidEntry && command[injectPos + 9] == '\"') {
                size_t closingQuotation {command.find("\"", injectPos + 10)};

                if (closingQuotation != std::string::npos) {
                    injectFile = command.substr(injectPos + 10, closingQuotation - injectPos - 10);
                    command = command.replace(injectPos, closingQuotation - injectPos + 2, "");
                }
                else {
                    invalidEntry = true;
                }
            }
            else if (!invalidEntry) {
                const size_t spacePos {command.find(" ", injectPos)};
                if (spacePos != std::string::npos) {
                    injectFile = command.substr(injectPos + 9, spacePos - injectPos - 9);
                    command = command.replace(injectPos, spacePos - injectPos + 1, "");
                }
                else {
                    injectFile = command.substr(injectPos + 9, command.size() - injectPos);
                    command = command.replace(injectPos, command.size() - injectPos, "");
                }
            }

            if (invalidEntry) {
                LOG(LogError) << "Couldn't launch game, invalid %INJECT% entry";
                LOG(LogError) << "Raw emulator launch command:";
                LOG(LogError) << commandRaw;

                window->queueInfoPopup(
                    Utils::String::format(_("ERROR: INVALID %s VARIABLE ENTRY"), "%INJECT%"), 6000);
                window->setAllowTextScrolling(true);
                window->setAllowFileAnimation(true);
                return;
            }
        }

        if (injectFile != "") {
#if defined(_WIN64)
            injectFile = Utils::String::replace(injectFile, "\\", "/");
            injectFile = Utils::String::replace(injectFile, "%BASENAME%",
                                                Utils::String::replace(baseName, "\"", ""));
            if (injectFile == "%ROM%") {
                injectFile = Utils::String::replace(injectFile, "%ROM%",
                                                    Utils::String::replace(romRaw, "\"", ""));
            }
            else {
                if (injectFile.size() < 3 || !(injectFile[1] == ':' && injectFile[2] == '/'))
                    injectFile =
                        Utils::FileSystem::getParent(Utils::String::replace(romPath, "\"", "")) +
                        "/" + injectFile;
            }
            injectFile = Utils::String::replace(injectFile, "/", "\\");
#else
            injectFile = Utils::String::replace(injectFile, "%BASENAME%",
                                                Utils::String::replace(baseName, "\\", ""));
            if (injectFile == "%ROM%") {
                injectFile = Utils::String::replace(injectFile, "%ROM%",
                                                    Utils::String::replace(romRaw, "\\", ""));
            }
            else {
                if (injectFile.front() != '/')
                    injectFile =
                        Utils::FileSystem::getParent(Utils::String::replace(romPath, "\\", "")) +
                        "/" + injectFile;
            }
#endif
            if (Utils::FileSystem::isRegularFile(injectFile) ||
                Utils::FileSystem::isSymlink(injectFile)) {
                LOG(LogDebug) << "FileData::launchGame(): Injecting from file \"" << injectFile
                              << "\"";
                std::string arguments;
                std::ifstream injectFileStream;
#if defined(_WIN64)
                injectFileStream.open(Utils::String::stringToWideString(injectFile));
#else
                injectFileStream.open(injectFile);
#endif
                for (std::string line; getline(injectFileStream, line);) {
                    // Remove Windows carriage return characters.
                    line = Utils::String::replace(line, "\r", "");
                    arguments += line;
                    if (arguments.size() > 4096)
                        break;
                }
                injectFileStream.close();

                if (arguments.empty()) {
                    LOG(LogDebug)
                        << "FileData::launchGame(): File empty or insufficient permissions, "
                           "nothing to inject";
                }
                else if (arguments.size() > 4096) {
                    LOG(LogWarning) << "FileData::launchGame(): Injection file exceeding maximum "
                                       "allowed size of 4096 bytes, skipping \""
                                    << injectFile << "\"";
                }
                else {
                    command.insert(injectPos, arguments + " ");
                }
            }
            else {
                LOG(LogDebug) << "FileData::launchGame(): File \"" << injectFile
                              << "\" does not exist, nothing to inject";
            }
        }

        injectPos = command.find("%INJECT%");
    }

#if defined(_WIN64)
    if (escapeSpecials) {
        bool foundSpecial {false};

        // The special characters need to be procesed in this order.
        std::string specialCharacters {"^&()=;,"};

        for (size_t i {0}; i < specialCharacters.size(); ++i) {
            // Don't modify this code, it breaks easily.
            const std::string special(1, specialCharacters[i]);
            if (romPath.find(special) != std::string::npos) {
                romPath = Utils::String::replace(romPath, special, "^" + special);
                foundSpecial = true;
            }
        }

        if (foundSpecial)
            romPath = Utils::String::replace(romPath, " ", "^ ");
    }
#endif

#if !defined(_WIN64)
#if defined(__APPLE__)
    if (isShortcut) {
        if (Utils::FileSystem::exists(Utils::String::replace(romPath, "\\", ""))) {
            LOG(LogInfo) << "Opening app or alias file \""
                         << Utils::String::replace(romPath, "\\", "") << "\"";
            command = Utils::String::replace(command, emulator.first, "open -W -a");
        }
        else {
            LOG(LogError) << "App or alias file \"" << romPath
                          << "\" doesn't exist or is unreadable";
            window->queueInfoPopup(_("ERROR: APP OR ALIAS FILE DOESN'T EXIST OR IS UNREADABLE"),
                                   6000);
            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }
    }
#else
    if (isShortcut) {
        // Note that the following is not an attempt to implement the entire FreeDesktop standard
        // for .desktop files, for example argument parsing is not really usable in this context.
        if (Utils::FileSystem::exists(Utils::String::replace(romPath, "\\", "")) &&
            !Utils::FileSystem::isDirectory(Utils::String::replace(romPath, "\\", ""))) {
            LOG(LogInfo) << "Parsing desktop file \"" << Utils::String::replace(romPath, "\\", "")
                         << "\"";
            bool validFile {false};
            bool execEntry {false};
            std::ifstream desktopFileStream;
            desktopFileStream.open(Utils::String::replace(romPath, "\\", ""));
            for (std::string line; getline(desktopFileStream, line);) {
                // Some non-standard .desktop files add a leading line such as
                // "#!/usr/bin/env xdg-open" and some lines may also be indented by
                // whitespace characters. So we need to handle such oddities in order
                // to parse and run these files.
                line = Utils::String::trim(line);
                if (line.substr(0, 2) == "#!")
                    continue;
                if (line.find("[Desktop Entry]") != std::string::npos) {
                    validFile = true;
                    continue;
                }
                if (line.substr(0, 5) == "Path=" && startDirectory == "") {
                    // We only parse the Path key if the %STARTDIR% variable has not been set in
                    // es_systems.xml.
                    const std::string startDirectoryTemp {
                        Utils::FileSystem::expandHomePath(line.substr(5, line.size() - 5))};
                    if (startDirectoryTemp.empty())
                        continue;
                    if (!Utils::FileSystem::isDirectory(startDirectoryTemp)) {
                        LOG(LogWarning) << "Path key set to nonexistent directory \""
                                        << startDirectoryTemp << "\"";
                    }
                    else {
                        LOG(LogDebug) << "FileData::launchGame(): Setting start directory to \""
                                      << startDirectoryTemp << "\" as defined by the Path key";
                        startDirectory = Utils::FileSystem::getEscapedPath(startDirectoryTemp);
                    }
                }
                if (line.substr(0, 5) == "Exec=") {
                    romPath = {line.substr(5, line.size() - 5)};
                    const std::string regexString {"[^%]%"};
                    // Field codes, some of these are deprecated but may still exist in older
                    // .desktop files. Any matching codes escaped by double %% characters will
                    // be left as-is.
                    std::string codeCharacters {"fFuUdDnNickvm"};
                    std::smatch searchResult;
                    while (!codeCharacters.empty()) {
                        while (std::regex_search(romPath, searchResult,
                                                 std::regex(regexString + codeCharacters.back())))
                            romPath.replace(searchResult.position(0) + 1, searchResult.length() - 1,
                                            "");
                        codeCharacters.pop_back();
                    }
                    romPath = Utils::String::replace(romPath, "%%", "%");
                    romPath = Utils::String::trim(romPath);
                    command = Utils::String::replace(command, emulator.first, "");
                    execEntry = true;
                }
            }
            desktopFileStream.close();
            if (!validFile || !execEntry) {
                LOG(LogError) << "File is invalid or unreadable";
                window->queueInfoPopup(_("ERROR: DESKTOP FILE IS INVALID OR UNREADABLE"), 6000);
                window->setAllowTextScrolling(true);
                window->setAllowFileAnimation(true);
                return;
            }
        }
        else {
            LOG(LogError) << "Desktop file \"" << romPath << "\" doesn't exist or is unreadable";
            window->queueInfoPopup(_("ERROR: DESKTOP FILE DOESN'T EXIST OR IS UNREADABLE"), 6000);
            window->setAllowTextScrolling(true);
            window->setAllowFileAnimation(true);
            return;
        }
    }
#endif
#endif

#if !defined(_WIN64) && !defined(__APPLE__) && !defined(__ANDROID__)
    // A shortcut (.desktop file) started with an emulator that can't open it, e.g. a shortcut of
    // RPCS3 with an RPCS3 added from the custom emulators folder: the emulator of the command
    // gets the arguments of the shortcut, which name the game.
    if (!isShortcut && Utils::String::toLower(Utils::FileSystem::getExtension(romRaw)) == ".desktop") {
        const std::string arguments {desktopExecArguments(romRaw)};
        if (!arguments.empty()) {
            LOG(LogInfo) << "The emulator gets the arguments of the shortcut: " << arguments;
            romPath = arguments;
        }
    }
#endif

#if !defined(__ANDROID__)
    // Per-game option to always start the emulator from the most recent save state.
    const StateEmulator stateEmu {detectStateEmulator(commandRaw, command)};
    if (stateEmu != StateEmulator::NONE &&
        getSourceFileData()->metadata.get("resumestate") == "true") {
        const auto mapping {readStateMapping(stateEmu)};
        const auto it {mapping.find(getSourceFileData()->getPath())};
        if (it == mapping.cend()) {
            LOG(LogInfo) << "No " << stateEmulatorName(stateEmu)
                         << " save state is known for this game yet, launching normally";
        }
        else {
            const std::string stateFile {mostRecentState(stateEmu, it->second)};
            if (stateFile.empty()) {
                LOG(LogInfo) << "No " << stateEmulatorName(stateEmu)
                             << " save state found for \"" << it->second
                             << "\", launching normally";
            }
            else {
                LOG(LogInfo) << "Loading most recent " << stateEmulatorName(stateEmu)
                             << " save state \"" << stateFile << "\"";
                insertStateArgument(command, stateArgument(stateEmu, stateFile));
            }
        }
    }

    // Replace the remaining variables with their actual values.
    command = Utils::String::replace(command, "%ROM%", romPath);
    command = Utils::String::replace(command, "%BASENAME%", baseName);
    command = Utils::String::replace(command, "%FILENAME%", fileName);
    command = Utils::String::replace(command, "%ROMRAW%", romRaw);
#if !defined(_WIN64)
    command = Utils::String::replace(command, "%ROMRAWWIN%", romRawWindows);
#endif
    command = Utils::String::replace(command, "%ROMPATH%",
                                     Utils::FileSystem::getEscapedPath(getROMDirectory()));
#else
    command = Utils::String::replace(command, "%ANDROIDPACKAGE%", androidPackage);
    // Escaped quotation marks should only be used for Extras on Android so it should be safe to
    // just change them to temporary variables and convert them back to the escaped quotation
    // marks when parsing the Extras.
    command = Utils::String::replace(command, "\\\"", "%QUOTATION%");

    // This is expanded to /data/user/<userid> and /storage/emulated/<userid> respectively.
    command = Utils::String::replace(command, "%INTERNALDATA%",
                                     Utils::Platform::Android::getInternalDirectory());
    command = Utils::String::replace(command, "%EXTERNALDATA%",
                                     Utils::Platform::Android::getExternalDirectory());

    const std::vector<std::string> androidVariabels {
        "%ACTION%=", "%CATEGORY%=", "%MIMETYPE%=", "%DATA%="};

    for (std::string variable : androidVariabels) {
        size_t dataPos {command.find(variable)};
        if (dataPos != std::string::npos) {
            bool invalidEntry {false};
            bool isQuoted {(command.length() > dataPos + variable.length() &&
                            command[dataPos + variable.length()] == '\"')};
            std::string value;

            if (isQuoted) {
                const size_t closeQuotePos {command.find("\"", dataPos + variable.length() + 1)};
                if (closeQuotePos != std::string::npos)
                    value = command.substr(dataPos + variable.length() + 1,
                                           closeQuotePos - (dataPos + variable.length() + 1));
                else
                    invalidEntry = true;
            }
            else {
                const size_t spacePos {command.find(" ", dataPos)};
                if (spacePos != std::string::npos)
                    value = command.substr(dataPos + variable.length(),
                                           spacePos - (dataPos + variable.length()));
                else
                    value = command.substr(dataPos + variable.length(),
                                           command.size() - dataPos + variable.length());
            }

            if (invalidEntry) {
                LOG(LogError) << "Invalid entry in systems configuration file es_systems.xml";
                LOG(LogError) << "Raw emulator launch command:";
                LOG(LogError) << commandRaw;

                window->queueInfoPopup(_("ERROR: INVALID ENTRY IN SYSTEMS CONFIGURATION FILE"),
                                       6000);
                window->setAllowTextScrolling(true);
                window->setAllowFileAnimation(true);
                return;
            }

            if (variable == "%ACTION%=")
                androidAction = value;
            else if (variable == "%DATA%=")
                androidData = value;
            else if (variable == "%CATEGORY%=")
                androidCategory = value;
            else if (variable == "%MIMETYPE%=")
                androidMimeType = value;
        }
    }

    std::vector<std::string> extraVariabels {"%EXTRA_", "%EXTRAARRAY_", "%EXTRAINTEGER_",
                                             "%EXTRABOOL_"};

    for (std::string variable : extraVariabels) {
        size_t extraPos {command.find(variable)};
        while (extraPos != std::string::npos) {
            if (extraPos != std::string::npos) {
                bool invalidEntry {false};
                bool isQuoted {false};
                std::string extraName;
                std::string extraValue;

                size_t equalPos {command.find("=", extraPos)};
                if (equalPos == std::string::npos)
                    invalidEntry = true;

                if (!invalidEntry && extraPos + variable.length() + 1 >= command.size())
                    invalidEntry = true;

                if (!invalidEntry) {
                    if (command.length() > equalPos && command[equalPos + 1] == '\"')
                        isQuoted = true;

                    extraName = command.substr(extraPos + variable.length(),
                                               equalPos - (extraPos + variable.length() + 1));

                    if (isQuoted) {
                        const size_t closeQuotePos {command.find("\"", equalPos + 2)};
                        if (closeQuotePos != std::string::npos)
                            extraValue =
                                command.substr(equalPos + 2, closeQuotePos - (equalPos + 2));
                        else
                            invalidEntry = true;
                    }
                    else {
                        const size_t spacePos {command.find(" ", extraPos)};
                        if (spacePos != std::string::npos)
                            extraValue = command.substr(equalPos + 1, spacePos - (equalPos + 1));
                        else
                            extraValue = command.substr(equalPos + 1, command.size() - equalPos);
                    }

                    if (invalidEntry) {
                        LOG(LogError)
                            << "Invalid entry in systems configuration file es_systems.xml";
                        LOG(LogError) << "Raw emulator launch command:";
                        LOG(LogError) << commandRaw;

                        window->queueInfoPopup(
                            _("ERROR: INVALID ENTRY IN SYSTEMS CONFIGURATION FILE"), 6000);
                        window->setAllowTextScrolling(true);
                        window->setAllowFileAnimation(true);
                        return;
                    }

                    if (extraName != "" && extraValue != "") {
                        // Expand the unescaped game directory path and ROM directory as well as
                        // the raw path to the game file if the corresponding variables have been
                        // used in the Extra definition. We also change back any temporary quotation
                        // mark variables to actual escaped quotation marks so they can be passed
                        // in the Intent.
                        extraValue = Utils::String::replace(extraValue, "%QUOTATION%", "\\\"");
                        extraValue =
                            Utils::String::replace(extraValue, "%GAMEDIRRAW%",
                                                   Utils::FileSystem::getParent(
                                                       Utils::String::replace(romPath, "\\", "")));
                        extraValue =
                            Utils::String::replace(extraValue, "%ROMPATHRAW%", getROMDirectory());
                        extraValue = Utils::String::replace(extraValue, "%ROMRAW%", romRaw);
                        extraValue =
                            Utils::String::replace(extraValue, "%ROMRAWWIN%", romRawWindows);
                        extraValue = Utils::String::replace(extraValue, "%BASENAME%", baseName);
                        extraValue = Utils::String::replace(extraValue, "//", "/");
                        // This is mostly intended for escaping double forward slashes as \/\/.
                        extraValue = Utils::String::replace(extraValue, "\\/", "/");

                        if (variable == "%EXTRA_")
                            androidExtrasString[extraName] = extraValue;
                        else if (variable == "%EXTRAARRAY_")
                            androidExtrasStringArray[extraName] = extraValue;
                        else if (variable == "%EXTRAINTEGER_")
                            androidExtrasInteger[extraName] = extraValue;
                        else if (variable == "%EXTRABOOL_")
                            androidExtrasBool[extraName] = extraValue;
                    }
                }
            }
            extraPos = command.find(variable, extraPos + 1);
        }
    }

    if (command.find("%ACTIVITY_CLEAR_TASK%") != std::string::npos)
        androidActivityFlags.emplace_back("%ACTIVITY_CLEAR_TASK%");
    if (command.find("%ACTIVITY_CLEAR_TOP%") != std::string::npos)
        androidActivityFlags.emplace_back("%ACTIVITY_CLEAR_TOP%");
    if (command.find("%ACTIVITY_NO_HISTORY%") != std::string::npos)
        androidActivityFlags.emplace_back("%ACTIVITY_NO_HISTORY%");
#endif

#if defined(_WIN64)
    // Hack to remove double quotation marks as these can occur under some special circumstances.
    const int quotationCount {static_cast<int>(
        std::count_if(command.cbegin(), command.cend(), [](char c) { return c == '\"'; }))};
    if (quotationCount % 2 != 0)
        command = Utils::String::replace(command, "\"\"", "\"");

    command = Utils::String::replace(
        command, "%ESPATH%", Utils::String::replace(Utils::FileSystem::getExePath(), "/", "\\"));
    command = Utils::String::replace(command, "%EMUDIR%",
                                     Utils::FileSystem::getEscapedPath(Utils::FileSystem::getParent(
                                         Utils::String::replace(emulator.first, "\"", ""))));
    command = Utils::String::replace(command, "%GAMEDIR%",
                                     Utils::FileSystem::getEscapedPath(Utils::FileSystem::getParent(
                                         Utils::String::replace(romPath, "\"", ""))));
    command = Utils::String::replace(
        command, "%GAMEDIRRAW%",
        Utils::String::replace(
            Utils::FileSystem::getParent(Utils::String::replace(romPath, "\"", "")), "/", "\\"));
#else
    command = Utils::String::replace(command, "%ESPATH%", Utils::FileSystem::getExePath());
    command = Utils::String::replace(command, "%EMUDIR%",
                                     Utils::FileSystem::getEscapedPath(Utils::FileSystem::getParent(
                                         Utils::String::replace(emulator.first, "\\", ""))));
    command = Utils::String::replace(command, "%GAMEDIR%",
                                     Utils::FileSystem::getEscapedPath(Utils::FileSystem::getParent(
                                         Utils::String::replace(romPath, "\\", ""))));
    command = Utils::String::replace(
        command, "%GAMEDIRRAW%",
        Utils::FileSystem::getParent(Utils::String::replace(romPath, "\\", "")));
#endif

    // Trim any leading and trailing whitespace characters as they could cause launch issues.
    command = Utils::String::trim(command);

#if defined(DEINIT_ON_LAUNCH)
    runInBackground = false;
#endif

    // swapBuffers() is called here to turn the screen black to eliminate some potential
    // flickering and to avoid showing the game launch message briefly when returning
    // from the game.
    if (!runInBackground)
        Renderer::getInstance()->swapBuffers();

    Scripting::fireEvent("game-start", romPath, getSourceFileData()->metadata.get("name"),
                         getSourceFileData()->getSystem()->getName(),
                         getSourceFileData()->getSystem()->getFullName());
    int returnValue {0};

    LOG(LogDebug) << "Raw emulator launch command:";
    LOG(LogDebug) << commandRaw;
#if defined(__ANDROID__)
    LOG(LogInfo) << "Expanded emulator launch arguments:";
    LOG(LogInfo) << "Package: " << androidPackage;
    if (androidActivity != "") {
        LOG(LogInfo) << "Activity: " << androidActivity;
    }
    if (androidAction != "") {
        LOG(LogInfo) << "Action: " << androidAction;
    }
    if (androidCategory != "") {
        LOG(LogInfo) << "Category: " << androidCategory;
    }
    if (androidMimeType != "") {
        LOG(LogInfo) << "MIME type: " << androidMimeType;
    }
    if (androidData != "") {
        LOG(LogInfo) << "Data: " << androidData;
    }
    for (auto& extra : androidExtrasString) {
        LOG(LogInfo) << "Extra name: " << extra.first;
        LOG(LogInfo) << "Extra value: " << extra.second;
    }
    for (auto& extra : androidExtrasStringArray) {
        LOG(LogInfo) << "Extra array name: " << extra.first;
        LOG(LogInfo) << "Extra array value: " << extra.second;
    }
    for (auto& extra : androidExtrasInteger) {
        LOG(LogInfo) << "Extra integer name: " << extra.first;
        LOG(LogInfo) << "Extra integer value: " << extra.second;
    }
    for (auto& extra : androidExtrasBool) {
        LOG(LogInfo) << "Extra bool name: " << extra.first;
        LOG(LogInfo) << "Extra bool value: " << extra.second;
    }

    if (Settings::getInstance()->getBool("RetroArchCoreQueryExperimental") &&
        androidPackage.substr(0, 13) == "com.retroarch") {
        std::string coreFile;

        for (auto& extra : androidExtrasString) {
            if (extra.first == "LIBRETRO")
                coreFile = extra.second;
        }

        if (coreFile != "") {
            const size_t pos {coreFile.find_last_of("/")};

            if (pos != std::string::npos && pos != coreFile.length() - 1) {
                coreFile = coreFile.substr(pos + 1, coreFile.length() - pos);
                LOG(LogInfo) << "Checking whether the RetroArch core \"" << coreFile
                             << "\" is installed";
            }
            else {
                coreFile = "";
            }
        }

        if (coreFile == "") {
            LOG(LogWarning) << "Could not check for installed RetroArch core as no core path "
                               "could be found in launch command";
        }
        else {
            const int returnValue {
                Utils::Platform::Android::checkRACoreInstalled(androidPackage, coreFile)};

            if (returnValue == 1) {
                LOG(LogInfo) << "Emulator core is installed, proceeding with game launch";
            }
            else if (returnValue == 0) {
                LOG(LogError) << "Couldn't launch game, emulator core is not installed";

                window->queueInfoPopup(
                    Utils::String::format(
                        _("ERROR: COULDN'T FIND EMULATOR CORE FILE '%s'"),
                        Utils::String::toUpper(Utils::FileSystem::getFileName(coreFile)).c_str()),
                    6000);
                window->setAllowTextScrolling(true);
                window->setAllowFileAnimation(true);
                return;
            }
            else if (returnValue == -1) {
                LOG(LogWarning) << "Timed out attempting to query RetroArch, proceeding with game "
                                   "launch anyway";
            }
            else if (returnValue == -2) {
                LOG(LogError) << "Unknown issue triggered an exception when querying RetroArch, "
                                 "proceeding with game launch anyway";
            }
        }
    }

#else
    LOG(LogInfo) << "Expanded emulator launch command:";
    LOG(LogInfo) << command;
#endif

#if defined(FLATPAK_BUILD)
    // Break out of the sandbox.
    command = "flatpak-spawn --host " + command;
#endif

    // RetroArch ends with 0 also when the core couldn't load the game, so the log of RetroArch
    // is read to tell if the game started.
    std::string retroArchLog;
    if (mameFallback && !runInBackground) {
        const size_t executable {command.find("retroarch")};
        if (executable != std::string::npos) {
            retroArchLog = Utils::FileSystem::getAppDataDirectory() + "/logs/retroarch-mame.log";
            Utils::FileSystem::removeFile(retroArchLog);
            const size_t end {command.find(' ', executable)};
            if (end != std::string::npos)
                command.insert(end, " --verbose --log-file \"" + retroArchLog + "\"");
        }
    }

    // Flush the log buffer to es_log.txt, otherwise game launch logging will only be written
    // once we have returned from the game.
    if (!runInBackground) {
        Log::flush();
    }

    // Possibly keep EmuSPLASH running in the background while the game is launched.

    Settings::getInstance()->setInt(
        "GameLaunchTime", static_cast<int>(Utils::Time::DateTime(Utils::Time::now()).getTime()));

#if !defined(__ANDROID__)
    StateSnapshot statesBefore;
    if (stateEmu != StateEmulator::NONE && !runInBackground)
        statesBefore = stateSnapshot(stateEmu);
#endif

    const auto launchStartTime {std::chrono::steady_clock::now()};
#if defined(_WIN64)
    returnValue = Utils::Platform::launchGameWindows(
        Utils::String::stringToWideString(command),
        Utils::String::stringToWideString(startDirectory), runInBackground, hideWindow);
#elif defined(__ANDROID__)
    returnValue = Utils::Platform::Android::launchGame(
        androidPackage, androidActivity, androidAction, androidCategory, androidMimeType,
        androidData, mEnvData->mStartPath, romRaw, androidExtrasString, androidExtrasStringArray,
        androidExtrasInteger, androidExtrasBool, androidActivityFlags, launchOnOtherScreen);
#else

#if defined(__linux__)
    std::unique_ptr<EmulatorConfig::FullscreenWatcher> fullscreenWatcher;
    if (forceFullscreen) {
        // The tools can only change X11 windows, so Qt emulators use XWayland on Wayland.
        if (EmulatorConfig::isWaylandSession())
            command = "QT_QPA_PLATFORM=xcb " + command;
        fullscreenWatcher = std::make_unique<EmulatorConfig::FullscreenWatcher>();
        LOG(LogInfo) << "Forcing the emulator window to fullscreen";
    }
#endif

#if !defined(__APPLE__)
    // The hotkeys of Input device settings > HOTKEYS: written into the settings of the
    // emulator, and watched while the game runs. RetroArch gets the settings of EmuSPLASH.
    const EmulatorProfiles::Profile* hotkeyProfile {EmulatorProfiles::findByCommand(command)};
    if (hotkeyProfile == nullptr)
        hotkeyProfile = EmulatorProfiles::findForGame(this);
    GameHotkeys::Launch hotkeyLaunch;
    if (hotkeyProfile != nullptr) {
        const std::string commandBefore {command};
        hotkeyLaunch = EmulatorProfiles::prepareLaunch(*hotkeyProfile, this, command);
        if (command != commandBefore) {
            LOG(LogInfo) << "Launch command with the settings of " << hotkeyProfile->name << ":";
            LOG(LogInfo) << command;
        }
    }
    if (!runInBackground)
        GameHotkeys::begin(hotkeyLaunch);
#endif

#if defined(DEINIT_ON_LAUNCH)
// Deinit both the AudioManager and the window which allows emulators to launch in KMS mode.
AudioManager::getInstance().deinit();
window->deinit();
returnValue = Utils::Platform::launchGameUnix(command, startDirectory, false);
AudioManager::getInstance().init();
window->init();
#else
returnValue = Utils::Platform::launchGameUnix(command, startDirectory, runInBackground);
#endif
#if !defined(__APPLE__)
    GameHotkeys::end();
#endif

#endif
#if !defined(__ANDROID__)
    // Remember which save state files belong to this game.
    if (stateEmu != StateEmulator::NONE && !runInBackground)
        learnStatePrefix(stateEmu, getSourceFileData()->getPath(), statesBefore);
#endif

    // MAME is split into many emulators and none of them runs every game, so a game that
    // doesn't start is tried with the other emulators of the system, one after the other.
    bool mameStarted {false};
    bool mameGaveUp {false};
    if (mameFallback && !runInBackground) {
        const bool quickExit {std::chrono::steady_clock::now() - launchStartTime <
                              std::chrono::seconds(2)};
        bool failedInLog {false};
        if (!retroArchLog.empty()) {
            std::string logText;
            std::ifstream logFile {retroArchLog};
            if (logFile.good()) {
                logText.assign(std::istreambuf_iterator<char>(logFile),
                               std::istreambuf_iterator<char>());
                logText = Utils::String::toLower(logText);
                for (const char* phrase : {"failed to load content", "failed to load libretro",
                                           "failed to open libretro", "failed to load game",
                                           "unable to load content"}) {
                    if (logText.find(phrase) != std::string::npos) {
                        failedInLog = true;
                        LOG(LogInfo) << "FileData::launchGame(): The log of RetroArch has \""
                                     << phrase << "\"";
                        break;
                    }
                }
            }
            else {
                LOG(LogInfo) << "FileData::launchGame(): No log of RetroArch was written";
            }
        }
        const bool failed {returnValue != 0 || quickExit || failedInLog};
        if (failed) {
            bool hasNext {false};
            int total {0};
            int number {0};
            for (const auto& entry : gameSystem->getSystemEnvData()->mLaunchCommands) {
                if (entry.second.empty() || !isEmulatorInstalled(entry.first))
                    continue;
                ++total;
                if (std::find(triedNow.cbegin(), triedNow.cend(), entry.second) ==
                    triedNow.cend()) {
                    hasNext = true;
                    if (number == 0)
                        number = total;
                }
            }
            if (hasNext) {
                window->queueInfoPopup(
                    Utils::String::format(
                        _("'%s' DID NOT START, TRYING THE NEXT MAME EMULATOR (%i OF %i)"),
                        Utils::String::toUpper(metadata.get("name")).c_str(), number, total),
                    5000);
                window->setAllowTextScrolling(true);
                window->setAllowFileAnimation(true);
                launchGame(triedNow);
                return;
            }
            if (triedNow.size() > 1) {
                window->queueInfoPopup(
                    Utils::String::format(
                        _("'%s' COULDN'T BE STARTED WITH ANY MAME EMULATOR"),
                        Utils::String::toUpper(metadata.get("name")).c_str()),
                    6000);
                mameGaveUp = true;
                returnValue = returnValue == 0 ? 1 : returnValue;
            }
        }
        else {
            mameStarted = true;
        }
    }

    // The emulator that finally started the game is remembered for the game.
    if (mameStarted && triedLabels.size() > 0) {
        getSourceFileData()->metadata.set("altemulator", usedLabel);
        if (!Settings::getInstance()->getBool("AlternativeEmulatorPerGame"))
            Settings::getInstance()->setBool("AlternativeEmulatorPerGame", true);
        LOG(LogInfo) << "FileData::launchGame(): Emulator \"" << usedLabel
                     << "\" is now used for the game";
    }

    // Notify the user in case of a failed game launch using a popup window.
    if (returnValue != 0) {
        LOG(LogWarning) << "Launch terminated with nonzero return value " << returnValue;

        if (!mameGaveUp) {
            window->queueInfoPopup(
                Utils::String::format(_("ERROR LAUNCHING GAME '%s' (ERROR CODE %i)"),
                                      Utils::String::toUpper(metadata.get("name")).c_str(),
                                      returnValue),
                6000);
        }
        window->setAllowTextScrolling(true);
        window->setAllowFileAnimation(true);
    }
    else {
        // Stop showing the game launch notification.
        window->stopInfoPopup();
#if defined(_WIN64)
        // If the RunInBackground setting has been enabled or if the %RUNINBACKGROUND% variable has
        // been set for the specific launch command, then block the video player, stop scrolling
        // game names and descriptions and keep the screensaver from getting activated.
        if (runInBackground)
            window->setLaunchedGame(true);
        else
            // Normalize deltaTime so that the screensaver does not start immediately
            // when returning from the game.
            window->normalizeNextUpdate();
#else
        // For some game systems we need to keep EmuSPLASH running while the game is launched.
        // This blocks the video player, stops the scrolling of game names and descriptions and
        // keeps the screensaver from getting activated.
        if (runInBackground)
            window->setLaunchedGame(true);
        // Normalize deltaTime so that the screensaver does not start immediately
        // when returning from the game.
        window->normalizeNextUpdate();
#endif
    }

    // If running in the background then don't trigger the game-end event, which will instead be
    // triggered in ViewController when manually waking up the application.
    if (!runInBackground) {
        Scripting::fireEvent("game-end", romPath, getSourceFileData()->metadata.get("name"),
                             getSourceFileData()->getSystem()->getName(),
                             getSourceFileData()->getSystem()->getFullName());
    }
    else {
        std::vector<std::string>& gameEndParams {window->getGameEndEventParams()};
        gameEndParams.emplace_back("game-end");
        gameEndParams.emplace_back(romPath);
        gameEndParams.emplace_back(getSourceFileData()->metadata.get("name"));
        gameEndParams.emplace_back(getSourceFileData()->getSystem()->getName());
        gameEndParams.emplace_back(getSourceFileData()->getSystem()->getFullName());
    }

    // Unless we're running in the background while the game is launched, re-enable the text
    // scrolling that was disabled in ViewController. Also poll the system status immediately
    // in case something changed while the game was running.
    if (!runInBackground) {
        window->setAllowTextScrolling(true);
        window->setAllowFileAnimation(true);
        SystemStatus::getInstance().setPollImmediately(true);
    }

    // Update number of times the game has been launched.
    FileData* gameToUpdate {getSourceFileData()};

    const int timesPlayed {gameToUpdate->metadata.getInt("playcount") + 1};
    gameToUpdate->metadata.set("playcount", std::to_string(static_cast<long long>(timesPlayed)));

    // This sets the lastplayed and playtime metadata values.
    if (!runInBackground)
        setPlayMetadata(false);

    // We make an explicit call to close the launch screen instead of waiting for
    // AnimationController to do it as that would be done too late. This is so because on
    // gamelist reload the helpsystem uses the state of the launch screen to select between
    // the dimmed and undimmed element properties.
    window->closeLaunchScreen();

    CollectionSystemsManager::getInstance()->refreshCollectionSystems(gameToUpdate);
    gameToUpdate->mSystem->onMetaDataSavePoint();
}

const std::pair<std::string, FileData::findEmulatorResult> FileData::findEmulator(
    std::string& command, const bool preCommand)
{
    // Extract the emulator executable from the launch command string. There are two ways
    // that the emulator can be defined in es_systems.xml, either using the find rules in
    // es_find_rules.xml or via the explicit emulator name. In the former case, we
    // need to process any configured systempath and staticpath rules (and for Windows also
    // winregistrypath and winregistryvalue rules), and in the latter case we simply search
    // for the emulator in the system path.

    std::string emuExecutable;
    std::string exePath;

    // Method 1, emulator is defined using find rules:

#if defined(_WIN64)
    std::vector<std::string> emulatorWinRegistryPaths;
    std::vector<std::string> emulatorWinRegistryValues;
#endif
#if defined(__ANDROID__)
    std::vector<std::string> emulatorAndroidPackages;
#endif
    std::vector<std::string> emulatorSystemPaths;
    std::vector<std::string> emulatorStaticPaths;
    std::string emulatorEntry;
    size_t startPos {0};
    size_t endPos {0};

    if (preCommand) {
        if ((startPos = command.find("%PRECOMMAND_")) != std::string::npos) {
            endPos = command.find("%", startPos + 1);
            if (endPos != std::string::npos)
                emulatorEntry = command.substr(startPos + 12, endPos - startPos - 12);
        }
    }
    else {
        if ((startPos = command.find("%EMULATOR_")) != std::string::npos) {
            endPos = command.find("%", startPos + 1);
            if (endPos != std::string::npos)
                emulatorEntry = command.substr(startPos + 10, endPos - startPos - 10);
        }
    }

    if (emulatorEntry != "") {
#if defined(_WIN64)
        emulatorWinRegistryPaths =
            SystemData::sFindRules.get()->mEmulators[emulatorEntry].winRegistryPaths;
        emulatorWinRegistryValues =
            SystemData::sFindRules.get()->mEmulators[emulatorEntry].winRegistryValues;
#endif
#if defined(__ANDROID__)
        emulatorAndroidPackages =
            SystemData::sFindRules.get()->mEmulators[emulatorEntry].androidPackages;
#endif
        emulatorSystemPaths = SystemData::sFindRules.get()->mEmulators[emulatorEntry].systemPaths;

        emulatorStaticPaths = SystemData::sFindRules.get()->mEmulators[emulatorEntry].staticPaths;
    }

    // Error handling in case of no emulator find rule.
#if defined(_WIN64)
    if (emulatorEntry != "" && emulatorWinRegistryPaths.empty() &&
        emulatorWinRegistryValues.empty() && emulatorSystemPaths.empty() &&
        emulatorStaticPaths.empty())
#elif defined(__ANDROID__)
    if (emulatorEntry != "" && emulatorAndroidPackages.empty() && emulatorSystemPaths.empty() &&
        emulatorStaticPaths.empty())
#else
    if (emulatorEntry != "" && emulatorSystemPaths.empty() && emulatorStaticPaths.empty())
#endif
        return std::make_pair(emulatorEntry, FileData::findEmulatorResult::NO_RULES);

#if defined(_WIN64)
    for (std::string& path : emulatorWinRegistryPaths) {
        // Search for the emulator using the App Paths keys in the Windows Registry.
        const std::string& registryKeyPath {
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + path};

        HKEY registryKey;
        LSTATUS keyStatus {-1};
        LSTATUS pathStatus {-1};
        std::wstring registryPath(1024, 0);
        DWORD pathSize {1024};

        // First look in HKEY_CURRENT_USER.
        keyStatus = RegOpenKeyExW(HKEY_CURRENT_USER,
                                  Utils::String::stringToWideString(registryKeyPath).c_str(), 0,
                                  KEY_QUERY_VALUE, &registryKey);

        // If not found, then try in HKEY_LOCAL_MACHINE.
        if (keyStatus != ERROR_SUCCESS) {
            keyStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                      Utils::String::stringToWideString(registryKeyPath).c_str(), 0,
                                      KEY_QUERY_VALUE, &registryKey);
        }

        // If the key exists, then try to retrieve its default value.
        if (keyStatus == ERROR_SUCCESS) {
            pathStatus = RegGetValue(registryKey, nullptr, nullptr, RRF_RT_REG_SZ, nullptr,
                                     &registryPath[0], &pathSize);
            registryPath.erase(std::find(registryPath.begin(), registryPath.end(), '\0'),
                               registryPath.end());
        }
        else {
            RegCloseKey(registryKey);
            continue;
        }

        // That a value was found does not guarantee that the emulator actually exists, so
        // check for that as well.
        if (pathStatus == ERROR_SUCCESS) {
            if (Utils::FileSystem::isRegularFile(Utils::String::wideStringToString(registryPath)) ||
                Utils::FileSystem::isSymlink(Utils::String::wideStringToString(registryPath))) {
                LOG(LogDebug) << "FileData::findEmulator(): "
                              << (preCommand ? "Pre-command" : "Emulator")
                              << " found via winregistrypath rule";
                exePath = Utils::FileSystem::getEscapedPath(
                    Utils::String::wideStringToString(registryPath));
                command.replace(startPos, endPos - startPos + 1, exePath);
                RegCloseKey(registryKey);
                return std::make_pair(exePath, FileData::findEmulatorResult::FOUND_FILE);
            }
        }
        RegCloseKey(registryKey);
    }

    for (std::string& value : emulatorWinRegistryValues) {
        // If the pipe character is found, then the string following this should be appended
        // to the key value, assuming the key is found.
        std::string appendString;
        const size_t pipePos {value.find('|')};

        if (pipePos != std::string::npos) {
            appendString = value.substr(pipePos + 1, std::string::npos);
            value = value.substr(0, pipePos);
        }

        // Search for the defined value in the Windows Registry.
        const std::string& registryValueKey {
            Utils::String::replace(Utils::FileSystem::getParent(value), "/", "\\")};
        const std::string& registryValue {Utils::FileSystem::getFileName(value)};

        HKEY registryKey;
        LSTATUS keyStatus {-1};
        LSTATUS pathStatus {-1};
        std::wstring path(1024, 0);
        DWORD pathSize {1024};

        // First look in HKEY_CURRENT_USER.
        keyStatus = RegOpenKeyExW(HKEY_CURRENT_USER,
                                  Utils::String::stringToWideString(registryValueKey).c_str(), 0,
                                  KEY_QUERY_VALUE, &registryKey);

        // If not found, then try in HKEY_LOCAL_MACHINE.
        if (keyStatus != ERROR_SUCCESS) {
            keyStatus = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                      Utils::String::stringToWideString(registryValueKey).c_str(),
                                      0, KEY_QUERY_VALUE, &registryKey);
        }

        // If the key exists, then try to retrieve the defined value.
        if (keyStatus == ERROR_SUCCESS) {
            pathStatus = RegGetValueW(registryKey, nullptr,
                                      Utils::String::stringToWideString(registryValue).c_str(),
                                      RRF_RT_REG_SZ, nullptr, &path[0], &pathSize);
            path.erase(std::find(path.begin(), path.end(), '\0'), path.end());
        }
        else {
            RegCloseKey(registryKey);
            continue;
        }

        if (path.empty()) {
            RegCloseKey(registryKey);
            continue;
        }

        if (!appendString.empty())
            path.append(Utils::String::stringToWideString(appendString));

        // That a value was found does not guarantee that the emulator actually exists,
        // so check for that as well.
        if (pathStatus == ERROR_SUCCESS) {
            if (Utils::FileSystem::isRegularFile(Utils::String::wideStringToString(path)) ||
                Utils::FileSystem::isSymlink(Utils::String::wideStringToString(path))) {
                LOG(LogDebug) << "FileData::findEmulator(): "
                              << (preCommand ? "Pre-command" : "Emulator")
                              << " found via winregistryvalue rule";
                exePath =
                    Utils::FileSystem::getEscapedPath(Utils::String::wideStringToString(path));
                command.replace(startPos, endPos - startPos + 1, exePath);
                RegCloseKey(registryKey);
                return std::make_pair(exePath, FileData::findEmulatorResult::FOUND_FILE);
            }
        }
        RegCloseKey(registryKey);
    }
#endif

#if defined(__ANDROID__)
    for (std::string& androidpackage : emulatorAndroidPackages) {
        // If a forward slash character is present in the androidpackage entry it means an explicit
        // Intent activity should be used rather than the default one. The checkEmulatorInstalled()
        // Java function will check for the activity as well and if it's not found it flags the
        // overall emulator entry as not found.
        std::string packageName {androidpackage};
        std::string activity;
        size_t separatorPos {packageName.find('/')};

        if (separatorPos != std::string::npos) {
            activity = packageName.substr(separatorPos + 1);
            packageName = packageName.substr(0, separatorPos);
        }

        if (Utils::Platform::Android::checkEmulatorInstalled(packageName, activity)) {
            return std::make_pair(androidpackage,
                                  FileData::findEmulatorResult::FOUND_ANDROID_PACKAGE);
        }
    }
#endif

    for (std::string& path : emulatorSystemPaths) {
#if defined(_WIN64)
        std::wstring pathWide {Utils::String::stringToWideString(path)};
        // Search for the emulator using the PATH environment variable.
        DWORD size {SearchPathW(nullptr, pathWide.c_str(), L".exe", 0, nullptr, nullptr)};

        if (size) {
            std::vector<wchar_t> pathBuffer(static_cast<size_t>(size) + 1);
            wchar_t* fileName {nullptr};

            SearchPathW(nullptr, pathWide.c_str(), L".exe", size + 1, pathBuffer.data(), &fileName);
            std::wstring pathString {pathBuffer.data()};

            if (pathString.length()) {
                exePath = Utils::String::wideStringToString(
                    pathString.substr(0, pathString.size() - std::wstring(fileName).size()));
                exePath.pop_back();
            }
        }
        if (exePath != "") {
            LOG(LogDebug) << "FileData::findEmulator(): "
                          << (preCommand ? "Pre-command" : "Emulator")
                          << " found via systempath rule";
            exePath += "\\" + path;
            exePath = Utils::FileSystem::getEscapedPath(exePath);
            command.replace(startPos, endPos - startPos + 1, exePath);
            return std::make_pair(exePath, FileData::findEmulatorResult::FOUND_FILE);
        }
#else
        exePath = Utils::FileSystem::getPathToBinary(path);
        if (exePath != "") {
            LOG(LogDebug) << "FileData::findEmulator(): "
                          << (preCommand ? "Pre-command" : "Emulator")
                          << " found via systempath rule";
            exePath += "/" + path;
            command.replace(startPos, endPos - startPos + 1, exePath);
            return std::make_pair(exePath, FileData::findEmulatorResult::FOUND_FILE);
        }
#endif
    }

    for (std::string& path : emulatorStaticPaths) {
        // If a pipe character is present in the staticpath entry it means we should substitute
        // the emulator with whatever is defined after the pipe character.
        std::string replaceCommand;
        const size_t pipePos {path.find('|')};

        if (pipePos != std::string::npos) {
            replaceCommand = path.substr(pipePos + 1);
            path = path.substr(0, pipePos);
        }

        path = Utils::FileSystem::expandHomePath(path);
        // If %ESPATH% is used for the rule, then expand it to the binary directory of EmuSPLASH.
        path = Utils::String::replace(path, "%ESPATH%", Utils::FileSystem::getExePath());
        // Likewise for the %ROMPATH% variable which expands to the configured ROM directory.
        path = Utils::String::replace(path, "%ROMPATH%", getROMDirectory());

        // Find the first matching file if a wildcard was used for the emulator entry.
        if (path.find('*') != std::string::npos) {
#if defined(_WIN64)
            Utils::FileSystem::StringList files {
                Utils::FileSystem::getMatchingFiles(Utils::String::replace(path, "\\", "/"))};
            if (files.size() > 0)
                path = Utils::String::replace(files.front(), "/", "\\");
#else
            Utils::FileSystem::StringList files {Utils::FileSystem::getMatchingFiles(path)};
            if (files.size() > 0)
                path = files.front();
#endif
        }

        if (Utils::FileSystem::isRegularFile(path) || Utils::FileSystem::isSymlink(path)) {
            LOG(LogDebug) << "FileData::findEmulator(): "
                          << (preCommand ? "Pre-command" : "Emulator")
                          << " found via staticpath rule";
            if (replaceCommand == "") {
                exePath = Utils::FileSystem::getEscapedPath(path);
            }
            else {
                LOG(LogDebug) << "FileData::findEmulator(): Replacing emulator in "
                                 "staticpath rule with explicitly defined command";
                exePath = replaceCommand;
            }
            command.replace(startPos, endPos - startPos + 1, exePath);
            return std::make_pair(exePath, FileData::findEmulatorResult::FOUND_FILE);
        }
    }

    // Method 2, exact emulator name:

    // If %ESPATH% is used, then expand it to the binary directory of EmuSPLASH.
    command = Utils::String::replace(command, "%ESPATH%", Utils::FileSystem::getExePath());

    // If the first character is a quotation mark, then we need to extract up to the
    // next quotation mark, otherwise we'll only extract up to the first space character.
    if (command.front() == '\"') {
        const std::string& emuTemp {command.substr(1, std::string::npos)};
        emuExecutable = emuTemp.substr(0, emuTemp.find('"'));
    }
    else {
        emuExecutable = command.substr(0, command.find(' '));
    }

    if (emuExecutable.find('*') != std::string::npos) {
        Utils::FileSystem::StringList files {Utils::FileSystem::getMatchingFiles(emuExecutable)};
        if (files.size() > 0) {
            command = Utils::String::replace(command, emuExecutable, files.front());
            emuExecutable = files.front();
        }
    }

#if defined(_WIN64)
    std::wstring emuExecutableWide {Utils::String::stringToWideString(emuExecutable)};
    // Search for the emulator using the PATH environment variable.
    const DWORD size {
        SearchPathW(nullptr, emuExecutableWide.c_str(), L".exe", 0, nullptr, nullptr)};

    if (size) {
        std::vector<wchar_t> pathBuffer(static_cast<size_t>(size) + 1);
        wchar_t* fileName {nullptr};

        SearchPathW(nullptr, emuExecutableWide.c_str(), L".exe", size + 1, pathBuffer.data(),
                    &fileName);

        exePath = Utils::String::wideStringToString(pathBuffer.data());
    }
#elif !defined(__ANDROID__)
    if (Utils::FileSystem::isRegularFile(emuExecutable) ||
        Utils::FileSystem::isSymlink(emuExecutable)) {
        exePath = Utils::FileSystem::getEscapedPath(emuExecutable);
    }
    else {
        exePath =
            Utils::FileSystem::getEscapedPath(Utils::FileSystem::getPathToBinary(emuExecutable));
        if (exePath != "")
            exePath += "/" + emuExecutable;
    }
#endif

    if (exePath.empty())
        return std::make_pair("", FileData::findEmulatorResult::NOT_FOUND);
    else
        return std::make_pair(exePath, FileData::findEmulatorResult::FOUND_FILE);
}

bool FileData::isEmulatorInstalled(const std::string& command)
{
#if defined(__ANDROID__) || defined(_WIN64)
    return true;
#else
    // Variables such as %RUNINBACKGROUND% before the emulator are left out.
    std::string emulatorCommand;
    for (const auto& token : Utils::String::delimitedStringToVector(command, " ")) {
        if (emulatorCommand.empty() && token.size() > 1 && token.front() == '%' &&
            token.find("%EMULATOR_") != 0 && token.find("%ESPATH%") != 0)
            continue;
        if (emulatorCommand.empty() && token.empty())
            continue;
        emulatorCommand.append((emulatorCommand.empty() ? "" : " ") + token);
    }
    if (emulatorCommand.empty())
        return false;

    std::string expanded {emulatorCommand};
    const std::pair<std::string, findEmulatorResult> emulator {findEmulator(expanded, false)};
    if (emulator.second != findEmulatorResult::FOUND_FILE)
        return false;

    // The core of RetroArch and other emulators that use one.
    const size_t corePos {emulatorCommand.find("%CORE_")};
    if (corePos == std::string::npos)
        return true;
    const size_t coreEnd {emulatorCommand.find('%', corePos + 6)};
    if (coreEnd == std::string::npos)
        return false;
    const std::string coreEntry {emulatorCommand.substr(corePos + 6, coreEnd - corePos - 6)};
    size_t nameEnd {emulatorCommand.find_first_of(" \"", coreEnd + 1)};
    if (nameEnd == std::string::npos)
        nameEnd = emulatorCommand.size();
    std::string coreName {emulatorCommand.substr(coreEnd + 1, nameEnd - coreEnd - 1)};
    while (!coreName.empty() && (coreName.front() == '/' || coreName.front() == '\\'))
        coreName.erase(coreName.begin());
    const std::string emulatorPath {Utils::String::replace(emulator.first, "\\", "")};
    for (std::string path : SystemData::sFindRules.get()->mCores[coreEntry].corePaths) {
        path = Utils::String::replace(path, "%EMUPATH%", Utils::FileSystem::getParent(emulatorPath));
        path = Utils::String::replace(path, "%ESPATH%", Utils::FileSystem::getExePath());
        const std::string coreFile {Utils::FileSystem::expandHomePath(path + "/" + coreName)};
        if (Utils::FileSystem::isRegularFile(coreFile) || Utils::FileSystem::isSymlink(coreFile))
            return true;
    }
    return false;
#endif
}

CollectionFileData::CollectionFileData(FileData* file, SystemData* system)
    : FileData(file->getSourceFileData()->getType(),
               file->getSourceFileData()->getPath(),
               file->getSourceFileData()->getSystemEnvData(),
               system)
{
    // We use this constructor to create a clone of the filedata, and change its system.
    mSourceFileData = file->getSourceFileData();
    mParent = nullptr;
    metadata = mSourceFileData->metadata;
    mSystemName = mSourceFileData->getSystem()->getName();
}

CollectionFileData::~CollectionFileData()
{
    // Need to remove collection file data at the collection object destructor.
    if (mParent)
        mParent->removeChild(this);
    mParent = nullptr;
}
