//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonHost.cpp
//
//  Runs the streaming add-on: its program is started in the background and EmuSPLASH takes the
//  role of the media center it was written for. The program asks for dialogs, settings and
//  the player using JSON-RPC calls, which are answered by the servers run here, and the
//  listings are requested from its HTTP server. Script add-ons (the search providers) are
//  run using Python together with compatibility modules that call back to the same server.
//
//  The method names of the calls are given by the protocol of the add-ons and can't be
//  changed. Calls that need the user interface are queued for the main thread.
//

#include "media/AddonHost.h"

#include "Log.h"
#include "Settings.h"
#include "media/AddonStore.h"
#include "media/Dependencies.h"
#include "media/HttpClient.h"
#include "media/SubtitleDownloader.h"
#include "resources/ResourceManager.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <regex>

namespace
{
    // The ports are fixed by the program: its HTTP server, the server for the calls made to
    // the add-on and the server for the calls made to the media center.
    constexpr int DAEMON_PORT {65220};
    constexpr int ADDON_PORT {65221};
    constexpr int HOST_PORT {9090};
    const std::string DAEMON_URL {"http://127.0.0.1:65220"};
    const std::string PLUGIN_PREFIX {"plugin://"};
    const std::string STREAMING_PLUGIN {std::string("plugin://") + AddonStore::STREAMING_ADDON};
    // Sent with the requests so the program treats them as coming from its own add-on.
    const std::string ADDON_USER_AGENT {AddonStore::STREAMING_ADDON};
    const std::string BUILD_VERSION {"21.2 (21.2.0) Git:20250101-emusplash"};

    // Settings of the streaming add-on that are given fixed values, as they control
    // functions that don't exist here or would break the connection to the program.
    const std::map<std::string, std::string>& forcedSettings()
    {
        static const std::map<std::string, std::string> settings {
            {"library_enabled", "false"},
            {"library_sync_enabled", "false"},
            {"library_sync_playback_enabled", "false"},
            {"skip_repository_search", "true"},
            {"binary_platform", "Auto"},
            {"local_only_client", "false"},
            {"local_force_library", "false"},
            {"local_port", std::to_string(ADDON_PORT)},
            {"local_host", ""},
            {"remote_host", "127.0.0.1"},
            {"remote_port", std::to_string(DAEMON_PORT)},
            {"remote_login", ""},
            {"remote_password", ""},
            {"auto_kodi_buffer_size", "false"},
            {"viewmode_global_enabled", "false"}};
        return settings;
    }

    std::string jsonQuote(const std::string& text)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
        writer.String(text.c_str(), static_cast<rapidjson::SizeType>(text.size()));
        return buffer.GetString();
    }

    std::string jsonValue(const rapidjson::Value& value)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
        value.Accept(writer);
        return buffer.GetString();
    }

    std::string jsonObject(const std::map<std::string, std::string>& values)
    {
        std::string json {"{"};
        for (const auto& [key, value] : values) {
            if (json.size() > 1)
                json.push_back(',');
            json.append(jsonQuote(key) + ":" + jsonQuote(value));
        }
        return json + "}";
    }

    // Converts any JSON value to text, lists are joined using " / ".
    std::string valueText(const rapidjson::Value& value)
    {
        if (value.IsString())
            return value.GetString();
        if (value.IsBool())
            return value.GetBool() ? "true" : "false";
        if (value.IsInt64())
            return std::to_string(value.GetInt64());
        if (value.IsUint64())
            return std::to_string(value.GetUint64());
        if (value.IsNumber()) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%g", value.GetDouble());
            return buffer;
        }
        if (value.IsArray()) {
            std::string text;
            for (const auto& item : value.GetArray()) {
                const std::string part {valueText(item)};
                if (part.empty())
                    continue;
                if (!text.empty())
                    text.append(" / ");
                text.append(part);
            }
            return text;
        }
        return "";
    }

    // Access to the parameters of a call, given either by position or by name.
    class Params
    {
    public:
        explicit Params(const std::string& json)
        {
            mDoc.Parse(json.c_str());
            if (mDoc.HasParseError())
                mDoc.SetNull();
        }
        const rapidjson::Value* get(size_t index, const char* name = nullptr) const
        {
            if (mDoc.IsArray()) {
                if (index < mDoc.Size())
                    return &mDoc[static_cast<rapidjson::SizeType>(index)];
                return nullptr;
            }
            if (mDoc.IsObject() && name != nullptr && mDoc.HasMember(name))
                return &mDoc[name];
            return nullptr;
        }
        std::string string(size_t index, const char* name = nullptr) const
        {
            const rapidjson::Value* value {get(index, name)};
            return value == nullptr ? "" : valueText(*value);
        }
        int integer(size_t index, int fallback = 0, const char* name = nullptr) const
        {
            const rapidjson::Value* value {get(index, name)};
            if (value == nullptr)
                return fallback;
            if (value->IsNumber())
                return static_cast<int>(value->GetDouble());
            if (value->IsBool())
                return value->GetBool() ? 1 : 0;
            if (value->IsString() && value->GetStringLength() > 0)
                return std::atoi(value->GetString());
            return fallback;
        }
        double number(size_t index, double fallback = 0.0) const
        {
            const rapidjson::Value* value {get(index)};
            if (value == nullptr)
                return fallback;
            if (value->IsNumber())
                return value->GetDouble();
            if (value->IsString())
                return std::atof(value->GetString());
            return fallback;
        }
        bool boolean(size_t index, bool fallback = false, const char* name = nullptr) const
        {
            const rapidjson::Value* value {get(index, name)};
            if (value == nullptr)
                return fallback;
            if (value->IsBool())
                return value->GetBool();
            if (value->IsNumber())
                return value->GetDouble() != 0.0;
            if (value->IsString())
                return std::string(value->GetString()) == "true";
            return fallback;
        }
        std::vector<std::string> strings(size_t index, const char* name = nullptr) const
        {
            std::vector<std::string> result;
            const rapidjson::Value* value {get(index, name)};
            if (value != nullptr && value->IsArray()) {
                for (const auto& item : value->GetArray())
                    result.emplace_back(valueText(item));
            }
            else if (value != nullptr && value->IsString()) {
                result.emplace_back(value->GetString());
            }
            return result;
        }
        std::vector<int> integers(size_t index) const
        {
            std::vector<int> result;
            const rapidjson::Value* value {get(index)};
            if (value != nullptr && value->IsArray()) {
                for (const auto& item : value->GetArray()) {
                    if (item.IsNumber())
                        result.emplace_back(static_cast<int>(item.GetDouble()));
                }
            }
            return result;
        }

    private:
        rapidjson::Document mDoc;
    };

    // Formats a translated text containing "%s" and "%d" using the given arguments.
    std::string formatText(const std::string& format, const std::vector<std::string>& arguments)
    {
        std::string result;
        size_t argument {0};
        for (size_t i {0}; i < format.size(); ++i) {
            if (format[i] != '%' || i + 1 >= format.size()) {
                result.push_back(format[i]);
                continue;
            }
            if (format[i + 1] == '%') {
                result.push_back('%');
                ++i;
                continue;
            }
            size_t end {i + 1};
            while (end < format.size() && std::strchr("-+ #0123456789.", format[end]) != nullptr)
                ++end;
            if (end < format.size() && std::strchr("sdifgxXeErc", format[end]) != nullptr) {
                if (argument < arguments.size())
                    result.append(arguments[argument]);
                ++argument;
                i = end;
                continue;
            }
            result.push_back(format[i]);
        }
        return result;
    }

    std::string joinLines(const std::vector<std::string>& lines)
    {
        std::string text;
        for (const auto& line : lines)
            text.append((text.empty() ? "" : "\n") + line);
        return text;
    }

    std::string languageName(const std::string& code)
    {
        const std::string name {SubtitleDownloader::getLanguageName(code)};
        return name.empty() ? "English" : name;
    }

    std::string pythonDirectory()
    {
        const std::string host {
            ResourceManager::getInstance().getResourcePath(":/media/python/_host.py", false)};
        return host.empty() ? "" : Utils::FileSystem::getParent(host);
    }

    std::string specialPath(const std::string& path)
    {
        const std::string prefix {"special://"};
        if (!Utils::String::startsWith(path, prefix))
            return path;
        const std::string home {AddonStore::getHomeDirectory()};
        const std::string rest {path.substr(prefix.size())};
        const size_t slash {rest.find('/')};
        const std::string name {slash == std::string::npos ? rest : rest.substr(0, slash)};
        const std::string tail {slash == std::string::npos ? "" : rest.substr(slash + 1)};
        std::string base;
        if (name == "profile" || name == "masterprofile" || name == "userdata") {
            if (Utils::String::startsWith(tail, "addon_data/"))
                return AddonStore::getDataDirectory() + "/" + tail.substr(11);
            if (tail == "addon_data")
                return AddonStore::getDataDirectory();
            base = home + "/userdata";
        }
        else if (name == "temp") {
            base = AddonStore::getTempDirectory();
        }
        else if (name == "logpath") {
            base = AddonStore::getLogDirectory();
        }
        else {
            base = home;
        }
        return tail.empty() ? base + "/" : base + "/" + tail;
    }

    bool sendAll(int socket, const std::string& data)
    {
        size_t sent {0};
        while (sent < data.size()) {
            const ssize_t result {
                send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL)};
            if (result <= 0) {
                if (result < 0 && errno == EINTR)
                    continue;
                return false;
            }
            sent += static_cast<size_t>(result);
        }
        return true;
    }

    int createServer(int port, std::string& error)
    {
        const int server {socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        if (server < 0) {
            error = std::strerror(errno);
            return -1;
        }
        int enable {1};
        setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(server, 32) != 0) {
            error = "Port " + std::to_string(port) + " is already in use by another program";
            close(server);
            return -1;
        }
        return server;
    }

    // Starts a program with its output written to a log file. Returns the process ID.
    int spawnProcess(const std::vector<std::string>& arguments,
                     const std::map<std::string, std::string>& environment,
                     const std::string& directory,
                     const std::string& logPath)
    {
        std::vector<char*> argv;
        for (const auto& argument : arguments)
            argv.emplace_back(const_cast<char*>(argument.c_str()));
        argv.emplace_back(nullptr);

        std::map<std::string, std::string> variables;
        for (char** variable {environ}; *variable != nullptr; ++variable) {
            const std::string entry {*variable};
            const size_t equals {entry.find('=')};
            if (equals != std::string::npos)
                variables[entry.substr(0, equals)] = entry.substr(equals + 1);
        }
        for (const auto& [key, value] : environment)
            variables[key] = value;
        std::vector<std::string> entries;
        for (const auto& [key, value] : variables)
            entries.emplace_back(key + "=" + value);
        std::vector<char*> envp;
        for (const auto& entry : entries)
            envp.emplace_back(const_cast<char*>(entry.c_str()));
        envp.emplace_back(nullptr);

        const int logFile {open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
        const pid_t pid {fork()};
        if (pid == 0) {
            // Only async-signal-safe calls until the program has been started.
            setpgid(0, 0);
            if (logFile >= 0) {
                dup2(logFile, STDOUT_FILENO);
                dup2(logFile, STDERR_FILENO);
            }
            const int null {open("/dev/null", O_RDONLY)};
            if (null >= 0)
                dup2(null, STDIN_FILENO);
            if (!directory.empty() && chdir(directory.c_str()) != 0)
                _exit(127);
            execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }
        if (logFile >= 0)
            close(logFile);
        return pid > 0 ? static_cast<int>(pid) : -1;
    }

    // Log files are started over when they get large.
    void trimLog(const std::string& path)
    {
        struct stat info {};
        if (stat(path.c_str(), &info) == 0 && info.st_size > 4 * 1024 * 1024)
            truncate(path.c_str(), 0);
    }

    // True while the process is running, reaps it when it has ended.
    // Background progress dialogs that haven't changed this long are removed.
    constexpr long long STALE_PROGRESS_TIME {180000};

    long long nowMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool processRunning(int pid, int* exitCode = nullptr)
    {
        if (pid <= 0)
            return false;
        int status {0};
        const pid_t result {waitpid(pid, &status, WNOHANG)};
        if (result == 0)
            return true;
        if (result == pid) {
            if (exitCode != nullptr)
                *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return false;
        }
        // The child has already been reaped elsewhere.
        if (errno == ECHILD)
            return kill(pid, 0) == 0;
        return false;
    }
} // namespace

