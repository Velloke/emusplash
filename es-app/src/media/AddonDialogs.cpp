//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonDialogs.cpp
//
//  Shows what the streaming add-on and its search providers ask for: messages, questions,
//  choices, text input, file and folder selection, progress, notifications and the status
//  shown while paused. Also the settings of the add-ons, built from their own definitions.
//  Used by the media screen and the video player, each with its own dialog stack.
//

#include "media/AddonDialogs.h"

#include "Settings.h"
#include "Window.h"
#include "guis/GuiTextEditKeyboardPopup.h"
#include "guis/GuiTextEditPopup.h"
#include "media/AddonStore.h"
#include "media/HttpClient.h"
#include "media/MediaLibrary.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace
{
    std::string quote(const std::string& text)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer {buffer};
        writer.String(text.c_str(), static_cast<rapidjson::SizeType>(text.size()));
        return buffer.GetString();
    }

    std::string clean(const std::string& text)
    {
        return Utils::String::trim(stripAddonFormatting(text));
    }

    std::string firstLine(const std::string& text)
    {
        const size_t newline {text.find('\n')};
        return newline == std::string::npos ? text : text.substr(0, newline);
    }

    std::string maskText(const std::string& value)
    {
        return value.empty() ? std::string() : std::string(std::min<size_t>(value.size(), 12), '*');
    }

    // Splits "Name(argument, argument)" into the name and the arguments.
    bool parseCommand(const std::string& command,
                      std::string& name,
                      std::vector<std::string>& arguments)
    {
        const size_t open {command.find('(')};
        const size_t close {command.rfind(')')};
        name = Utils::String::trim(command.substr(0, open));
        arguments.clear();
        if (open == std::string::npos || close == std::string::npos || close < open)
            return !name.empty();
        std::string current;
        bool quoted {false};
        for (const char character : command.substr(open + 1, close - open - 1)) {
            if (character == '"') {
                quoted = !quoted;
                continue;
            }
            if (character == ',' && !quoted) {
                arguments.emplace_back(Utils::String::trim(current));
                current.clear();
                continue;
            }
            current.push_back(character);
        }
        if (!Utils::String::trim(current).empty() || !arguments.empty())
            arguments.emplace_back(Utils::String::trim(current));
        return true;
    }

    // Settings of the streaming add-on that belong to functions of the media center it was
    // written for, which don't exist here.
    bool isHiddenSetting(const std::string& addonId, const std::string& id)
    {
        if (id.empty())
            return false;
        if (addonId == AddonStore::PROVIDER_ADDON)
            return id == "kodi_language";
        if (addonId != AddonStore::STREAMING_ADDON)
            return false;
        static const std::set<std::string> hidden {"skip_repository_search",
                                                   "binary_platform",
                                                   "preload_timeout",
                                                   "startup_delay",
                                                   "action_cmd_open_path_download",
                                                   "action_cmd_open_path_library",
                                                   "action_cmd_open_path_torrents",
                                                   "library_path",
                                                   "action_cmd_reset_path_library",
                                                   "do_not_disturb",
                                                   "overlay_status_offset",
                                                   "auto_kodi_buffer_size",
                                                   "action_cmd_paste_bundle",
                                                   "osdb_auto_language",
                                                   "trakt_sync_collections",
                                                   "trakt_sync_watchlist",
                                                   "trakt_sync_userlists",
                                                   "trakt_sync_watched",
                                                   "trakt_sync_watchedback",
                                                   "trakt_sync_playback_progress",
                                                   "trakt_sync_hidden",
                                                   "strm_language"};
        if (hidden.count(id) != 0)
            return true;
        for (const char* prefix : {"viewmode_", "trakt_sync_added_", "trakt_sync_removed_",
                                   "action_trakt_select_list_"}) {
            if (Utils::String::startsWith(id, prefix))
                return true;
        }
        return false;
    }

    bool isHiddenCategory(const std::string& addonId, const AddonSettings::Category& category)
    {
        if (addonId != AddonStore::STREAMING_ADDON)
            return false;
        for (const auto& setting : category.settings) {
            if (setting.id == "library_enabled" || setting.id == "local_only_client")
                return true;
        }
        return false;
    }

    std::string formatSlider(const AddonSettings::Definition& setting, const std::string& value)
    {
        if (setting.option == "float") {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.1f", std::atof(value.c_str()));
            return buffer;
        }
        const std::string number {std::to_string(static_cast<int>(std::atof(value.c_str())))};
        return setting.option == "percent" ? number + "%" : number;
    }
} // namespace

AddonDialogs::AddonDialogs(MediaDialogStack& dialogs, Delegate* delegate)
    : mDialogs {dialogs}
    , mDelegate {delegate}
{
    mProgressTitle =
        MediaStyle::makeText(MediaStyle::font(0.034f, FONT_PATH_BOLD), MediaStyle::TEXT);
    mProgressMessage = std::make_unique<TextComponent>("", MediaStyle::font(0.027f),
                                                       MediaStyle::TEXT_SECONDARY, ALIGN_LEFT,
                                                       ALIGN_TOP, glm::ivec2 {0, 0});
    mProgressMessage->setLineSpacing(1.35f);
    mProgressHint = MediaStyle::makeText(MediaStyle::font(0.024f), MediaStyle::TEXT_DIM,
                                         ALIGN_RIGHT);
    mBackgroundTitle = MediaStyle::makeText(MediaStyle::font(0.025f, FONT_PATH_BOLD),
                                            MediaStyle::TEXT);
    mBackgroundMessage =
        MediaStyle::makeText(MediaStyle::font(0.022f), MediaStyle::TEXT_SECONDARY);
    mOverlayText = MediaStyle::makeText(MediaStyle::font(0.023f), MediaStyle::TEXT, ALIGN_RIGHT);
}

