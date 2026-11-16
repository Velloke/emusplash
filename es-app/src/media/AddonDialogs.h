//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonDialogs.h
//
//  Shows what the streaming add-on and its search providers ask for: messages, questions,
//  choices, text input, file and folder selection, progress, notifications and the status
//  shown while paused. Also the settings of the add-ons, built from their own definitions.
//  Used by the media screen and the video player, each with its own dialog stack.
//

#ifndef ES_APP_MEDIA_ADDON_DIALOGS_H
#define ES_APP_MEDIA_ADDON_DIALOGS_H

#include "media/AddonHost.h"
#include "media/MediaComponents.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class AddonDialogs
{
public:
    // Requests that change the listing or start the player, handled by the media screen.
    class Delegate
    {
    public:
        virtual ~Delegate() = default;
        // Opens a listing, replacing the current one if replace is set.
        virtual void addonNavigate(const std::string& url, bool replace) = 0;
        // Runs an add-on address without showing its listing.
        virtual void addonRun(const std::string& url) = 0;
        virtual void addonRefresh() = 0;
        // Plays an address of the program, or resolves and plays an add-on address.
        virtual void addonPlay(const std::string& url) = 0;
        // The address of the search that is waiting for the text, empty if it's not one.
        virtual std::string addonSearchAddress() { return ""; }
        // Shows what the text finds, while it's typed.
        virtual void addonSearchPreview(const std::string& address, const std::string& text)
        {
            (void)address;
            (void)text;
        }
        // Called every frame while the text is typed, as the screen below isn't updated then.
        virtual void addonSearchUpdate(int deltaTime) { (void)deltaTime; }
        // The text was accepted or the search cancelled, the preview is to be removed.
        virtual void addonSearchPreviewEnd(bool cancelled, const std::string& text)
        {
            (void)cancelled;
            (void)text;
        }
    };

    AddonDialogs(MediaDialogStack& dialogs, Delegate* delegate);
    ~AddonDialogs();

    // Shows the waiting requests, to be called every frame while the owner is on top.
    void update(int deltaTime);
    // Progress dialogs, drawn on top of everything else.
    void render(const glm::mat4& trans, bool showBackgroundProgress);
    // The status shown by the program while paused.
    void renderOverlay(const glm::mat4& trans);
    // A modal progress dialog takes all input, back cancels it.
    bool input(InputConfig* config, Input input);
    bool hasModalProgress() const;

    // Runs a command of the media center the add-ons were written for.
    void runBuiltin(const std::string& command);

    // Dialogs that are also used by the media screen.
    void openSettings(const std::string& addonId);
    void openTextInput(const std::string& title,
                       const std::string& value,
                       const std::function<void(const std::string&)>& okCallback,
                       const std::function<void()>& cancelCallback = nullptr,
                       const std::function<void(const std::string&)>& changeCallback = nullptr,
                       const std::function<void(int)>& updateCallback = nullptr);
    // Browses for a folder (empty mask) or a file matching one of the extensions in the mask,
    // e.g. ".torrent". The callback receives an empty string if cancelled.
    void openBrowser(const std::string& title,
                     bool folders,
                     const std::string& mask,
                     const std::function<void(const std::string&)>& callback);
    void showText(const std::string& title, const std::string& text);
    // A progress panel in the middle of the screen, also used for installations.
    void renderProgressPanel(const glm::mat4& trans,
                             const std::string& heading,
                             const std::string& message,
                             int percent,
                             const std::string& hint);

private:
    struct OpenRequest {
        std::shared_ptr<AddonHost::UiRequest> request;
        MediaDialog* dialog;
    };

    void handleRequest(const std::shared_ptr<AddonHost::UiRequest>& request);
    void track(const std::shared_ptr<AddonHost::UiRequest>& request, MediaDialog* dialog);
    void showMessage(const std::shared_ptr<AddonHost::UiRequest>& request);
    void showConfirm(const std::shared_ptr<AddonHost::UiRequest>& request);
    void showSelect(const std::shared_ptr<AddonHost::UiRequest>& request);
    void showMultiSelect(const std::shared_ptr<AddonHost::UiRequest>& request);
    void showInsert(const std::shared_ptr<AddonHost::UiRequest>& request);
    void openSettingsCategory(const std::string& addonId, size_t category, bool* changed);

    MediaDialogStack& mDialogs;
    Delegate* mDelegate;
    std::vector<OpenRequest> mOpenRequests;
    // Text input shown in a popup, cancelled if the popup closes without an answer.
    std::shared_ptr<bool> mTextInputPending;
    std::function<void()> mTextInputCancel;

    std::unique_ptr<TextComponent> mProgressTitle;
    std::unique_ptr<TextComponent> mProgressMessage;
    std::unique_ptr<TextComponent> mProgressHint;
    std::unique_ptr<TextComponent> mBackgroundTitle;
    std::unique_ptr<TextComponent> mBackgroundMessage;
    std::unique_ptr<TextComponent> mOverlayText;
};

#endif // ES_APP_MEDIA_ADDON_DIALOGS_H
