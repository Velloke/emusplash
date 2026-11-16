//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  MediaComponents.cpp
//
//  Building blocks for the media screens: the shared visual style, a scrolling list,
//  dialogs that are drawn on top of a fullscreen GUI, and the background that is taken
//  from the currently selected theme.
//

#include "media/MediaComponents.h"

#include "CollectionSystemsManager.h"
#include "Settings.h"
#include "SystemData.h"
#include "ThemeData.h"
#include "media/SubtitleDownloader.h"
#include "renderers/Renderer.h"
#include "utils/StringUtil.h"

#include <SDL2/SDL_timer.h>

#include <algorithm>
#include <ctime>

#define SCROLL_DELAY_INITIAL 350
#define SCROLL_DELAY_REPEAT 70
#define SCROLL_DELAY_FAST 35
// Left and right are ignored this long after up or down was released.
#define DIAGONAL_GUARD_TIME 250

namespace MediaStyle
{
    float width(float fraction) { return Renderer::getScreenWidth() * fraction; }

    float height(float fraction) { return Renderer::getScreenHeight() * fraction; }

    std::shared_ptr<Font> font(float heightFraction, const std::string& path)
    {
        return Font::get(std::round(height(heightFraction)), path);
    }

    std::string clockText() { return clockTextAfter(0.0); }

    std::string clockTextAfter(double seconds)
    {
        const std::time_t time {std::time(nullptr) + static_cast<std::time_t>(seconds)};
        std::tm local {};
        localtime_r(&time, &local);
        char buffer[16];
        std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
        return buffer;
    }

    void drawRect(const glm::mat4& trans,
                  float x,
                  float y,
                  float w,
                  float h,
                  unsigned int color,
                  float cornerRadius)
    {
        if (w <= 0.0f || h <= 0.0f)
            return;
        // The rectangle is always drawn at the origin of the transform as the rounded corners
        // are calculated relative to the origin.
        Renderer* renderer {Renderer::getInstance()};
        renderer->setMatrix(glm::translate(trans, glm::round(glm::vec3 {x, y, 0.0f})));
        renderer->drawRect(0.0f, 0.0f, w, h, color, color, false, 1.0f, 1.0f,
                           Renderer::BlendFactor::SRC_ALPHA,
                           Renderer::BlendFactor::ONE_MINUS_SRC_ALPHA, cornerRadius);
    }

    void drawGradient(const glm::mat4& trans,
                      float x,
                      float y,
                      float w,
                      float h,
                      unsigned int colorTop,
                      unsigned int colorBottom)
    {
        if (w <= 0.0f || h <= 0.0f)
            return;
        Renderer* renderer {Renderer::getInstance()};
        renderer->setMatrix(glm::translate(trans, glm::round(glm::vec3 {x, y, 0.0f})));
        renderer->drawRect(0.0f, 0.0f, w, h, colorTop, colorBottom, false);
    }

    void setText(TextComponent& component, const std::string& text)
    {
        if (component.getValue() != text)
            component.setText(text);
    }

    std::unique_ptr<TextComponent> makeText(const std::shared_ptr<Font>& font,
                                            unsigned int color,
                                            Alignment alignment)
    {
        auto text = std::make_unique<TextComponent>("", font, color, alignment, ALIGN_CENTER,
                                                    glm::ivec2 {0, 0});
        text->setRemoveLineBreaks(true);
        return text;
    }
} // namespace MediaStyle

//--------------------------------------------------------------------------------------------------
// MediaListView
//--------------------------------------------------------------------------------------------------

MediaListView::MediaListView()
    : mLabelFont {MediaStyle::font(0.034f)}
    , mValueFont {MediaStyle::font(0.028f)}
    , mRowHeight {MediaStyle::height(0.062f)}
    , mCursor {0}
    , mFirstVisible {0}
    , mFocused {true}
    , mRowsDirty {true}
    , mHasIcons {false}
    , mHasDetails {false}
    , mScrollDirection {0}
    , mLastVerticalInput {0}
    , mScrollTimer {0}
    , mScrollCount {0}
{
}

void MediaListView::setFonts(const std::shared_ptr<Font>& labelFont,
                             const std::shared_ptr<Font>& valueFont)
{
    mLabelFont = labelFont;
    mValueFont = valueFont;
    mLabels.clear();
    mValues.clear();
    mDetails.clear();
    mBadges.clear();
    mTags.clear();
    mRowsDirty = true;
}

void MediaListView::setRowHeight(float height)
{
    mRowHeight = height;
    mLabels.clear();
    mValues.clear();
    mDetails.clear();
    mBadges.clear();
    mTags.clear();
    mRowsDirty = true;
}

void MediaListView::setItems(const std::vector<Item>& items, int cursor)
{
    mItems = items;
    updateHasIcons();
    mScrollDirection = 0;
    mCursor = -1;
    setCursor(cursor);
    mRowsDirty = true;
}

void MediaListView::setItem(int index, const Item& item)
{
    if (index < 0 || index >= size())
        return;
    mItems[index] = item;
    updateHasIcons();
    mRowsDirty = true;
}

bool MediaListView::isVerticalInputActive() const
{
    return mScrollDirection != 0 || SDL_GetTicks() - mLastVerticalInput < DIAGONAL_GUARD_TIME;
}

int MediaListView::getVisibleRows() const
{
    return std::max(1, static_cast<int>(mSize.y / getRowHeight()));
}

void MediaListView::setCursor(int cursor)
{
    const int newCursor {mItems.empty() ? 0 : std::clamp(cursor, 0, size() - 1)};
    const bool changed {newCursor != mCursor};
    mCursor = newCursor;
    if (changed)
        mTagFocused = false;

    // Keep the cursor row visible, preferably with some rows of context around it.
    const int visible {getVisibleRows()};
    if (size() <= visible) {
        mFirstVisible = 0;
    }
    else {
        const int margin {std::min(2, (visible - 1) / 2)};
        if (mCursor < mFirstVisible + margin)
            mFirstVisible = mCursor - margin;
        else if (mCursor > mFirstVisible + visible - 1 - margin)
            mFirstVisible = mCursor - visible + 1 + margin;
        mFirstVisible = std::clamp(mFirstVisible, 0, size() - visible);
    }

    mRowsDirty = true;
    if (changed && mCursorChangedCallback)
        mCursorChangedCallback(mCursor);
}

void MediaListView::moveCursor(int amount, bool wrap)
{
    if (mItems.empty())
        return;
    int cursor {mCursor + amount};
    if (wrap && std::abs(amount) == 1) {
        if (cursor < 0)
            cursor = size() - 1;
        else if (cursor >= size())
            cursor = 0;
    }
    setCursor(std::clamp(cursor, 0, size() - 1));
}

bool MediaListView::input(InputConfig* config, Input input)
{
    if (config->isMappedLike("up", input) || config->isMappedLike("down", input)) {
        mLastVerticalInput = SDL_GetTicks();
        if (input.value != 0) {
            const int direction {config->isMappedLike("up", input) ? -1 : 1};
            moveCursor(direction, true);
            mScrollDirection = direction;
            mScrollTimer = SCROLL_DELAY_INITIAL;
            mScrollCount = 0;
        }
        else {
            mScrollDirection = 0;
        }
        return true;
    }
    if (config->isMappedLike("leftshoulder", input) || config->isMappedLike("rightshoulder", input)) {
        if (input.value != 0) {
            const int page {std::max(1, getVisibleRows() - 1)};
            moveCursor(config->isMappedLike("leftshoulder", input) ? -page : page, false);
        }
        return true;
    }
    if (config->isMappedLike("lefttrigger", input) || config->isMappedLike("righttrigger", input)) {
        if (input.value != 0)
            setCursor(config->isMappedLike("lefttrigger", input) ? 0 : size() - 1);
        return true;
    }
    return false;
}