AddonDialogs::~AddonDialogs()
{
    // Questions that were never answered are cancelled, the program waits for them.
    for (auto& open : mOpenRequests) {
        if (!open.request->isAnswered())
            open.request->answer("null");
    }
    if (mTextInputPending)
        *mTextInputPending = false;
}

void AddonDialogs::track(const std::shared_ptr<AddonHost::UiRequest>& request, MediaDialog* dialog)
{
    mOpenRequests.emplace_back(OpenRequest {request, dialog});
}

void AddonDialogs::update(int deltaTime)
{
    // The text input popup has been closed, as this is only updated while it's not shown.
    // Without an answer it was cancelled. Other questions are shown again afterwards, the
    // popup used to block them after it had been answered.
    if (mTextInputPending) {
        const bool cancelled {*mTextInputPending};
        *mTextInputPending = false;
        mTextInputPending.reset();
        const std::function<void()> cancel {mTextInputCancel};
        mTextInputCancel = nullptr;
        if (cancelled && cancel)
            cancel();
    }

    AddonHost& host {AddonHost::getInstance()};
    for (auto it = mOpenRequests.begin(); it != mOpenRequests.end();) {
        const std::shared_ptr<AddonHost::UiRequest>& request {it->request};
        if (!mDialogs.contains(it->dialog)) {
            if (!request->isAnswered())
                request->answer(request->method == "Dialog_Select" ||
                                        request->method == "Dialog_Select_Large" ?
                                    "-1" :
                                    "null");
            it = mOpenRequests.erase(it);
            continue;
        }
        // The program can close its questions, e.g. after a timeout.
        const bool closeAll {host.getCloseGeneration() != request->closeGeneration};
        const bool closeConfirm {request->method == "Dialog_Confirm" &&
                                 host.getConfirmGeneration() != request->confirmGeneration};
        if (closeAll || closeConfirm || request->isAnswered()) {
            it->dialog->close();
            if (!request->isAnswered())
                request->answer(request->method == "Dialog_Confirm" ? "0" : "null");
            it = mOpenRequests.erase(it);
            continue;
        }
        ++it;
    }

    for (const auto& notification : host.takeNotifications()) {
        std::string text {clean(notification.message)};
        const std::string heading {clean(notification.heading)};
        if (!heading.empty() && heading != text)
            text = heading + ": " + text;
        Window::getInstance()->queueInfoPopup(Utils::String::replace(text, "\n", " "), 4000);
    }

    // Only one text input popup at a time.
    if (mTextInputPending)
        return;
    while (std::shared_ptr<AddonHost::UiRequest> request {host.takeUiRequest(mDelegate != nullptr)}) {
        handleRequest(request);
        if (mTextInputPending)
            break;
    }
}

void AddonDialogs::handleRequest(const std::shared_ptr<AddonHost::UiRequest>& request)
{
    const std::string& method {request->method};
    if (method == "Dialog") {
        showMessage(request);
    }
    else if (method == "Dialog_Confirm") {
        showConfirm(request);
    }
    else if (method == "Dialog_Select" || method == "Dialog_Select_Large") {
        showSelect(request);
    }
    else if (method == "Dialog_MultiSelect") {
        showMultiSelect(request);
    }
    else if (method == "Dialog_Text") {
        showText(request->title, request->message);
    }
    else if (method == "Keyboard") {
        // A search shows what it finds while the text is typed.
        const std::string searchAddress {mDelegate ? mDelegate->addonSearchAddress() : ""};
        Delegate* delegate {mDelegate};
        openTextInput(
            clean(request->title).empty() ? "Elementum" : clean(request->title), request->text,
            [request, delegate, searchAddress](const std::string& value) {
                if (!searchAddress.empty())
                    delegate->addonSearchPreviewEnd(false, value);
                request->answer(quote(value));
            },
            [request, delegate, searchAddress] {
                if (!searchAddress.empty())
                    delegate->addonSearchPreviewEnd(true, "");
                request->answer("null");
            },
            searchAddress.empty() ?
                std::function<void(const std::string&)>() :
                [delegate, searchAddress](const std::string& text) {
                    delegate->addonSearchPreview(searchAddress, text);
                },
            searchAddress.empty() ?
                std::function<void(int)>() :
                [delegate](int deltaTime) { delegate->addonSearchUpdate(deltaTime); });
    }
    else if (method == "Dialog_Browse_Single") {
        // Types 0 and 3 are folders, the others files.
        const bool folders {request->browseType == 0 || request->browseType == 3};
        openBrowser(clean(request->title), folders, request->mask,
                    [request](const std::string& path) {
                        request->answer(path.empty() ? "null" : quote(path));
                    });
    }
    else if (method == "DialogInsert") {
        showInsert(request);
    }
    else if (method == "AddonSettings") {
        openSettings(request->addonId);
        request->answer("null");
    }
    else if (method == "Update") {
        if (mDelegate)
            mDelegate->addonNavigate(request->text, false);
        request->answer("null");
    }
    else if (method == "Refresh") {
        if (mDelegate)
            mDelegate->addonRefresh();
        request->answer("null");
    }
    else if (method == "Player_Open") {
        if (mDelegate)
            mDelegate->addonPlay(request->text);
        request->answer("null");
    }
    else if (method == "Builtin") {
        runBuiltin(request->text);
        request->answer("null");
    }
    else {
        request->answer("null");
    }
}