//--------------------------------------------------------------------------------------------------
// Requests and UI requests.
//--------------------------------------------------------------------------------------------------

bool AddonHost::Request::isReady() const
{
    return mFuture.valid() &&
           mFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void AddonHost::Request::cancel() { *mCancel = true; }

AddonHost::Response AddonHost::Request::take()
{
    if (!mFuture.valid())
        return Response();
    return mFuture.get();
}

void AddonHost::UiRequest::answer(const std::string& json)
{
    bool expected {false};
    if (!mAnswered.compare_exchange_strong(expected, true))
        return;
    mPromise.set_value(json);
}

//--------------------------------------------------------------------------------------------------
// Life cycle.
//--------------------------------------------------------------------------------------------------

AddonHost& AddonHost::getInstance()
{
    static AddonHost instance;
    return instance;
}

AddonHost::AddonHost()
    : mState {State::STOPPED}
    , mStopping {false}
    , mAddonServer {-1}
    , mHostServer {-1}
    , mConnectionCount {0}
    , mStartPending {false}
    , mDaemonPid {-1}
    , mRestartRequested {false}
    , mTaskStop {false}
    , mConfirmGeneration {0}
    , mCloseGeneration {0}
    , mNextHandle {1}
    , mSettingsOpen {false}
{
    mTaskThread = std::thread(&AddonHost::taskLoop, this);
}

AddonHost::~AddonHost()
{
    stop();
    {
        std::unique_lock<std::mutex> lock {mTaskMutex};
        mTaskStop = true;
    }
    mTaskCondition.notify_all();
    if (mTaskThread.joinable())
        mTaskThread.join();
}

std::string AddonHost::getStateMessage() const
{
    std::unique_lock<std::mutex> lock {mStateMutex};
    return mStateMessage;
}

void AddonHost::setState(State state, const std::string& message)
{
    {
        std::unique_lock<std::mutex> lock {mStateMutex};
        mStateMessage = message;
    }
    mState = state;
}

void AddonHost::start()
{
    std::unique_lock<std::mutex> lock {mThreadMutex};
    if (!mStopping && (mState == State::STARTING || mState == State::RUNNING))
        return;
    // A stop that is still in progress is waited for by the new thread, not here.
    auto stopThread = std::make_shared<std::thread>(std::move(mStopThread));
    auto previous = std::make_shared<std::thread>(std::move(mDaemonThread));
    mStartPending = true;
    setState(State::STARTING, "Starting Elementum");
    mDaemonThread = std::thread([this, stopThread, previous] {
        if (stopThread->joinable())
            stopThread->join();
        if (previous->joinable())
            previous->join();
        mStopping = false;
        mStartPending = false;
        daemonLoop();
    });
}

void AddonHost::requestStop()
{
    std::unique_lock<std::mutex> lock {mThreadMutex};
    mStopping = true;
    auto daemon = std::make_shared<std::thread>(std::move(mDaemonThread));
    auto previous = std::make_shared<std::thread>(std::move(mStopThread));
    mStopThread = std::thread([this, daemon, previous] {
        if (previous->joinable())
            previous->join();
        if (daemon->joinable())
            daemon->join();
        shutdown();
    });
}

void AddonHost::stop()
{
    requestStop();
    std::thread stopThread;
    {
        std::unique_lock<std::mutex> lock {mThreadMutex};
        stopThread = std::move(mStopThread);
    }
    if (stopThread.joinable())
        stopThread.join();
}

void AddonHost::shutdown()
{
    stopDaemon();
    {
        std::unique_lock<std::mutex> lock {mScriptMutex};
        for (const int pid : mScriptPids) {
            kill(-pid, SIGTERM);
            processRunning(pid);
        }
        mScriptPids.clear();
    }
    stopServers();

    // Calls still waiting for an answer are cancelled.
    {
        std::unique_lock<std::mutex> lock {mUiMutex};
        for (const auto& request : mPendingRequests)
            request->answer("null");
        mPendingRequests.clear();
        mUiRequests.clear();
        mProgressDialogs.clear();
        mOverlays.clear();
    }
    {
        std::unique_lock<std::mutex> lock {mPlayerMutex};
        mPlayerState = PlayerState();
        mPlayerCommands.clear();
    }
    if (!mStartPending && mState != State::FAILED)
        setState(State::STOPPED, "");
}

// Faster reading of the video, e.g. by the dub. The values are set once and only where the
// setting still has the value that comes with the add-on, so changes by the user are kept.
void AddonHost::applyStreamingDefaults()
{
    Settings* appSettings {Settings::getInstance()};
    if (appSettings->getBool("MediaStreamingDefaultsApplied"))
        return;
    std::shared_ptr<AddonSettings> settings {
        AddonStore::getInstance().getSettings(AddonStore::STREAMING_ADDON)};
    if (!settings)
        return;
    // Setting, the value that comes with the add-on and the new value.
    const char* defaults[][3] {{"download_storage", "1", "1"},
                               {"auto_memory_size", "true", "true"},
                               {"auto_memory_size_strategy", "1", "2"},
                               {"connections_limit", "", "200"}};
    for (const auto& entry : defaults) {
        const std::string current {settings->get(entry[0])};
        if (current.empty() || current == entry[1])
            settings->set(entry[0], entry[2]);
    }
    appSettings->setBool("MediaStreamingDefaultsApplied", true);
    LOG(LogInfo) << "AddonHost: Set the default values of the memory and connection settings";
}

void AddonHost::daemonLoop()
{
    std::string error;
    if (!AddonStore::getInstance().isInstalled(AddonStore::STREAMING_ADDON)) {
        setState(State::FAILED, "Elementum is not installed");
        return;
    }

    // The program is downloaded if it's missing or older than the add-on.
    const AddonStore::Result binary {AddonStore::getInstance().ensureDaemonBinary(
        [this](const std::string& message, int percent) {
            setState(State::STARTING,
                     percent >= 0 ? message + " " + std::to_string(percent) + "%" : message);
        },
        &mStopping)};
    if (!binary.success) {
        setState(State::FAILED, binary.error);
        return;
    }
    if (mStopping)
        return;

    applyStreamingDefaults();

    if (!startServers(error)) {
        setState(State::FAILED, error);
        return;
    }

    int restarts {0};
    while (!mStopping) {
        setState(State::STARTING, "Starting Elementum");
        if (!spawnDaemon(error)) {
            setState(State::FAILED, error);
            return;
        }
        if (!waitForDaemon()) {
            if (mStopping)
                return;
            int exitCode {0};
            if (!processRunning(mDaemonPid, &exitCode)) {
                setState(State::FAILED, "Elementum stopped while starting (code " +
                                            std::to_string(exitCode) + "), see " +
                                            AddonStore::getLogDirectory() + "/elementum.log");
            }
            else {
                setState(State::FAILED, "Elementum didn't start in time");
                stopDaemon();
            }
            return;
        }
        setState(State::RUNNING, "");
        LOG(LogInfo) << "AddonHost: Elementum is running";

        // Watch the program until it ends or is stopped.
        int exitCode {0};
        while (!mStopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            {
                std::unique_lock<std::mutex> lock {mScriptMutex};
                for (auto it = mScriptPids.begin(); it != mScriptPids.end();) {
                    if (!processRunning(*it))
                        it = mScriptPids.erase(it);
                    else
                        ++it;
                }
            }
            if (!processRunning(mDaemonPid, &exitCode))
                break;
        }
        if (mStopping)
            return;
        mDaemonPid = -1;
        // Exit code 5 is used by the program to ask for a restart, e.g. after changes to the
        // settings that it can't apply while running.
        if ((exitCode == 5 || mRestartRequested) && restarts < 10) {
            mRestartRequested = false;
            ++restarts;
            LOG(LogInfo) << "AddonHost: Restarting Elementum";
            continue;
        }
        setState(State::FAILED, "Elementum stopped (code " + std::to_string(exitCode) + ")");
        return;
    }
}

bool AddonHost::spawnDaemon(std::string& error)
{
    AddonStore::Addon addon;
    if (!AddonStore::getInstance().getAddon(AddonStore::STREAMING_ADDON, addon)) {
        error = "Elementum is not installed";
        return false;
    }
    const std::string binary {AddonStore::getDaemonBinaryPath()};
    const std::string profile {AddonStore::getAddonDataDirectory(AddonStore::STREAMING_ADDON)};
    const std::string temp {AddonStore::getTempDirectory() + "/elementum"};
    Utils::FileSystem::createDirectory(profile);
    Utils::FileSystem::createDirectory(temp);
    Utils::FileSystem::createDirectory(AddonStore::getLogDirectory());

    const std::vector<std::string> arguments {binary,
                                              "-remotePort=" + std::to_string(ADDON_PORT),
                                              "-localPort=" + std::to_string(DAEMON_PORT),
                                              "-addonPath=" + addon.path,
                                              "-profilePath=" + profile,
                                              "-tempPath=" + temp,
                                              "-logPath=" + AddonStore::getLogDirectory() +
                                                  "/elementum.log",
                                              "-disableLibrarySync"};
    const std::map<std::string, std::string> environment {
        {"LD_LIBRARY_PATH", Utils::FileSystem::getParent(binary)}};
    trimLog(AddonStore::getLogDirectory() + "/elementum.log");
    trimLog(AddonStore::getLogDirectory() + "/elementum-output.log");
    const int pid {spawnProcess(arguments, environment, Utils::FileSystem::getParent(binary),
                                AddonStore::getLogDirectory() + "/elementum-output.log")};
    if (pid <= 0) {
        error = "Couldn't start the Elementum program";
        return false;
    }
    mDaemonPid = pid;
    LOG(LogInfo) << "AddonHost: Started Elementum (process " << pid << ")";
    return true;
}

bool AddonHost::waitForDaemon()
{
    HttpClient::Options options;
    options.userAgent = ADDON_USER_AGENT;
    options.timeout = 5;
    options.connectTimeout = 2;
    options.cancel = &mStopping;
    const auto start {std::chrono::steady_clock::now()};
    // The first start can take a while, e.g. while the program sets up its database.
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(90)) {
        if (mStopping || !processRunning(mDaemonPid))
            return false;
        const HttpClient::Response response {HttpClient::request(DAEMON_URL + "/", options)};
        if (response.error.empty() && response.code == 200)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return false;
}

void AddonHost::stopDaemon()
{
    const int pid {mDaemonPid};
    if (pid <= 0)
        return;
    HttpClient::Options options;
    options.userAgent = ADDON_USER_AGENT;
    options.timeout = 2;
    options.connectTimeout = 1;
    HttpClient::request(DAEMON_URL + "/shutdown", options);
    for (int i {0}; i < 40 && processRunning(pid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (processRunning(pid)) {
        kill(pid, SIGTERM);
        for (int i {0}; i < 30 && processRunning(pid); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (processRunning(pid)) {
        kill(pid, SIGKILL);
        processRunning(pid);
    }
    mDaemonPid = -1;
    LOG(LogInfo) << "AddonHost: Stopped Elementum";
}

void AddonHost::restartDaemon()
{
    if (mState != State::RUNNING)
        return;
    mRestartRequested = true;
    // The program is stopped and started again by daemonLoop(). This is done on a thread of
    // its own, as the program may have stopped answering.
    std::thread([this] {
        {
            std::unique_lock<std::mutex> lock {mScriptMutex};
            for (const int pid : mScriptPids)
                kill(-pid, SIGTERM);
        }
        {
            // Questions and progress dialogs of the stopped searches are closed.
            std::unique_lock<std::mutex> lock {mUiMutex};
            for (const auto& request : mPendingRequests)
                request->answer("null");
            mUiRequests.clear();
            mProgressDialogs.clear();
            mOverlays.clear();
        }
        ++mCloseGeneration;
        ++mConfirmGeneration;
        const int pid {mDaemonPid};
        if (pid <= 0)
            return;
        kill(pid, SIGTERM);
        for (int i {0}; i < 30 && kill(pid, 0) == 0 && mDaemonPid == pid; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (mDaemonPid == pid && kill(pid, 0) == 0)
            kill(pid, SIGKILL);
    }).detach();
}

void AddonHost::reloadSettings()
{
    if (mState != State::RUNNING)
        return;
    queueTask([] {
        HttpClient::Options options;
        options.userAgent = ADDON_USER_AGENT;
        options.timeout = 10;
        HttpClient::request(DAEMON_URL + "/reload", options);
        HttpClient::request(DAEMON_URL + "/cmd/clear_page_cache", options);
    });
}

void AddonHost::queueTask(const std::function<void()>& task)
{
    {
        std::unique_lock<std::mutex> lock {mTaskMutex};
        mTasks.emplace_back(task);
    }
    mTaskCondition.notify_one();
}

void AddonHost::taskLoop()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock {mTaskMutex};
            mTaskCondition.wait(lock, [this] { return mTaskStop || !mTasks.empty(); });
            if (mTaskStop)
                return;
            task = mTasks.front();
            mTasks.pop_front();
        }
        task();
    }
}

//--------------------------------------------------------------------------------------------------
// Servers.
//--------------------------------------------------------------------------------------------------

bool AddonHost::startServers(std::string& error)
{
    if (mAddonServer >= 0 && mHostServer >= 0)
        return true;
    mAddonServer = createServer(ADDON_PORT, error);
    if (mAddonServer < 0)
        return false;
    mHostServer = createServer(HOST_PORT, error);
    if (mHostServer < 0) {
        close(mAddonServer);
        mAddonServer = -1;
        return false;
    }
    mAddonServerThread = std::thread(&AddonHost::acceptLoop, this, mAddonServer, false);
    mHostServerThread = std::thread(&AddonHost::acceptLoop, this, mHostServer, true);
    return true;
}

void AddonHost::stopServers()
{
    if (mAddonServerThread.joinable())
        mAddonServerThread.join();
    if (mHostServerThread.joinable())
        mHostServerThread.join();
    if (mAddonServer >= 0)
        close(mAddonServer);
    if (mHostServer >= 0)
        close(mHostServer);
    mAddonServer = -1;
    mHostServer = -1;

    // Open connections are closed and waited for.
    std::unique_lock<std::mutex> lock {mConnectionMutex};
    for (const int socket : mConnections)
        ::shutdown(socket, SHUT_RDWR);
    mConnectionCondition.wait_for(lock, std::chrono::seconds(3),
                                  [this] { return mConnectionCount == 0; });
}

void AddonHost::acceptLoop(int serverSocket, bool hostApi)
{
    while (!mStopping) {
        pollfd descriptor {serverSocket, POLLIN, 0};
        if (poll(&descriptor, 1, 200) <= 0)
            continue;
        const int connection {accept4(serverSocket, nullptr, nullptr, SOCK_CLOEXEC)};
        if (connection < 0)
            continue;
        {
            std::unique_lock<std::mutex> lock {mConnectionMutex};
            mConnections.insert(connection);
            ++mConnectionCount;
        }
        std::thread(&AddonHost::connectionLoop, this, connection, hostApi).detach();
    }
}

void AddonHost::connectionLoop(int socket, bool hostApi)
{
    std::string buffer;
    char chunk[16384];
    while (!mStopping) {
        pollfd descriptor {socket, POLLIN, 0};
        const int ready {poll(&descriptor, 1, 200)};
        if (ready == 0)
            continue;
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            break;
        const ssize_t received {recv(socket, chunk, sizeof(chunk), 0)};
        if (received <= 0)
            break;
        buffer.append(chunk, static_cast<size_t>(received));

        size_t newline {0};
        bool failed {false};
        while ((newline = buffer.find('\n')) != std::string::npos) {
            const std::string line {buffer.substr(0, newline)};
            buffer.erase(0, newline + 1);
            if (Utils::String::trim(line).empty())
                continue;

            rapidjson::Document request;
            request.Parse(line.c_str());
            if (request.HasParseError() || !request.IsObject() || !request.HasMember("method") ||
                !request["method"].IsString())
                continue;
            const std::string method {request["method"].GetString()};
            const std::string params {request.HasMember("params") ? jsonValue(request["params"]) :
                                                                    "[]"};
            const std::string id {request.HasMember("id") ? jsonValue(request["id"]) : "null"};

            std::string result;
            std::string error {"null"};
            try {
                result = handleCall(method, params, hostApi);
            }
            catch (const std::exception& exception) {
                result = "null";
                error = jsonQuote(exception.what());
            }
            if (result.empty()) {
                result = "null";
                error = jsonQuote("Method not found: " + method);
            }
            const std::string response {"{\"jsonrpc\":\"2.0\",\"id\":" + id +
                                        ",\"result\":" + result + ",\"error\":" + error + "}\n"};
            if (!sendAll(socket, response)) {
                failed = true;
                break;
            }
        }
        if (failed)
            break;
    }
    close(socket);
    std::unique_lock<std::mutex> lock {mConnectionMutex};
    mConnections.erase(socket);
    --mConnectionCount;
    mConnectionCondition.notify_all();
}

std::string AddonHost::handleCall(const std::string& method,
                                  const std::string& params,
                                  bool hostApi)
{
    // The compatibility modules of the scripts use the add-on server for both kinds of calls.
    if (hostApi || method.find('.') != std::string::npos)
        return handleHostCall(method, params);
    return handleAddonCall(method, params);
}

void AddonHost::queueUi(const std::shared_ptr<UiRequest>& request)
{
    request->confirmGeneration = mConfirmGeneration;
    request->closeGeneration = mCloseGeneration;
    std::unique_lock<std::mutex> lock {mUiMutex};
    if (mStopping) {
        request->answer("null");
        return;
    }
    mUiRequests.emplace_back(request);
    if (request->blocking)
        mPendingRequests.insert(request);
}

std::string AddonHost::waitForUi(const std::shared_ptr<UiRequest>& request)
{
    request->blocking = true;
    std::future<std::string> future {request->mPromise.get_future()};
    queueUi(request);
    while (future.wait_for(std::chrono::milliseconds(200)) != std::future_status::ready) {
        if (mStopping) {
            request->answer("null");
            break;
        }
    }
    const std::string result {future.get()};
    std::unique_lock<std::mutex> lock {mUiMutex};
    mPendingRequests.erase(request);
    return result;
}

std::shared_ptr<AddonHost::UiRequest> AddonHost::takeUiRequest(bool canNavigate)
{
    static const std::set<std::string> navigation {"Update", "Refresh", "Player_Open",
                                                   "AddonSettings", "Builtin"};
    std::unique_lock<std::mutex> lock {mUiMutex};
    for (auto it = mUiRequests.begin(); it != mUiRequests.end(); ++it) {
        if (!canNavigate && navigation.count((*it)->method) != 0)
            continue;
        std::shared_ptr<UiRequest> request {*it};
        mUiRequests.erase(it);
        return request;
    }
    return nullptr;
}

bool AddonHost::hasUiRequests() const
{
    std::unique_lock<std::mutex> lock {mUiMutex};
    return !mUiRequests.empty();
}

std::vector<AddonHost::ProgressDialog> AddonHost::getProgressDialogs()
{
    std::unique_lock<std::mutex> lock {mUiMutex};
    // Background dialogs of searches that ended without closing them are removed after a
    // while, as they would otherwise stay on the screen until the program is restarted.
    const long long now {nowMilliseconds()};
    for (auto it = mProgressDialogs.begin(); it != mProgressDialogs.end();) {
        if (it->second.background && now - it->second.updated > STALE_PROGRESS_TIME)
            it = mProgressDialogs.erase(it);
        else
            ++it;
    }
    std::vector<ProgressDialog> dialogs;
    for (const auto& [id, dialog] : mProgressDialogs)
        dialogs.emplace_back(dialog);
    return dialogs;
}

void AddonHost::cancelProgressDialog(const std::string& id)
{
    std::unique_lock<std::mutex> lock {mUiMutex};
    const auto it {mProgressDialogs.find(id)};
    if (it != mProgressDialogs.end())
        it->second.canceled = true;
}

AddonHost::Overlay AddonHost::getOverlay() const
{
    std::unique_lock<std::mutex> lock {mUiMutex};
    for (const auto& [id, overlay] : mOverlays) {
        if (overlay.visible)
            return overlay;
    }
    return Overlay();
}

std::vector<AddonHost::Notification> AddonHost::takeNotifications()
{
    std::unique_lock<std::mutex> lock {mUiMutex};
    std::vector<Notification> notifications;
    notifications.swap(mNotifications);
    return notifications;
}

void AddonHost::setCurrentPath(const std::string& path)
{
    std::unique_lock<std::mutex> lock {mUiMutex};
    mCurrentPath = path;
}

//--------------------------------------------------------------------------------------------------
// Player.
//--------------------------------------------------------------------------------------------------

void AddonHost::setPlayerState(const PlayerState& state)
{
    std::unique_lock<std::mutex> lock {mPlayerMutex};
    mPlayerState = state;
}

std::vector<AddonHost::PlayerCommand> AddonHost::takePlayerCommands()
{
    std::unique_lock<std::mutex> lock {mPlayerMutex};
    std::vector<PlayerCommand> commands;
    commands.swap(mPlayerCommands);
    return commands;
}

void AddonHost::sendPlayerEvent(const std::string& method, const std::string& data)
{
    if (mState != State::RUNNING)
        return;
    queueTask([method, data] {
        // The data is sent base64 encoded, as by the media center the program was written for.
        static const char* alphabet {
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
        std::string encoded;
        int value {0};
        int bits {-6};
        for (const unsigned char character : data) {
            value = (value << 8) + character;
            bits += 8;
            while (bits >= 0) {
                encoded.push_back(alphabet[(value >> bits) & 0x3F]);
                bits -= 6;
            }
        }
        if (bits > -6)
            encoded.push_back(alphabet[((value << 8) >> (bits + 8)) & 0x3F]);
        while (encoded.size() % 4 != 0)
            encoded.push_back('=');

        HttpClient::Options options;
        options.userAgent = ADDON_USER_AGENT;
        options.timeout = 5;
        HttpClient::request(DAEMON_URL + "/notification?sender=xbmc&method=" + method +
                                "&data=" + encoded,
                            options);
    });
}

//--------------------------------------------------------------------------------------------------
// Listings.
//--------------------------------------------------------------------------------------------------

bool AddonHost::isPluginUrl(const std::string& url)
{
    return Utils::String::startsWith(url, PLUGIN_PREFIX);
}

std::string AddonHost::getRootUrl() { return STREAMING_PLUGIN + "/"; }

std::shared_ptr<AddonHost::Request> AddonHost::open(const std::string& pluginUrl)
{
    auto request = std::make_shared<Request>();
    request->mUrl = pluginUrl;
    auto promise = std::make_shared<std::promise<Response>>();
    request->mFuture = promise->get_future();
    std::shared_ptr<std::atomic<bool>> cancel {request->mCancel};
    // A detached thread, so a request that is no longer needed never blocks the caller.
    std::thread([this, pluginUrl, promise, cancel] {
        promise->set_value(performRequest(pluginUrl, *cancel));
    }).detach();
    return request;
}

void AddonHost::runAction(const std::string& pluginUrl)
{
    queueTask([this, pluginUrl] {
        std::atomic<bool> cancel {false};
        const Response response {performRequest(pluginUrl, cancel)};
        if (response.type == Response::Type::PLAY || response.type == Response::Type::NAVIGATE) {
            auto request = std::make_shared<UiRequest>();
            request->method = response.type == Response::Type::PLAY ? "Player_Open" : "Update";
            request->text = response.url;
            queueUi(request);
        }
        else if (response.type == Response::Type::FAILED && !response.error.empty()) {
            std::unique_lock<std::mutex> lock {mUiMutex};
            mNotifications.emplace_back(Notification {"Elementum", response.error, ""});
        }
    });
}

AddonHost::Response AddonHost::performRequest(const std::string& pluginUrl,
                                              const std::atomic<bool>& cancel)
{
    // A playable address is often resolved in several steps: the links of a movie redirect
    // to the playback of the chosen torrent, which redirects to the file once it has been
    // buffered. Add-on addresses are resolved again, as the media center would do.
    std::string url {pluginUrl};
    Response response;
    for (int step {0}; step < 8; ++step) {
        response = performSingleRequest(url, cancel);
        if (response.type != Response::Type::PLAY || !isPluginUrl(response.url) || cancel)
            break;
        url = response.url;
    }
    if (response.type == Response::Type::PLAY && isPluginUrl(response.url)) {
        response.type = Response::Type::FAILED;
        response.error = "Couldn't resolve the stream";
    }
    return response;
}

AddonHost::Response AddonHost::performSingleRequest(const std::string& pluginUrl,
                                                    const std::atomic<bool>& cancel)
{
    Response response;
    if (!Utils::String::startsWith(pluginUrl, STREAMING_PLUGIN)) {
        response.type = Response::Type::FAILED;
        response.error = "Unsupported address: " + pluginUrl;
        return response;
    }
    const std::string url {DAEMON_URL + pluginUrl.substr(STREAMING_PLUGIN.size())};

    // Playback requests wait for the buffering to finish, which can take long.
    int bufferTimeout {std::atoi(settingValue("buffer_timeout").c_str())};
    if (bufferTimeout < 60)
        bufferTimeout = 60;
    HttpClient::Options options;
    options.userAgent = ADDON_USER_AGENT;
    options.followRedirects = false;
    options.timeout = bufferTimeout * 2;
    options.connectTimeout = 5;
    options.cancel = &cancel;

    // The authorization of the tracker waits until the code has been entered on its website,
    // which takes minutes. A shorter wait or a second attempt would cancel it.
    const bool authorization {pluginUrl.find("/trakt/") != std::string::npos ||
                              pluginUrl.find("/authorize") != std::string::npos};
    if (authorization)
        options.timeout = 900;

    HttpClient::Response http;
    for (int attempt {0}; attempt < (authorization ? 1 : 3); ++attempt) {
        http = HttpClient::request(url, options);
        if (http.error.empty() || cancel)
            break;
        // The program may still be starting.
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (authorization) {
        LOG(LogInfo) << "AddonHost: The authorization request ended, " <<
            (cancel ? "it was cancelled" : (http.error.empty() ? "answered" : http.error));
    }
    if (cancel) {
        response.type = Response::Type::FAILED;
        return response;
    }
    if (!http.error.empty()) {
        response.type = Response::Type::FAILED;
        response.error = "Couldn't connect to Elementum (" + http.error + ")";
        return response;
    }

    if (http.code == 300) {
        response.type = Response::Type::PLAY;
        response.url = http.location;
        return response;
    }
    if (http.code == 301) {
        response.type = Response::Type::NAVIGATE;
        response.url = http.location;
        response.replace = pluginUrl.find("keyboard=1") != std::string::npos;
        return response;
    }
    if (http.code >= 302 && http.code <= 307) {
        response.type = Response::Type::PLAY;
        response.url = http.location;
        return response;
    }
    if (http.code >= 400 && Utils::String::trim(http.body).empty()) {
        response.type = Response::Type::FAILED;
        response.error = "Elementum answered with code " + std::to_string(http.code);
        return response;
    }

    const std::string body {Utils::String::trim(http.body)};
    if (body.empty() || body == "null") {
        response.type = Response::Type::DONE;
        return response;
    }
    rapidjson::Document doc;
    doc.Parse(body.c_str());
    if (doc.HasParseError() || !doc.IsObject()) {
        // A message, e.g. an error, which is shown as a notification.
        response.type = Response::Type::FAILED;
        response.error = translateLabel(body);
        return response;
    }
    if (!doc.HasMember("items") || !doc["items"].IsArray()) {
        response.type = Response::Type::DONE;
        return response;
    }

    response.type = Response::Type::LISTING;
    if (doc.HasMember("content_type") && doc["content_type"].IsString()) {
        response.contentType = doc["content_type"].GetString();
        if (response.contentType.find('_') != std::string::npos)
            response.contentType.clear();
    }
    for (const auto& value : doc["items"].GetArray()) {
        if (!value.IsObject())
            continue;
        Item item;
        auto text = [&value](const char* name) {
            return value.HasMember(name) ? valueText(value[name]) : std::string();
        };
        item.label = text("label");
        item.label2 = text("label2");
        if (item.label.find("LOCALIZE") != std::string::npos)
            item.label = Utils::String::startsWith(item.label, "LOCALIZE") ?
                             translateLabel(item.label) :
                             translateText(item.label);
        if (item.label2.find("LOCALIZE") != std::string::npos)
            item.label2 = translateText(item.label2);
        item.icon = text("icon");
        item.thumbnail = text("thumbnail");
        item.path = text("path");
        item.playable = value.HasMember("is_playable") && value["is_playable"].IsBool() &&
                        value["is_playable"].GetBool();
        item.isAction = value.HasMember("is_action") && value["is_action"].IsBool() &&
                        value["is_action"].GetBool();
        if (value.HasMember("info") && value["info"].IsObject()) {
            for (const auto& member : value["info"].GetObject()) {
                const std::string key {Utils::String::toLower(member.name.GetString())};
                const std::string content {valueText(member.value)};
                if (!content.empty())
                    item.info[key] = content;
            }
        }
        if (value.HasMember("art") && value["art"].IsObject()) {
            for (const auto& member : value["art"].GetObject()) {
                if (member.value.IsString() && member.value.GetStringLength() > 0)
                    item.art[member.name.GetString()] = member.value.GetString();
            }
        }
        if (value.HasMember("properties") && value["properties"].IsObject()) {
            for (const auto& member : value["properties"].GetObject())
                item.properties[member.name.GetString()] = valueText(member.value);
        }
        if (value.HasMember("context_menu") && value["context_menu"].IsArray()) {
            for (const auto& entry : value["context_menu"].GetArray()) {
                if (!entry.IsArray() || entry.Size() < 2 || !entry[0].IsString() ||
                    !entry[1].IsString())
                    continue;
                std::string label {entry[0].GetString()};
                if (Utils::String::startsWith(label, "LOCALIZE"))
                    label = translateLabel(label);
                std::string action {entry[1].GetString()};
                if (Utils::String::startsWith(action, "XBMC."))
                    action = action.substr(5);
                // The video library and the view types of the media center don't exist here.
                if (action.find("/library/") != std::string::npos ||
                    action.find("/setviewmode/") != std::string::npos)
                    continue;
                item.contextMenu.emplace_back(label, action);
            }
        }
        if (Utils::String::endsWith(item.path, "/library") ||
            Utils::String::endsWith(item.path, "/elementum_library") ||
            item.path.find("/library/") != std::string::npos)
            continue;
        response.items.emplace_back(item);
    }
    return response;
}

//--------------------------------------------------------------------------------------------------
// Translations and settings.
//--------------------------------------------------------------------------------------------------

std::string AddonHost::translateText(const std::string& text)
{
    if (text.find("LOCALIZE[") == std::string::npos)
        return text;
    std::shared_ptr<AddonSettings> settings {
        AddonStore::getInstance().getSettings(AddonStore::STREAMING_ADDON)};
    static const std::regex reference {R"(LOCALIZE\[(\d+)\])"};
    std::string result;
    std::string rest {text};
    std::smatch match;
    while (std::regex_search(rest, match, reference)) {
        result.append(match.prefix());
        const std::string translated {
            settings ? settings->getString(std::atoi(match[1].str().c_str())) : ""};
        result.append(translated.empty() ? match[1].str() : translated);
        rest = match.suffix();
    }
    result.append(rest);
    return result;
}

std::string AddonHost::translateLabel(const std::string& label)
{
    if (label.find("LOCALIZE") == std::string::npos)
        return label;
    if (label.find(";;") == std::string::npos)
        return translateText(label);

    std::vector<std::string> parts;
    size_t start {0};
    while (true) {
        const size_t separator {label.find(";;", start)};
        parts.emplace_back(label.substr(start, separator == std::string::npos ?
                                                   std::string::npos :
                                                   separator - start));
        if (separator == std::string::npos)
            break;
        start = separator + 2;
    }
    const std::string format {translateText(parts.front())};
    std::vector<std::string> arguments;
    for (size_t i {1}; i < parts.size(); ++i) {
        arguments.emplace_back(Utils::String::startsWith(parts[i], "LOCALIZE") ?
                                   translateText(parts[i]) :
                                   parts[i]);
    }
    return formatText(format, arguments);
}

std::string AddonHost::settingValue(const std::string& id)
{
    const auto forced {forcedSettings().find(id)};
    if (forced != forcedSettings().cend())
        return forced->second;
    std::shared_ptr<AddonSettings> settings {
        AddonStore::getInstance().getSettings(AddonStore::STREAMING_ADDON)};
    return settings ? settings->get(id) : "";
}

std::string AddonHost::allSettingsJson()
{
    std::shared_ptr<AddonSettings> settings {
        AddonStore::getInstance().getSettings(AddonStore::STREAMING_ADDON)};
    if (!settings)
        return "[]";
    std::string json {"["};
    for (const auto& category : settings->getCategories()) {
        for (const auto& setting : category.settings) {
            if (setting.id.empty())
                continue;
            if (json.size() > 1)
                json.push_back(',');
            json.append("{\"key\":" + jsonQuote(setting.id) +
                        ",\"type\":" + jsonQuote(setting.type) +
                        ",\"option\":" + jsonQuote(setting.option) +
                        ",\"value\":" + jsonQuote(settingValue(setting.id)) + "}");
        }
    }
    return json + "]";
}

std::string AddonHost::preferredLanguage()
{
    // The interface is in English, but the language of the audio is the best match for the
    // titles and descriptions the user wants.
    const std::string language {Settings::getInstance()->getString("MediaAudioLanguage")};
    if (language.size() == 2)
        return language;
    return "en";
}

//--------------------------------------------------------------------------------------------------
// Scripts.
//--------------------------------------------------------------------------------------------------

void AddonHost::runScript(const std::string& addonId, const std::vector<std::string>& arguments)
{
    AddonStore::Addon addon;
    if (!AddonStore::getInstance().getAddon(addonId, addon) || addon.library.empty()) {
        LOG(LogWarning) << "AddonHost: Can't run the script add-on " << addonId;
        return;
    }
    // Python 3 of the system, or the own copy in ~/EmuSPLASH/dependencies/python.
    const std::string python {Dependencies::python()};
    if (python.empty()) {
        std::unique_lock<std::mutex> lock {mUiMutex};
        mNotifications.emplace_back(
            Notification {"Elementum", "Python 3 is needed for " + addon.name, ""});
        return;
    }
    const std::string shim {pythonDirectory()};
    std::string pythonPath {shim};
    AddonStore::Addon streaming;
    if (AddonStore::getInstance().getAddon(AddonStore::STREAMING_ADDON, streaming))
        pythonPath.append(":" + streaming.path + "/resources/site-packages");

    std::vector<std::string> argv {python, addon.path + "/" + addon.library};
    argv.insert(argv.end(), arguments.cbegin(), arguments.cend());
    const std::map<std::string, std::string> environment {
        {"PYTHONPATH", pythonPath},
        {"PYTHONIOENCODING", "utf-8"},
        {"PYTHONDONTWRITEBYTECODE", "1"},
        {"PYTHONWARNINGS", "ignore"},
        {"EMUSPLASH_ADDON_ID", addon.id},
        {"EMUSPLASH_ADDON_HOME", AddonStore::getHomeDirectory()},
        {"EMUSPLASH_RPC_PORT", std::to_string(ADDON_PORT)},
        {"EMUSPLASH_LANGUAGE", preferredLanguage()}};
    Utils::FileSystem::createDirectory(AddonStore::getLogDirectory());
    Utils::FileSystem::createDirectory(AddonStore::getTempDirectory());
    const std::string logPath {AddonStore::getLogDirectory() + "/" + addon.id + ".log"};
    trimLog(logPath);
    const int pid {spawnProcess(argv, environment, addon.path, logPath)};
    if (pid > 0) {
        std::unique_lock<std::mutex> lock {mScriptMutex};
        mScriptPids.insert(pid);
    }
}

//--------------------------------------------------------------------------------------------------
// Calls to the media center.
//--------------------------------------------------------------------------------------------------

std::string AddonHost::handleHostCall(const std::string& method, const std::string& params)
{
    const Params p {params};
    AddonStore& store {AddonStore::getInstance()};

    if (method == "JSONRPC.Ping")
        return jsonQuote("pong");
    if (method == "JSONRPC.Version")
        return "{\"version\":{\"major\":13,\"minor\":0,\"patch\":0}}";

    if (method == "XBMC.GetInfoLabels") {
        std::vector<std::string> labels {p.strings(0, "labels")};
        std::map<std::string, std::string> values;
        PlayerState player;
        {
            std::unique_lock<std::mutex> lock {mPlayerMutex};
            player = mPlayerState;
        }
        for (const auto& label : labels) {
            std::string value;
            if (label == "System.BuildVersion")
                value = BUILD_VERSION;
            else if (label == "System.FriendlyName")
                value = "EmuSPLASH";
            else if (label == "Container.FolderPath") {
                std::unique_lock<std::mutex> lock {mUiMutex};
                value = mCurrentPath;
            }
            else if (Utils::String::startsWith(label, "VideoPlayer.")) {
                const auto it {player.labels.find(label.substr(12))};
                if (it != player.labels.cend())
                    value = it->second;
            }
            values[label] = value;
        }
        return jsonObject(values);
    }

    if (method == "Addons.GetAddons") {
        const std::string type {p.string(0, "type")};
        const rapidjson::Value* enabledValue {p.get(2, "enabled")};
        const bool onlyEnabled {enabledValue != nullptr && enabledValue->IsBool() &&
                                enabledValue->GetBool()};
        std::string list;
        int count {0};
        for (const auto& addon : store.getAddons()) {
            if (!type.empty() && type != "unknown" && addon.type != type)
                continue;
            if (onlyEnabled && !addon.enabled)
                continue;
            if (!list.empty())
                list.push_back(',');
            list.append("{\"addonid\":" + jsonQuote(addon.id) + ",\"type\":" +
                        jsonQuote(addon.type) + ",\"name\":" + jsonQuote(addon.name) +
                        ",\"version\":" + jsonQuote(addon.version) +
                        ",\"enabled\":" + (addon.enabled ? "true" : "false") + "}");
            ++count;
        }
        return "{\"addons\":[" + list + "],\"limits\":{\"start\":0,\"end\":" +
               std::to_string(count) + ",\"total\":" + std::to_string(count) + "}}";
    }
    if (method == "Addons.GetAddonDetails") {
        AddonStore::Addon addon;
        if (!store.getAddon(p.string(0, "addonid"), addon))
            return "{\"addon\":{}}";
        return "{\"addon\":{\"addonid\":" + jsonQuote(addon.id) +
               ",\"type\":" + jsonQuote(addon.type) + ",\"name\":" + jsonQuote(addon.name) +
               ",\"version\":" + jsonQuote(addon.version) +
               ",\"enabled\":" + (addon.enabled ? "true" : "false") + ",\"installed\":true}}";
    }
    if (method == "Addons.SetAddonEnabled") {
        const std::string id {p.string(0, "addonid")};
        const rapidjson::Value* value {p.get(1, "enabled")};
        bool enabled {true};
        if (value != nullptr && value->IsBool())
            enabled = value->GetBool();
        else if (value != nullptr && value->IsString())
            enabled = std::string(value->GetString()) != "false";
        store.setEnabled(id, enabled);
        return jsonQuote("OK");
    }
    if (method == "Addons.ExecuteAddon") {
        const std::string id {p.string(0, "addonid")};
        std::vector<std::string> arguments;
        const rapidjson::Value* value {p.get(1, "params")};
        if (value != nullptr && value->IsArray()) {
            for (const auto& argument : value->GetArray())
                arguments.emplace_back(valueText(argument));
        }
        else if (value != nullptr && value->IsString()) {
            arguments.emplace_back(value->GetString());
        }
        runScript(id, arguments);
        return jsonQuote("OK");
    }

    if (method == "Settings.GetSettingValue") {
        const std::string setting {p.string(0, "setting")};
        std::string value;
        if (setting == "locale.language")
            value = "resource.language.en_gb";
        else if (setting == "locale.subtitlelanguage")
            value = languageName(
                Settings::getInstance()->getString("MediaSubtitleDownloadLanguage"));
        else if (setting == "locale.audiolanguage")
            value = languageName(Settings::getInstance()->getString("MediaAudioLanguage"));
        return "{\"value\":" + jsonQuote(value) + "}";
    }

    if (method == "Player.GetActivePlayers") {
        std::unique_lock<std::mutex> lock {mPlayerMutex};
        if (mPlayerState.active)
            return "[{\"playerid\":1,\"playertype\":\"internal\",\"type\":\"video\"}]";
        return "[]";
    }
    if (method == "Player.GetItem") {
        std::unique_lock<std::mutex> lock {mPlayerMutex};
        const auto title {mPlayerState.labels.find("Title")};
        return "{\"item\":{\"label\":" +
               jsonQuote(title == mPlayerState.labels.cend() ? "" : title->second) +
               ",\"type\":\"unknown\"}}";
    }

    // There's no video library here, so it's always empty.
    if (method == "VideoLibrary.GetMovies")
        return "{\"movies\":[],\"limits\":{\"start\":0,\"end\":0,\"total\":0}}";
    if (method == "VideoLibrary.GetTVShows")
        return "{\"tvshows\":[],\"limits\":{\"start\":0,\"end\":0,\"total\":0}}";
    if (method == "VideoLibrary.GetSeasons")
        return "{\"seasons\":[],\"limits\":{\"start\":0,\"end\":0,\"total\":0}}";
    if (method == "VideoLibrary.GetEpisodes")
        return "{\"episodes\":[],\"limits\":{\"start\":0,\"end\":0,\"total\":0}}";
    if (method == "Files.GetSources")
        return "{\"sources\":[],\"limits\":{\"start\":0,\"end\":0,\"total\":0}}";
    if (Utils::String::startsWith(method, "VideoLibrary.") ||
        Utils::String::startsWith(method, "Files.") || method == "Input.Left" ||
        Utils::String::startsWith(method, "Input."))
        return jsonQuote("OK");

    return "";
}

//--------------------------------------------------------------------------------------------------
// Calls to the add-on.
//--------------------------------------------------------------------------------------------------

std::string AddonHost::handleAddonCall(const std::string& method, const std::string& params)
{
    const Params p {params};
    AddonStore& store {AddonStore::getInstance()};

    auto newHandle = [this](const char* prefix) {
        return std::string(prefix) + std::to_string(mNextHandle++);
    };
    auto notify = [this](const std::string& heading, const std::string& message,
                         const std::string& icon) {
        std::unique_lock<std::mutex> lock {mUiMutex};
        mNotifications.emplace_back(Notification {heading, message, icon});
    };
    auto uiRequest = [](const std::string& requestMethod) {
        auto request = std::make_shared<UiRequest>();
        request->method = requestMethod;
        return request;
    };

    // State and simple information.
    if (method == "Ping")
        return "true";
    if (method == "Reset") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        mProgressDialogs.clear();
        mOverlays.clear();
        return "null";
    }
    if (method == "GetPlatform") {
        struct utsname name {};
        uname(&name);
        return "{\"os\":\"linux\",\"arch\":\"x64\",\"version\":" + jsonQuote(name.release) +
               ",\"kodi\":21,\"build\":" + jsonQuote(BUILD_VERSION) + ",\"fork\":true}";
    }
    if (method == "GetAddonInfo") {
        AddonStore::Addon addon;
        store.getAddon(AddonStore::STREAMING_ADDON, addon);
        return jsonObject({{"author", addon.provider},
                           {"changelog", addon.changelog},
                           {"description", addon.description},
                           {"disclaimer", ""},
                           {"fanart", addon.fanart},
                           {"icon", addon.icon},
                           {"id", addon.id},
                           {"name", addon.name},
                           {"profile", AddonStore::getAddonDataDirectory(addon.id)},
                           {"stars", "0"},
                           {"summary", addon.summary},
                           {"type", addon.type},
                           {"version", addon.version},
                           {"path", addon.path},
                           {"home", AddonStore::getHomeDirectory()},
                           {"xbmc", AddonStore::getHomeDirectory()}});
    }
    if (method == "TranslatePath")
        return jsonQuote(specialPath(p.string(0)));
    if (method == "Translate")
        return jsonQuote(translateLabel(p.string(0)));
    if (method == "TranslateText")
        return jsonQuote(translateText(p.string(0)));
    if (method == "GetLocalizedString") {
        std::shared_ptr<AddonSettings> settings {store.getSettings(AddonStore::STREAMING_ADDON)};
        return jsonQuote(settings ? settings->getString(p.integer(0)) : "");
    }
    if (method == "GetLanguage") {
        const int format {p.integer(0)};
        const std::string code {preferredLanguage()};
        if (format == 0)
            return jsonQuote(p.boolean(1) ? code + "-" + code : code);
        if (format == 1)
            return jsonQuote(code == "en" ? "eng" : code);
        return jsonQuote(languageName(code));
    }
    if (method == "ConvertLanguage") {
        const std::string language {p.string(0)};
        const int format {p.integer(1)};
        const std::string code {SubtitleDownloader::normalizeLanguage(language)};
        if (format == 0)
            return jsonQuote(code);
        if (format == 2)
            return jsonQuote(languageName(code));
        return jsonQuote(language);
    }
    if (method == "Log" || method == "SystemInformation" || method == "UpNext_Notify" ||
        method == "ToggleWatched" || method == "GetKodiLog" || method == "Playlist_Add" ||
        method == "Playlist_Clear" || method == "UpdateAddonRepos" ||
        method == "Player_Open_With_Labels" || method == "SetResolvedUrl")
        return "null";
    if (method == "UpdateLocalAddons") {
        store.scan();
        return "null";
    }
    if (method == "Window_Is_Loading" || method == "Playlist_Left" || method == "Playlist_Size")
        return "0";
    if (method == "Wait_Window_Loaded")
        return "1";
    if (method == "GetCurrentView")
        return jsonQuote("");

    // Settings of the streaming add-on.
    if (method == "GetSetting")
        return jsonQuote(settingValue(p.string(0)));
    if (method == "GetAllSettings")
        return allSettingsJson();
    if (method == "SetSetting") {
        const std::string id {p.string(0)};
        if (forcedSettings().count(id) == 0) {
            std::shared_ptr<AddonSettings> settings {
                store.getSettings(AddonStore::STREAMING_ADDON)};
            if (settings)
                settings->set(id, p.string(1));
        }
        return "null";
    }
    if (method == "AddonSettings") {
        auto request = uiRequest("AddonSettings");
        request->addonId = p.string(0);
        queueUi(request);
        return "null";
    }
    if (method == "AddonSettingsOpened")
        return mSettingsOpen ? "1" : "0";
    if (method == "AddonFailure" || method == "AddonCheck") {
        const std::string id {p.string(0)};
        std::unique_lock<std::mutex> lock {mUiMutex};
        if (method == "AddonCheck")
            return std::to_string(mProviderFailures[id]);
        if (settingValue("provider_disable_failing") != "true")
            return "0";
        const int failures {++mProviderFailures[id]};
        if (failures > std::atoi(settingValue("provider_allowed_failures").c_str())) {
            mNotifications.emplace_back(
                Notification {"Elementum", translateText("LOCALIZE[30111]"), ""});
            lock.unlock();
            queueTask([id] {
                HttpClient::Options options;
                options.userAgent = ADDON_USER_AGENT;
                HttpClient::request(DAEMON_URL + "/provider/" + id + "/disable", options);
            });
        }
        return std::to_string(failures);
    }
    if (method == "GetWindowProperty") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        return jsonQuote(mWindowProperties[p.string(0)]);
    }
    if (method == "SetWindowProperty") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        mWindowProperties[p.string(0)] = p.string(1);
        return "null";
    }

    // Add-ons.
    if (method == "InstallAddon") {
        const std::string id {p.string(0)};
        if (store.isInstalled(id))
            return "null";
        queueTask([this, id, notify] {
            const std::string handle {"install-" + id};
            {
                std::unique_lock<std::mutex> lock {mUiMutex};
                mProgressDialogs[handle] = ProgressDialog {handle, "Installing " + id, "", 0,
                                                           true, false};
            }
            const AddonStore::Result result {AddonStore::getInstance().installFromRepository(
                id, [this, handle](const std::string& message, int percent) {
                    std::unique_lock<std::mutex> lock {mUiMutex};
                    auto it {mProgressDialogs.find(handle)};
                    if (it != mProgressDialogs.end()) {
                        it->second.message = message;
                        it->second.percent = std::max(0, percent);
                    }
                })};
            {
                std::unique_lock<std::mutex> lock {mUiMutex};
                mProgressDialogs.erase(handle);
            }
            if (!result.success)
                notify("Elementum", result.error, "");
        });
        return "null";
    }

    // Notifications and dialogs.
    if (method == "Notify") {
        notify(translateLabel(p.string(0)), translateLabel(p.string(1)), p.string(2));
        return "null";
    }
    if (method == "Dialog") {
        auto request = uiRequest("Dialog");
        request->title = translateLabel(p.string(0));
        request->message = translateLabel(p.string(1));
        const std::string result {waitForUi(request)};
        return result == "null" ? "0" : "1";
    }
    if (method == "Dialog_Confirm" || method == "Dialog_Confirm_With_Timeout") {
        auto request = uiRequest("Dialog_Confirm");
        request->title = translateLabel(p.string(0));
        request->message = translateLabel(p.string(1));
        if (method == "Dialog_Confirm_With_Timeout") {
            request->focused = p.boolean(2);
            request->timeout = p.integer(3);
        }
        const std::string result {waitForUi(request)};
        return result == "1" ? "1" : "0";
    }
    if (method == "Dialog_Select" || method == "Dialog_Select_With_Options" ||
        method == "Dialog_Select_Large" || method == "Dialog_MultiSelect") {
        auto request = uiRequest(method == "Dialog_MultiSelect" ? "Dialog_MultiSelect" :
                                                                 "Dialog_Select");
        request->title = translateLabel(p.string(0));
        if (method == "Dialog_Select_Large") {
            request->title.append(" " + p.string(1));
            request->items = p.strings(2);
            request->method = "Dialog_Select_Large";
        }
        else {
            request->items = p.strings(1);
        }
        for (auto& item : request->items)
            item = translateLabel(item);
        if (method == "Dialog_Select_With_Options") {
            request->autoclose = p.integer(2);
            request->preselect = p.integer(3, -1);
        }
        if (method == "Dialog_MultiSelect")
            request->preselected = p.integers(2);
        const std::string result {waitForUi(request)};
        if (method == "Dialog_MultiSelect")
            return result;
        return result == "null" ? "-1" : result;
    }
    if (method == "Dialog_Text") {
        auto request = uiRequest("Dialog_Text");
        request->title = translateLabel(p.string(0));
        request->message = translateText(p.string(1));
        queueUi(request);
        return "null";
    }
    if (method == "Keyboard") {
        auto request = uiRequest("Keyboard");
        request->text = p.string(0);
        request->title = translateLabel(p.string(1));
        request->hidden = p.boolean(2);
        return waitForUi(request);
    }
    if (method == "Dialog_Browse_Single") {
        auto request = uiRequest("Dialog_Browse_Single");
        request->browseType = p.integer(0);
        request->title = translateLabel(p.string(1));
        request->mask = p.string(3);
        request->text = p.string(6);
        const std::string result {waitForUi(request)};
        return result == "null" ? jsonQuote(p.string(6)) : result;
    }
    if (method == "DialogInsert") {
        auto request = uiRequest("DialogInsert");
        const std::string result {waitForUi(request)};
        return result == "null" ? "{\"type\":\"cancelled\",\"path\":\"\"}" : result;
    }
    if (method == "Dialog_CloseAll") {
        ++mCloseGeneration;
        ++mConfirmGeneration;
        return "null";
    }
    if (method == "Dialog_CloseAllConfirms") {
        ++mConfirmGeneration;
        return "null";
    }

    // Progress dialogs and the status overlay.
    if (method == "DialogProgress_Create") {
        ProgressDialog dialog;
        dialog.id = newHandle("progress-");
        dialog.heading = translateLabel(p.string(0));
        std::vector<std::string> lines;
        for (size_t i {1}; i <= 3; ++i) {
            const std::string line {translateLabel(p.string(i))};
            if (!line.empty())
                lines.emplace_back(line);
        }
        dialog.message = joinLines(lines);
        dialog.updated = nowMilliseconds();
        std::unique_lock<std::mutex> lock {mUiMutex};
        mProgressDialogs[dialog.id] = dialog;
        return jsonQuote(dialog.id);
    }
    if (method == "DialogProgress_Update" || method == "DialogProgressBG_Update") {
        const std::string id {p.string(0)};
        std::unique_lock<std::mutex> lock {mUiMutex};
        auto it {mProgressDialogs.find(id)};
        if (it == mProgressDialogs.end())
            return "null";
        it->second.percent = std::clamp(p.integer(1), 0, 100);
        it->second.updated = nowMilliseconds();
        if (method == "DialogProgress_Update") {
            std::vector<std::string> lines;
            for (size_t i {2}; i <= 4; ++i) {
                const std::string line {p.string(i)};
                if (!line.empty())
                    lines.emplace_back(line);
            }
            it->second.message = joinLines(lines);
        }
        else {
            const std::string heading {p.string(2)};
            if (!heading.empty())
                it->second.heading = translateLabel(heading);
            it->second.message = translateLabel(p.string(3));
        }
        return "null";
    }
    if (method == "DialogProgress_IsCanceled") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        const auto it {mProgressDialogs.find(p.string(0))};
        return it != mProgressDialogs.cend() && it->second.canceled ? "1" : "0";
    }
    if (method == "DialogProgress_Close" || method == "DialogProgressBG_Close") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        mProgressDialogs.erase(p.string(0));
        return "null";
    }
    if (method == "DialogProgressBG_Create") {
        ProgressDialog dialog;
        dialog.id = newHandle("background-");
        dialog.heading = translateLabel(p.string(0));
        dialog.message = translateLabel(p.string(1));
        dialog.background = true;
        dialog.updated = nowMilliseconds();
        std::unique_lock<std::mutex> lock {mUiMutex};
        mProgressDialogs[dialog.id] = dialog;
        return jsonQuote(dialog.id);
    }
    if (method == "DialogProgressBG_IsFinished") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        return mProgressDialogs.count(p.string(0)) == 0 ? "1" : "0";
    }
    if (method == "DialogProgressBG_Cleanup") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        for (auto it = mProgressDialogs.begin(); it != mProgressDialogs.end();) {
            if (it->second.background && !Utils::String::startsWith(it->first, "install-"))
                it = mProgressDialogs.erase(it);
            else
                ++it;
        }
        return "null";
    }
    if (method == "OverlayStatus_Create") {
        const std::string id {newHandle("overlay-")};
        std::unique_lock<std::mutex> lock {mUiMutex};
        mOverlays[id] = Overlay();
        return jsonQuote(id);
    }
    if (method == "OverlayStatus_Update") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        auto it {mOverlays.find(p.string(0))};
        if (it != mOverlays.end()) {
            it->second.percent = p.integer(1);
            std::vector<std::string> lines;
            for (size_t i {2}; i <= 4; ++i) {
                const std::string line {p.string(i)};
                if (!line.empty())
                    lines.emplace_back(line);
            }
            it->second.text = joinLines(lines);
        }
        return "null";
    }
    if (method == "OverlayStatus_Show" || method == "OverlayStatus_Hide") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        auto it {mOverlays.find(p.string(0))};
        if (it != mOverlays.end())
            it->second.visible = method == "OverlayStatus_Show";
        return "null";
    }
    if (method == "OverlayStatus_Close") {
        std::unique_lock<std::mutex> lock {mUiMutex};
        mOverlays.erase(p.string(0));
        return "null";
    }

    // Navigation.
    if (method == "Refresh") {
        queueUi(uiRequest("Refresh"));
        return "null";
    }
    if (method == "Update" || method == "OpenDirectory") {
        auto request = uiRequest("Update");
        request->text = p.string(0);
        queueUi(request);
        return "null";
    }

    // The player.
    if (method == "Player_Open" || method == "Player_Open_With_Timeout") {
        auto request = uiRequest("Player_Open");
        request->text = p.string(0);
        queueUi(request);
        return "null";
    }
    {
        std::unique_lock<std::mutex> lock {mPlayerMutex};
        if (method == "Player_IsPlaying")
            return mPlayerState.active ? "1" : "0";
        if (method == "Player_IsPaused")
            return mPlayerState.active && mPlayerState.paused ? "1" : "0";
        if (method == "Player_GetPlayingFile")
            return jsonQuote(mPlayerState.active ? mPlayerState.file : "");
        if (method == "Player_WatchTimes") {
            char position[32];
            char duration[32];
            snprintf(position, sizeof(position), "%.3f", mPlayerState.position);
            snprintf(duration, sizeof(duration), "%.3f", mPlayerState.duration);
            return jsonObject({{"watchedTime", mPlayerState.active ? position : "0"},
                               {"videoDuration", mPlayerState.active ? duration : "0"},
                               {"error", mPlayerState.active ? "" : "Stopped playing"}});
        }
        if (method == "Player_Seek") {
            if (!mPlayerState.active)
                return jsonQuote("Not playing");
            PlayerCommand command;
            command.type = PlayerCommand::Type::SEEK;
            command.position = p.number(0);
            mPlayerCommands.emplace_back(command);
            return jsonQuote("");
        }
        if (method == "Player_GetSubtitles") {
            if (!mPlayerState.active)
                return "null";
            std::string list;
            for (const auto& subtitle : mPlayerState.subtitles)
                list.append((list.empty() ? "" : ",") + jsonQuote(subtitle));
            return "[" + list + "]";
        }
        if (method == "Player_SetSubtitles") {
            PlayerCommand command;
            command.type = PlayerCommand::Type::ADD_SUBTITLES;
            command.paths = p.strings(0);
            mPlayerCommands.emplace_back(command);
            return "null";
        }
    }

    // Calls made by the compatibility modules of the script add-ons.
    if (method == "Addon_GetSettings") {
        std::shared_ptr<AddonSettings> settings {store.getSettings(p.string(0))};
        if (!settings)
            return "{}";
        std::map<std::string, std::string> values {settings->getAll()};
        if (p.string(0) == AddonStore::STREAMING_ADDON) {
            for (const auto& [id, value] : forcedSettings())
                values[id] = value;
        }
        return jsonObject(values);
    }
    if (method == "Addon_SetSetting") {
        std::shared_ptr<AddonSettings> settings {store.getSettings(p.string(0))};
        if (settings)
            settings->set(p.string(1), p.string(2));
        return "null";
    }
    if (method == "Addon_GetStrings") {
        std::shared_ptr<AddonSettings> settings {store.getSettings(p.string(0))};
        return settings ? jsonObject(settings->getStrings()) : "{}";
    }
    if (method == "Addon_GetInfo") {
        AddonStore::Addon addon;
        if (!store.getAddon(p.string(0), addon))
            return "{}";
        return jsonObject({{"author", addon.provider},
                           {"changelog", addon.changelog},
                           {"description", addon.description},
                           {"fanart", addon.fanart},
                           {"icon", addon.icon},
                           {"id", addon.id},
                           {"name", addon.name},
                           {"path", addon.path},
                           {"profile", AddonStore::getAddonDataDirectory(addon.id)},
                           {"stars", "0"},
                           {"summary", addon.summary},
                           {"type", addon.type},
                           {"version", addon.version}});
    }
    if (method == "Host_InfoLabel") {
        const std::string labels {"[[" + jsonQuote(p.string(0)) + "]]"};
        rapidjson::Document doc;
        doc.Parse(handleHostCall("XBMC.GetInfoLabels", labels).c_str());
        if (!doc.HasParseError() && doc.IsObject()) {
            for (const auto& member : doc.GetObject())
                return jsonQuote(valueText(member.value));
        }
        return jsonQuote("");
    }
    if (method == "Host_Builtin") {
        auto request = uiRequest("Builtin");
        request->text = p.string(0);
        queueUi(request);
        return "null";
    }
    if (method == "Host_JsonRpc") {
        rapidjson::Document doc;
        doc.Parse(p.string(0).c_str());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("method") ||
            !doc["method"].IsString())
            return jsonQuote("{}");
        const std::string result {handleHostCall(
            doc["method"].GetString(), doc.HasMember("params") ? jsonValue(doc["params"]) : "{}")};
        return jsonQuote("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":" +
                         (result.empty() ? std::string("null") : result) + "}");
    }

    return "";
}
