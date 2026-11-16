//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  PlatformUtil.cpp
//
//  Platform utility functions.
//

#if !defined(_WIN64)
#define MAX_GAME_LOG_OUTPUT 5242880
#endif

#include "utils/PlatformUtil.h"

#include "InputManager.h"
#include "Log.h"
#include "Scripting.h"
#include "Settings.h"
#include "Window.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"
#if defined(_WIN64)
#include "utils/StringUtil.h"
#include <SDL2/SDL_opengl.h>
#endif

#include <SDL2/SDL.h>
#include <SDL2/SDL_events.h>

#if !defined(_WIN64)
#include <unistd.h>
#endif

#if defined(__HAIKU__)
#include <sys/time.h>
#endif

#include <array>
#include <fcntl.h>
#include <fstream>

#define MAX_RUNNING_COMMANDS 8
inline static std::atomic<int> sRunningCommands {0};

namespace Utils
{
    namespace Platform
    {
        namespace
        {
            std::function<void(int)> sLaunchWatcher;
        } // namespace

        void setLaunchWatcher(const std::function<void(int)>& watcher) { sLaunchWatcher = watcher; }

#if defined(__linux__) && !defined(__ANDROID__)
        namespace
        {
            // Programs are also looked for in the sbin folders, which often aren't in the
            // path of a normal user.
            std::string findProgram(const std::string& name)
            {
                const std::string inPath {Utils::FileSystem::getPathToBinary(name)};
                if (!inPath.empty())
                    return inPath + "/" + name;
                for (const char* directory : {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}) {
                    const std::string path {std::string(directory) + "/" + name};
                    if (Utils::FileSystem::isRegularFile(path) ||
                        Utils::FileSystem::isSymlink(path))
                        return path;
                }
                return "";
            }

            std::string commandOutput(const std::string& command)
            {
                std::string output;
                FILE* pipe {popen((command + " 2>/dev/null").c_str(), "r")};
                if (pipe == nullptr)
                    return output;
                std::array<char, 256> buffer {};
                while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
                    output.append(buffer.data());
                pclose(pipe);
                return output;
            }

            // Whether logind (systemd-logind or elogind) is running on the system bus.
            bool logindRunning()
            {
                if (Utils::FileSystem::isDirectory("/run/systemd/seats"))
                    return true;
                const std::string dbusSend {findProgram("dbus-send")};
                if (dbusSend.empty())
                    return false;
                const std::string output {commandOutput(
                    dbusSend + " --system --print-reply --dest=org.freedesktop.DBus "
                               "/org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner "
                               "string:org.freedesktop.login1")};
                return output.find("boolean true") != std::string::npos;
            }

            std::string initSystem()
            {
                std::string name;
                std::ifstream file {"/proc/1/comm"};
                std::getline(file, name);
                if (name == "init" && Utils::FileSystem::exists("/run/openrc"))
                    return "openrc";
                if (name.find("s6") != std::string::npos)
                    return "s6";
                return name;
            }

            // Root rights for the command, from sudo or doas without a password.
            std::string withRootRights(const std::string& command, const std::string& program)
            {
                if (geteuid() == 0)
                    return command;
                const std::string sudo {findProgram("sudo")};
                if (!sudo.empty() &&
                    system((sudo + " -n -l " + program + " >/dev/null 2>&1").c_str()) == 0)
                    return sudo + " -n " + command;
                const std::string doas {findProgram("doas")};
                if (!doas.empty() && Utils::FileSystem::exists("/etc/doas.conf")) {
                    const std::string output {
                        commandOutput(doas + " -C /etc/doas.conf " + program)};
                    if (output.find("permit nopass") != std::string::npos)
                        return doas + " -n " + command;
                }
                return "";
            }
        } // namespace
#endif