void AddonDialogs::showMessage(const std::shared_ptr<AddonHost::UiRequest>& request)
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(
        clean(request->title).empty() ? "Elementum" : clean(request->title), 0.50f))};
    dialog->setMessage(clean(request->message));
    MediaDialog::Option ok;
    ok.label = "OK";
    ok.onSelect = [dialog, request] {
        request->answer("1");
        dialog->close();
    };
    dialog->setOptions({ok});
    track(request, dialog);
}

void AddonDialogs::showConfirm(const std::shared_ptr<AddonHost::UiRequest>& request)
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(
        clean(request->title).empty() ? "Elementum" : clean(request->title), 0.50f))};
    dialog->setMessage(clean(request->message));
    MediaDialog::Option yes;
    yes.label = "Yes";
    yes.onSelect = [dialog, request] {
        request->answer("1");
        dialog->close();
    };
    MediaDialog::Option no;
    no.label = "No";
    no.onSelect = [dialog, request] {
        request->answer("0");
        dialog->close();
    };
    dialog->setOptions({yes, no}, request->focused ? 0 : 1);

    // A countdown on the answer that is chosen when the time runs out.
    if (request->timeout > 0 && request->timeout < 1200) {
        auto remaining = std::make_shared<int>(request->timeout * 1000);
        const bool focused {request->focused};
        dialog->setUpdateCallback([dialog, request, remaining, focused, yes, no](int deltaTime) {
            const int before {(*remaining + 999) / 1000};
            *remaining -= deltaTime;
            if (*remaining <= 0) {
                request->answer(focused ? "1" : "0");
                dialog->close();
                return;
            }
            const int seconds {(*remaining + 999) / 1000};
            if (seconds != before) {
                MediaDialog::Option first {yes};
                MediaDialog::Option second {no};
                if (focused)
                    first.label = "Yes (" + std::to_string(seconds) + ")";
                else
                    second.label = "No (" + std::to_string(seconds) + ")";
                const int cursor {dialog->getCursor()};
                dialog->setOptions({first, second}, cursor);
            }
        });
    }
    track(request, dialog);
}

void AddonDialogs::showSelect(const std::shared_ptr<AddonHost::UiRequest>& request)
{
    const bool large {request->method == "Dialog_Select_Large"};
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(
        clean(request->title).empty() ? "Elementum" : clean(request->title),
        large ? 0.78f : 0.55f))};
    std::vector<MediaDialog::Option> options;
    for (size_t i {0}; i < request->items.size(); ++i) {
        MediaDialog::Option option;
        // Large lists have two lines per item, separated by a line break.
        const std::vector<std::string> lines {
            Utils::String::delimitedStringToVector(request->items[i], "\n")};
        option.label = clean(lines.empty() ? request->items[i] : lines.front());
        if (large && lines.size() > 1)
            option.detail = clean(lines[1]);
        const int index {static_cast<int>(i)};
        option.onSelect = [dialog, request, index] {
            request->answer(std::to_string(index));
            dialog->close();
        };
        options.emplace_back(option);
    }
    if (options.empty()) {
        request->answer("-1");
        dialog->close();
        return;
    }
    dialog->setOptions(options, std::max(0, request->preselect));
    if (request->autoclose > 0) {
        auto remaining = std::make_shared<int>(request->autoclose);
        dialog->setUpdateCallback([dialog, request, remaining](int deltaTime) {
            *remaining -= deltaTime;
            if (*remaining <= 0) {
                request->answer("-1");
                dialog->close();
            }
        });
    }
    track(request, dialog);
}

void AddonDialogs::showMultiSelect(const std::shared_ptr<AddonHost::UiRequest>& request)
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(
        clean(request->title).empty() ? "Elementum" : clean(request->title), 0.55f))};
    auto selected = std::make_shared<std::set<int>>(request->preselected.cbegin(),
                                                    request->preselected.cend());
    std::vector<MediaDialog::Option> options;
    MediaDialog::Option done;
    done.label = "Done";
    done.highlight = true;
    done.onSelect = [dialog, request, selected] {
        std::string list;
        for (const int index : *selected)
            list.append((list.empty() ? "" : ",") + std::to_string(index));
        request->answer("[" + list + "]");
        dialog->close();
    };
    options.emplace_back(done);
    for (size_t i {0}; i < request->items.size(); ++i) {
        MediaDialog::Option option;
        option.label = clean(request->items[i]);
        const int index {static_cast<int>(i)};
        option.value = [selected, index] {
            return selected->count(index) != 0 ? std::string("Selected") : std::string("");
        };
        option.onSelect = [selected, index] {
            if (selected->count(index) != 0)
                selected->erase(index);
            else
                selected->insert(index);
        };
        options.emplace_back(option);
    }
    dialog->setSubtitle("A selects or deselects, choose Done when ready");
    dialog->setOptions(options, 1);
    track(request, dialog);
}

void AddonDialogs::showInsert(const std::shared_ptr<AddonHost::UiRequest>& request)
{
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>("Add torrent", 0.50f))};
    MediaDialog::Option link;
    link.label = "Magnet link or address...";
    link.onSelect = [this, dialog, request] {
        openTextInput("MAGNET LINK OR ADDRESS", "", [dialog, request](const std::string& value) {
            if (Utils::String::trim(value).empty())
                return;
            request->answer("{\"type\":\"url\",\"path\":" + quote(Utils::String::trim(value)) +
                            "}");
            dialog->close();
        });
    };
    MediaDialog::Option file;
    file.label = "Torrent file...";
    file.onSelect = [this, dialog, request] {
        openBrowser("Choose torrent file", false, ".torrent",
                    [dialog, request](const std::string& path) {
                        if (path.empty())
                            return;
                        request->answer("{\"type\":\"file\",\"path\":" + quote(path) + "}");
                        dialog->close();
                    });
    };
    dialog->setOptions({link, file});
    track(request, dialog);
}