void MediaListView::update(int deltaTime)
{
    if (mScrollDirection != 0) {
        mScrollTimer -= deltaTime;
        while (mScrollTimer <= 0 && mScrollDirection != 0) {
            const int previous {mCursor};
            moveCursor(mScrollDirection, false);
            ++mScrollCount;
            mScrollTimer += mScrollCount > 12 ? SCROLL_DELAY_FAST : SCROLL_DELAY_REPEAT;
            if (mCursor == previous)
                mScrollDirection = 0;
        }
    }
    GuiComponent::update(deltaTime);
}

bool MediaListView::hasIcons() const { return mHasIcons; }

void MediaListView::updateHasIcons()
{
    mHasIcons = false;
    const bool hadDetails {mHasDetails};
    mHasDetails = false;
    for (const auto& item : mItems) {
        if (!item.icon.empty())
            mHasIcons = true;
        if (!item.detail.empty())
            mHasDetails = true;
    }
    if (hadDetails != mHasDetails) {
        mLabels.clear();
        mValues.clear();
        mDetails.clear();
        mBadges.clear();
        mTags.clear();
    }
}

ImageComponent* MediaListView::getIcon(const std::string& path)
{
    auto it {mIcons.find(path)};
    if (it == mIcons.end()) {
        auto icon = std::make_unique<ImageComponent>();
        const float size {std::round(mRowHeight * 0.42f)};
        icon->setResize(size, size);
        icon->setImage(path);
        // Icons of add-on listings can be many, only the recently used ones are kept.
        if (mIcons.size() > 120)
            mIcons.clear();
        it = mIcons.emplace(path, std::move(icon)).first;
    }
    return it->second.get();
}

void MediaListView::updateRows()
{
    const int visible {getVisibleRows()};
    const float rowHeight {getRowHeight()};
    while (static_cast<int>(mLabels.size()) < visible) {
        mLabels.emplace_back(MediaStyle::makeText(mLabelFont, MediaStyle::TEXT, ALIGN_LEFT));
        mValues.emplace_back(
            MediaStyle::makeText(mValueFont, MediaStyle::TEXT_SECONDARY, ALIGN_RIGHT));
        mDetails.emplace_back(
            MediaStyle::makeText(mValueFont, MediaStyle::TEXT_SECONDARY, ALIGN_LEFT));
        mBadges.emplace_back(MediaStyle::makeText(mValueFont, MediaStyle::TEXT, ALIGN_LEFT));
        mTags.emplace_back(MediaStyle::makeText(mValueFont, MediaStyle::TEXT, ALIGN_LEFT));
    }

    const float padding {MediaStyle::width(0.012f)};
    // With a second line, the label takes the upper part of the row.
    const float labelHeight {mHasDetails ? std::round(rowHeight * 0.56f) : rowHeight};
    const float labelOffset {mHasDetails ? std::round(rowHeight * 0.06f) : 0.0f};
    for (int row {0}; row < visible; ++row) {
        const int index {mFirstVisible + row};
        TextComponent& label {*mLabels[row]};
        TextComponent& value {*mValues[row]};
        TextComponent& detail {*mDetails[row]};
        TextComponent& badge {*mBadges[row]};
        TextComponent& tag {*mTags[row]};
        if (index >= size()) {
            MediaStyle::setText(tag, "");
            MediaStyle::setText(label, "");
            MediaStyle::setText(value, "");
            MediaStyle::setText(detail, "");
            MediaStyle::setText(badge, "");
            continue;
        }

        const Item& item {mItems[index]};
        const bool selected {index == mCursor};
        float valueWidth {0.0f};
        if (!item.value.empty()) {
            valueWidth = std::min(mValueFont->sizeText(item.value).x + 1.0f, mSize.x * 0.45f);
        }

        // Room for the icons in front of the labels.
        const float iconSpace {hasIcons() ? std::round(mRowHeight * 0.75f) : 0.0f};
        const float labelWidth {
            std::max(0.0f, mSize.x - padding * 2.0f - iconSpace -
                               (valueWidth > 0.0f ? valueWidth + padding * 1.5f : 0.0f))};
        // The badge and the tag follow the label, which is shortened if they don't fit. The
        // short tag is used if the long one would shorten the label.
        const float gap {std::round(padding * 1.2f)};
        float badgeWidth {0.0f};
        if (!item.badge.empty())
            badgeWidth = std::min(mValueFont->sizeText(item.badge).x + 1.0f, labelWidth * 0.4f);
        const float naturalLabelWidth {mLabelFont->sizeText(item.label).x + 1.0f};
        const float badgeSpace {badgeWidth > 0.0f ? badgeWidth + gap : 0.0f};
        std::string tagText {item.tag};
        float tagWidth {0.0f};
        if (!tagText.empty()) {
            tagWidth = mValueFont->sizeText(tagText).x + 1.0f;
            if (naturalLabelWidth + badgeSpace + gap + tagWidth > labelWidth &&
                !item.tagShort.empty()) {
                tagText = item.tagShort;
                tagWidth = mValueFont->sizeText(tagText).x + 1.0f;
            }
            tagWidth = std::min(tagWidth, labelWidth * 0.3f);
        }
        const float tagSpace {tagWidth > 0.0f ? tagWidth + gap : 0.0f};
        float shownLabelWidth {labelWidth};
        if (badgeWidth > 0.0f || tagWidth > 0.0f) {
            shownLabelWidth =
                std::min(naturalLabelWidth, std::max(0.0f, labelWidth - badgeSpace - tagSpace));
        }
        if (badgeWidth > 0.0f) {
            if (badge.getSize() != glm::vec2 {badgeWidth, labelHeight})
                badge.setSize(badgeWidth, labelHeight);
            badge.setPosition(padding + iconSpace + shownLabelWidth + gap,
                              row * rowHeight + labelOffset);
        }
        if (tagWidth > 0.0f) {
            if (tag.getSize() != glm::vec2 {tagWidth, labelHeight})
                tag.setSize(tagWidth, labelHeight);
            tag.setPosition(padding + iconSpace + shownLabelWidth + badgeSpace + gap,
                            row * rowHeight + labelOffset);
        }
        MediaStyle::setText(tag, tagWidth > 0.0f ? tagText : "");
        // A selected tag has dark text on a light box.
        const unsigned int tagColor {selected && mTagFocused ? 0x10161CFF : item.tagColor};
        if (tagWidth > 0.0f && tag.getColor() != tagColor)
            tag.setColor(tagColor);
        MediaStyle::setText(badge, item.badge);
        if (!item.badge.empty() && badge.getColor() != item.badgeColor)
            badge.setColor(item.badgeColor);
        if (label.getSize() != glm::vec2 {shownLabelWidth, labelHeight})
            label.setSize(shownLabelWidth, labelHeight);
        if (value.getSize() != glm::vec2 {valueWidth, labelHeight})
            value.setSize(valueWidth, labelHeight);
        label.setPosition(padding + iconSpace, row * rowHeight + labelOffset);
        value.setPosition(mSize.x - padding - valueWidth, row * rowHeight + labelOffset);
        if (mHasDetails) {
            const float detailWidth {std::max(0.0f, mSize.x - padding * 2.0f - iconSpace)};
            const float detailHeight {rowHeight - labelHeight - labelOffset};
            if (detail.getSize() != glm::vec2 {detailWidth, detailHeight})
                detail.setSize(detailWidth, detailHeight);
            detail.setPosition(padding + iconSpace, row * rowHeight + labelOffset + labelHeight -
                                                        std::round(rowHeight * 0.04f));
            MediaStyle::setText(detail, item.detail);
        }

        unsigned int labelColor {MediaStyle::TEXT};
        unsigned int valueColor {MediaStyle::TEXT_SECONDARY};
        unsigned int detailColor {MediaStyle::TEXT_DIM};
        if (selected && mFocused) {
            labelColor = 0xFFFFFFFF;
            valueColor = 0xFFFFFFFF;
            detailColor = 0xFFFFFFD0;
        }
        else if (item.highlight) {
            labelColor = MediaStyle::ACCENT;
        }
        else if (item.dimmed) {
            labelColor = MediaStyle::TEXT_DIM;
        }
        if (item.valueColor != 0 && !(selected && mFocused))
            valueColor = item.valueColor;

        MediaStyle::setText(label, item.label);
        MediaStyle::setText(value, item.value);
        if (label.getColor() != labelColor)
            label.setColor(labelColor);
        if (value.getColor() != valueColor)
            value.setColor(valueColor);
        if (detail.getColor() != detailColor)
            detail.setColor(detailColor);
    }
    mRowsDirty = false;
}