        std::string getPowerCommand(QuitMode mode)
        {
#if defined(__linux__) && !defined(__ANDROID__)
            const bool reboot {mode == QuitMode::REBOOT};
            const std::string custom {Utils::String::trim(Settings::getInstance()->getString(
                reboot ? "RebootCommand" : "PowerOffCommand"))};
            if (!custom.empty())
                return custom;

            // systemd, and logind which is used with other init systems as well.
            const std::string systemctl {findProgram("systemctl")};
            if (!systemctl.empty() && Utils::FileSystem::isDirectory("/run/systemd/system"))
                return systemctl + (reboot ? " reboot" : " poweroff");
            if (logindRunning()) {
                const std::string loginctl {findProgram("loginctl")};
                if (!loginctl.empty())
                    return loginctl + (reboot ? " reboot" : " poweroff");
                const std::string dbusSend {findProgram("dbus-send")};
                if (!dbusSend.empty()) {
                    return dbusSend + " --system --print-reply "
                                      "--dest=org.freedesktop.login1 /org/freedesktop/login1 "
                                      "org.freedesktop.login1.Manager." +
                           std::string(reboot ? "Reboot" : "PowerOff") + " boolean:true";
                }
            }

            // The commands of the init system, which need root rights.
            const std::string init {initSystem()};
            std::string program;
            std::string arguments;
            if (init == "runit") {
                program = findProgram("runit-init");
                arguments = reboot ? " 6" : " 0";
            }
            else if (init == "dinit") {
                program = findProgram("dinitctl");
                arguments = reboot ? " reboot" : " shutdown";
            }
            else if (init == "openrc") {
                program = findProgram("openrc-shutdown");
                arguments = reboot ? " -r now" : " -p now";
            }
            else if (init == "s6") {
                program = findProgram("s6-linux-init-hpr");
                arguments = reboot ? " -r" : " -p";
            }
            if (program.empty()) {
                program = findProgram("shutdown");
                arguments = reboot ? " -r now" : " -h now";
            }
            if (program.empty())
                return "";
            return withRootRights(program + arguments, program);
#elif defined(__APPLE__)
            return "";
#elif defined(_WIN64)
            return mode == QuitMode::REBOOT ? "shutdown -r -t 0" : "shutdown -s -t 0";
#elif defined(__FreeBSD__)
            return mode == QuitMode::REBOOT ? "shutdown -r now" : "shutdown -p now";
#elif defined(__HAIKU__)
            return mode == QuitMode::REBOOT ? "shutdown -r" : "shutdown";
#else
            return mode == QuitMode::REBOOT ? "shutdown --reboot now" : "shutdown --poweroff now";
#endif
        }

        std::string getPowerCommandHelp(QuitMode mode)
        {
            const bool reboot {mode == QuitMode::REBOOT};
            return std::string("Couldn't find a way to ") + (reboot ? "reboot" : "power off") +
                   " the computer. One of these is needed:\n"
                   "- systemd, or elogind with other init systems (e.g. sudo apt install "
                   "elogind)\n"
                   "- sudo or doas allowing the shutdown command of the init system without a "
                   "password\n"
                   "- the " +
                   (reboot ? std::string("RebootCommand") : std::string("PowerOffCommand")) +
                   " setting in es_settings.xml, e.g. a command of your own";
        }

        int runRebootCommand()
        {
#if defined(__APPLE__)
            return 0;
#else
            const std::string command {getPowerCommand(QuitMode::REBOOT)};
            if (command.empty()) {
                LOG(LogError) << "Couldn't find a command for rebooting";
                return -1;
            }
            LOG(LogInfo) << "Rebooting using \"" << command << "\"";
            return system(command.c_str());
#endif
        }

        int runPoweroffCommand()
        {
#if defined(__APPLE__)
            return 0;
#else
            const std::string command {getPowerCommand(QuitMode::POWEROFF)};
            if (command.empty()) {
                LOG(LogError) << "Couldn't find a command for powering off";
                return -1;
            }
            LOG(LogInfo) << "Powering off using \"" << command << "\"";
            return system(command.c_str());
#endif
        }

        int runSuspendCommand()
        {
            #if defined(__APPLE__)
            return 0;
            #elif defined(_WIN64)
            return system("rundll32.exe powrprof.dll,SetSuspendState 0,1,0");
            #elif defined(__FreeBSD__)
            if (Utils::FileSystem::getPathToBinary("zzz") != "")
                return system("zzz");

            return -1;
            #else
            // This should work on all Linux distributions that use systemd.
            if (Utils::FileSystem::getPathToBinary("systemctl") != "")
                return system("systemctl suspend");

            // Fallback to the zzz command on some distributions.
            if (Utils::FileSystem::getPathToBinary("zzz") != "")
                return system("zzz");

            return -1;
            #endif
        }