void AddonDialogs::showText(const std::string& title, const std::string& text)
{
    MediaDialog* dialog {mDialogs.push(
        std::make_unique<MediaDialog>(clean(title).empty() ? "Elementum" : clean(title), 0.72f))};
    dialog->setMessage(stripAddonFormatting(text));
    MediaDialog::Option close;
    close.label = "Close";
    close.onSelect = [dialog] { dialog->close(); };
    dialog->setOptions({close});
}

void AddonDialogs::openTextInput(const std::string& title,
                                 const std::string& value,
                                 const std::function<void(const std::string&)>& okCallback,
                                 const std::function<void()>& cancelCallback,
                                 const std::function<void(const std::string&)>& changeCallback,
                                 const std::function<void(int)>& updateCallback)
{
    auto pending = std::make_shared<bool>(true);
    mTextInputPending = pending;
    mTextInputCancel = cancelCallback;
    auto callback = [pending, okCallback](const std::string& text) {
        *pending = false;
        if (okCallback)
            okCallback(text);
    };
    Window* window {Window::getInstance()};
    const std::string upperTitle {Utils::String::toUpper(title)};
    if (Settings::getInstance()->getBool("VirtualKeyboard")) {
        // A search leaves the lower part of the screen to the keyboard, the results are above it,
        // and it's left without asking.
        auto* popup {new GuiTextEditKeyboardPopup(
            Renderer::getScreenHeight() * 0.2f, upperTitle, value, callback, false, "OK",
            changeCallback ? "" : "SAVE CHANGES?")};
        if (changeCallback) {
            popup->setCompact(0.42f);
            popup->setTextChangedCallback(changeCallback);
            // Up from the top row searches and moves on to the results.
            popup->setNavigateUpCallback([callback](const std::string& text) {
                if (Utils::String::trim(text).empty())
                    return false;
                callback(text);
                return true;
            });
        }
        if (updateCallback)
            popup->setUpdateCallback(updateCallback);
        window->pushGui(popup);
    }
    else {
        auto* popup {new GuiTextEditPopup(upperTitle, value, callback, false, "OK",
                                          changeCallback ? "" : "SAVE CHANGES?")};
        if (changeCallback)
            popup->setTextChangedCallback(changeCallback);
        if (updateCallback)
            popup->setUpdateCallback(updateCallback);
        window->pushGui(popup);
    }
}

void AddonDialogs::openBrowser(const std::string& title,
                               bool folders,
                               const std::string& mask,
                               const std::function<void(const std::string&)>& callback)
{
    struct State {
        std::vector<std::string> paths;
        std::vector<int> cursors;
        bool done {false};
    };
    auto state = std::make_shared<State>();
    MediaDialog* dialog {mDialogs.push(std::make_unique<MediaDialog>(
        title.empty() ? (folders ? "Choose folder" : "Choose file") : title, 0.58f))};
    std::vector<std::string> extensions;
    for (const auto& extension : Utils::String::delimitedStringToVector(mask, "|"))
        extensions.emplace_back(Utils::String::toLower(Utils::String::trim(extension)));

    auto rebuild = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weakRebuild {rebuild};
    *rebuild = [dialog, state, folders, extensions, callback, weakRebuild](int cursor) {
        const std::shared_ptr<std::function<void(int)>> self {weakRebuild.lock()};
        if (!self)
            return;
        std::vector<MediaDialog::Option> options;
        auto enter = [dialog, state, self](const std::string& path) {
            state->cursors.emplace_back(dialog->getCursor());
            state->paths.emplace_back(path);
            (*self)(0);
        };
        if (state->paths.empty()) {
            dialog->setSubtitle("Choose a location");
            for (const auto& root : MediaLibrary::getBrowseRoots()) {
                MediaDialog::Option option;
                option.label = root.name;
                const std::string path {root.path};
                option.value = [path] { return path; };
                option.onSelect = [enter, path] { enter(path); };
                options.emplace_back(option);
            }
        }
        else {
            const std::string current {state->paths.back()};
            dialog->setSubtitle(current);
            if (folders) {
                MediaDialog::Option use;
                use.label = "Use this folder";
                use.highlight = true;
                use.onSelect = [dialog, state, current, callback] {
                    state->done = true;
                    dialog->close();
                    callback(current);
                };
                options.emplace_back(use);
            }
            MediaDialog::Option parent;
            parent.label = "..";
            parent.onSelect = [state, self] {
                state->paths.pop_back();
                const int previous {state->cursors.empty() ? 0 : state->cursors.back()};
                if (!state->cursors.empty())
                    state->cursors.pop_back();
                (*self)(previous);
            };
            options.emplace_back(parent);
            for (const auto& folder : MediaLibrary::listSubdirectories(current)) {
                MediaDialog::Option option;
                option.label = folder.name;
                const std::string path {folder.path};
                option.onSelect = [enter, path] { enter(path); };
                options.emplace_back(option);
            }
            if (!folders) {
                std::vector<std::pair<std::string, std::string>> files;
                std::error_code errorCode;
                for (const auto& entry : std::filesystem::directory_iterator(current, errorCode)) {
                    if (!entry.is_regular_file(errorCode))
                        continue;
                    const std::string name {entry.path().filename().string()};
                    if (name.empty() || name[0] == '.')
                        continue;
                    const std::string extension {
                        Utils::String::toLower(entry.path().extension().string())};
                    if (!extensions.empty() && std::find(extensions.cbegin(), extensions.cend(),
                                                         extension) == extensions.cend())
                        continue;
                    files.emplace_back(name, entry.path().string());
                }
                std::sort(files.begin(), files.end(),
                          [](const auto& a, const auto& b) { return MediaLibrary::naturalLess(
                                                                 a.first, b.first); });
                for (const auto& [name, path] : files) {
                    MediaDialog::Option option;
                    option.label = name;
                    option.highlight = true;
                    const std::string file {path};
                    option.onSelect = [dialog, state, file, callback] {
                        state->done = true;
                        dialog->close();
                        callback(file);
                    };
                    options.emplace_back(option);
                }
            }
        }
        dialog->setOptions(options, cursor);
    };
    dialog->setBackHandler([state, rebuild] {
        if (state->paths.empty())
            return false;
        state->paths.pop_back();
        const int cursor {state->cursors.empty() ? 0 : state->cursors.back()};
        if (!state->cursors.empty())
            state->cursors.pop_back();
        (*rebuild)(cursor);
        return true;
    });
    // The dialog holds the rebuild function through its options.
    dialog->setCancelCallback([rebuild, state, callback] {
        if (!state->done)
            callback("");
    });
    (*rebuild)(0);
}

