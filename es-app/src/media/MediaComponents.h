//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaComponents.h
//
//  Building blocks for the media screens: the shared visual style, a scrolling list,
//  dialogs that are drawn on top of a fullscreen GUI, and the background that is taken
//  from the currently selected theme.
//

#ifndef ES_APP_MEDIA_MEDIA_COMPONENTS_H
#define ES_APP_MEDIA_MEDIA_COMPONENTS_H

#include "GuiComponent.h"
#include "components/ImageComponent.h"
#include "components/TextComponent.h"
#include "resources/Font.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class ThemeData;

namespace MediaStyle
{
    // clang-format off
    constexpr unsigned int SIDEBAR        {0x000000FF};
    constexpr unsigned int ACCENT         {0x2E8FD8FF};
    constexpr unsigned int TEXT           {0xF2F2F2FF};
    constexpr unsigned int TEXT_SECONDARY {0xB8B8B8FF};
    constexpr unsigned int TEXT_DIM       {0x7D7D7DFF};
    constexpr unsigned int PANEL          {0x141416F4};
    constexpr unsigned int PANEL_HEADER   {0x0A0A0BFF};
    constexpr unsigned int CONTENT_SHADE  {0x0000009E};
    constexpr unsigned int ROW_UNFOCUSED  {0xFFFFFF22};
    constexpr unsigned int DIALOG_SHADE   {0x000000B4};
    // clang-format on

    // Sizes relative to the screen.
    float width(float fraction);
    float height(float fraction);
    std::shared_ptr<Font> font(float heightFraction, const std::string& path = FONT_PATH_REGULAR);

    // Current time of day, e.g. "21:47".
    std::string clockText();
    // Time of day after the given number of seconds, used for "ends at".
    std::string clockTextAfter(double seconds);

    void drawRect(const glm::mat4& trans,
                  float x,
                  float y,
                  float w,
                  float h,
                  unsigned int color,
                  float cornerRadius = 0.0f);
    void drawGradient(const glm::mat4& trans,
                      float x,
                      float y,
                      float w,
                      float h,
                      unsigned int colorTop,
                      unsigned int colorBottom);

    // Updates the text only if it has changed, as building the text is fairly expensive.
    void setText(TextComponent& component, const std::string& text);

    std::unique_ptr<TextComponent> makeText(const std::shared_ptr<Font>& font,
                                            unsigned int color,
                                            Alignment alignment = ALIGN_LEFT);
} // namespace MediaStyle

// Vertical list with a highlighted cursor row, a label to the left and an optional value
// to the right. Holding up or down scrolls continuously.
class MediaListView : public GuiComponent
{
public:
    struct Item {
        std::string label;
        std::string value;
        float progress {-1.0f}; // Shown as a small bar if 0.0 to 1.0.
        bool highlight {false}; // Label drawn using the accent color.
        bool dimmed {false};
        std::string icon;       // Optional image shown in front of the label.
        std::string detail;     // Optional second line below the label.
        std::string badge;      // Optional text after the label in its own color, e.g. a rating.
        unsigned int badgeColor {0xFFD34AFF};
        // Optional text after the badge, e.g. a sign that there is a trailer. The short one is
        // used if the label would have to be shortened for the long one.
        std::string tag;
        std::string tagShort;
        unsigned int tagColor {0xB8B8B8FF};
        unsigned int valueColor {0}; // Color of the value, 0 for the normal color.
    };

    MediaListView();

    void setFonts(const std::shared_ptr<Font>& labelFont, const std::shared_ptr<Font>& valueFont);
    void setRowHeight(float height);
    // Rows are taller when the items have a second line.
    float getRowHeight() const { return mHasDetails ? std::round(mRowHeight * 1.6f) : mRowHeight; }

    void setItems(const std::vector<Item>& items, int cursor = 0);
    void setItem(int index, const Item& item);
    const std::vector<Item>& getItems() const { return mItems; }
    int size() const { return static_cast<int>(mItems.size()); }

    int getCursor() const { return mCursor; }
    // The tag of the cursor row can be selected on its own (right from the label). Moving the
    // cursor unselects it.
    bool cursorHasTag() const { return mCursor >= 0 && mCursor < size() && !mItems[mCursor].tag.empty(); }
    bool isTagFocused() const { return mTagFocused; }
    void setTagFocused(bool focused)
    {
        if (focused != mTagFocused) {
            mTagFocused = focused;
            mRowsDirty = true;
        }
    }
    void setCursor(int cursor);
    void setFocused(bool focused) { mFocused = focused; }
    bool isFocused() const { return mFocused; }
    void setCursorChangedCallback(const std::function<void(int)>& callback)
    {
        mCursorChangedCallback = callback;
    }
    int getVisibleRows() const;
    void stopScrolling() { mScrollDirection = 0; }
    // True while up or down is held or was just released. Used to ignore left and right
    // pressed by accident when moving diagonally on a directional pad or thumbstick.
    bool isVerticalInputActive() const;

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;
    void onSizeChanged() override { mRowsDirty = true; }

private:
    void moveCursor(int amount, bool wrap);
    void updateRows();
    bool hasIcons() const;
    void updateHasIcons();
    ImageComponent* getIcon(const std::string& path);