void MediaListView::render(const glm::mat4& parentTrans)
{
    if (!isVisible())
        return;
    if (mRowsDirty)
        updateRows();

    const glm::mat4 trans {parentTrans * getTransform()};
    const int visible {getVisibleRows()};
    const float rowHeight {getRowHeight()};
    const float padding {MediaStyle::width(0.012f)};
    const bool scrollbar {size() > visible};
    const float rowWidth {scrollbar ? mSize.x - MediaStyle::width(0.008f) : mSize.x};

    for (int row {0}; row < visible; ++row) {
        const int index {mFirstVisible + row};
        if (index >= size())
            break;
        const Item& item {mItems[index]};
        const float y {row * rowHeight};

        if (index == mCursor) {
            MediaStyle::drawRect(trans, 0.0f, y, rowWidth, rowHeight,
                                 mFocused ? MediaStyle::ACCENT : MediaStyle::ROW_UNFOCUSED,
                                 mRowHeight * 0.08f);
        }

        if (!item.icon.empty()) {
            ImageComponent* icon {getIcon(item.icon)};
            const glm::vec2 iconSize {icon->getSize()};
            icon->setPosition(std::round(padding + (mRowHeight * 0.6f - iconSize.x) / 2.0f),
                              std::round(y + (rowHeight - iconSize.y) / 2.0f));
            const bool selected {index == mCursor && mFocused};
            icon->setColorShift(selected ? 0xFFFFFFFF :
                                           (item.dimmed ? 0xFFFFFF70 : 0xD8D8D8FF));
            icon->render(trans);
        }

        mLabels[row]->render(trans);
        if (!item.badge.empty())
            mBadges[row]->render(trans);
        if (!item.tag.empty()) {
            if (index == mCursor && mTagFocused && mFocused) {
                const TextComponent& tag {*mTags[row]};
                const float margin {std::round(padding * 0.5f)};
                const float boxHeight {std::round(mRowHeight * 0.66f)};
                MediaStyle::drawRect(trans, tag.getPosition().x - margin,
                                     y + std::round((mRowHeight - boxHeight) / 2.0f),
                                     tag.getSize().x + margin * 2.0f, boxHeight, 0xF2F4F6FF,
                                     boxHeight * 0.18f);
            }
            mTags[row]->render(trans);
        }
        mValues[row]->render(trans);
        if (mHasDetails)
            mDetails[row]->render(trans);

        if (item.progress >= 0.0f) {
            const float barWidth {std::min(mSize.x * 0.25f, MediaStyle::width(0.12f))};
            const float barHeight {std::max(2.0f, std::round(mRowHeight * 0.07f))};
            const float barY {y + rowHeight - barHeight * 2.2f};
            const bool onAccent {index == mCursor && mFocused};
            const float barX {padding + (hasIcons() ? std::round(mRowHeight * 0.75f) : 0.0f)};
            MediaStyle::drawRect(trans, barX, barY, barWidth, barHeight, 0xFFFFFF38);
            MediaStyle::drawRect(trans, barX, barY,
                                 barWidth * std::clamp(item.progress, 0.0f, 1.0f), barHeight,
                                 onAccent ? 0xFFFFFFFF : MediaStyle::ACCENT);
        }
    }

    // Scroll position indicator.
    if (scrollbar) {
        const float trackWidth {std::max(2.0f, std::round(MediaStyle::width(0.003f)))};
        const float trackX {mSize.x - trackWidth};
        const float thumbHeight {std::max(mSize.y * visible / size(), rowHeight * 0.5f)};
        const float thumbY {(mSize.y - thumbHeight) * mFirstVisible /
                            static_cast<float>(size() - visible)};
        MediaStyle::drawRect(trans, trackX, 0.0f, trackWidth, mSize.y, 0xFFFFFF1A);
        MediaStyle::drawRect(trans, trackX, thumbY, trackWidth, thumbHeight, 0xFFFFFF80);
    }
}

//--------------------------------------------------------------------------------------------------
// MediaGridView
//--------------------------------------------------------------------------------------------------

MediaGridView::MediaGridView()
    : mColumns {6}
    , mFixedColumns {0}
    , mVisibleRows {2}
    , mCellWidth {1.0f}
    , mCellHeight {1.0f}
    , mPosterHeight {1.0f}
    , mCursor {0}
    , mFirstRow {0}
    , mFocused {true}
    , mLoadsThisFrame {0}
    , mScrollDirection {0}
    , mScrollTimer {0}
    , mScrollCount {0}
    , mLastVerticalInput {0}
{
    mLabel = MediaStyle::makeText(MediaStyle::font(0.024f), MediaStyle::TEXT, ALIGN_CENTER);
    mPlaceholder = std::make_unique<TextComponent>("", MediaStyle::font(0.026f),
                                                   MediaStyle::TEXT_SECONDARY, ALIGN_CENTER,
                                                   ALIGN_CENTER, glm::ivec2 {0, 0});
}

void MediaGridView::updateLayout()
{
    if (mSize.x <= 0.0f || mSize.y <= 0.0f)
        return;
    const float targetWidth {MediaStyle::height(0.165f)};
    mColumns = mFixedColumns > 0 ?
                   mFixedColumns :
                   std::max(3, static_cast<int>(std::round(mSize.x / targetWidth)));
    mCellWidth = mSize.x / static_cast<float>(mColumns);
    const float spacing {std::round(mCellWidth * 0.08f)};
    mPosterHeight = std::round((mCellWidth - spacing) * 1.5f);
    mCellHeight = mPosterHeight + std::round(MediaStyle::height(0.050f));
    mVisibleRows = std::max(1, static_cast<int>(mSize.y / mCellHeight));
    mLabel->setSize(mCellWidth - spacing, MediaStyle::height(0.040f));
    mPlaceholder->setSize(mCellWidth - spacing * 2.0f, mPosterHeight * 0.8f);
    setCursor(mCursor);
}

void MediaGridView::setColumns(int columns)
{
    if (columns == mFixedColumns)
        return;
    mFixedColumns = columns;
    updateLayout();
}

void MediaGridView::setItems(const std::vector<Item>& items, int cursor)
{
    mItems = items;
    mScrollDirection = 0;
    mCursor = -1;
    mFirstRow = 0;
    setCursor(cursor);
}

void MediaGridView::setImage(int index, const std::string& image)
{
    if (index >= 0 && index < size())
        mItems[index].image = image;
}

void MediaGridView::setCursor(int cursor)
{
    const int newCursor {mItems.empty() ? 0 : std::clamp(cursor, 0, size() - 1)};
    const bool changed {newCursor != mCursor};
    mCursor = newCursor;
    const int row {mCursor / std::max(1, mColumns)};
    if (row < mFirstRow)
        mFirstRow = row;
    else if (row >= mFirstRow + mVisibleRows)
        mFirstRow = row - mVisibleRows + 1;
    const int rows {(size() + mColumns - 1) / std::max(1, mColumns)};
    mFirstRow = std::clamp(mFirstRow, 0, std::max(0, rows - mVisibleRows));
    if (changed && mCursorChangedCallback)
        mCursorChangedCallback(mCursor);
}

void MediaGridView::getVisibleRange(int& first, int& last) const
{
    first = mFirstRow * mColumns;
    last = std::min(size(), (mFirstRow + mVisibleRows + 1) * mColumns) - 1;
}