void AddonDialogs::runBuiltin(const std::string& command)
{
    std::string name;
    std::vector<std::string> arguments;
    if (!parseCommand(command, name, arguments))
        return;
    std::string lower {Utils::String::toLower(name)};
    if (Utils::String::startsWith(lower, "xbmc."))
        lower = lower.substr(5);

    if (lower == "runplugin" && !arguments.empty()) {
        if (mDelegate)
            mDelegate->addonRun(arguments.front());
        else
            AddonHost::getInstance().runAction(arguments.front());
    }
    else if ((lower == "container.update" || lower == "activatewindow") && !arguments.empty()) {
        // For a window, the address is the second argument.
        std::string url;
        for (const auto& argument : arguments) {
            if (AddonHost::isPluginUrl(argument)) {
                url = argument;
                break;
            }
        }
        const bool replace {std::find(arguments.cbegin(), arguments.cend(), "replace") !=
                            arguments.cend()};
        if (!url.empty() && mDelegate)
            mDelegate->addonNavigate(url, replace);
    }
    else if (lower == "container.refresh") {
        if (mDelegate)
            mDelegate->addonRefresh();
    }
    else if (lower == "playmedia" && !arguments.empty()) {
        if (mDelegate)
            mDelegate->addonPlay(arguments.front());
    }
    else if (lower == "runscript" && !arguments.empty()) {
        std::vector<std::string> scriptArguments(arguments.begin() + 1, arguments.end());
        AddonHost::getInstance().runScript(arguments.front(), scriptArguments);
    }
    else if ((lower == "addon.opensettings" || lower == "openaddonsettings") &&
             !arguments.empty()) {
        openSettings(arguments.front());
    }
    else if (lower == "notification" && arguments.size() >= 2) {
        Window::getInstance()->queueInfoPopup(clean(arguments[0]) + ": " + clean(arguments[1]),
                                              4000);
    }
}

//--------------------------------------------------------------------------------------------------
// Settings of the add-ons.
//--------------------------------------------------------------------------------------------------

void AddonDialogs::openSettings(const std::string& addonId)
{
    std::shared_ptr<AddonSettings> settings {AddonStore::getInstance().getSettings(addonId)};
    AddonStore::Addon addon;
    if (!settings || !AddonStore::getInstance().getAddon(addonId, addon)) {
        Window::getInstance()->queueInfoPopup("THE ADD-ON " + addonId + " IS NOT INSTALLED",
                                              4000);
        return;
    }
    // Values changed by the add-on itself are read again.
    settings->reloadValues();
    AddonHost::getInstance().setSettingsOpen(true);

    MediaDialog* dialog {
        mDialogs.push(std::make_unique<MediaDialog>(addon.name + " settings", 0.50f))};
    auto changed = std::make_shared<bool>(false);
    std::vector<MediaDialog::Option> options;
    const auto& categories {settings->getCategories()};
    for (size_t i {0}; i < categories.size(); ++i) {
        if (isHiddenCategory(addonId, categories[i]))
            continue;
        bool anyVisible {false};
        for (const auto& setting : categories[i].settings) {
            if (!setting.id.empty() && !isHiddenSetting(addonId, setting.id))
                anyVisible = true;
        }
        if (!anyVisible)
            continue;
        MediaDialog::Option option;
        option.label = settings->translateLabel(categories[i].label);
        const size_t category {i};
        bool* changedFlag {changed.get()};
        option.onSelect = [this, addonId, category, changedFlag, changed] {
            openSettingsCategory(addonId, category, changedFlag);
        };
        options.emplace_back(option);
    }

    MediaDialog::Option defaults;
    defaults.label = "Reset to defaults";
    defaults.dimmed = true;
    defaults.onSelect = [this, settings, changed, addon] {
        MediaDialog* confirm {mDialogs.push(std::make_unique<MediaDialog>("Reset", 0.45f))};
        confirm->setMessage("All settings of " + addon.name + " are set to their defaults.");
        MediaDialog::Option reset;
        reset.label = "Reset";
        reset.onSelect = [confirm, settings, changed] {
            settings->resetToDefaults();
            *changed = true;
            confirm->close();
        };
        MediaDialog::Option cancel;
        cancel.label = "Cancel";
        cancel.onSelect = [confirm] { confirm->close(); };
        confirm->setOptions({reset, cancel}, 1);
    };
    options.emplace_back(defaults);
    dialog->setOptions(options);
    dialog->setCancelCallback([addonId, changed] {
        AddonHost::getInstance().setSettingsOpen(false);
        if (*changed && addonId == AddonStore::STREAMING_ADDON)
            AddonHost::getInstance().reloadSettings();
    });
}

