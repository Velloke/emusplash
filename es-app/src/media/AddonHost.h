//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonHost.h
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

#ifndef ES_APP_MEDIA_ADDON_HOST_H
#define ES_APP_MEDIA_ADDON_HOST_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class AddonHost
{
public:
    enum class State {
        STOPPED,
        STARTING,
        RUNNING,
        FAILED
    };

    // An entry of a listing.
    struct Item {
        std::string label;
        std::string label2;
        std::string icon;
        std::string thumbnail;
        std::string path;
        bool playable {false};
        bool isAction {false};
        // Lower-case keys, e.g. "title", "year", "plot". Lists are joined using " / ".
        std::map<std::string, std::string> info;
        // E.g. "poster", "fanart", "thumb".
        std::map<std::string, std::string> art;
        std::map<std::string, std::string> properties;
        // Label and action.
        std::vector<std::pair<std::string, std::string>> contextMenu;
    };

    struct Response {
        enum class Type {
            LISTING,  // Items to show.
            PLAY,     // The URL is to be played.
            NAVIGATE, // The URL is a listing to open instead.
            DONE,     // An action without a result.
            FAILED
        };
        Type type {Type::DONE};
        std::string contentType;
        std::vector<Item> items;
        std::string url;
        std::string error;
        bool replace {false};
    };

    // A request to the program running in the background.
    class Request
    {
    public:
        bool isReady() const;
        Response take();
        void cancel();
        const std::string& getUrl() const { return mUrl; }

    private:
        friend class AddonHost;
        std::string mUrl;
        std::shared_ptr<std::atomic<bool>> mCancel {std::make_shared<std::atomic<bool>>(false)};
        std::future<Response> mFuture;
    };

    // A call that needs the user interface. Blocking calls wait for the answer.
    class UiRequest
    {
    public:
        std::string method;
        std::string addonId;
        std::string title;
        std::string message;
        std::vector<std::string> items;
        std::vector<int> preselected;
        int preselect {-1};
        int autoclose {0}; // Milliseconds.
        bool focused {false};
        int timeout {0}; // Seconds.
        bool hidden {false};
        std::string text; // Default text, path or URL.
        int browseType {0};
        std::string mask;
        unsigned int confirmGeneration {0};
        unsigned int closeGeneration {0};
        bool blocking {false};

        // The answer as a JSON value, e.g. "1", "\"text\"" or "null". Only the first one counts.
        void answer(const std::string& json);
        bool isAnswered() const { return mAnswered; }

    private:
        friend class AddonHost;
        std::promise<std::string> mPromise;
        std::atomic<bool> mAnswered {false};
    };

    struct ProgressDialog {
        std::string id;
        std::string heading;
        std::string message;
        int percent {0};
        bool background {false};
        bool canceled {false};
        long long updated {0}; // Milliseconds, steady clock.
    };

    struct Overlay {
        bool visible {false};
        int percent {0};
        std::string text;
    };

    struct Notification {
        std::string heading;
        std::string message;
        std::string icon;
    };

    // Published by the video player while a stream is playing.
    struct PlayerState {
        bool active {false};
        bool paused {false};
        double position {0.0};
        double duration {0.0};
        std::string file;
        std::vector<std::string> subtitles;
        std::map<std::string, std::string> labels; // E.g. "Title", "Year", "Season".
    };

    struct PlayerCommand {
        enum class Type {
            SEEK,
            ADD_SUBTITLES
        };
        Type type {Type::SEEK};
        double position {0.0};
        std::vector<std::string> paths;
    };

    static AddonHost& getInstance();

    // Starts the servers and the program in the background, returns at once.
    void start();
    // Stops everything in the background, returns at once.
    void requestStop();
    // Stops everything and waits until done.
    void stop();
    State getState() const { return mState; }
    std::string getStateMessage() const;

    static bool isPluginUrl(const std::string& url);
    static std::string getRootUrl();
    // Opens a listing or runs an action in the background.
    std::shared_ptr<Request> open(const std::string& pluginUrl);
    // Runs an action without waiting for its result, e.g. from the settings.
    void runAction(const std::string& pluginUrl);
    // Asks the program to read its settings again, after they have been changed.
    void reloadSettings();
    // Stops the program and starts it again, also if it has stopped answering.
    void restartDaemon();

    // Translates the labels of the streaming add-on, e.g. "LOCALIZE[30214]".
    std::string translateLabel(const std::string& label);
    std::string translateText(const std::string& text);

    // Calls waiting for the user interface. Calls that change the listing or open the player
    // are only returned if canNavigate is set.
    std::shared_ptr<UiRequest> takeUiRequest(bool canNavigate);
    bool hasUiRequests() const;
    unsigned int getConfirmGeneration() const { return mConfirmGeneration; }
    unsigned int getCloseGeneration() const { return mCloseGeneration; }

    std::vector<ProgressDialog> getProgressDialogs();
    void cancelProgressDialog(const std::string& id);
    Overlay getOverlay() const;
    std::vector<Notification> takeNotifications();

    // The video player.
    void setPlayerState(const PlayerState& state);
    std::vector<PlayerCommand> takePlayerCommands();
    // Sends a player event to the program, e.g. "Player.OnPlay".
    void sendPlayerEvent(const std::string& method, const std::string& data);

    // The listing currently shown, reported to the add-ons as the folder path.
    void setCurrentPath(const std::string& path);
    // Whether the settings of an add-on are being shown, which the program waits for.
    void setSettingsOpen(bool open) { mSettingsOpen = open; }

    // Runs a script add-on with the given arguments.
    void runScript(const std::string& addonId, const std::vector<std::string>& arguments);
    // The language preferred by the user, as a two letter code.
    static std::string preferredLanguage();

private:
    AddonHost();
    ~AddonHost();

    // Servers.
    bool startServers(std::string& error);
    void stopServers();
    void acceptLoop(int serverSocket, bool hostApi);
    void connectionLoop(int socket, bool hostApi);
    std::string handleCall(const std::string& method, const std::string& params, bool hostApi);
    std::string handleHostCall(const std::string& method, const std::string& params);
    std::string handleAddonCall(const std::string& method, const std::string& params);
    std::string waitForUi(const std::shared_ptr<UiRequest>& request);
    void queueUi(const std::shared_ptr<UiRequest>& request);

    // The program.
    void daemonLoop();
    void applyStreamingDefaults();
    void shutdown();
    bool spawnDaemon(std::string& error);
    bool waitForDaemon();
    void stopDaemon();
    void setState(State state, const std::string& message);
    Response performRequest(const std::string& pluginUrl, const std::atomic<bool>& cancel);
    Response performSingleRequest(const std::string& pluginUrl, const std::atomic<bool>& cancel);
    void queueTask(const std::function<void()>& task);
    void taskLoop();

    std::string settingValue(const std::string& id);
    std::string allSettingsJson();

    std::atomic<State> mState;
    mutable std::mutex mStateMutex;
    std::string mStateMessage;
    std::atomic<bool> mStopping;

    // Servers and connections.
    int mAddonServer;
    int mHostServer;
    std::thread mAddonServerThread;
    std::thread mHostServerThread;
    std::mutex mConnectionMutex;
    std::condition_variable mConnectionCondition;
    std::set<int> mConnections;
    int mConnectionCount;

    // The program and the scripts.
    std::mutex mThreadMutex;
    std::thread mDaemonThread;
    std::thread mStopThread;
    std::atomic<bool> mStartPending;
    std::atomic<int> mDaemonPid;
    std::atomic<bool> mRestartRequested;
    std::mutex mScriptMutex;
    std::set<int> mScriptPids;
    std::thread mReaperThread;

    // Background tasks, e.g. player events.
    std::thread mTaskThread;
    std::mutex mTaskMutex;
    std::condition_variable mTaskCondition;
    std::deque<std::function<void()>> mTasks;
    bool mTaskStop;

    // User interface.
    mutable std::mutex mUiMutex;
    std::deque<std::shared_ptr<UiRequest>> mUiRequests;
    std::set<std::shared_ptr<UiRequest>> mPendingRequests;
    std::map<std::string, ProgressDialog> mProgressDialogs;
    std::map<std::string, Overlay> mOverlays;
    std::vector<Notification> mNotifications;
    std::map<std::string, std::string> mWindowProperties;
    std::map<std::string, int> mProviderFailures;
    std::atomic<unsigned int> mConfirmGeneration;
    std::atomic<unsigned int> mCloseGeneration;
    unsigned int mNextHandle;
    std::string mCurrentPath;
    std::atomic<bool> mSettingsOpen;

    // Player.
    mutable std::mutex mPlayerMutex;
    PlayerState mPlayerState;
    std::vector<PlayerCommand> mPlayerCommands;
};

#endif // ES_APP_MEDIA_ADDON_HOST_H
