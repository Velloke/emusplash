//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  EmulatorConfig.cpp
//
//  Emulators added by the user, e.g. an AppImage or a script, which are written to the
//  custom es_systems.xml file. The system is copied there from the bundled file the first
//  time, so its other emulators stay available, and the added emulator becomes the default.
//

#include "EmulatorConfig.h"

#include "FileData.h"
#include "GamelistFileParser.h"
#include "Log.h"
#include "Settings.h"
#include "SystemData.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

#include <unistd.h>

namespace
{
    // Common emulators with the arguments that start a game in fullscreen, and the systems
    // they are used for.
    struct KnownEmulator {
        std::string key;
        std::string arguments;
        std::vector<std::string> systems;
    };

    const std::vector<KnownEmulator>& knownEmulators()
    {
        static const std::vector<KnownEmulator> emulators {
            {"yuzu", "-f -g %ROM%", {"switch"}},
            {"suyu", "-f -g %ROM%", {"switch"}},
            {"sudachi", "-f -g %ROM%", {"switch"}},
            {"citron", "-f -g %ROM%", {"switch"}},
            {"torzu", "-f -g %ROM%", {"switch"}},
            {"eden", "-f -g %ROM%", {"switch"}},
            {"ryujinx", "--fullscreen %ROM%", {"switch"}},
            {"cemu", "-f -g %ROM%", {"wiiu"}},
            {"pcsx2", "-fullscreen -nogui -- %ROM%", {"ps2"}},
            {"rpcs3", "--no-gui %ROM%", {"ps3"}},
            {"ppsspp", "--fullscreen %ROM%", {"psp"}},
            {"duckstation", "-fullscreen -nogui %ROM%", {"psx"}},
            {"dolphin", "-b -e %ROM%", {"gc", "wii"}},
            {"primehack", "-b -e %ROM%", {"wii"}},
            {"xemu", "-full-screen -dvd_path %ROM%", {"xbox"}},
            {"citra", "-f %ROM%", {"n3ds"}},
            {"lime3ds", "-f %ROM%", {"n3ds"}},
            {"azahar", "-f %ROM%", {"n3ds"}},
            {"mgba", "-f %ROM%", {"gba", "gb", "gbc"}},
            {"melonds", "-f %ROM%", {"nds"}},
        };
        return emulators;
    }

    const KnownEmulator* findKnownEmulator(const std::string& normalizedName);

