//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GameHotkeys.cpp
//
//  The hotkeys while a game runs, see GameHotkeys.h.
//

#include "emulators/GameHotkeys.h"

#include "Log.h"
#include "Settings.h"
#include "utils/PlatformUtil.h"
#include "utils/StringUtil.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace GameHotkeys
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        // A combination of buttons, which counts when it has been held for a moment so that a
        // quick touch of both doesn't trigger it.
        struct Combo {
            std::vector<std::string> buttons;
            bool pressing {false};
            Clock::time_point pressedSince;
            bool fired {false};
        };

        struct KeyWatch {
            KeyAction action;
            Combo combo;
        };

#if defined(__linux__) && !defined(__ANDROID__)
        // A pad read directly from its event device, when SDL has none.
        struct EventPad {
            int fd {-1};
            bool triggerButtons {false};
            input_absinfo leftTrigger {};
            input_absinfo rightTrigger {};
            bool hasAxes {false};
        };
#endif

        struct State {
            std::vector<SDL_GameController*> controllers;
#if defined(__linux__) && !defined(__ANDROID__)
            std::vector<EventPad> eventPads;
#endif
            std::string previousHint;
            bool hadHint {false};
            bool active {false};
            Combo close;
            int waitMs {0};
            int killWaitMs {2000};
            bool triggered {false};
            Clock::time_point triggeredAt;
            bool terminated {false};
            Clock::time_point terminatedAt;
            bool killed {false};
            std::vector<KeyWatch> keys;
        };
        State sState;

        bool isPressedSdl(SDL_GameController* controller, const std::string& button)
        {
            static const std::vector<std::pair<std::string, SDL_GameControllerButton>> buttons {
                {"south", SDL_CONTROLLER_BUTTON_A},
                {"east", SDL_CONTROLLER_BUTTON_B},
                {"west", SDL_CONTROLLER_BUTTON_X},
                {"north", SDL_CONTROLLER_BUTTON_Y},
                {"back", SDL_CONTROLLER_BUTTON_BACK},
                {"start", SDL_CONTROLLER_BUTTON_START},
                {"guide", SDL_CONTROLLER_BUTTON_GUIDE},
                {"l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
                {"r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
                {"l3", SDL_CONTROLLER_BUTTON_LEFTSTICK},
                {"r3", SDL_CONTROLLER_BUTTON_RIGHTSTICK}};
            if (button == "l2")
                return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >
                       16000;
            if (button == "r2")
                return SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >
                       16000;
            for (const auto& [name, sdlButton] : buttons) {
                if (name == button)
                    return SDL_GameControllerGetButton(controller, sdlButton) != 0;
            }
            return false;
        }

#if defined(__linux__) && !defined(__ANDROID__)
        bool testBit(const unsigned char* bits, int bit)
        {
            return (bits[bit / 8] >> (bit % 8)) & 1;
        }

        // The pads among the event devices: the ones with the buttons of a gamepad.
        std::vector<EventPad> openEventPads()
        {
            std::vector<EventPad> pads;
            DIR* folder {opendir("/dev/input")};
            if (folder == nullptr)
                return pads;
            while (dirent* entry {readdir(folder)}) {
                const std::string name {entry->d_name};
                if (name.rfind("event", 0) != 0)
                    continue;
                const std::string path {"/dev/input/" + name};
                const int fd {open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
                if (fd < 0)
                    continue;
                unsigned char keys[KEY_MAX / 8 + 1] {};
                if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 ||
                    !testBit(keys, BTN_SOUTH) || !testBit(keys, BTN_SELECT)) {
                    close(fd);
                    continue;
                }
                EventPad pad;
                pad.fd = fd;
                pad.triggerButtons = testBit(keys, BTN_TL2) && testBit(keys, BTN_TR2);
                pad.hasAxes = ioctl(fd, EVIOCGABS(ABS_Z), &pad.leftTrigger) == 0 &&
                              ioctl(fd, EVIOCGABS(ABS_RZ), &pad.rightTrigger) == 0;
                pads.emplace_back(pad);
                LOG(LogDebug) << "GameHotkeys: Reading the pad " << path << " directly";
            }
            closedir(folder);
            return pads;
        }

        bool isPressedEvent(const EventPad& pad, const std::string& button)
        {
            static const std::vector<std::pair<std::string, int>> codes {
                {"south", BTN_SOUTH}, {"east", BTN_EAST},     {"west", BTN_WEST},
                {"north", BTN_NORTH}, {"back", BTN_SELECT},   {"start", BTN_START},
                {"guide", BTN_MODE},  {"l1", BTN_TL},         {"r1", BTN_TR},
                {"l3", BTN_THUMBL},   {"r3", BTN_THUMBR}};
            unsigned char state[KEY_MAX / 8 + 1] {};
            if (ioctl(pad.fd, EVIOCGKEY(sizeof(state)), state) < 0)
                return false;
            if (button == "l2" || button == "r2") {
                if (pad.triggerButtons)
                    return testBit(state, button == "l2" ? BTN_TL2 : BTN_TR2);
                if (!pad.hasAxes)
                    return false;
                input_absinfo info {};
                if (ioctl(pad.fd, EVIOCGABS(button == "l2" ? ABS_Z : ABS_RZ), &info) < 0)
                    return false;
                return info.value > info.minimum + (info.maximum - info.minimum) / 2;
            }
            for (const auto& [name, code] : codes) {
                if (name == button)
                    return testBit(state, code);
            }
            return false;
        }

        // The processes of the game: the ones that hold the pipe its output is read from, and
        // all processes started by them (some close the pipe, as the runtime of an AppImage or
        // a game started by a launcher may do). EmuSPLASH starts the emulator in the background of
        // a shell that ends at once, so the emulator isn't a child of EmuSPLASH, but it keeps the
        // pipe.
        std::vector<pid_t> gameProcesses(int pipe)
        {
            std::vector<pid_t> found;
            struct stat info;
            if (fstat(pipe, &info) != 0)
                return found;
            const std::string wanted {"pipe:[" + std::to_string(info.st_ino) + "]"};
            const pid_t self {getpid()};
            std::map<pid_t, std::vector<pid_t>> children;
            DIR* proc {opendir("/proc")};
            if (proc == nullptr)
                return found;
            while (dirent* entry {readdir(proc)}) {
                const std::string name {entry->d_name};
                if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
                    continue;
                const pid_t pid {static_cast<pid_t>(std::stol(name))};
                if (pid == self)
                    continue;
                // The parent is the second field after the name, which is in parentheses
                // and can contain spaces.
                if (FILE* statFile {fopen(("/proc/" + name + "/stat").c_str(), "r")}) {
                    char line[512] {};
                    const size_t length {fread(line, 1, sizeof(line) - 1, statFile)};
                    fclose(statFile);
                    const std::string text(line, length);
                    const size_t close {text.rfind(')')};
                    if (close != std::string::npos && close + 4 < text.size()) {
                        const pid_t parent {
                            static_cast<pid_t>(std::atol(text.c_str() + close + 4))};
                        children[parent].emplace_back(pid);
                    }
                }
                const std::string fdFolder {"/proc/" + name + "/fd"};
                DIR* fds {opendir(fdFolder.c_str())};
                if (fds == nullptr)
                    continue;
                while (dirent* fd {readdir(fds)}) {
                    char target[64] {};
                    const std::string link {fdFolder + "/" + fd->d_name};
                    const ssize_t length {readlink(link.c_str(), target, sizeof(target) - 1)};
                    if (length > 0 && wanted == std::string(target, static_cast<size_t>(length))) {
                        found.emplace_back(pid);
                        break;
                    }
                }
                closedir(fds);
            }
            closedir(proc);
            for (size_t index {0}; index < found.size(); ++index) {
                const auto descendants = children.find(found[index]);
                if (descendants == children.cend())
                    continue;
                for (const pid_t child : descendants->second) {
                    if (child != self &&
                        std::find(found.cbegin(), found.cend(), child) == found.cend())
                        found.emplace_back(child);
                }
            }
            return found;
        }

        void signalGame(int pipe, int signal)
        {
            const std::vector<pid_t> processes {gameProcesses(pipe)};
            if (processes.empty()) {
                LOG(LogWarning) << "GameHotkeys: No process of the game found";
            }
            for (const pid_t pid : processes) {
                if (signal == SIGKILL) {
                    LOG(LogWarning) << "GameHotkeys: Killing process " << pid;
                }
                else {
                    LOG(LogInfo) << "GameHotkeys: Ending process " << pid;
                }
                kill(pid, signal);
            }
        }

        // Keys sent to the focused window with the XTest extension. The libraries are opened
        // when first needed, so EmuSPLASH doesn't depend on them.
        class KeySender
        {
        public:
            static KeySender& get()
            {
                static KeySender sender;
                return sender;
            }

            // "Ctrl+S", "Shift+F10", "F12" as Qt writes key sequences.
            bool send(const std::string& sequence)
            {
                if (!open())
                    return false;
                std::vector<unsigned int> codes;
                for (std::string part : Utils::String::delimitedStringToVector(sequence, "+")) {
                    part = Utils::String::trim(part);
                    if (part.empty())
                        continue;
                    const unsigned long keysym {mStringToKeysym(keysymName(part).c_str())};
                    const unsigned int code {mKeysymToKeycode(mDisplay, keysym)};
                    if (keysym == 0 || code == 0) {
                        LOG(LogWarning) << "GameHotkeys: Unknown key \"" << part << "\" in \""
                                        << sequence << "\"";
                        return false;
                    }
                    codes.emplace_back(code);
                }
                for (const unsigned int code : codes)
                    mFakeKeyEvent(mDisplay, code, 1, 0);
                mFlush(mDisplay);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                for (auto code {codes.rbegin()}; code != codes.rend(); ++code)
                    mFakeKeyEvent(mDisplay, *code, 0, 0);
                mFlush(mDisplay);
                return true;
            }

        private:
            using OpenDisplay = void* (*)(const char*);
            using StringToKeysym = unsigned long (*)(const char*);
            using KeysymToKeycode = unsigned char (*)(void*, unsigned long);
            using Flush = int (*)(void*);
            using FakeKeyEvent = int (*)(void*, unsigned int, int, unsigned long);

            static std::string keysymName(const std::string& qtName)
            {
                static const std::vector<std::pair<std::string, std::string>> names {
                    {"ctrl", "Control_L"},  {"shift", "Shift_L"},  {"alt", "Alt_L"},
                    {"meta", "Super_L"},    {"return", "Return"},  {"enter", "Return"},
                    {"esc", "Escape"},      {"escape", "Escape"},  {"tab", "Tab"},
                    {"space", "space"},     {"backspace", "BackSpace"},
                    {"del", "Delete"},      {"delete", "Delete"},  {"ins", "Insert"},
                    {"insert", "Insert"},   {"home", "Home"},      {"end", "End"},
                    {"pgup", "Prior"},      {"pgdown", "Next"},    {"up", "Up"},
                    {"down", "Down"},       {"left", "Left"},      {"right", "Right"},
                    {"pause", "Pause"},     {"print", "Print"}};
                const std::string lower {Utils::String::toLower(qtName)};
                for (const auto& [qt, x] : names) {
                    if (lower == qt)
                        return x;
                }
                if (qtName.size() == 1)
                    return lower;
                return qtName;
            }

            bool open()
            {
                if (mDisplay != nullptr)
                    return true;
                if (mTried)
                    return false;
                mTried = true;
                if (std::getenv("DISPLAY") == nullptr) {
                    LOG(LogWarning) << "GameHotkeys: No X11 display, keys can't be sent";
                    return false;
                }
                void* x11 {dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL)};
                void* xtst {dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL)};
                if (x11 == nullptr || xtst == nullptr) {
                    LOG(LogWarning) << "GameHotkeys: libX11 or libXtst (package libxtst6) "
                                       "not found, keys can't be sent";
                    return false;
                }
                auto openDisplay = reinterpret_cast<OpenDisplay>(dlsym(x11, "XOpenDisplay"));
                mStringToKeysym = reinterpret_cast<StringToKeysym>(dlsym(x11, "XStringToKeysym"));
                mKeysymToKeycode =
                    reinterpret_cast<KeysymToKeycode>(dlsym(x11, "XKeysymToKeycode"));
                mFlush = reinterpret_cast<Flush>(dlsym(x11, "XFlush"));
                mFakeKeyEvent = reinterpret_cast<FakeKeyEvent>(dlsym(xtst, "XTestFakeKeyEvent"));
                if (openDisplay == nullptr || mStringToKeysym == nullptr ||
                    mKeysymToKeycode == nullptr || mFlush == nullptr || mFakeKeyEvent == nullptr)
                    return false;
                mDisplay = openDisplay(nullptr);
                if (mDisplay == nullptr) {
                    LOG(LogWarning) << "GameHotkeys: Couldn't open the X11 display";
                }
                return mDisplay != nullptr;
            }

            bool mTried {false};
            void* mDisplay {nullptr};
            StringToKeysym mStringToKeysym {nullptr};
            KeysymToKeycode mKeysymToKeycode {nullptr};
            Flush mFlush {nullptr};
            FakeKeyEvent mFakeKeyEvent {nullptr};
        };
