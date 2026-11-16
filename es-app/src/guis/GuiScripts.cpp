//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiScripts.cpp
//
//  "SKRIPTID" menu which lists the user's fish functions from
//  ~/.config/fish/functions and runs the selected one in the background.
//  A notification popup is shown when the script has finished.
//

#include "guis/GuiScripts.h"

#include "Log.h"
#include "Window.h"
#include "components/TextComponent.h"
#include "guis/GuiMsgBox.h"
#include "guis/GuiSettings.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

#if !defined(_WIN64)
#include <sys/wait.h>
#endif

namespace
{
    // Scripts that are currently running, so the same one is not started twice.
    std::mutex sRunningMutex;
    std::set<std::string> sRunning;

    // Quotes a string for use as a single argument in a /bin/sh command line.
    std::string shellQuote(const std::string& str)
    {
        return "'" + Utils::String::replace(str, "'", "'\\''") + "'";
    }

    // The last non-empty line of the output, shortened to fit in a popup.
    std::string lastOutputLine(const std::string& output)
    {
        std::string last;
        size_t start {0};
        while (start <= output.size()) {
            size_t end {output.find('\n', start)};
            if (end == std::string::npos)
                end = output.size();
            const std::string line {Utils::String::trim(output.substr(start, end - start))};
            if (!line.empty())
                last = line;
            start = end + 1;
        }
        if (last.size() > 80)
            last = last.substr(0, 77) + "...";
        return last;
    }
} // namespace

namespace GuiScripts
{
    std::string getScriptsDirectory()
    {
        std::string configHome;
        if (const char* xdg {getenv("XDG_CONFIG_HOME")}; xdg != nullptr && xdg[0] != '\0')
            configHome = xdg;
        else
            configHome = Utils::FileSystem::getSystemHomeDirectory() + "/.config";
        return configHome + "/fish/functions";
    }

    std::vector<std::string> getScripts()
    {
        std::vector<std::string> scripts;
        const std::string dir {getScriptsDirectory()};
        if (!Utils::FileSystem::isDirectory(dir))
            return scripts;

        for (const auto& path : Utils::FileSystem::getDirContent(dir)) {
            if (!Utils::FileSystem::isRegularFile(path) ||
                Utils::FileSystem::getExtension(path) != ".fish")
                continue;
            const std::string name {Utils::FileSystem::getStem(path)};
            // Skip fish's own prompt/greeting functions and private helpers.
            if (name.empty() || name.front() == '_' || name.rfind("fish_", 0) == 0)
                continue;
            scripts.emplace_back(name);
        }
        std::sort(scripts.begin(), scripts.end());
        return scripts;
    }

    void runScript(const std::string& name)
    {
        {
            std::unique_lock<std::mutex> lock {sRunningMutex};
            if (sRunning.count(name) != 0) {
                Window::getInstance()->queueInfoPopup(
                    "SKRIPT '" + name + "' JUBA TÖÖTAB", 3000);
                return;
            }
            sRunning.insert(name);
        }

        Window::getInstance()->queueInfoPopup("KÄIVITAN SKRIPTI '" + name + "'...", 2000);
        LOG(LogInfo) << "Running fish script \"" << name << "\" in the background";

        std::thread([name] {
            // The function is autoloaded by fish from ~/.config/fish/functions, just like
            // when typing its name in a terminal. No terminal is attached and stdin is
            // closed so scripts can't wait for input.
            const std::string command {"fish -c " + shellQuote(name) + " </dev/null 2>&1"};

            std::string output;
            int exitCode {-1};
#if !defined(_WIN64)
            if (FILE* pipe {popen(command.c_str(), "r")}) {
                char buffer[512];
                while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
                    output.append(buffer);
                const int status {pclose(pipe)};
                if (status != -1 && WIFEXITED(status))
                    exitCode = WEXITSTATUS(status);
            }
#endif
            const std::string lastLine {lastOutputLine(output)};
            LOG(LogInfo) << "Fish script \"" << name << "\" finished with exit code "
                         << exitCode;
            if (!output.empty()) {
                LOG(LogDebug) << "Script output:\n" << output;
            }

            std::string message;
            if (exitCode == 0)
                message = "SKRIPT '" + name + "' VALMIS";
            else if (exitCode == 127)
                message = "SKRIPTI '" + name + "' EI SAANUD KÄIVITADA (KAS FISH ON PAIGALDATUD?)";
            else
                message = "SKRIPT '" + name + "' EBAÕNNESTUS (KOOD " + std::to_string(exitCode) +
                          ")";
            if (!lastLine.empty() && exitCode != 127)
                message.append(": " + lastLine);

            Window::getInstance()->queueInfoPopup(message, 6000);

            std::unique_lock<std::mutex> lock {sRunningMutex};
            sRunning.erase(name);
        }).detach();
    }

    GuiSettings* create(unsigned int textColor)
    {
        auto s = new GuiSettings("SCRIPTS");
        s->getMenu().setSubtitle(getScriptsDirectory());
        const unsigned int color {textColor};
        const std::vector<std::string> scripts {getScripts()};

        if (scripts.empty()) {
            ComponentListRow row;
            row.addElement(std::make_shared<TextComponent>("SKRIPTE EI LEITUD",
                                                           Font::get(FONT_SIZE_MEDIUM), color),
                           true);
            s->addRow(row);
            return s;
        }

        for (const auto& name : scripts) {
            ComponentListRow row;
            row.makeAcceptInputHandler([name] {
                Window::getInstance()->pushGui(new GuiMsgBox(
                    "KÄIVITA SKRIPT '" + name + "'?", "JAH", [name] { runScript(name); }, "EI",
                    nullptr));
            });
            auto text = std::make_shared<TextComponent>(name, Font::get(FONT_SIZE_MEDIUM), color);
            text->setSelectable(true);
            row.addElement(text, true);
            s->addRow(row);
        }
        return s;
    }
} // namespace GuiScripts