void AddonDialogs::openSettingsCategory(const std::string& addonId, size_t category, bool* changed)
{
    std::shared_ptr<AddonSettings> settings {AddonStore::getInstance().getSettings(addonId)};
    if (!settings || category >= settings->getCategories().size())
        return;
    const AddonSettings::Category& definition {settings->getCategories()[category]};
    MediaDialog* dialog {mDialogs.push(
        std::make_unique<MediaDialog>(settings->translateLabel(definition.label), 0.62f))};

    auto rebuild = std::make_shared<std::function<void(const std::string&)>>();
    std::weak_ptr<std::function<void(const std::string&)>> weakRebuild {rebuild};
    *rebuild = [this, dialog, settings, addonId, category, changed,
                weakRebuild](const std::string& focusId) {
        const std::shared_ptr<std::function<void(const std::string&)>> self {
            weakRebuild.lock()};
        if (!self)
            return;
        const auto& list {settings->getCategories()[category].settings};
        std::vector<MediaDialog::Option> options;
        std::vector<std::string> ids;
        int cursor {-1};

        auto setValue = [settings, changed, self](const std::string& id,
                                                  const std::string& value) {
            settings->set(id, value);
            *changed = true;
            (*self)(id);
        };

        for (size_t index {0}; index < list.size(); ++index) {
            const AddonSettings::Definition& setting {list[index]};
            if (setting.type == "sep" || isHiddenSetting(addonId, setting.id) ||
                !settings->isVisible(category, index))
                continue;
            const bool enabled {settings->isEnabled(category, index)};
            const std::string label {settings->translateLabel(setting.label)};
            const std::string id {setting.id};
            const std::string value {settings->get(id)};

            MediaDialog::Option option;
            option.label = (setting.subsetting ? "     " : "") + firstLine(label);
            if (label.find('\n') != std::string::npos)
                option.detail = Utils::String::replace(label.substr(label.find('\n') + 1), "\n",
                                                       " ");
            option.dimmed = !enabled;

            if (setting.type == "lsep") {
                if (label.empty())
                    continue;
                option.label = label;
                option.highlight = true;
                option.dimmed = false;
            }
            else if (setting.type == "bool") {
                option.value = [settings, id] {
                    return settings->get(id) == "true" ? std::string("On") : std::string("Off");
                };
                if (enabled) {
                    option.onAdjust = [settings, id, setValue](int) {
                        setValue(id, settings->get(id) == "true" ? "false" : "true");
                    };
                }
            }
            else if (setting.type == "enum" || setting.type == "labelenum" ||
                     setting.type == "select") {
                const std::vector<std::pair<std::string, std::string>> choices {
                    settings->getChoices(setting)};
                option.value = [settings, id, choices] {
                    const std::string current {settings->get(id)};
                    for (const auto& [key, text] : choices) {
                        if (key == current)
                            return clean(text);
                    }
                    return current;
                };
                if (enabled && !choices.empty()) {
                    option.onAdjust = [settings, id, choices, setValue](int direction) {
                        const std::string current {settings->get(id)};
                        int position {0};
                        for (size_t i {0}; i < choices.size(); ++i) {
                            if (choices[i].first == current)
                                position = static_cast<int>(i);
                        }
                        const int count {static_cast<int>(choices.size())};
                        position = ((position + direction) % count + count) % count;
                        setValue(id, choices[position].first);
                    };
                    option.onSelect = [this, settings, id, label, choices, setValue] {
                        MediaDialog* picker {
                            mDialogs.push(std::make_unique<MediaDialog>(firstLine(label), 0.50f))};
                        std::vector<MediaDialog::Option> items;
                        int selected {0};
                        for (size_t i {0}; i < choices.size(); ++i) {
                            MediaDialog::Option item;
                            item.label = clean(choices[i].second);
                            const std::string key {choices[i].first};
                            if (key == settings->get(id))
                                selected = static_cast<int>(i);
                            item.onSelect = [picker, id, key, setValue] {
                                picker->close();
                                setValue(id, key);
                            };
                            items.emplace_back(item);
                        }
                        picker->setOptions(items, selected);
                    };
                }
            }
            else if (setting.type == "slider") {
                std::vector<std::string> range {
                    Utils::String::delimitedStringToVector(setting.range, ",")};
                double minimum {0.0};
                double step {1.0};
                double maximum {100.0};
                if (range.size() == 3) {
                    minimum = std::atof(range[0].c_str());
                    step = std::atof(range[1].c_str());
                    maximum = std::atof(range[2].c_str());
                }
                else if (range.size() == 2) {
                    minimum = std::atof(range[0].c_str());
                    maximum = std::atof(range[1].c_str());
                }
                const AddonSettings::Definition copy {setting};
                option.value = [settings, id, copy] { return formatSlider(copy, settings->get(id)); };
                if (enabled) {
                    option.onAdjust = [settings, id, minimum, step, maximum, copy,
                                       setValue](int direction) {
                        double current {std::atof(settings->get(id).c_str())};
                        current = std::clamp(current + direction * step, minimum, maximum);
                        char buffer[32];
                        if (copy.option == "float")
                            snprintf(buffer, sizeof(buffer), "%g", current);
                        else
                            snprintf(buffer, sizeof(buffer), "%d", static_cast<int>(current));
                        setValue(id, buffer);
                    };
                    option.onSelect = [this, settings, id, label, minimum, maximum, setValue] {
                        openTextInput(firstLine(label), settings->get(id),
                                      [id, minimum, maximum, setValue](const std::string& text) {
                                          const double number {std::clamp(
                                              std::atof(text.c_str()), minimum, maximum)};
                                          char buffer[32];
                                          snprintf(buffer, sizeof(buffer), "%g", number);
                                          setValue(id, buffer);
                                      });
                    };
                }
            }
            else if (setting.type == "folder" || setting.type == "file" ||
                     setting.type == "audio" || setting.type == "video" ||
                     setting.type == "image" || setting.type == "executable") {
                option.value = [settings, id] { return settings->get(id); };
                if (enabled) {
                    const bool folders {setting.type == "folder"};
                    option.onSelect = [this, id, label, folders, setValue] {
                        openBrowser(firstLine(label), folders, "",
                                    [id, setValue](const std::string& path) {
                                        if (!path.empty())
                                            setValue(id, path);
                                    });
                    };
                    // X restores the default, e.g. the folder for the torrent files, as an
                    // empty path would be the home folder.
                    const std::string defaultValue {setting.defaultValue};
                    option.onRemove = [id, setValue, defaultValue] { setValue(id, defaultValue); };
                }
            }
            else if (setting.type == "action") {
                const std::string action {setting.action};
                if (enabled && !action.empty()) {
                    const bool closeAfter {setting.option == "close"};
                    option.onSelect = [this, dialog, action, closeAfter] {
                        if (closeAfter)
                            dialog->close();
                        runBuiltin(action);
                    };
                }
            }
            else {
                // Text and numbers. Texts that can't be changed and have no value are notes.
                if (!enabled && value.empty() && setting.type == "text") {
                    option.dimmed = true;
                }
                else {
                    const bool hidden {setting.option == "hidden"};
                    option.value = [settings, id, hidden] {
                        const std::string current {settings->get(id)};
                        return hidden ? maskText(current) : current;
                    };
                    if (enabled) {
                        const bool numeric {setting.type == "number"};
                        option.onSelect = [this, settings, id, label, numeric, setValue] {
                            openTextInput(firstLine(label), settings->get(id),
                                          [id, numeric, setValue](const std::string& text) {
                                              const std::string trimmed {
                                                  Utils::String::trim(text)};
                                              setValue(id, numeric && !trimmed.empty() ?
                                                               std::to_string(std::atoi(
                                                                   trimmed.c_str())) :
                                                               trimmed);
                                          });
                        };
                        option.onRemove = [id, setValue] { setValue(id, ""); };
                    }
                }
            }
            if (!id.empty() && id == focusId)
                cursor = static_cast<int>(options.size());
            ids.emplace_back(setting.type == "lsep" ? "" : id);
            options.emplace_back(option);
        }

        // Headings without any settings below them are left out.
        std::vector<MediaDialog::Option> filtered;
        int filteredCursor {0};
        for (size_t i {0}; i < options.size(); ++i) {
            const bool heading {options[i].highlight && !options[i].onSelect &&
                                !options[i].onAdjust && ids[i].empty()};
            if (heading && (i + 1 >= options.size() ||
                            (options[i + 1].highlight && ids[i + 1].empty())))
                continue;
            if (static_cast<int>(i) == cursor)
                filteredCursor = static_cast<int>(filtered.size());
            filtered.emplace_back(options[i]);
        }
        if (cursor < 0) {
            // The first setting rather than a heading.
            for (size_t i {0}; i < filtered.size(); ++i) {
                if (filtered[i].onSelect || filtered[i].onAdjust) {
                    filteredCursor = static_cast<int>(i);
                    break;
                }
            }
            if (!focusId.empty())
                filteredCursor = std::min(dialog->getCursor(),
                                          std::max(0, static_cast<int>(filtered.size()) - 1));
        }
        dialog->setOptions(filtered, filteredCursor);
    };
    // The options hold the rebuild function, the dialog keeps it alive.
    dialog->setCancelCallback([rebuild] {});
    (*rebuild)("");
}