#endif

        bool comboDown(const std::vector<std::string>& buttons)
        {
            for (SDL_GameController* controller : sState.controllers) {
                bool all {true};
                for (const std::string& button : buttons)
                    all = all && isPressedSdl(controller, button);
                if (all)
                    return true;
            }
#if defined(__linux__) && !defined(__ANDROID__)
            for (const EventPad& pad : sState.eventPads) {
                bool all {true};
                for (const std::string& button : buttons)
                    all = all && isPressedEvent(pad, button);
                if (all)
                    return true;
            }
#endif
            return false;
        }

        // True once when the combination has been held for a moment (a few polls, so a quick
        // touch of both buttons doesn't count), again only after it has been let go.
        bool held(Combo& combo, Clock::time_point now)
        {
            if (combo.buttons.empty())
                return false;
            if (!comboDown(combo.buttons)) {
                combo.pressing = false;
                combo.fired = false;
                return false;
            }
            if (!combo.pressing) {
                combo.pressing = true;
                combo.pressedSince = now;
                return false;
            }
            if (combo.fired || now - combo.pressedSince < std::chrono::milliseconds(50))
                return false;
            combo.fired = true;
            return true;
        }

        void watch(int pipe)
        {
            const Clock::time_point now {Clock::now()};
            if (held(sState.close, now)) {
                if (!sState.triggered) {
                    sState.triggered = true;
                    sState.triggeredAt = now;
                    LOG(LogInfo) << "GameHotkeys: Close game pressed"
                                 << (sState.waitMs > 0 ? ", the emulator may close itself first" :
                                                         "");
                }
#if defined(__linux__) && !defined(__ANDROID__)
                else if (!sState.terminated) {
                    // Pressed again: the game is ended at once.
                    LOG(LogInfo) << "GameHotkeys: Close game pressed again";
                    sState.terminated = true;
                    sState.terminatedAt = now;
                    signalGame(pipe, SIGTERM);
                }
                else if (!sState.killed) {
                    LOG(LogInfo) << "GameHotkeys: Close game pressed again";
                    sState.killed = true;
                    signalGame(pipe, SIGKILL);
                }
#endif
            }
            if (!sState.triggered) {
                for (KeyWatch& key : sState.keys) {
                    if (!held(key.combo, now))
                        continue;
#if defined(__linux__) && !defined(__ANDROID__)
                    LOG(LogInfo) << "GameHotkeys: " << key.action.name << " pressed, sending \""
                                 << key.action.keys << "\" to the emulator";
                    KeySender::get().send(key.action.keys);
#endif
                }
                return;
            }
#if defined(__linux__) && !defined(__ANDROID__)
            if (!sState.terminated && now - sState.triggeredAt >=
                                          std::chrono::milliseconds(sState.waitMs)) {
                sState.terminated = true;
                sState.terminatedAt = now;
                signalGame(pipe, SIGTERM);
            }
            else if (sState.terminated && !sState.killed &&
                     now - sState.terminatedAt >= std::chrono::milliseconds(sState.killWaitMs)) {
                sState.killed = true;
                LOG(LogWarning) << "GameHotkeys: The game didn't end";
                signalGame(pipe, SIGKILL);
            }
#else
            (void)pipe;
#endif
        }
    } // namespace

    const std::vector<Button>& getButtons()
    {
        static const std::vector<Button> buttons {
            {"south", "A / CROSS"},     {"east", "B / CIRCLE"},     {"west", "X / SQUARE"},
            {"north", "Y / TRIANGLE"},  {"l1", "L1"},               {"r1", "R1"},
            {"l2", "L2"},               {"r2", "R2"},               {"l3", "L3"},
            {"r3", "R3"},               {"start", "START"},         {"guide", "GUIDE / PS"}};
        return buttons;
    }

    std::vector<std::pair<std::string, std::string>> getCombinations(bool closeGame)
    {
        std::vector<std::pair<std::string, std::string>> combinations {{"off", "OFF"}};
        for (const Button& button : getButtons())
            combinations.emplace_back("back+" + button.id, "SELECT + " + button.label);
        // Closing a game can also start with START, the other hotkeys not: RetroArch has one
        // enable button for all of them.
        if (closeGame) {
            for (const Button& button : getButtons()) {
                if (button.id != "start")
                    combinations.emplace_back("start+" + button.id, "START + " + button.label);
            }
        }
        combinations.emplace_back("l3+r3", "L3 + R3");
        combinations.emplace_back("l1+r1", "L1 + R1");
        combinations.emplace_back("l2+r2", "L2 + R2");
        combinations.emplace_back("start+back", "START + SELECT");
        return combinations;
    }

    std::vector<std::string> getCombination(const std::string& setting)
    {
        const std::string value {Settings::getInstance()->getString(setting)};
        if (value.empty() || value == "off")
            return {};
        return Utils::String::delimitedStringToVector(value, "+");
    }

    void begin(const Launch& launch)
    {
        end();
        sState = State {};
        sState.close.buttons = getCombination("HotkeyCloseGame");
        sState.waitMs = launch.closeWaitMs;
        sState.killWaitMs = launch.killWaitMs;
        for (const KeyAction& action : launch.keyActions) {
            if (action.buttons.empty() || action.keys.empty())
                continue;
            KeyWatch key;
            key.action = action;
            key.combo.buttons = action.buttons;
            sState.keys.emplace_back(key);
        }
        if (sState.close.buttons.empty() && sState.keys.empty())
            return;
        sState.active = true;
        // The pads are read while the window of the emulator has the focus.
        const char* hint {SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)};
        sState.hadHint = hint != nullptr;
        sState.previousHint = hint != nullptr ? hint : "";
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        for (int index {0}; index < SDL_NumJoysticks(); ++index) {
            if (!SDL_IsGameController(index))
                continue;
            if (SDL_GameController* controller {SDL_GameControllerOpen(index)})
                sState.controllers.emplace_back(controller);
        }
#if defined(__linux__) && !defined(__ANDROID__)
        // The event devices are read directly too: SDL may not get the buttons while the
        // window of the emulator has the focus, the state of an event device is always there.
        sState.eventPads = openEventPads();
        LOG(LogInfo) << "GameHotkeys: Watching " << sState.controllers.size()
                     << " SDL pads and " << sState.eventPads.size() << " event devices"
                     << (sState.close.buttons.empty() ?
                             "" :
                             ", close game: " +
                                 Settings::getInstance()->getString("HotkeyCloseGame"));
#endif
        Utils::Platform::setLaunchWatcher(watch);
    }

    void end()
    {
        Utils::Platform::setLaunchWatcher(nullptr);
        for (SDL_GameController* controller : sState.controllers)
            SDL_GameControllerClose(controller);
#if defined(__linux__) && !defined(__ANDROID__)
        for (const EventPad& pad : sState.eventPads)
            close(pad.fd);
#endif
        if (sState.active) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                        sState.hadHint ? sState.previousHint.c_str() : "0");
        }
        sState = State {};
    }
} // namespace GameHotkeys