    std::vector<Item> mItems;
    std::vector<std::unique_ptr<TextComponent>> mLabels;
    std::vector<std::unique_ptr<TextComponent>> mValues;
    std::vector<std::unique_ptr<TextComponent>> mDetails;
    std::vector<std::unique_ptr<TextComponent>> mBadges;
    std::vector<std::unique_ptr<TextComponent>> mTags;
    bool mTagFocused {false};
    std::shared_ptr<Font> mLabelFont;
    std::shared_ptr<Font> mValueFont;
    std::map<std::string, std::unique_ptr<ImageComponent>> mIcons;
    std::function<void(int)> mCursorChangedCallback;

    float mRowHeight;
    int mCursor;
    int mFirstVisible;
    bool mFocused;
    bool mRowsDirty;
    bool mHasIcons;
    bool mHasDetails;

    int mScrollDirection;
    unsigned int mLastVerticalInput;
    int mScrollTimer;
    int mScrollCount;
};

// Grid of posters with the titles below them, used for movies and series.
class MediaGridView : public GuiComponent
{
public:
    struct Item {
        std::string label;
        std::string image; // Local file, shown when it exists.
        std::string icon;  // Shown instead while there's no image, e.g. for menus.
        bool dimmed {false};
        bool watched {false};
        float progress {-1.0f};
    };

    MediaGridView();

    void setItems(const std::vector<Item>& items, int cursor = 0);
    // Updates the image of an item once it has been loaded.
    void setImage(int index, const std::string& image);
    const std::vector<Item>& getItems() const { return mItems; }
    int size() const { return static_cast<int>(mItems.size()); }
    int getCursor() const { return mCursor; }
    void setCursor(int cursor);
    void setFocused(bool focused) { mFocused = focused; }
    void setCursorChangedCallback(const std::function<void(int)>& callback)
    {
        mCursorChangedCallback = callback;
    }
    // The items that are currently shown, to load their images first.
    void getVisibleRange(int& first, int& last) const;
    int getColumns() const { return mColumns; }
    // A fixed number of columns, 0 chooses it from the width.
    void setColumns(int columns);
    void stopScrolling() { mScrollDirection = 0; }
    bool isVerticalInputActive() const;

    // Left on the first column isn't handled, so the caller can move the focus.
    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;
    void onSizeChanged() override { updateLayout(); }

private:
    void updateLayout();
    void move(int amount);
    ImageComponent* getImage(const std::string& path, bool allowLoad);

    std::vector<Item> mItems;
    std::map<std::string, std::unique_ptr<ImageComponent>> mImages;
    std::map<std::string, std::unique_ptr<ImageComponent>> mIcons;
    std::unique_ptr<TextComponent> mLabel;
    std::unique_ptr<TextComponent> mPlaceholder;
    std::function<void(int)> mCursorChangedCallback;

    int mColumns;
    int mFixedColumns;
    int mVisibleRows;
    float mCellWidth;
    float mCellHeight;
    float mPosterHeight;
    int mCursor;
    int mFirstRow;
    bool mFocused;
    int mLoadsThisFrame;

    int mScrollDirection;
    int mScrollTimer;
    int mScrollCount;
    unsigned int mLastVerticalInput;
};

// A single row of posters that scrolls sideways. The selected poster stays in a fixed slot,
// or isn't drawn at all if it's shown elsewhere, with the following posters next to it.
// Optionally the titles are shown in a row below the posters.
class MediaStripView : public GuiComponent
{
public:
    using Item = MediaGridView::Item;

    MediaStripView();

    // The size of the posters, the slot of the selected one and whether it's left out.
    void setLayout(float posterWidth,
                   float posterHeight,
                   float spacing,
                   int focusSlot,
                   bool skipSelected,
                   bool showLabels);
    void setItems(const std::vector<Item>& items, int cursor = 0);
    void setImage(int index, const std::string& image);
    const std::vector<Item>& getItems() const { return mItems; }
    int size() const { return static_cast<int>(mItems.size()); }
    int getCursor() const { return mCursor; }
    void setCursor(int cursor);
    void setFocused(bool focused) { mFocused = focused; }
    void setCursorChangedCallback(const std::function<void(int)>& callback)
    {
        mCursorChangedCallback = callback;
    }
    void getVisibleRange(int& first, int& last) const;
    void stopScrolling() { mScrollDirection = 0; }

    // Up and down aren't handled, so the caller can use them.
    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;

private:
    void move(int amount);
    int firstShown() const;
    int slotCount() const;
    ImageComponent* getImage(const std::string& path);

    std::vector<Item> mItems;
    std::map<std::string, std::unique_ptr<ImageComponent>> mImages;
    std::map<std::string, std::unique_ptr<ImageComponent>> mIcons;
    std::unique_ptr<TextComponent> mLabel;
    std::unique_ptr<TextComponent> mPlaceholder;
    std::function<void(int)> mCursorChangedCallback;