        int runSystemCommand(const std::string& cmdUtf8, bool blocking)
        {
            #if defined(_WIN64) || defined(__IOS__)
            return 0;
            #else
            if (!blocking) {
                // We need to keep track of how many scripts we have executed so in case a script
                // hangs forever we just don't keep forking processes indefinitely which will
                // completely exhaust the system eventually. The approach is very simple, just
                // refuse to launch any scripts above MAX_RUNNING_COMMANDS and then leave it to
                // the user to figure out why their scripts are hanging.
                if (sRunningCommands >= MAX_RUNNING_COMMANDS) {
                    LOG(LogWarning)
                    << "runSystemCommand: Maximum running commands of " << MAX_RUNNING_COMMANDS
                    << " reached, skipping execution of: " << cmdUtf8;
                    return -1;
                }

                try {
                    std::thread([cmdUtf8]() {
                        ++sRunningCommands;
                        static_cast<void>(system(cmdUtf8.c_str()));
                        --sRunningCommands;
                    }).detach();
                }
                catch (...) {
                    return -1;
                }

                return 0;
            }
            else {
                return system(cmdUtf8.c_str());
            }
            #endif
        }

        int runSystemCommand(const std::wstring& cmdUtf16, bool blocking)
        {
            #if defined(_WIN64)
            if (!blocking && sRunningCommands >= MAX_RUNNING_COMMANDS) {
                LOG(LogWarning) << "runSystemCommand: Maximum running commands of "
                << MAX_RUNNING_COMMANDS << " reached, skipping execution of: "
                << Utils::String::replace(
                    Utils::String::wideStringToString(cmdUtf16), "/", "\\");
                return -1;
            }

            auto createProcessFunc = [](std::wstring cmdUtf16) {
                STARTUPINFOW si {};
                PROCESS_INFORMATION pi {};

                si.cb = sizeof(si);

                // Always hide the console window.
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;

                bool processReturnValue {true};
                DWORD errorCode {0};

                std::wstring startDirectoryTemp {Utils::String::stringToWideString(
                    Utils::FileSystem::getAppDataDirectory() + "/scripts/")};
                    wchar_t* startDirectory {&startDirectoryTemp[0]};

                    // clang-format off
                    processReturnValue = CreateProcessW(
                        nullptr,                         // No application name (use command line).
                    const_cast<wchar_t*>(cmdUtf16.c_str()), // Command line.
                                                        nullptr,                         // Process attributes.
                                                        nullptr,                         // Thread attributes.
                                                        FALSE,                           // Handles inheritance.
                                                        0,                               // Creation flags.
                                                        nullptr,                         // Use parent's environment block.
                                                        startDirectory,                  // Starting directory.
                                                        &si,                             // Pointer to the STARTUPINFOW structure.
                                                        &pi);                            // Pointer to the PROCESS_INFORMATION structure.
                    // clang-format on

                    // We always wait forever for the script to finish its execution.
                    WaitForSingleObject(pi.hProcess, INFINITE);

                    // If the return value is false, then something failed.
                    if (!processReturnValue) {
                        LPWSTR pBuffer {nullptr};

                        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                                       nullptr, GetLastError(),
                                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                       reinterpret_cast<LPWSTR>(&pBuffer), 0, nullptr);

                        errorCode = GetLastError();

                        if (pBuffer)
                            LocalFree(pBuffer);
                    }
                    else {
                        // Close process and thread handles.
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    }

                    return errorCode;
            };

            if (!blocking) {
                try {
                    std::thread([createProcessFunc, cmdUtf16]() {
                        ++sRunningCommands;
                        createProcessFunc(cmdUtf16);
                        --sRunningCommands;
                    }).detach();
                }
                catch (...) {
                    return -1;
                }

                return 0;
            }
            else {
                return createProcessFunc(cmdUtf16);
            }
            #else
            return 0;
            #endif
        }