bool MediaGridView::isVerticalInputActive() const
{
    return mScrollDirection == -mColumns || mScrollDirection == mColumns ||
           SDL_GetTicks() - mLastVerticalInput < DIAGONAL_GUARD_TIME;
}

void MediaGridView::move(int amount)
{
    if (mItems.empty())
        return;
    int cursor {mCursor + amount};
    if (amount == -1 || amount == 1) {
        // Left and right stay on the same row.
        if (cursor / mColumns != mCursor / mColumns || cursor < 0)
            return;
    }
    if (cursor < 0)
        cursor = mCursor % mColumns == mCursor ? mCursor : 0;
    if (cursor >= size()) {
        // Moving down to a shorter last row goes to its last item.
        if (amount > 1 && mCursor / mColumns < (size() - 1) / mColumns)
            cursor = size() - 1;
        else
            return;
    }
    setCursor(cursor);
}

bool MediaGridView::input(InputConfig* config, Input input)
{
    const bool up {config->isMappedLike("up", input)};
    const bool down {config->isMappedLike("down", input)};
    const bool left {config->isMappedLike("left", input)};
    const bool right {config->isMappedLike("right", input)};
    if (up || down || left || right) {
        if (up || down)
            mLastVerticalInput = SDL_GetTicks();
        if (input.value == 0) {
            mScrollDirection = 0;
            return !left || mCursor % mColumns != 0;
        }
        if ((left || right) && isVerticalInputActive())
            return true;
        if (left && mCursor % mColumns == 0)
            return false;
        const int amount {up ? -mColumns : (down ? mColumns : (left ? -1 : 1))};
        move(amount);
        mScrollDirection = amount;
        mScrollTimer = SCROLL_DELAY_INITIAL;
        mScrollCount = 0;
        return true;
    }
    if (config->isMappedLike("leftshoulder", input) || config->isMappedLike("rightshoulder", input)) {
        if (input.value != 0) {
            const int page {mColumns * std::max(1, mVisibleRows)};
            const int target {mCursor + (config->isMappedLike("leftshoulder", input) ? -page : page)};
            setCursor(std::clamp(target, 0, size() - 1));
        }
        return true;
    }
    if (config->isMappedLike("lefttrigger", input) || config->isMappedLike("righttrigger", input)) {
        if (input.value != 0)
            setCursor(config->isMappedLike("lefttrigger", input) ? 0 : size() - 1);
        return true;
    }
    return false;
}

void MediaGridView::update(int deltaTime)
{
    mLoadsThisFrame = 0;
    if (mScrollDirection != 0) {
        mScrollTimer -= deltaTime;
        while (mScrollTimer <= 0 && mScrollDirection != 0) {
            const int previous {mCursor};
            move(mScrollDirection);
            ++mScrollCount;
            mScrollTimer += mScrollCount > 8 ? SCROLL_DELAY_FAST * 2 : SCROLL_DELAY_REPEAT * 2;
            if (mCursor == previous)
                mScrollDirection = 0;
        }
    }
    GuiComponent::update(deltaTime);
}

ImageComponent* MediaGridView::getImage(const std::string& path, bool allowLoad)
{
    auto it {mImages.find(path)};
    if (it != mImages.end())
        return it->second.get();
    if (!allowLoad)
        return nullptr;
    // Loading is spread over the frames so scrolling stays smooth.
    if (mLoadsThisFrame >= 2)
        return nullptr;
    ++mLoadsThisFrame;
    if (mImages.size() > 160)
        mImages.clear();
    auto image = std::make_unique<ImageComponent>(true, true);
    image->setImage(path);
    return mImages.emplace(path, std::move(image)).first->second.get();
}

void MediaGridView::render(const glm::mat4& parentTrans)
{
    if (!isVisible())
        return;
    const glm::mat4 trans {parentTrans * getTransform()};
    const float spacing {std::round(mCellWidth * 0.08f)};
    const float posterWidth {mCellWidth - spacing};
    const float radius {MediaStyle::height(0.006f)};

    for (int row {0}; row <= mVisibleRows; ++row) {
        const float y {row * mCellHeight};
        if (y + mPosterHeight > mSize.y + 1.0f)
            break;
        for (int column {0}; column < mColumns; ++column) {
            const int index {(mFirstRow + row) * mColumns + column};
            if (index >= size())
                break;
            const Item& item {mItems[index]};
            const float x {column * mCellWidth + spacing / 2.0f};
            const bool selected {index == mCursor};

            if (selected) {
                const float border {std::max(3.0f, std::round(MediaStyle::height(0.004f)))};
                MediaStyle::drawRect(trans, x - border, y - border, posterWidth + border * 2.0f,
                                     mPosterHeight + border * 2.0f,
                                     mFocused ? MediaStyle::ACCENT : MediaStyle::ROW_UNFOCUSED,
                                     radius + border);
            }
            MediaStyle::drawRect(trans, x, y, posterWidth, mPosterHeight, 0x15181FFF, radius);

            ImageComponent* image {nullptr};
            if (!item.image.empty())
                image = getImage(item.image, true);
            if (image != nullptr) {
                // Posters fill the cell, other pictures keep their shape.
                const glm::vec2 original {image->getTextureSize()};
                if (original.x > 0.0f && original.y > 0.0f) {
                    const float scale {std::min(posterWidth / original.x,
                                                mPosterHeight / original.y)};
                    const float ratio {(original.x / original.y) / (posterWidth / mPosterHeight)};
                    if (ratio > 0.9f && ratio < 1.1f)
                        image->setResize(posterWidth, mPosterHeight);
                    else
                        image->setResize(std::round(original.x * scale),
                                         std::round(original.y * scale));
                }
                const glm::vec2 imageSize {image->getSize()};
                image->setPosition(std::round(x + (posterWidth - imageSize.x) / 2.0f),
                                   std::round(y + (mPosterHeight - imageSize.y) / 2.0f));
                image->setColorShift(item.dimmed ? 0xFFFFFF80 : 0xFFFFFFFF);
                image->render(trans);
            }
            else if (!item.icon.empty()) {
                auto it {mIcons.find(item.icon)};
                if (it == mIcons.end()) {
                    auto icon = std::make_unique<ImageComponent>();
                    const float size {std::round(posterWidth * 0.42f)};
                    icon->setResize(size, size);
                    icon->setImage(item.icon);
                    it = mIcons.emplace(item.icon, std::move(icon)).first;
                }
                ImageComponent* icon {it->second.get()};
                const glm::vec2 iconSize {icon->getSize()};
                icon->setPosition(std::round(x + (posterWidth - iconSize.x) / 2.0f),
                                  std::round(y + (mPosterHeight - iconSize.y) / 2.0f));
                icon->setColorShift(selected ? 0xFFFFFFFF : 0xD0D0D0FF);
                icon->render(trans);
            }
            else {
                // The title in the middle until there's a picture.
                MediaStyle::setText(*mPlaceholder, item.label);
                mPlaceholder->setPosition(x + spacing / 2.0f, y + mPosterHeight * 0.1f);
                mPlaceholder->render(trans);
            }

            if (item.watched) {
                const float size {std::round(posterWidth * 0.16f)};
                MediaStyle::drawRect(trans, x + posterWidth - size - spacing * 0.3f,
                                     y + spacing * 0.3f, size, size, MediaStyle::ACCENT,
                                     size / 2.0f);
            }
            if (item.progress >= 0.0f) {
                const float barHeight {std::max(3.0f, std::round(mPosterHeight * 0.02f))};
                MediaStyle::drawRect(trans, x, y + mPosterHeight - barHeight, posterWidth,
                                     barHeight, 0x000000B0);
                MediaStyle::drawRect(trans, x, y + mPosterHeight - barHeight,
                                     posterWidth * std::clamp(item.progress, 0.0f, 1.0f),
                                     barHeight, MediaStyle::ACCENT);
            }

            MediaStyle::setText(*mLabel, item.label);
            const unsigned int color {selected && mFocused ? 0xFFFFFFFF :
                                                             (item.dimmed ? MediaStyle::TEXT_DIM :
                                                                            MediaStyle::TEXT_SECONDARY)};
            if (mLabel->getColor() != color)
                mLabel->setColor(color);
            mLabel->setPosition(x, y + mPosterHeight + MediaStyle::height(0.004f));
            mLabel->render(trans);
        }
    }

    // Scroll position indicator.
    const int rows {(size() + mColumns - 1) / std::max(1, mColumns)};
    if (rows > mVisibleRows) {
        const float trackWidth {std::max(2.0f, std::round(MediaStyle::width(0.003f)))};
        const float trackX {mSize.x - trackWidth};
        const float thumbHeight {std::max(mSize.y * mVisibleRows / rows, mCellHeight * 0.3f)};
        const float thumbY {(mSize.y - thumbHeight) * mFirstRow /
                            static_cast<float>(rows - mVisibleRows)};
        MediaStyle::drawRect(trans, trackX, 0.0f, trackWidth, mSize.y, 0xFFFFFF1A);
        MediaStyle::drawRect(trans, trackX, thumbY, trackWidth, thumbHeight, 0xFFFFFF80);
    }
}