    float mPosterWidth;
    float mPosterHeight;
    float mSpacing;
    int mFocusSlot;
    bool mSkipSelected;
    bool mShowLabels;
    int mCursor;
    bool mFocused;
    int mLoadsThisFrame;
    int mScrollDirection;
    int mScrollTimer;
    int mScrollCount;
};

// Draws an image inside a box: posters of about the same shape fill it, other pictures keep
// their shape and are centered.
void renderImageInBox(const glm::mat4& trans,
                      ImageComponent& image,
                      float x,
                      float y,
                      float width,
                      float height,
                      unsigned int colorShift = 0xFFFFFFFF);

// Dialog with a title and a list of options, drawn centered on top of a fullscreen GUI.
// Options can be selected and optionally adjusted using left and right.
class MediaDialog : public GuiComponent
{
public:
    struct Option {
        std::string label;
        std::function<std::string()> value;
        std::function<void()> onSelect;
        std::function<void(int)> onAdjust;
        // Called when pressing X on the option, e.g. to delete an entry.
        std::function<void()> onRemove;
        bool highlight {false};
        bool dimmed {false};
        std::string detail; // Optional second line.
        // Optional color of the value, e.g. for a choice that isn't available. 0 for the
        // normal color.
        std::function<unsigned int()> valueColor;
    };

    MediaDialog(const std::string& title, float widthFraction = 0.42f);

    void setSubtitle(const std::string& subtitle);
    void setOptions(const std::vector<Option>& options, int cursor = 0);
    int getCursor() const { return mList.getCursor(); }
    void setCursor(int cursor) { mList.setCursor(cursor); }
    // Re-reads the option values.
    void refresh();
    void setTitle(const std::string& title);
    // A text shown above the options, which can be scrolled using up and down if it's long.
    void setMessage(const std::string& message);
    // Called every frame with the elapsed time, e.g. for dialogs that close by themselves.
    void setUpdateCallback(const std::function<void(int)>& callback) { mUpdateCallback = callback; }

    // Called when the dialog is dismissed using the back button.
    void setCancelCallback(const std::function<void()>& callback) { mCancelCallback = callback; }
    // Called first when pressing back, if it returns true the dialog stays open.
    void setBackHandler(const std::function<bool()>& handler) { mBackHandler = handler; }
    void close() { mClosed = true; }
    bool isClosed() const { return mClosed; }

    bool input(InputConfig* config, Input input) override;
    void update(int deltaTime) override;
    void render(const glm::mat4& parentTrans) override;
    std::vector<HelpPrompt> getHelpPrompts() override;

private:
    void updateLayout();
    // The height of the subtitle below the title, 0 without one.
    float subtitleSpace() const;

    MediaListView mList;
    std::unique_ptr<TextComponent> mTitle;
    std::unique_ptr<TextComponent> mSubtitle;
    int mSubtitleLines {1};
    std::unique_ptr<TextComponent> mMessage;
    float mMessageHeight;
    float mMessageScroll;
    std::vector<Option> mOptions;
    std::function<void()> mCancelCallback;
    std::function<bool()> mBackHandler;
    std::function<void(int)> mUpdateCallback;
    float mWidthFraction;
    bool mClosed;
};

// A stack of dialogs, only the topmost one receives input.
class MediaDialogStack
{
public:
    MediaDialog* push(std::unique_ptr<MediaDialog> dialog);
    bool empty() const { return mDialogs.empty(); }
    // True if the dialog is still open.
    bool contains(const MediaDialog* dialog) const;
    MediaDialog* top() const { return mDialogs.empty() ? nullptr : mDialogs.back().get(); }
    void clear() { mDialogs.clear(); }

    bool input(InputConfig* config, Input input);
    void update(int deltaTime);
    void render(const glm::mat4& parentTrans);
    void refresh();
    std::vector<HelpPrompt> getHelpPrompts();

private:
    void removeClosed();

    std::vector<std::unique_ptr<MediaDialog>> mDialogs;
};

// Background made of the static images of the currently selected theme, using the
// favorites collection as it's always available.
class MediaBackground
{
public:
    MediaBackground();
    void render(const glm::mat4& parentTrans);

private:
    void loadElements(const std::shared_ptr<ThemeData>& theme, const std::string& view);

    std::shared_ptr<ThemeData> mTheme;
    std::vector<std::unique_ptr<ImageComponent>> mImages;
};

// Fills the dialog with the languages for downloading subtitles. With defaultOnly set, choosing
// a language sets the default and closes the dialog, otherwise X adds or removes additional
// languages and the dialog is closed using the first option.
void buildSubtitleLanguagePicker(MediaDialog* dialog,
                                 bool defaultOnly,
                                 const std::function<void()>& doneCallback);

#endif // ES_APP_MEDIA_MEDIA_COMPONENTS_H