        int launchGameUnix(const std::string& cmdUtf8,
                           const std::string& startDirectory,
                           bool runInBackground)
        {
            #if defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)
            std::string command {std::string(cmdUtf8) + " 2>&1 &"};

            // Launching games while keeping EmuSPLASH running in the background is very crude as for
            // instance no output from the command is captured and no real error handling is
            // implemented. It should therefore only be used when absolutely necessary.
            if (runInBackground) {
                if (startDirectory != "")
                    command = "cd " + startDirectory + " && " + command;
                LOG(LogDebug)
                << "Platform::launchGameUnix(): Launching game while keeping EmuSPLASH running "
                "in the background, no command output will be written to the log file";
                #if defined(__IOS__)
                return 0;
                #else
                return system(command.c_str());
                #endif
            }

            FILE* commandPipe;
            std::array<char, 128> buffer {};
            std::string commandOutput;
            int returnValue;

            if (startDirectory != "")
                command = "cd " + startDirectory + " && " + command;

            if (!(commandPipe = reinterpret_cast<FILE*>(popen(command.c_str(), "r")))) {
                LOG(LogError) << "Couldn't open pipe to command";
                return -1;
            }

            int fd {fileno(commandPipe)};
            fd_set readfds;
            struct timeval timeout;

            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            timeout.tv_sec = 0;
            timeout.tv_usec = 10000;

            SDL_Event event {};

            // We're not completely suspended when launching a game, instead we'll continue to
            // poll events. As part of this we'll handle adding and removal of controllers, all
            // other events are discarded.
            while (true) {
                // Check if pipe is available for reading.
                if (select(fd + 1, &readfds, nullptr, nullptr, &timeout) != 0) {
                    if (fgets(buffer.data(), buffer.size(), commandPipe) != nullptr)
                        commandOutput.append(
                            commandOutput.length() < MAX_GAME_LOG_OUTPUT ? buffer.data() : "");
                        else
                            break;
                }
                FD_SET(fd, &readfds);
                timeout.tv_sec = 0;
                timeout.tv_usec = 10000;
                // Drop all events except those for adding and removing controllers.
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_CONTROLLERDEVICEADDED ||
                        event.type == SDL_CONTROLLERDEVICEREMOVED)
                        InputManager::getInstance().parseEvent(event);
                }
                if (sLaunchWatcher)
                    sLaunchWatcher(fd);
            }

            returnValue = pclose(commandPipe);

            // We need to shift the return value as it contains some flags (which we don't need).
            returnValue >>= 8;

            const size_t commandOutputSize {commandOutput.size()};

            // Remove any trailing newline from the command output.
            if (commandOutput.size()) {
                if (commandOutput.back() == '\n')
                    commandOutput.pop_back();
            }

            if (returnValue) {
                LOG(LogError) << "launchGameUnix - return value "
                << std::to_string(returnValue) + ":";
                if (commandOutput.size())
                    LOG(LogError) << commandOutput;
                else
                    LOG(LogError) << "No error output provided by game or emulator";
            }
            else if (commandOutput.size()) {
                LOG(LogDebug) << "Platform::launchGameUnix():";
                LOG(LogDebug) << "Output from launched game:\n" << commandOutput;
                if (commandOutputSize >= MAX_GAME_LOG_OUTPUT) {
                    LOG(LogWarning) << "Output was capped to " << commandOutputSize << " bytes";
                }
            }

            return returnValue;

            #else
            return 0;
            #endif
        }

        int launchGameWindows(const std::wstring& cmdUtf16,
                              const std::wstring& startDirectory,
                              bool runInBackground,
                              bool hideWindow)
        {
            #if defined(_WIN64)
            STARTUPINFOW si {};
            PROCESS_INFORMATION pi {};

            si.cb = sizeof(si);
            if (hideWindow) {
                // Optionally hide the window. This is intended primarily for hiding console windows
                // when launching scripts (used for example by Steam games and source ports).
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
            }
            bool processReturnValue {true};
            DWORD errorCode {0};

            std::wstring startDirectoryTemp {startDirectory};
            wchar_t* startDir {startDirectory == L"" ? nullptr : &startDirectoryTemp[0]};

            // clang-format off
            processReturnValue = CreateProcessW(
                nullptr,                         // No application name (use command line).
            const_cast<wchar_t*>(cmdUtf16.c_str()), // Command line.
                                                nullptr,                         // Process attributes.
                                                nullptr,                         // Thread attributes.
                                                FALSE,                           // Handles inheritance.
                                                0,                               // Creation flags.
                                                nullptr,                         // Use parent's environment block.
                                                startDir,                        // Starting directory, possibly the same as parent.
                                                &si,                             // Pointer to the STARTUPINFOW structure.
                                                &pi);                            // Pointer to the PROCESS_INFORMATION structure.
            // clang-format on

            if (!runInBackground) {
                int width {};
                int height {};

                // Hack to make the emulator window render correctly when launching games while
                // running in full screen mode. If not done, the emulator window will simply be
                // black although the game actually works and outputs sounds, accepts input etc.
                // There is sometimes a white flash the first time an emulator is started during
                // the program session and possibly some other brief screen flashing on game
                // launch but it's at least a tolerable workaround.
                SDL_GetWindowSize(Renderer::getInstance()->getSDLWindow(), &width, &height);
                SDL_SetWindowSize(Renderer::getInstance()->getSDLWindow(), width + 1, height);
                SDL_Delay(100);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                Renderer::getInstance()->swapBuffers();
                SDL_Event event {};

                // We're not completely suspended when launching a game, instead we'll continue to
                // poll events. As part of this we'll handle adding and removal of controllers, all
                // other events are discarded.
                while (true) {
                    if (WaitForSingleObject(pi.hProcess, 10) == 0)
                        break;
                    // Drop all events except those for adding and removing controllers.
                    while (SDL_PollEvent(&event)) {
                        if (event.type == SDL_CONTROLLERDEVICEADDED ||
                            event.type == SDL_CONTROLLERDEVICEREMOVED)
                            InputManager::getInstance().parseEvent(event);
                    }
                }

                SDL_SetWindowSize(Renderer::getInstance()->getSDLWindow(), width, height);
            }

            // If the return value is false, then something failed.
            if (!processReturnValue) {
                LPWSTR pBuffer {nullptr};

                FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr,
                               GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPWSTR>(&pBuffer), 0, nullptr);

                errorCode = GetLastError();
                std::string errorMessage {Utils::String::wideStringToString(pBuffer)};

                if (pBuffer)
                    LocalFree(pBuffer);

                // Remove trailing newline from the error message.
                if (errorMessage.size()) {
                    if (errorMessage.back() == '\n')
                        errorMessage.pop_back();
                    if (errorMessage.size()) {
                        if (errorMessage.back() == '\r')
                            errorMessage.pop_back();
                    }
                }

                LOG(LogError) << "launchGameWindows - system error code " << errorCode << ": "
                << errorMessage;
            }
            else {
                // Close process and thread handles.
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }

            return errorCode;

            #else // _WIN64
            return 0;
            #endif
        }

        unsigned int getTaskbarState()
        {
            #if defined(_WIN64)
            APPBARDATA barData;
            barData.cbSize = sizeof(APPBARDATA);
            return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &barData));
            #else
            return 0;
            #endif
        }

        void hideTaskbar()
        {
            #if defined(_WIN64)
            APPBARDATA barData;
            barData.cbSize = sizeof(APPBARDATA);
            barData.lParam = ABS_AUTOHIDE;
            SHAppBarMessage(ABM_SETSTATE, &barData);
            #endif
        }

        void revertTaskbarState(unsigned int& state)
        {
            #if defined(_WIN64)
            APPBARDATA barData;
            barData.cbSize = sizeof(APPBARDATA);
            barData.lParam = state;
            SHAppBarMessage(ABM_SETSTATE, &barData);
            #endif
        }

        int quitES(QuitMode mode)
        {
            sQuitMode = mode;

            SDL_Event quit {};
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
            return 0;
        }

        void processQuitMode()
        {
            switch (sQuitMode) {
                case QuitMode::REBOOT: {
                    LOG(LogInfo) << "Rebooting system";
                    Scripting::fireEvent("reboot");
                    Scripting::fireEvent("quit");
                    runRebootCommand();
                    break;
                }
                case QuitMode::POWEROFF: {
                    LOG(LogInfo) << "Powering off system";
                    Scripting::fireEvent("poweroff");
                    Scripting::fireEvent("quit");
                    runPoweroffCommand();
                    break;
                }
                default: {
                    Scripting::fireEvent("quit");
                    break;
                }
            }
        }

        void emergencyShutdown()
        {
            LOG(LogError) << "Critical - Performing emergency shutdown...";
            Scripting::fireEvent("quit");

            while (Window::getInstance()->getGuiStackSize() > 1 &&
                Window::getInstance()->peekGui() != nullptr)
                delete Window::getInstance()->peekGui();
            Window::getInstance()->deinit();
            Log::flush();

            exit(EXIT_FAILURE);
        }

    } // namespace Platform

} // namespace Utils