//--------------------------------------------------------------------------------------------------
// MediaStripView
//--------------------------------------------------------------------------------------------------

void renderImageInBox(const glm::mat4& trans,
                      ImageComponent& image,
                      float x,
                      float y,
                      float width,
                      float height,
                      unsigned int colorShift)
{
    const glm::vec2 original {image.getTextureSize()};
    if (original.x <= 0.0f || original.y <= 0.0f || width <= 0.0f || height <= 0.0f)
        return;
    const float ratio {(original.x / original.y) / (width / height)};
    if (ratio > 0.9f && ratio < 1.1f) {
        image.setResize(std::round(width), std::round(height));
    }
    else {
        const float scale {std::min(width / original.x, height / original.y)};
        image.setResize(std::round(original.x * scale), std::round(original.y * scale));
    }
    const glm::vec2 size {image.getSize()};
    image.setOrigin(0.0f, 0.0f);
    image.setPosition(std::round(x + (width - size.x) / 2.0f),
                      std::round(y + (height - size.y) / 2.0f));
    image.setColorShift(colorShift);
    image.render(trans);
}

MediaStripView::MediaStripView()
    : mPosterWidth {1.0f}
    , mPosterHeight {1.5f}
    , mSpacing {0.0f}
    , mFocusSlot {0}
    , mSkipSelected {false}
    , mShowLabels {false}
    , mCursor {0}
    , mFocused {true}
    , mLoadsThisFrame {0}
    , mScrollDirection {0}
    , mScrollTimer {0}
    , mScrollCount {0}
{
    mLabel = std::make_unique<TextComponent>("", MediaStyle::font(0.030f), MediaStyle::TEXT,
                                             ALIGN_CENTER, ALIGN_CENTER, glm::ivec2 {0, 0});
    mPlaceholder = std::make_unique<TextComponent>("", MediaStyle::font(0.026f),
                                                   MediaStyle::TEXT_SECONDARY, ALIGN_CENTER,
                                                   ALIGN_CENTER, glm::ivec2 {0, 0});
}

void MediaStripView::setLayout(float posterWidth,
                               float posterHeight,
                               float spacing,
                               int focusSlot,
                               bool skipSelected,
                               bool showLabels)
{
    mPosterWidth = std::max(1.0f, std::round(posterWidth));
    mPosterHeight = std::max(1.0f, std::round(posterHeight));
    mSpacing = std::round(spacing);
    mFocusSlot = std::max(0, focusSlot);
    mSkipSelected = skipSelected;
    mShowLabels = showLabels;
    mLabel->setSize(mPosterWidth + mSpacing - MediaStyle::width(0.01f),
                    MediaStyle::height(0.10f));
    mPlaceholder->setSize(mPosterWidth * 0.85f, mPosterHeight * 0.8f);
}

void MediaStripView::setItems(const std::vector<Item>& items, int cursor)
{
    mItems = items;
    mScrollDirection = 0;
    mCursor = -1;
    setCursor(cursor);
}

void MediaStripView::setImage(int index, const std::string& image)
{
    if (index >= 0 && index < size())
        mItems[index].image = image;
}

void MediaStripView::setCursor(int cursor)
{
    const int newCursor {mItems.empty() ? 0 : std::clamp(cursor, 0, size() - 1)};
    const bool changed {newCursor != mCursor};
    mCursor = newCursor;
    if (changed && mCursorChangedCallback)
        mCursorChangedCallback(mCursor);
}

int MediaStripView::slotCount() const
{
    return static_cast<int>(std::ceil(mSize.x / std::max(1.0f, mPosterWidth + mSpacing))) + 1;
}

int MediaStripView::firstShown() const
{
    return mSkipSelected ? mCursor + 1 : mCursor - mFocusSlot;
}

void MediaStripView::getVisibleRange(int& first, int& last) const
{
    first = std::max(0, firstShown() - (mSkipSelected ? 1 : 0));
    last = std::min(size() - 1, firstShown() + slotCount());
}

void MediaStripView::move(int amount)
{
    if (mItems.empty())
        return;
    const int cursor {std::clamp(mCursor + amount, 0, size() - 1)};
    if (cursor != mCursor)
        setCursor(cursor);
}

bool MediaStripView::input(InputConfig* config, Input input)
{
    const bool left {config->isMappedLike("left", input)};
    const bool right {config->isMappedLike("right", input)};
    if (left || right) {
        if (input.value == 0) {
            mScrollDirection = 0;
            return true;
        }
        const int amount {left ? -1 : 1};
        move(amount);
        mScrollDirection = amount;
        mScrollTimer = SCROLL_DELAY_INITIAL;
        mScrollCount = 0;
        return true;
    }
    if (config->isMappedLike("leftshoulder", input) || config->isMappedLike("rightshoulder", input)) {
        if (input.value != 0) {
            const int page {std::max(1, slotCount() - 2)};
            move(config->isMappedLike("leftshoulder", input) ? -page : page);
        }
        return true;
    }
    if (config->isMappedLike("lefttrigger", input) || config->isMappedLike("righttrigger", input)) {
        if (input.value != 0)
            setCursor(config->isMappedLike("lefttrigger", input) ? 0 : size() - 1);
        return true;
    }
    return false;
}

void MediaStripView::update(int deltaTime)
{
    mLoadsThisFrame = 0;
    if (mScrollDirection != 0) {
        mScrollTimer -= deltaTime;
        while (mScrollTimer <= 0 && mScrollDirection != 0) {
            const int previous {mCursor};
            move(mScrollDirection);
            ++mScrollCount;
            mScrollTimer += mScrollCount > 8 ? SCROLL_DELAY_FAST * 2 : SCROLL_DELAY_REPEAT * 3;
            if (mCursor == previous)
                mScrollDirection = 0;
        }
    }
    GuiComponent::update(deltaTime);
}

ImageComponent* MediaStripView::getImage(const std::string& path)
{
    auto it {mImages.find(path)};
    if (it != mImages.end())
        return it->second.get();
    if (mLoadsThisFrame >= 2)
        return nullptr;
    ++mLoadsThisFrame;
    if (mImages.size() > 120)
        mImages.clear();
    auto image = std::make_unique<ImageComponent>(true, true);
    image->setImage(path);
    return mImages.emplace(path, std::move(image)).first->second.get();
}

