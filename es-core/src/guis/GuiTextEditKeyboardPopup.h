//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiTextEditKeyboardPopup.h
//
//  Text edit popup with a virtual keyboard.
//  Has a default mode and a complex mode, both with various options passed as arguments.
//

#ifndef ES_CORE_GUIS_GUI_TEXT_EDIT_KEYBOARD_POPUP_H
#define ES_CORE_GUIS_GUI_TEXT_EDIT_KEYBOARD_POPUP_H

#include "GuiComponent.h"
#include "components/BackgroundComponent.h"
#include "components/ButtonComponent.h"
#include "components/ComponentGrid.h"
#include "components/TextComponent.h"
#include "components/TextEditComponent.h"
#include "utils/LocalizationUtil.h"

class GuiTextEditKeyboardPopup : public GuiComponent
{
public:
    GuiTextEditKeyboardPopup(const float verticalPosition,
                             const std::string& title,
                             const std::string& initValue,
                             const std::function<void(const std::string&)>& okCallback,
                             bool multiLine,
                             const std::string& acceptBtnHelpText = _("OK"),
                             const std::string& saveConfirmationText = _("SAVE CHANGES?"),
                             const std::string& infoString = "",
                             const std::string& defaultValue = "",
                             const std::string& loadBtnHelpText = _("LOAD DEFAULT"),
                             const std::string& clearBtnHelpText = _("CLEAR"),
                             const std::string& cancelBtnHelpText = _("DISCARD CHANGES"));

    // A smaller keyboard at the bottom of the screen, with the given part of its height, to
    // leave room for what's shown behind it, e.g. the results of a search.
    void setCompact(float heightFraction);

    void onSizeChanged() override;
    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;

    // Called whenever the text changes while the popup is open.
    // Called every frame, e.g. to keep the screen below up to date.
    void setUpdateCallback(const std::function<void(int)>& callback) { mUpdateCallback = callback; }
    void setTextChangedCallback(const std::function<void(const std::string&)>& callback)
    {
        mTextChangedCallback = callback;
    }
    // Component shown inside the popup above the title, e.g. live search results. The popup
    // background is extended upwards by the given height to make room for it. Only the
    // topmost GUI is rendered by Window, so it has to be rendered from here.
    void setPreviewComponent(GuiComponent* component,
                             float height,
                             bool ownBackground = false);

    // Called when pressing up on the top row of the keyboard. If it returns true the popup
    // is closed, e.g. to move to the search results.
    void setNavigateUpCallback(const std::function<bool(const std::string&)>& callback)
    {
        mNavigateUpCallback = callback;
    }

    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    class KeyboardButton
    {
    public:
        std::shared_ptr<ButtonComponent> button;
        const std::string key;
        const std::string shiftedKey;
        const std::string altedKey;
        const std::string altshiftedKey;
        KeyboardButton(const std::shared_ptr<ButtonComponent> b,
                       const std::string& k,
                       const std::string& sk,
                       const std::string& ak,
                       const std::string& ask)
            : button {b}
            , key {k}
            , shiftedKey {sk}
            , altedKey {ak}
            , altshiftedKey {ask} {};
    };

    void updateDeleteRepeat(int deltaTime);
    void updateNavigationRepeat(int deltaTime);

    void shiftKeys();
    void altKeys();
    void altShiftKeys();

    std::shared_ptr<ButtonComponent> makeButton(const std::string& key,
                                                const std::string& shiftedKey,
                                                const std::string& altedKey,
                                                const std::string& altshiftedKey);
    std::vector<KeyboardButton> mKeyboardButtons;

    std::shared_ptr<ButtonComponent> mShiftButton;
    std::shared_ptr<ButtonComponent> mAltButton;

    Renderer* mRenderer;
    BackgroundComponent mBackground;
    ComponentGrid mGrid;

    std::shared_ptr<TextComponent> mTitle;
    std::shared_ptr<TextComponent> mInfoString;
    std::shared_ptr<TextComponent> mDefaultValue;
    std::shared_ptr<TextEditComponent> mText;
    std::shared_ptr<ComponentGrid> mKeyboardGrid;

    std::string mInitValue;
    std::string mAcceptBtnHelpText;
    std::string mSaveConfirmationText;
    std::string mLoadBtnHelpText;
    std::string mClearBtnHelpText;
    std::string mCancelBtnHelpText;

    std::function<void(const std::string&)> mOkCallback;
    std::function<void(int)> mUpdateCallback;
    bool mCompact {false};
    std::function<void(const std::string&)> mTextChangedCallback;
    GuiComponent* mPreviewComponent {nullptr};
    float mPreviewHeight {0.0f};
    bool mPreviewOwnBackground {false};
    std::function<bool(const std::string&)> mNavigateUpCallback;

    void layoutPreview();
    bool isAtTopOfKeyboard();
    std::string mLastChangedText;

    bool mMultiLine;
    bool mComplexMode;
    bool mDeleteRepeat;
    bool mShift;
    bool mAlt;

    float mVerticalPosition;

    int mHorizontalKeyCount;
    int mDeleteRepeatTimer;
    int mNavigationRepeatTimer;
    int mNavigationRepeatDirX;
    int mNavigationRepeatDirY;
};

#endif // ES_CORE_GUIS_GUI_TEXT_EDIT_KEYBOARD_POPUP_H