//--------------------------------------------------------------------------------------------------
// Progress and status.
//--------------------------------------------------------------------------------------------------

bool AddonDialogs::hasModalProgress() const
{
    for (const auto& dialog : AddonHost::getInstance().getProgressDialogs()) {
        if (!dialog.background)
            return true;
    }
    return false;
}

bool AddonDialogs::input(InputConfig* config, Input input)
{
    bool modal {false};
    for (const auto& dialog : AddonHost::getInstance().getProgressDialogs()) {
        if (dialog.background)
            continue;
        modal = true;
        if (input.value != 0 &&
            (config->isMappedTo("b", input) ||
             (config->getDeviceId() == DEVICE_KEYBOARD && input.id == SDLK_ESCAPE)))
            AddonHost::getInstance().cancelProgressDialog(dialog.id);
    }
    return modal;
}

void AddonDialogs::render(const glm::mat4& trans, bool showBackgroundProgress)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float padding {MediaStyle::height(0.022f)};
    const std::vector<AddonHost::ProgressDialog> dialogs {
        AddonHost::getInstance().getProgressDialogs()};

    // Background progress at the top right, below each other.
    float backgroundY {MediaStyle::height(0.125f)};
    if (showBackgroundProgress) {
        for (const auto& dialog : dialogs) {
            if (!dialog.background)
                continue;
            const float width {std::round(screenWidth * 0.27f)};
            const float height {std::round(MediaStyle::height(0.095f))};
            const float x {screenWidth - width - std::round(screenWidth * 0.02f)};
            MediaStyle::drawRect(trans, x, backgroundY, width, height, 0x101318E8,
                                 MediaStyle::height(0.008f));
            mBackgroundTitle->setSize(width - padding * 2.0f, MediaStyle::height(0.035f));
            MediaStyle::setText(*mBackgroundTitle, clean(dialog.heading));
            mBackgroundTitle->setPosition(x + padding, backgroundY + padding * 0.4f);
            mBackgroundTitle->render(trans);
            mBackgroundMessage->setSize(width - padding * 2.0f, MediaStyle::height(0.030f));
            MediaStyle::setText(*mBackgroundMessage,
                                Utils::String::replace(clean(dialog.message), "\n", "  "));
            mBackgroundMessage->setPosition(x + padding,
                                            backgroundY + padding * 0.4f +
                                                MediaStyle::height(0.035f));
            mBackgroundMessage->render(trans);
            const float barHeight {std::max(3.0f, std::round(MediaStyle::height(0.005f)))};
            MediaStyle::drawRect(trans, x + padding, backgroundY + height - padding * 0.6f,
                                 width - padding * 2.0f, barHeight, 0xFFFFFF30);
            MediaStyle::drawRect(trans, x + padding, backgroundY + height - padding * 0.6f,
                                 (width - padding * 2.0f) * std::clamp(dialog.percent, 0, 100) /
                                     100.0f,
                                 barHeight, MediaStyle::ACCENT);
            backgroundY += height + MediaStyle::height(0.012f);
        }
    }

    // A modal progress dialog in the middle, only the newest one.
    const AddonHost::ProgressDialog* modal {nullptr};
    for (const auto& dialog : dialogs) {
        if (!dialog.background)
            modal = &dialog;
    }
    if (modal == nullptr)
        return;
    renderProgressPanel(trans, clean(modal->heading).empty() ? "Elementum" : clean(modal->heading),
                        clean(modal->message), modal->percent,
                        modal->canceled ? "Cancelling..." :
                                          std::to_string(modal->percent) + "%     Back to cancel");
}