void MediaStripView::render(const glm::mat4& parentTrans)
{
    if (!isVisible() || mItems.empty())
        return;
    const glm::mat4 trans {parentTrans * getTransform()};
    const float step {mPosterWidth + mSpacing};
    const float radius {MediaStyle::height(0.005f)};
    const float labelY {mPosterHeight + MediaStyle::height(0.045f)};

    // The label row is drawn first, so the highlight of the selected title is behind the text.
    if (mShowLabels) {
        MediaStyle::drawRect(trans, -mSize.x, labelY, mSize.x * 3.0f, mLabel->getSize().y,
                             0x00000080);
    }

    const int first {firstShown()};
    for (int slot {mSkipSelected ? -1 : 0}; slot < slotCount(); ++slot) {
        const int index {first + slot};
        if (index < 0 || index >= size())
            continue;
        if (mSkipSelected && index == mCursor)
            continue;
        const float x {slot * step + mSpacing / 2.0f};
        if (x + mPosterWidth < -step || x > mSize.x + step)
            continue;
        const Item& item {mItems[index]};
        const bool selected {index == mCursor};

        if (selected) {
            const float border {std::max(3.0f, std::round(MediaStyle::height(0.004f)))};
            MediaStyle::drawRect(trans, x - border, -border, mPosterWidth + border * 2.0f,
                                 mPosterHeight + border * 2.0f,
                                 mFocused ? MediaStyle::ACCENT : MediaStyle::ROW_UNFOCUSED,
                                 radius + border);
        }
        MediaStyle::drawRect(trans, x, 0.0f, mPosterWidth, mPosterHeight, 0x15181FFF, radius);

        ImageComponent* image {item.image.empty() ? nullptr : getImage(item.image)};
        if (image != nullptr) {
            renderImageInBox(trans, *image, x, 0.0f, mPosterWidth, mPosterHeight,
                             item.dimmed ? 0xFFFFFF80 : 0xFFFFFFFF);
        }
        else if (!item.icon.empty()) {
            auto it {mIcons.find(item.icon)};
            if (it == mIcons.end()) {
                auto icon = std::make_unique<ImageComponent>();
                const float size {std::round(mPosterWidth * 0.42f)};
                icon->setResize(size, size);
                icon->setImage(item.icon);
                it = mIcons.emplace(item.icon, std::move(icon)).first;
            }
            ImageComponent* icon {it->second.get()};
            const glm::vec2 iconSize {icon->getSize()};
            icon->setPosition(std::round(x + (mPosterWidth - iconSize.x) / 2.0f),
                              std::round((mPosterHeight - iconSize.y) / 2.0f));
            icon->render(trans);
        }
        else {
            MediaStyle::setText(*mPlaceholder, item.label);
            mPlaceholder->setPosition(x + mPosterWidth * 0.075f, mPosterHeight * 0.1f);
            mPlaceholder->render(trans);
        }
        if (item.watched) {
            const float size {std::round(mPosterWidth * 0.14f)};
            MediaStyle::drawRect(trans, x + mPosterWidth - size * 1.3f, size * 0.3f, size, size,
                                 MediaStyle::ACCENT, size / 2.0f);
        }
        if (item.progress >= 0.0f) {
            const float barHeight {std::max(3.0f, std::round(mPosterHeight * 0.02f))};
            MediaStyle::drawRect(trans, x, mPosterHeight - barHeight, mPosterWidth, barHeight,
                                 0x000000B0);
            MediaStyle::drawRect(trans, x, mPosterHeight - barHeight,
                                 mPosterWidth * std::clamp(item.progress, 0.0f, 1.0f), barHeight,
                                 MediaStyle::ACCENT);
        }

        if (mShowLabels) {
            const float labelX {slot * step + MediaStyle::width(0.005f)};
            if (selected) {
                MediaStyle::drawRect(trans, slot * step, labelY, step, mLabel->getSize().y,
                                     mFocused ? MediaStyle::ACCENT : MediaStyle::ROW_UNFOCUSED);
            }
            MediaStyle::setText(*mLabel, item.label);
            mLabel->setPosition(labelX, labelY);
            mLabel->render(trans);
        }
    }
}

//--------------------------------------------------------------------------------------------------
// MediaDialog
//--------------------------------------------------------------------------------------------------

MediaDialog::MediaDialog(const std::string& title, float widthFraction)
    : mMessageHeight {0.0f}
    , mMessageScroll {0.0f}
    , mWidthFraction {widthFraction}
    , mClosed {false}
{
    mTitle = MediaStyle::makeText(MediaStyle::font(0.036f, FONT_PATH_BOLD), MediaStyle::TEXT,
                                  ALIGN_LEFT);
    mTitle->setText(title);
    mList.setFonts(MediaStyle::font(0.031f), MediaStyle::font(0.028f));
    mList.setRowHeight(MediaStyle::height(0.056f));
    updateLayout();
}

float MediaDialog::subtitleSpace() const
{
    if (!mSubtitle)
        return 0.0f;
    return MediaStyle::height(0.045f + 0.03f * static_cast<float>(std::max(0, mSubtitleLines - 1)));
}

void MediaDialog::setSubtitle(const std::string& subtitle)
{
    if (subtitle.empty()) {
        mSubtitle.reset();
    }
    else {
        if (!mSubtitle)
            mSubtitle = MediaStyle::makeText(MediaStyle::font(0.024f), MediaStyle::TEXT_DIM,
                                             ALIGN_LEFT);
        MediaStyle::setText(*mSubtitle, subtitle);
    }
    // A subtitle can have several lines.
    mSubtitleLines = static_cast<int>(std::count(subtitle.cbegin(), subtitle.cend(), '\n')) + 1;
    updateLayout();
}

void MediaDialog::setOptions(const std::vector<Option>& options, int cursor)
{
    mOptions = options;
    std::vector<MediaListView::Item> items;
    for (const auto& option : mOptions) {
        MediaListView::Item item;
        item.label = option.label;
        item.value = option.value ? option.value() : "";
        item.highlight = option.highlight;
        item.dimmed = option.dimmed;
        item.detail = option.detail;
        item.valueColor = option.valueColor ? option.valueColor() : 0;
        items.emplace_back(item);
    }
    mList.setItems(items, cursor);
    updateLayout();
}

void MediaDialog::setTitle(const std::string& title) { MediaStyle::setText(*mTitle, title); }

void MediaDialog::setMessage(const std::string& message)
{
    if (message.empty()) {
        mMessage.reset();
    }
    else {
        const float padding {MediaStyle::height(0.022f)};
        const float width {std::min(Renderer::getScreenWidth() * mWidthFraction,
                                    Renderer::getScreenWidth() * 0.9f) -
                           padding * 2.0f};
        mMessage = std::make_unique<TextComponent>("", MediaStyle::font(0.028f), MediaStyle::TEXT,
                                                   ALIGN_LEFT, ALIGN_TOP, glm::ivec2 {0, 1});
        mMessage->setLineSpacing(1.35f);
        mMessage->setSize(width, 0.0f);
        mMessage->setText(message);
    }
    mMessageScroll = 0.0f;
    updateLayout();
}

void MediaDialog::refresh()
{
    for (size_t i {0}; i < mOptions.size(); ++i) {
        if (!mOptions[i].value)
            continue;
        MediaListView::Item item {mList.getItems()[i]};
        const std::string value {mOptions[i].value()};
        const unsigned int color {mOptions[i].valueColor ? mOptions[i].valueColor() : 0};
        if (item.value != value || item.valueColor != color) {
            item.value = value;
            item.valueColor = color;
            mList.setItem(static_cast<int>(i), item);
        }
    }
}