    // Lower case letters and digits only, e.g. "Suyu-Linux_x86_64" gives "suyulinuxx8664".
    std::string normalized(const std::string& text)
    {
        std::string result;
        for (const char character : Utils::String::toLower(text)) {
            if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))
                result.push_back(character);
        }
        return result;
    }

    const KnownEmulator* findKnownEmulator(const std::string& normalizedName)
    {
        const KnownEmulator* best {nullptr};
        for (const auto& emulator : knownEmulators()) {
            if (normalizedName.find(emulator.key) != std::string::npos &&
                (best == nullptr || emulator.key.size() > best->key.size()))
                best = &emulator;
        }
        return best;
    }

    // The name of the emulator in the find rules, e.g. "SUYU", found by the file name.
    std::string findRuleEmulator(const std::string& fileName)
    {
        if (!SystemData::sFindRules)
            return "";
        const std::string name {normalized(fileName)};
        std::string best;
        size_t bestLength {0};
        for (const auto& emulator : SystemData::sFindRules->getEmulatorNames()) {
            std::vector<std::string> candidates {emulator};
            for (const auto& path : SystemData::sFindRules->getEmulatorPaths(emulator)) {
                std::string base {Utils::FileSystem::getFileName(path)};
                base = base.substr(0, base.find('*'));
                base = base.substr(0, base.find('.'));
                candidates.emplace_back(base);
            }
            for (const auto& candidate : candidates) {
                const std::string key {normalized(candidate)};
                // Parts of Flatpak names, e.g. "info" in "info.cemu.Cemu", say nothing.
                if (key == "info" || key == "flatpak" || key == "appimage" || key == "linux")
                    continue;
                if (key.size() >= 4 && key.size() > bestLength &&
                    name.find(key) != std::string::npos) {
                    best = emulator;
                    bestLength = key.size();
                }
            }
        }
        return best;
    }

    // The arguments used for the emulator in the bundled systems file, preferably for the
    // given system, e.g. "-f -g %ROM%".
    std::string bundledArguments(const std::string& emulator, const std::string& systemName)
    {
        const std::string custom {EmulatorConfig::getCustomSystemsFile()};
        const std::string token {"%EMULATOR_" + emulator + "%"};
        std::string anySystem;
        for (const auto& path : SystemData::getConfigPath()) {
            if (path == custom)
                continue;
            pugi::xml_document doc;
            if (!doc.load_file(path.c_str()))
                continue;
            for (pugi::xml_node system {doc.child("systemList").child("system")}; system;
                 system = system.next_sibling("system")) {
                const std::string name {Utils::String::trim(system.child("name").text().as_string())};
                for (pugi::xml_node command {system.child("command")}; command;
                     command = command.next_sibling("command")) {
                    const std::string text {command.text().as_string()};
                    const size_t position {text.find(token)};
                    if (position == std::string::npos)
                        continue;
                    const std::string arguments {
                        Utils::String::trim(text.substr(position + token.size()))};
                    if (name == systemName)
                        return arguments;
                    if (anySystem.empty())
                        anySystem = arguments;
                }
            }
        }
        return anySystem;
    }

    // Label for a command that has none, which is needed once there are several commands.
    std::string labelForCommand(const std::string& command)
    {
        for (const auto& token : Utils::String::delimitedStringToVector(command, " ")) {
            if (token.empty())
                continue;
            if (token.find("%EMULATOR_") == 0) {
                std::string name {token.substr(10)};
                if (!name.empty() && name.back() == '%')
                    name.pop_back();
                return name;
            }
            if (token.front() == '%')
                continue;
            std::string path {token};
            path.erase(std::remove(path.begin(), path.end(), '"'), path.end());
            const std::string stem {Utils::FileSystem::getStem(path)};
            return stem.empty() ? "Default" : stem;
        }
        return "Default";
    }

    pugi::xml_node findSystem(pugi::xml_node systemList, const std::string& name)
    {
        for (pugi::xml_node system {systemList.child("system")}; system;
             system = system.next_sibling("system")) {
            if (Utils::String::trim(system.child("name").text().as_string()) == name)
                return system;
        }
        return pugi::xml_node();
    }

    // Loads the custom file and finds the system in it, copied from the bundled file or made
    // from the loaded system if needed.
    bool prepareSystem(SystemData* system,
                       pugi::xml_document& doc,
                       pugi::xml_node& node,
                       std::string& error)
    {
        const std::string path {EmulatorConfig::getCustomSystemsFile()};
        if (Utils::FileSystem::exists(path)) {
            if (!doc.load_file(path.c_str())) {
                error = "Couldn't read " + path;
                return false;
            }
        }
        pugi::xml_node systemList {doc.child("systemList")};
        if (!systemList)
            systemList = doc.append_child("systemList");
        node = findSystem(systemList, system->getName());
        if (node)
            return true;

        // Copied from the bundled file, which is the last one.
        const std::vector<std::string> configPaths {SystemData::getConfigPath()};
        for (auto it = configPaths.crbegin(); it != configPaths.crend(); ++it) {
            if (*it == path)
                continue;
            pugi::xml_document bundled;
            if (!bundled.load_file(it->c_str()))
                continue;
            const pugi::xml_node source {findSystem(bundled.child("systemList"), system->getName())};
            if (source) {
                node = systemList.append_copy(source);
                return true;
            }
        }

        // Made from the loaded system.
        node = systemList.append_child("system");
        node.append_child("name").text() = system->getName().c_str();
        node.append_child("fullname").text() = system->getFullName().c_str();
        node.append_child("path").text() = system->getStartPath().c_str();
        std::string extensions;
        for (const auto& extension : system->getExtensions())
            extensions.append((extensions.empty() ? "" : " ") + extension);
        node.append_child("extension").text() = extensions.c_str();
        for (const auto& [command, label] : system->getSystemEnvData()->mLaunchCommands) {
            pugi::xml_node commandNode {node.append_child("command")};
            if (!label.empty())
                commandNode.append_attribute("label") = label.c_str();
            commandNode.text() = command.c_str();
        }
        node.append_child("platform").text() = system->getName().c_str();
        node.append_child("theme").text() = system->getThemeFolder().c_str();
        return true;
    }

    bool saveDocument(pugi::xml_document& doc, std::string& error)
    {
        const std::string path {EmulatorConfig::getCustomSystemsFile()};
        Utils::FileSystem::createDirectory(Utils::FileSystem::getParent(path));
        if (!doc.child("systemList")) {
            error = "Nothing to write";
            return false;
        }
        if (!doc.save_file(path.c_str(), "    ")) {
            error = "Couldn't write " + path;
            return false;
        }
        LOG(LogInfo) << "EmulatorConfig: Updated \"" << path << "\"";
        return true;
    }
} // namespace