void AddonDialogs::renderProgressPanel(const glm::mat4& trans,
                                       const std::string& heading,
                                       const std::string& message,
                                       int percent,
                                       const std::string& hint)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float padding {MediaStyle::height(0.022f)};
    MediaStyle::drawRect(trans, 0.0f, 0.0f, screenWidth, screenHeight, MediaStyle::DIALOG_SHADE);
    const float width {std::round(screenWidth * 0.48f)};
    const float messageHeight {MediaStyle::height(0.15f)};
    const float height {MediaStyle::height(0.075f) + messageHeight + MediaStyle::height(0.09f)};
    const float x {std::round((screenWidth - width) / 2.0f)};
    const float y {std::round((screenHeight - height) / 2.0f)};
    const float radius {MediaStyle::height(0.012f)};
    MediaStyle::drawRect(trans, x, y, width, height, MediaStyle::PANEL, radius);
    MediaStyle::drawRect(trans, x, y + MediaStyle::height(0.075f), width,
                         std::max(1.0f, height * 0.004f), MediaStyle::ACCENT);

    mProgressTitle->setSize(width - padding * 2.0f, MediaStyle::height(0.075f));
    MediaStyle::setText(*mProgressTitle, heading);
    mProgressTitle->setPosition(x + padding, y);
    mProgressTitle->render(trans);

    mProgressMessage->setSize(width - padding * 2.0f, messageHeight);
    MediaStyle::setText(*mProgressMessage, message);
    mProgressMessage->setPosition(x + padding, y + MediaStyle::height(0.075f) + padding * 0.6f);
    mProgressMessage->render(trans);

    const float barY {y + height - MediaStyle::height(0.075f)};
    const float barHeight {std::max(4.0f, std::round(MediaStyle::height(0.008f)))};
    MediaStyle::drawRect(trans, x + padding, barY, width - padding * 2.0f, barHeight, 0xFFFFFF30,
                         barHeight / 2.0f);
    if (percent >= 0) {
        MediaStyle::drawRect(trans, x + padding, barY,
                             (width - padding * 2.0f) * std::clamp(percent, 0, 100) / 100.0f,
                             barHeight, MediaStyle::ACCENT, barHeight / 2.0f);
    }
    mProgressHint->setSize(width - padding * 2.0f, MediaStyle::height(0.04f));
    MediaStyle::setText(*mProgressHint, hint);
    mProgressHint->setPosition(x + padding, barY + barHeight + padding * 0.3f);
    mProgressHint->render(trans);
}

void AddonDialogs::renderOverlay(const glm::mat4& trans)
{
    const AddonHost::Overlay overlay {AddonHost::getInstance().getOverlay()};
    if (!overlay.visible || overlay.text.empty())
        return;
    // A single line at the top right: the progress and the speeds. The file name of the last
    // line is left out, the title of the video is shown elsewhere.
    std::vector<std::string> lines;
    for (const auto& line : Utils::String::delimitedStringToVector(clean(overlay.text), "\n")) {
        const std::string trimmed {Utils::String::trim(line)};
        if (!trimmed.empty())
            lines.emplace_back(trimmed);
    }
    if (lines.size() > 2)
        lines.resize(2);
    std::string text;
    for (const auto& line : lines)
        text.append((text.empty() ? "" : "   |   ") + line);
    if (text.empty())
        return;
    const float screenWidth {Renderer::getScreenWidth()};
    const float padding {std::round(MediaStyle::width(0.008f))};
    const float maxWidth {std::round(screenWidth * 0.60f)};
    MediaStyle::setText(*mOverlayText, text);
    const float textWidth {
        std::min(mOverlayText->getFont()->sizeText(text).x, maxWidth - padding * 2.0f)};
    const float width {textWidth + padding * 2.0f};
    const float height {std::round(MediaStyle::height(0.045f))};
    const float x {screenWidth - width - std::round(screenWidth * 0.005f)};
    const float y {std::round(MediaStyle::height(0.018f))};
    MediaStyle::drawRect(trans, x, y, width, height, 0x000000B4, MediaStyle::height(0.008f));
    mOverlayText->setSize(textWidth, height);
    mOverlayText->setPosition(x + padding, y);
    mOverlayText->render(trans);
}