void MediaDialog::updateLayout()
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};
    const float padding {MediaStyle::height(0.022f)};
    const float titleHeight {MediaStyle::height(0.075f)};
    const float subtitleHeight {subtitleSpace()};
    // A long message takes at most half of the screen and is scrolled.
    mMessageHeight =
        mMessage ? std::min(mMessage->getSize().y, screenHeight * (mList.size() > 1 ? 0.30f : 0.62f)) :
                   0.0f;
    const float messageSpace {mMessage ? mMessageHeight + padding : 0.0f};
    const int maxRows {std::max(3, static_cast<int>((screenHeight * 0.70f - titleHeight -
                                                      subtitleHeight - messageSpace -
                                                      padding * 2.0f) /
                                                     mList.getRowHeight()))};
    const int rows {std::clamp(mList.size(), 1, maxRows)};

    const float dialogWidth {std::min(screenWidth * mWidthFraction, screenWidth * 0.9f)};
    const float listHeight {rows * mList.getRowHeight()};
    const float dialogHeight {titleHeight + subtitleHeight + messageSpace + listHeight +
                              padding * 2.0f};

    setSize(dialogWidth, dialogHeight);
    setPosition(std::round((screenWidth - dialogWidth) / 2.0f),
                std::round((screenHeight - dialogHeight) / 2.0f));

    mTitle->setSize(dialogWidth - padding * 2.0f, titleHeight);
    mTitle->setPosition(padding, 0.0f);
    if (mSubtitle) {
        mSubtitle->setSize(dialogWidth - padding * 2.0f, subtitleHeight);
        mSubtitle->setPosition(padding, titleHeight + padding * 0.5f);
    }
    mList.setSize(dialogWidth - padding, listHeight);
    mList.setPosition(padding * 0.5f, titleHeight + subtitleHeight + messageSpace + padding);
    mList.setCursor(mList.getCursor());
}

bool MediaDialog::input(InputConfig* config, Input input)
{
    // A long message is scrolled instead of moving between the options.
    if (mMessage && mMessage->getSize().y > mMessageHeight + 1.0f &&
        (config->isMappedLike("up", input) || config->isMappedLike("down", input) ||
         config->isMappedLike("leftshoulder", input) ||
         config->isMappedLike("rightshoulder", input))) {
        if (input.value != 0) {
            const float line {MediaStyle::font(0.028f)->getHeight(1.35f)};
            float amount {line * 2.0f};
            if (config->isMappedLike("leftshoulder", input) ||
                config->isMappedLike("rightshoulder", input))
                amount = mMessageHeight - line;
            if (config->isMappedLike("up", input) || config->isMappedLike("leftshoulder", input))
                amount = -amount;
            mMessageScroll = std::clamp(mMessageScroll + amount, 0.0f,
                                        mMessage->getSize().y - mMessageHeight);
        }
        return true;
    }
    if (mList.input(config, input))
        return true;

    if (input.value == 0)
        return true;

    const int cursor {mList.getCursor()};
    const bool hasOption {cursor >= 0 && cursor < static_cast<int>(mOptions.size())};

    if (config->isMappedTo("a", input)) {
        if (hasOption) {
            // The callback may close this dialog or open a new one, so copy it first.
            const std::function<void()> onSelect {mOptions[cursor].onSelect};
            const std::function<void(int)> onAdjust {mOptions[cursor].onAdjust};
            if (onSelect)
                onSelect();
            else if (onAdjust)
                onAdjust(1);
            if (!mClosed)
                refresh();
        }
        return true;
    }
    if (config->isMappedLike("left", input) || config->isMappedLike("right", input)) {
        // Most likely pressed by accident while moving up or down.
        if (mList.isVerticalInputActive())
            return true;
        if (hasOption && mOptions[cursor].onAdjust) {
            const std::function<void(int)> onAdjust {mOptions[cursor].onAdjust};
            onAdjust(config->isMappedLike("left", input) ? -1 : 1);
            if (!mClosed)
                refresh();
        }
        return true;
    }
    if (config->isMappedTo("x", input)) {
        if (hasOption && mOptions[cursor].onRemove) {
            const std::function<void()> onRemove {mOptions[cursor].onRemove};
            onRemove();
            if (!mClosed)
                refresh();
        }
        return true;
    }
    if (config->isMappedTo("b", input) ||
        (config->getDeviceId() == DEVICE_KEYBOARD && input.id == SDLK_ESCAPE)) {
        if (mBackHandler) {
            const std::function<bool()> backHandler {mBackHandler};
            if (backHandler())
                return true;
        }
        close();
        if (mCancelCallback)
            mCancelCallback();
        return true;
    }
    // Everything else is swallowed while the dialog is open.
    return true;
}

void MediaDialog::update(int deltaTime)
{
    mList.update(deltaTime);
    if (mUpdateCallback) {
        const std::function<void(int)> callback {mUpdateCallback};
        callback(deltaTime);
    }
}

void MediaDialog::render(const glm::mat4& parentTrans)
{
    const glm::mat4 trans {parentTrans * getTransform()};
    const float titleHeight {MediaStyle::height(0.075f)};
    const float radius {MediaStyle::height(0.012f)};

    MediaStyle::drawRect(trans, 0.0f, 0.0f, mSize.x, mSize.y, MediaStyle::PANEL, radius);
    MediaStyle::drawRect(trans, 0.0f, 0.0f, mSize.x, titleHeight, MediaStyle::PANEL_HEADER,
                         radius);
    // Square off the lower corners of the title bar.
    MediaStyle::drawRect(trans, 0.0f, titleHeight - radius, mSize.x, radius,
                         MediaStyle::PANEL_HEADER);
    MediaStyle::drawRect(trans, 0.0f, titleHeight, mSize.x, std::max(1.0f, mSize.y * 0.002f),
                         MediaStyle::ACCENT);

    mTitle->render(trans);
    if (mSubtitle)
        mSubtitle->render(trans);
    if (mMessage) {
        const float padding {MediaStyle::height(0.022f)};
        const float top {titleHeight + subtitleSpace() + padding};
        const glm::vec4 origin {trans * glm::vec4 {padding, top, 0.0f, 1.0f}};
        Renderer::getInstance()->pushClipRect(
            glm::ivec2 {static_cast<int>(std::round(origin.x)),
                        static_cast<int>(std::round(origin.y))},
            glm::ivec2 {static_cast<int>(std::round(mMessage->getSize().x)),
                        static_cast<int>(std::round(mMessageHeight))});
        mMessage->setPosition(padding, std::round(top - mMessageScroll));
        mMessage->render(trans);
        Renderer::getInstance()->popClipRect();
        // Shows that there's more text.
        if (mMessage->getSize().y > mMessageHeight + 1.0f) {
            const float trackWidth {std::max(2.0f, std::round(MediaStyle::width(0.003f)))};
            const float thumbHeight {mMessageHeight * mMessageHeight / mMessage->getSize().y};
            const float thumbY {top + (mMessageHeight - thumbHeight) * mMessageScroll /
                                          (mMessage->getSize().y - mMessageHeight)};
            MediaStyle::drawRect(trans, mSize.x - padding * 0.5f, top, trackWidth, mMessageHeight,
                                 0xFFFFFF1A);
            MediaStyle::drawRect(trans, mSize.x - padding * 0.5f, thumbY, trackWidth, thumbHeight,
                                 0xFFFFFF80);
        }
    }
    mList.render(trans);
}

std::vector<HelpPrompt> MediaDialog::getHelpPrompts()
{
    std::vector<HelpPrompt> prompts;
    prompts.push_back(HelpPrompt("up/down", "choose"));
    bool adjustable {false};
    for (const auto& option : mOptions) {
        if (option.onAdjust) {
            adjustable = true;
            break;
        }
    }
    if (adjustable)
        prompts.push_back(HelpPrompt("left/right", "change"));
    const int cursor {mList.getCursor()};
    if (cursor >= 0 && cursor < static_cast<int>(mOptions.size()) && mOptions[cursor].onRemove)
        prompts.push_back(HelpPrompt("x", "remove"));
    prompts.push_back(HelpPrompt("a", "select"));
    prompts.push_back(HelpPrompt("b", "back"));
    return prompts;
}

//--------------------------------------------------------------------------------------------------
// MediaDialogStack
//--------------------------------------------------------------------------------------------------

MediaDialog* MediaDialogStack::push(std::unique_ptr<MediaDialog> dialog)
{
    mDialogs.emplace_back(std::move(dialog));
    return mDialogs.back().get();
}