namespace EmulatorConfig
{
    std::string getCustomSystemsFile()
    {
        return Utils::FileSystem::getAppDataDirectory() + "/custom_systems/es_systems.xml";
    }

    bool isInstalled(SystemData* system, const std::string& command)
    {
        if (system == nullptr || system->getRootFolder() == nullptr)
            return false;
        return system->getRootFolder()->isEmulatorInstalled(command);
    }

    std::vector<std::pair<std::string, std::string>> getInstalledCommands(SystemData* system)
    {
        std::vector<std::pair<std::string, std::string>> commands;
        for (const auto& entry : system->getSystemEnvData()->mLaunchCommands) {
            if (isInstalled(system, entry.first))
                commands.emplace_back(entry);
        }
        return commands;
    }

    std::string displayLabel(const std::string& command, const std::string& label)
    {
        return label.empty() ? labelForCommand(command) : label;
    }

    std::string commandForFile(SystemData* system, const std::string& path)
    {
        const std::string quoted {path.find(' ') != std::string::npos ? "\"" + path + "\"" : path};
        if (Utils::String::toLower(Utils::FileSystem::getExtension(path)) == ".sh")
            return "bash " + quoted + " %ROM%";

        // The list of common emulators comes first, as its arguments always start the game
        // in fullscreen. The other emulators get the arguments of the bundled systems file.
        const std::string fileName {Utils::FileSystem::getFileName(path)};
        std::string arguments;
        std::string emulator;
        if (const KnownEmulator* known {findKnownEmulator(normalized(fileName))}) {
            arguments = known->arguments;
            emulator = Utils::String::toUpper(known->key);
        }
        if (arguments.empty()) {
            emulator = findRuleEmulator(fileName);
            if (!emulator.empty())
                arguments =
                    bundledArguments(emulator, system == nullptr ? "" : system->getName());
        }
        if (arguments.find("%ROM%") == std::string::npos)
            arguments.append((arguments.empty() ? "" : " ") + std::string("%ROM%"));
        LOG(LogInfo) << "EmulatorConfig: Launch command for \"" << fileName << "\""
                     << (emulator.empty() ? "" : " (" + emulator + ")") << ": " << arguments;
        return quoted + " " + arguments;
    }