bool MediaDialogStack::contains(const MediaDialog* dialog) const
{
    for (const auto& item : mDialogs) {
        if (item.get() == dialog && !item->isClosed())
            return true;
    }
    return false;
}

bool MediaDialogStack::input(InputConfig* config, Input input)
{
    if (mDialogs.empty())
        return false;
    // The input may push new dialogs, so keep a pointer to the receiver.
    MediaDialog* dialog {mDialogs.back().get()};
    dialog->input(config, input);
    removeClosed();
    return true;
}

void MediaDialogStack::update(int deltaTime)
{
    if (!mDialogs.empty())
        mDialogs.back()->update(deltaTime);
    removeClosed();
}

void MediaDialogStack::render(const glm::mat4& parentTrans)
{
    if (mDialogs.empty())
        return;
    MediaStyle::drawRect(parentTrans, 0.0f, 0.0f, Renderer::getScreenWidth(),
                         Renderer::getScreenHeight(), MediaStyle::DIALOG_SHADE);
    // Only the topmost dialog, the ones below would otherwise show through.
    mDialogs.back()->render(parentTrans);
}

void MediaDialogStack::refresh()
{
    for (auto& dialog : mDialogs)
        dialog->refresh();
}

std::vector<HelpPrompt> MediaDialogStack::getHelpPrompts()
{
    if (mDialogs.empty())
        return std::vector<HelpPrompt>();
    return mDialogs.back()->getHelpPrompts();
}

void MediaDialogStack::removeClosed()
{
    mDialogs.erase(std::remove_if(mDialogs.begin(), mDialogs.end(),
                                  [](const std::unique_ptr<MediaDialog>& dialog) {
                                      return dialog->isClosed();
                                  }),
                   mDialogs.end());
}

//--------------------------------------------------------------------------------------------------
// MediaBackground
//--------------------------------------------------------------------------------------------------

MediaBackground::MediaBackground()
{
    const auto& collections {CollectionSystemsManager::getInstance()->getAutoCollectionSystems()};
    const auto it {collections.find("favorites")};
    if (it == collections.cend() || it->second.system == nullptr)
        return;

    mTheme = it->second.system->getTheme();
    if (!mTheme)
        return;

    loadElements(mTheme, "gamelist");
    if (mImages.empty())
        loadElements(mTheme, "system");
}

void MediaBackground::loadElements(const std::shared_ptr<ThemeData>& theme, const std::string& view)
{
    if (!theme->hasView(view))
        return;

    for (const auto& [name, element] : theme->getViewElements(view).elements) {
        if (element.type != "image")
            continue;
        if (element.has("visible") && !element.get<bool>("visible"))
            continue;
        // Only static images, not the ones showing game media or metadata.
        if (element.has("imageType") || element.has("metadataElement") ||
            element.has("gameselector"))
            continue;
        if (!element.has("path") && !element.has("default") && !element.has("color"))
            continue;

        // Loaded immediately to avoid the images popping in, using the texture manager as
        // forced loading isn't supported for textures outside of it.
        auto image = std::make_unique<ImageComponent>(true, true);
        image->setDefaultZIndex(30.0f);
        image->applyTheme(theme, view, name, ThemeFlags::ALL);
        if (!image->getThemeImageTypes().empty() || image->getMetadataElement())
            continue;
        // Only the images covering (nearly) the whole screen, the panels, logos and other
        // decorations of the theme would collide with the media screen layout.
        const glm::vec2 size {image->getSize() * image->getScale()};
        if (size.x < Renderer::getScreenWidth() * 0.9f ||
            size.y < Renderer::getScreenHeight() * 0.9f)
            continue;
        mImages.emplace_back(std::move(image));
    }

    std::stable_sort(mImages.begin(), mImages.end(),
                     [](const std::unique_ptr<ImageComponent>& a,
                        const std::unique_ptr<ImageComponent>& b) {
                         return a->getZIndex() < b->getZIndex();
                     });
}

void MediaBackground::render(const glm::mat4& parentTrans)
{
    const float screenWidth {Renderer::getScreenWidth()};
    const float screenHeight {Renderer::getScreenHeight()};

    // Base layer in case the theme images don't cover the whole screen.
    MediaStyle::drawGradient(parentTrans, 0.0f, 0.0f, screenWidth, screenHeight, 0x1A2230FF,
                             0x07090CFF);
    for (auto& image : mImages)
        image->render(parentTrans);
}

//--------------------------------------------------------------------------------------------------
// Subtitle language selection.
//--------------------------------------------------------------------------------------------------

void buildSubtitleLanguagePicker(MediaDialog* dialog,
                                 bool defaultOnly,
                                 const std::function<void()>& doneCallback)
{
    Settings* settings {Settings::getInstance()};
    dialog->setSubtitle(defaultOnly ? "Choose the language for downloaded subtitles" :
                                      "A selects the default, X adds or removes other languages");

    // The options hold the rebuild function, which itself only holds a weak reference to avoid
    // a reference cycle.
    auto rebuild = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> weakRebuild {rebuild};
    *rebuild = [dialog, settings, defaultOnly, doneCallback, weakRebuild](int cursor) {
        const std::shared_ptr<std::function<void(int)>> rebuild {weakRebuild.lock()};
        if (!rebuild)
            return;
        std::vector<MediaDialog::Option> options;
        const std::string defaultLanguage {settings->getString("MediaSubtitleDownloadLanguage")};
        const std::vector<std::string> extra {Utils::String::delimitedStringToVector(
            settings->getString("MediaSubtitleExtraLanguages"), ",")};
        if (!defaultOnly) {
            MediaDialog::Option done;
            done.label = "Done";
            done.highlight = true;
            done.onSelect = [dialog, doneCallback] {
                dialog->close();
                if (doneCallback)
                    doneCallback();
            };
            options.emplace_back(done);
        }
        for (const auto& language : SubtitleDownloader::getLanguages()) {
            MediaDialog::Option option;
            option.label = language.name;
            const std::string code {language.code};
            const bool isDefault {code == defaultLanguage};
            const bool isExtra {std::find(extra.cbegin(), extra.cend(), code) != extra.cend()};
            option.value = [isDefault, isExtra] {
                return isDefault ? std::string("Default") :
                                   (isExtra ? std::string("Also") : std::string(""));
            };
            option.onSelect = [dialog, settings, code, defaultOnly, doneCallback,
                               rebuild] {
                settings->setString("MediaSubtitleDownloadLanguage", code);
                settings->saveFile();
                if (defaultOnly) {
                    dialog->close();
                    if (doneCallback)
                        doneCallback();
                    return;
                }
                (*rebuild)(dialog->getCursor());
            };
            if (!defaultOnly) {
                option.onRemove = [dialog, settings, code, rebuild] {
                    std::vector<std::string> languages {Utils::String::delimitedStringToVector(
                        settings->getString("MediaSubtitleExtraLanguages"), ",")};
                    const auto it {std::find(languages.begin(), languages.end(), code)};
                    if (it != languages.end())
                        languages.erase(it);
                    else if (code != settings->getString("MediaSubtitleDownloadLanguage"))
                        languages.emplace_back(code);
                    std::string value;
                    for (const auto& language : languages)
                        value.append((value.empty() ? "" : ",") + language);
                    settings->setString("MediaSubtitleExtraLanguages", value);
                    settings->saveFile();
                    (*rebuild)(dialog->getCursor());
                };
            }
            options.emplace_back(option);
        }
        dialog->setOptions(options, cursor);
    };

    // Start on the current default language.
    int cursor {0};
    const std::string defaultLanguage {settings->getString("MediaSubtitleDownloadLanguage")};
    const auto& languages {SubtitleDownloader::getLanguages()};
    for (size_t i {0}; i < languages.size(); ++i) {
        if (languages[i].code == defaultLanguage)
            cursor = static_cast<int>(i) + (defaultOnly ? 0 : 1);
    }
    (*rebuild)(cursor);
}