    // The entry of a system in the custom file must keep everything the loaded system has
    // (its extensions and emulators). Without the extensions of its games the custom entry has
    // no games and isn't used, and the added emulator, and the emulator chosen for a game, is
    // lost at the next start. True if something was added.
    bool completeCustomEntry(SystemData* system, pugi::xml_node& node, const std::string& skipLabel)
    {
        bool changed {false};
        pugi::xml_node extensionNode {node.child("extension")};
        if (!extensionNode)
            extensionNode = node.insert_child_after("extension", node.child("path"));
        std::vector<std::string> extensions {
            Utils::String::delimitedStringToVector(extensionNode.text().as_string(), " ")};
        for (const auto& extension : system->getExtensions()) {
            if (std::find(extensions.cbegin(), extensions.cend(), extension) ==
                extensions.cend()) {
                extensions.emplace_back(extension);
                changed = true;
            }
        }
        if (changed) {
            std::string text;
            for (const auto& extension : extensions) {
                if (!extension.empty())
                    text.append((text.empty() ? "" : " ") + extension);
            }
            extensionNode.text() = text.c_str();
        }
        std::set<std::string> labels;
        pugi::xml_node lastCommand;
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;
             commandNode = commandNode.next_sibling("command")) {
            labels.insert(commandNode.attribute("label").as_string());
            lastCommand = commandNode;
        }
        for (const auto& [loadedCommand, loadedLabel] :
             system->getSystemEnvData()->mLaunchCommands) {
            const std::string commandLabel {loadedLabel.empty() ?
                                                labelForCommand(loadedCommand) :
                                                loadedLabel};
            if (commandLabel == skipLabel || labels.count(commandLabel) != 0)
                continue;
            pugi::xml_node copy {lastCommand ?
                                     node.insert_child_after("command", lastCommand) :
                                     node.insert_child_after("command", extensionNode)};
            copy.append_attribute("label") = commandLabel.c_str();
            copy.text() = loadedCommand.c_str();
            labels.insert(commandLabel);
            lastCommand = copy;
            changed = true;
        }
        return changed;
    }

    bool addCustomCommand(SystemData* system,
                          const std::string& label,
                          const std::string& command,
                          std::string& error,
                          bool makeDefault)
    {
        pugi::xml_document doc;
        pugi::xml_node node;
        if (!prepareSystem(system, doc, node, error))
            return false;

        completeCustomEntry(system, node, label);

        // Every command needs a label once there are several.
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;) {
            pugi::xml_node next {commandNode.next_sibling("command")};
            const std::string existing {commandNode.attribute("label").as_string()};
            if (existing == label)
                node.remove_child(commandNode);
            else if (existing.empty())
                commandNode.append_attribute("label") =
                    labelForCommand(commandNode.text().as_string()).c_str();
            commandNode = next;
        }

        // The default emulator is the first one, others are placed after the existing ones.
        pugi::xml_node first {node.child("command")};
        pugi::xml_node last;
        for (pugi::xml_node commandNode {first}; commandNode;
             commandNode = commandNode.next_sibling("command"))
            last = commandNode;
        const pugi::xml_node extension {node.child("extension")};
        pugi::xml_node added {(makeDefault && first) ? node.insert_child_before("command", first) :
                              last                   ? node.insert_child_after("command", last) :
                              extension              ? node.insert_child_after("command", extension) :
                                                       node.append_child("command")};
        added.append_attribute("label") = label.c_str();
        added.append_attribute("custom") = "true";
        added.text() = command.c_str();
        if (!saveDocument(doc, error))
            return false;

        // The change is used at once, without reloading the systems.
        auto& commands {system->getSystemEnvData()->mLaunchCommands};
        commands.erase(std::remove_if(commands.begin(), commands.end(),
                                      [&label](const std::pair<std::string, std::string>& entry) {
                                          return entry.second == label;
                                      }),
                       commands.end());
        for (auto& entry : commands) {
            if (entry.second.empty())
                entry.second = labelForCommand(entry.first);
        }
        if (makeDefault) {
            commands.insert(commands.begin(), std::make_pair(command, label));
            system->setAlternativeEmulator("");
            GamelistFileParser::updateGamelist(system, true);
        }
        else {
            commands.emplace_back(command, label);
        }
        return true;
    }

    bool removeCustomCommand(SystemData* system, const std::string& label, std::string& error)
    {
        pugi::xml_document doc;
        pugi::xml_node node;
        if (!prepareSystem(system, doc, node, error))
            return false;
        bool removed {false};
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;
             commandNode = commandNode.next_sibling("command")) {
            if (commandNode.attribute("label").as_string() == label &&
                commandNode.attribute("custom").as_bool()) {
                node.remove_child(commandNode);
                removed = true;
                break;
            }
        }
        if (!removed) {
            error = "Only emulators that have been added can be removed";
            return false;
        }
        if (!node.child("command")) {
            error = "The system needs at least one emulator";
            return false;
        }
        if (!saveDocument(doc, error))
            return false;

        auto& commands {system->getSystemEnvData()->mLaunchCommands};
        commands.erase(std::remove_if(commands.begin(), commands.end(),
                                      [&label](const std::pair<std::string, std::string>& entry) {
                                          return entry.second == label;
                                      }),
                       commands.end());
        if (system->getAlternativeEmulator() == label) {
            system->setAlternativeEmulator("");
            GamelistFileParser::updateGamelist(system, true);
        }
        return true;
    }

    bool updateCustomCommand(SystemData* system,
                             const std::string& label,
                             const std::string& command,
                             std::string& error)
    {
        pugi::xml_document doc;
        pugi::xml_node node;
        if (!prepareSystem(system, doc, node, error))
            return false;
        bool updated {false};
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;
             commandNode = commandNode.next_sibling("command")) {
            if (commandNode.attribute("label").as_string() == label &&
                commandNode.attribute("custom").as_bool()) {
                commandNode.text() = command.c_str();
                updated = true;
                break;
            }
        }
        if (!updated) {
            error = "Only emulators that have been added can be changed";
            return false;
        }
        if (!saveDocument(doc, error))
            return false;
        for (auto& entry : system->getSystemEnvData()->mLaunchCommands) {
            if (entry.second == label)
                entry.first = command;
        }
        return true;
    }

    bool isForceFullscreen(const std::string& systemName, const std::string& label)
    {
        if (label.empty())
            return false;
        pugi::xml_document doc;
        if (!Utils::FileSystem::exists(getCustomSystemsFile()) ||
            !doc.load_file(getCustomSystemsFile().c_str()))
            return false;
        const pugi::xml_node node {findSystem(doc.child("systemList"), systemName)};
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;
             commandNode = commandNode.next_sibling("command")) {
            if (commandNode.attribute("label").as_string() == label)
                return commandNode.attribute("forcefullscreen").as_bool();
        }
        return false;
    }

    bool setForceFullscreen(SystemData* system,
                            const std::string& label,
                            bool enabled,
                            std::string& error)
    {
        pugi::xml_document doc;
        pugi::xml_node node;
        if (!prepareSystem(system, doc, node, error))
            return false;
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;
             commandNode = commandNode.next_sibling("command")) {
            if (commandNode.attribute("label").as_string() != label)
                continue;
            if (enabled) {
                if (!commandNode.attribute("forcefullscreen"))
                    commandNode.append_attribute("forcefullscreen") = "true";
                else
                    commandNode.attribute("forcefullscreen") = "true";
            }
            else {
                commandNode.remove_attribute("forcefullscreen");
            }
            return saveDocument(doc, error);
        }
        error = "The emulator wasn't found";
        return false;
    }

    std::string getFullscreenTool()
    {
        for (const char* tool : {"wmctrl", "xdotool"}) {
            if (!Utils::FileSystem::getPathToBinary(tool).empty())
                return tool;
        }
        return "";
    }

    bool isWaylandSession()
    {
        const char* type {std::getenv("XDG_SESSION_TYPE")};
        const char* display {std::getenv("WAYLAND_DISPLAY")};
        return (type != nullptr && std::string(type) == "wayland") ||
               (display != nullptr && display[0] != '\0');
    }

    bool isCustomCommand(SystemData* system, const std::string& label)
    {
        pugi::xml_document doc;
        if (!doc.load_file(getCustomSystemsFile().c_str()))
            return false;
        const pugi::xml_node systemList {doc.child("systemList")};
        const pugi::xml_node node {findSystem(systemList, system->getName())};
        for (pugi::xml_node commandNode {node.child("command")}; commandNode;
             commandNode = commandNode.next_sibling("command")) {
            if (commandNode.attribute("label").as_string() == label)
                return commandNode.attribute("custom").as_bool();
        }
        return false;
    }

    namespace
    {
        // The executable of a launch command, without the quotes.
        std::string commandExecutable(const std::string& command)
        {
            std::string rest {Utils::String::trim(command)};
            while (!rest.empty() && rest.front() == '%') {
                const size_t space {rest.find(' ')};
                if (space == std::string::npos)
                    return "";
                rest = Utils::String::trim(rest.substr(space + 1));
            }
            if (!rest.empty() && rest.front() == '"') {
                const size_t quote {rest.find('"', 1)};
                return quote == std::string::npos ? rest.substr(1) : rest.substr(1, quote - 1);
            }
            return rest.substr(0, rest.find(' '));
        }

        // The systems of the bundled systems file that use each emulator of the find rules.
        std::map<std::string, std::set<std::string>> bundledEmulatorSystems()
        {
            std::map<std::string, std::set<std::string>> systems;
            const std::string custom {getCustomSystemsFile()};
            for (const auto& path : SystemData::getConfigPath()) {
                if (path == custom)
                    continue;
                pugi::xml_document doc;
                if (!doc.load_file(path.c_str()))
                    continue;
                for (pugi::xml_node system {doc.child("systemList").child("system")}; system;
                     system = system.next_sibling("system")) {
                    const std::string name {
                        Utils::String::trim(system.child("name").text().as_string())};
                    for (pugi::xml_node command {system.child("command")}; command;
                         command = command.next_sibling("command")) {
                        const std::string text {command.text().as_string()};
                        size_t position {0};
                        while ((position = text.find("%EMULATOR_", position)) !=
                               std::string::npos) {
                            const size_t end {text.find('%', position + 10)};
                            if (end == std::string::npos)
                                break;
                            systems[text.substr(position + 10, end - position - 10)].insert(name);
                            position = end + 1;
                        }
                    }
                }
            }
            return systems;
        }
    } // namespace

    namespace
    {
        // A program compiled for Linux (e.g. an RPCS3 built by the user), without an extension
        // or with ".bin", that can be run. Libraries (".so") are left out.
        bool isProgram(const std::string& path)
        {
            const std::string name {Utils::FileSystem::getFileName(path)};
            const std::string extension {
                Utils::String::toLower(Utils::FileSystem::getExtension(path))};
            if ((!extension.empty() && extension != ".bin") ||
                name.find(".so") != std::string::npos || access(path.c_str(), X_OK) != 0)
                return false;
            char magic[4] {};
            std::ifstream file {path, std::ios::binary};
            return file.read(magic, 4) && magic[0] == 0x7F && magic[1] == 'E' &&
                   magic[2] == 'L' && magic[3] == 'F';
        }
    } // namespace

    std::vector<std::string> scanEmulatorDirectory()
    {
        std::vector<std::string> messages;
        std::string directory {Settings::getInstance()->getString("EmulatorDirectory")};
        while (directory.size() > 1 && directory.back() == '/')
            directory.pop_back();
        if (directory.empty() || !Utils::FileSystem::isDirectory(directory))
            return messages;

        std::vector<SystemData*> systems;
        for (SystemData* system : SystemData::sSystemVector) {
            if (!system->isCollection())
                systems.emplace_back(system);
        }

        // Emulators whose AppImage has been removed from the folder are removed as well.
        for (SystemData* system : systems) {
            const auto commands {system->getSystemEnvData()->mLaunchCommands};
            for (const auto& [command, label] : commands) {
                const std::string executable {commandExecutable(command)};
                if (label.empty() || !Utils::String::startsWith(executable, directory + "/") ||
                    Utils::FileSystem::exists(executable) || !isCustomCommand(system, label))
                    continue;
                std::string error;
                if (removeCustomCommand(system, label, error)) {
                    messages.emplace_back("Removed emulator: " + system->getFullName() + " - " +
                                          Utils::FileSystem::getFileName(executable));
                }
                else {
                    LOG(LogWarning) << "EmulatorConfig: Couldn't remove \"" << label
                                    << "\": " << error;
                }
            }
        }

        // Entries of the custom file that miss extensions or emulators of their system, e.g. made
        // from a system that had other extensions, are completed (used at the next start).
        {
            pugi::xml_document doc;
            const std::string path {getCustomSystemsFile()};
            if (Utils::FileSystem::exists(path) && doc.load_file(path.c_str())) {
                bool changed {false};
                for (SystemData* system : systems) {
                    pugi::xml_node node {findSystem(doc.child("systemList"), system->getName())};
                    if (!node)
                        continue;
                    bool added {false};
                    for (pugi::xml_node commandNode {node.child("command")}; commandNode;
                         commandNode = commandNode.next_sibling("command")) {
                        if (commandNode.attribute("custom").as_bool())
                            added = true;
                    }
                    if (added && completeCustomEntry(system, node, "")) {
                        changed = true;
                        messages.emplace_back("Repaired the emulators of " +
                                              system->getFullName() + ", restart EmuSPLASH");
                    }
                }
                std::string error;
                if (changed && !saveDocument(doc, error)) {
                    LOG(LogWarning) << "EmulatorConfig: Couldn't save " << path << ": " << error;
                }
            }
        }

        // The AppImages and the programs (compiled emulators) in the folder and its subfolders.
        std::vector<std::string> files;
        std::set<std::string> programs;
        for (const auto& path : Utils::FileSystem::getDirContent(directory, true)) {
            const std::string name {Utils::FileSystem::getFileName(path)};
            if (name.empty() || name.front() == '.' || !Utils::FileSystem::isRegularFile(path))
                continue;
            if (Utils::String::toLower(Utils::FileSystem::getExtension(path)) == ".appimage") {
                files.emplace_back(path);
            }
            else if (isProgram(path)) {
                files.emplace_back(path);
                programs.insert(path);
            }
        }
        std::sort(files.begin(), files.end());

        const std::map<std::string, std::set<std::string>> bundled {bundledEmulatorSystems()};

        // The emulators in the custom systems file, in case a system is loaded without them,
        // so that they're not announced as new at every start.
        std::map<std::string, std::set<std::string>> customExecutables;
        {
            pugi::xml_document customDoc;
            if (customDoc.load_file(getCustomSystemsFile().c_str())) {
                for (pugi::xml_node systemNode : customDoc.child("systemList").children("system")) {
                    const std::string systemName {systemNode.child_value("name")};
                    for (pugi::xml_node commandNode : systemNode.children("command"))
                        customExecutables[systemName].insert(
                            commandExecutable(commandNode.text().as_string()));
                }
            }
        }
        for (const auto& path : files) {
            const std::string fileName {Utils::FileSystem::getFileName(path)};
            const std::string name {normalized(fileName)};
            // RetroArch needs a core for each system, it's set up using the bundled entries.
            if (name.find("retroarch") != std::string::npos)
                continue;

            std::set<std::string> systemNames;
            if (const KnownEmulator* known {findKnownEmulator(name)}) {
                systemNames.insert(known->systems.cbegin(), known->systems.cend());
                const auto it {bundled.find(Utils::String::toUpper(known->key))};
                if (it != bundled.cend())
                    systemNames.insert(it->second.cbegin(), it->second.cend());
            }
            else {
                const auto it {bundled.find(findRuleEmulator(fileName))};
                if (it != bundled.cend())
                    systemNames = it->second;
            }
            if (systemNames.empty()) {
                // A folder of a compiled emulator has other programs too, they're skipped quietly.
                if (programs.count(path) == 0) {
                    LOG(LogInfo) << "EmulatorConfig: Couldn't tell the system of \"" << path
                                 << "\"";
                }
                continue;
            }

            // Added to the systems that have games, unless it's there already.
            for (SystemData* system : systems) {
                if (systemNames.count(system->getName()) == 0)
                    continue;
                bool exists {false};
                std::set<std::string> labels;
                for (const auto& [command, label] : system->getSystemEnvData()->mLaunchCommands) {
                    labels.insert(label);
                    if (commandExecutable(command) == path)
                        exists = true;
                }
                const auto customIt {customExecutables.find(system->getName())};
                if (customIt != customExecutables.cend() && customIt->second.count(path) != 0)
                    exists = true;
                if (exists)
                    continue;
                std::string label {Utils::FileSystem::getStem(path)};
                for (int number {2}; labels.count(label) != 0; ++number)
                    label = Utils::FileSystem::getStem(path) + " (" + std::to_string(number) + ")";
                std::string error;
                if (addCustomCommand(system, label, commandForFile(system, path), error, false)) {
                    messages.emplace_back("Found new emulator: " + system->getFullName() + " - " +
                                          fileName);
                }
                else {
                    LOG(LogWarning) << "EmulatorConfig: Couldn't add \"" << path
                                    << "\": " << error;
                }
            }
        }
        for (const auto& message : messages)
            LOG(LogInfo) << "EmulatorConfig: " << message;
        return messages;
    }

    namespace
    {
        // Output of a command, run using the shell.
        std::string runCommand(const std::string& command)
        {
            std::string output;
            FILE* pipe {popen((command + " 2>/dev/null").c_str(), "r")};
            if (pipe == nullptr)
                return output;
            char buffer[256];
            while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
                output.append(buffer);
            pclose(pipe);
            return output;
        }

        // The ids of the windows that are shown.
        std::vector<std::string> listWindows(const std::string& tool)
        {
            std::vector<std::string> windows;
            const std::string output {runCommand(
                tool == "wmctrl" ? "wmctrl -l" : "xdotool search --onlyvisible --name .")};
            for (const auto& line : Utils::String::delimitedStringToVector(output, "\n")) {
                const std::string id {line.substr(0, line.find(' '))};
                if (!id.empty())
                    windows.emplace_back(id);
            }
            return windows;
        }
    } // namespace

    FullscreenWatcher::FullscreenWatcher()
        : mTool {getFullscreenTool()}
        , mStop {false}
    {
        if (mTool.empty()) {
            LOG(LogWarning) << "EmulatorConfig: Forced fullscreen needs wmctrl or xdotool, "
                               "neither was found";
            return;
        }
        // The windows that exist before the emulator is started, e.g. the one of EmuSPLASH.
        std::vector<std::string> before {listWindows(mTool)};
        mThread = std::thread(&FullscreenWatcher::run, this, before);
    }

    FullscreenWatcher::~FullscreenWatcher()
    {
        mStop = true;
        if (mThread.joinable())
            mThread.join();
    }

    void FullscreenWatcher::run(std::vector<std::string> before)
    {
        const std::set<std::string> known(before.cbegin(), before.cend());
        std::set<std::string> handled;
        const auto start {std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point firstFound {};
        // Windows that appear within 90 seconds are made fullscreen, and once one has been
        // found the ones that follow within 15 seconds, e.g. the game window after a
        // loading window.
        while (!mStop) {
            const auto now {std::chrono::steady_clock::now()};
            if (now - start > std::chrono::seconds(90))
                break;
            if (!handled.empty() && now - firstFound > std::chrono::seconds(15))
                break;
            for (const auto& window : listWindows(mTool)) {
                if (known.count(window) != 0 || handled.count(window) != 0)
                    continue;
                // Some time for the emulator to set up its window first.
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                if (mStop)
                    return;
                runCommand(mTool == "wmctrl" ?
                               "wmctrl -i -r " + window + " -b add,fullscreen" :
                               "xdotool windowstate --add FULLSCREEN " + window);
                LOG(LogInfo) << "EmulatorConfig: Made the window " << window << " fullscreen";
                if (handled.empty())
                    firstFound = std::chrono::steady_clock::now();
                handled.insert(window);
            }
            for (int i {0}; i < 5 && !mStop; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
} // namespace EmulatorConfig
