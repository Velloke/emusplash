//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  GuiVersionHistory.cpp
//
//  The list of the versions of this build and what was done in each, read from the file
//  versions.txt of the resources. Every patch adds an entry at the top of the file.
//

#include "guis/GuiVersionHistory.h"

#include "InputConfig.h"
#include "Log.h"
#include "resources/ResourceManager.h"
#include "Window.h"
#include "components/BackgroundComponent.h"
#include "components/TextComponent.h"
#include "guis/GuiSettings.h"
#include "renderers/Renderer.h"
#include "resources/Font.h"
#include "utils/FileSystemUtil.h"
#include "utils/LocalizationUtil.h"
#include "utils/StringUtil.h"

#include <algorithm>
#include <fstream>

namespace VersionHistory
{
    namespace
    {
        // Breaks the text into lines that fit the width, the next lines are indented.
        std::vector<std::string> wrap(const std::string& text,
                                      float maxWidth,
                                      const std::shared_ptr<Font>& font =
                                          Font::get(FONT_SIZE_SMALL))
        {
            std::vector<std::string> lines;
            std::string line;
            const std::vector<std::string> words {
                Utils::String::delimitedStringToVector(text, " ")};
            for (const std::string& word : words) {
                if (word.empty())
                    continue;
                const std::string tryLine {line.empty() ? word : line + " " + word};
                if (!line.empty() && font->sizeText(tryLine).x > maxWidth) {
                    lines.emplace_back(line);
                    line = "  " + word;
                }
                else {
                    line = tryLine;
                }
            }
            if (!line.empty())
                lines.emplace_back(line);
            return lines;
        }

        std::string trim(const std::string& text) { return Utils::String::trim(text); }

        // What was done in a version: the whole text in the frame of the menus, scrolled with
        // up and down (held for scrolling on) and the shoulder buttons a page at a time. No row
        // is selected, it's only read.
        class VersionDetails : public GuiComponent
        {
        public:
            VersionDetails(const Version& version)
                : mRenderer {Renderer::getInstance()}
            {
                const float screenWidth {Renderer::getScreenWidth()};
                const float screenHeight {Renderer::getScreenHeight()};
                const bool vertical {Renderer::getIsVerticalOrientation()};
                // The same width as the menus.
                const float width {std::min(screenHeight * 1.05f,
                                            screenWidth * (vertical ? 0.94f : 0.90f))};
                mPadding = width * 0.04f;
                const float textWidth {width - mPadding * 3.0f};

                mTitle = std::make_shared<TextComponent>(
                    Utils::String::toUpper(version.name),
                    Font::get(FONT_SIZE_LARGE * Utils::Localization::sMenuTitleScaleFactor),
                    mMenuColorTitle, ALIGN_CENTER, ALIGN_CENTER, glm::ivec2 {0, 0});
                mTitleHeight = mTitle->getFont()->getLetterHeight() +
                               (vertical ? screenWidth : screenHeight) * 0.0637f;

                std::string heading {version.title};
                if (!version.date.empty())
                    heading.append("  (" + version.date + ")");
                const auto headingFont {Font::get(FONT_SIZE_MEDIUM)};
                std::string headingText;
                for (const std::string& line :
                     wrap(Utils::String::toUpper(heading), textWidth, headingFont))
                    headingText.append(headingText.empty() ? line : "\n" + line);
                mHeading = std::make_shared<TextComponent>(headingText, headingFont,
                                                           mMenuColorPrimary, ALIGN_LEFT,
                                                           ALIGN_TOP, glm::ivec2 {1, 1});

                const auto bodyFont {Font::get(FONT_SIZE_SMALL)};
                std::string bodyText;
                for (const std::string& change : version.changes) {
                    if (!bodyText.empty())
                        bodyText.append("\n");
                    for (const std::string& line :
                         wrap("- " + Utils::String::toUpper(change), textWidth, bodyFont))
                        bodyText.append(bodyText.empty() || bodyText.back() == '\n' ?
                                            line :
                                            "\n" + line);
                }
                mBody = std::make_shared<TextComponent>(bodyText, bodyFont, mMenuColorPrimary,
                                                        ALIGN_LEFT, ALIGN_TOP,
                                                        glm::ivec2 {1, 1});
                mLineHeight = bodyFont->getHeight(1.5f);
                const float gap {mLineHeight * 0.5f};
                mBody->setPosition(0.0f, mHeading->getSize().y + gap);
                const float contentHeight {mBody->getPosition().y + mBody->getSize().y};

                // As high as the text needs, at most as high as a menu.
                const float maxHeight {screenHeight * (vertical ? 0.70f : 0.80f)};
                const float height {
                    std::min(maxHeight, mTitleHeight + contentHeight + mPadding * 1.5f)};
                setSize(width, height);
                setPosition((screenWidth - width) / 2.0f, screenHeight * 0.13f);
                mBackground.fitTo(mSize);
                mTitle->setSize(width, mTitleHeight);

                mTextArea = {mPadding, mTitleHeight + mPadding * 0.25f, textWidth,
                             height - mTitleHeight - mPadding};
                mMaxScroll = std::max(0.0f, contentHeight - mTextArea.w);
            }

            bool input(InputConfig* config, Input input) override
            {
                if (config->isMappedLike("up", input) || config->isMappedLike("down", input)) {
                    if (input.value != 0) {
                        mDirection = config->isMappedLike("up", input) ? -1 : 1;
                        mHeldTime = 0;
                        scrollBy(mDirection * mLineHeight);
                    }
                    else {
                        mDirection = 0;
                    }
                    return true;
                }
                if (input.value == 0)
                    return true;
                if (config->isMappedTo("leftshoulder", input) ||
                    config->isMappedTo("lefttrigger", input)) {
                    scrollBy(-mTextArea.w * 0.9f);
                    return true;
                }
                if (config->isMappedTo("rightshoulder", input) ||
                    config->isMappedTo("righttrigger", input)) {
                    scrollBy(mTextArea.w * 0.9f);
                    return true;
                }
                if (config->isMappedTo("b", input) || config->isMappedTo("back", input) ||
                    config->isMappedTo("a", input)) {
                    delete this;
                    return true;
                }
                return true;
            }

            void update(int deltaTime) override
            {
                // Held, the text goes on scrolling after a moment.
                if (mDirection != 0) {
                    mHeldTime += deltaTime;
                    if (mHeldTime > 350)
                        scrollBy(mDirection * mLineHeight * 12.0f * deltaTime / 1000.0f);
                }
                GuiComponent::update(deltaTime);
            }

            void render(const glm::mat4& parentTrans) override
            {
                const glm::mat4 trans {parentTrans * getTransform()};
                mBackground.render(trans);
                mTitle->render(trans);

                const float resolution {mRenderer->getScreenResolutionModifier()};
                mRenderer->setMatrix(trans);
                mRenderer->drawRect(0.0f, mTitleHeight, mSize.x, std::max(1.0f, resolution),
                                    0xC6C7C6FF, 0xC6C7C6FF);

                mRenderer->pushClipRect(
                    glm::ivec2 {static_cast<int>(std::round(trans[3].x + mTextArea.x)),
                                static_cast<int>(std::round(trans[3].y + mTextArea.y))},
                    glm::ivec2 {static_cast<int>(std::round(mTextArea.z + mPadding)),
                                static_cast<int>(std::round(mTextArea.w))});
                const glm::mat4 textTrans {glm::translate(
                    trans, glm::vec3 {mTextArea.x, mTextArea.y - mScroll, 0.0f})};
                mHeading->render(textTrans);
                mBody->render(textTrans);
                mRenderer->popClipRect();

                // A thin bar on the right shows where in the text the view is.
                if (mMaxScroll > 0.0f) {
                    const float barWidth {std::max(2.0f, 4.0f * resolution)};
                    const float barX {mTextArea.x + mTextArea.z + mPadding * 0.5f};
                    const float thumbHeight {
                        std::max(mLineHeight, mTextArea.w * mTextArea.w /
                                                  (mTextArea.w + mMaxScroll))};
                    const float thumbY {mTextArea.y + (mTextArea.w - thumbHeight) *
                                                          (mScroll / mMaxScroll)};
                    mRenderer->setMatrix(trans);
                    mRenderer->drawRect(barX, mTextArea.y, barWidth, mTextArea.w, 0xC6C7C680,
                                        0xC6C7C680);
                    mRenderer->drawRect(barX, thumbY, barWidth, thumbHeight, mMenuColorPrimary,
                                        mMenuColorPrimary);
                }
            }

            std::vector<HelpPrompt> getHelpPrompts() override
            {
                std::vector<HelpPrompt> prompts;
                if (mMaxScroll > 0.0f) {
                    prompts.push_back(HelpPrompt("up/down", "scroll"));
                    prompts.push_back(HelpPrompt("lr", "page"));
                }
                prompts.push_back(HelpPrompt("b", "back"));
                return prompts;
            }

        private:
            void scrollBy(float amount)
            {
                mScroll = std::clamp(mScroll + amount, 0.0f, mMaxScroll);
            }

            Renderer* mRenderer;
            BackgroundComponent mBackground;
            std::shared_ptr<TextComponent> mTitle;
            std::shared_ptr<TextComponent> mHeading;
            std::shared_ptr<TextComponent> mBody;
            // x, y, width and height of the text inside the frame.
            glm::vec4 mTextArea {0.0f};
            float mPadding {0.0f};
            float mTitleHeight {0.0f};
            float mLineHeight {1.0f};
            float mScroll {0.0f};
            float mMaxScroll {0.0f};
            int mDirection {0};
            int mHeldTime {0};
        };
    } // namespace

    std::vector<Version> load()
    {
        std::vector<Version> versions;
        const std::string path {
            ResourceManager::getInstance().getResourcePath(":/versions.txt", false)};
        std::ifstream file {path};
        if (!file.is_open()) {
            LOG(LogWarning) << "VersionHistory: Couldn't open \"" << path << "\"";
            return versions;
        }
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (Utils::String::startsWith(line, "# ")) {
                // "# name | date | title", the lines that are only a comment have no "|".
                std::vector<std::string> fields;
                size_t from {2};
                while (true) {
                    const size_t bar {line.find('|', from)};
                    if (bar == std::string::npos) {
                        fields.emplace_back(trim(line.substr(from)));
                        break;
                    }
                    fields.emplace_back(trim(line.substr(from, bar - from)));
                    from = bar + 1;
                }
                if (fields.size() < 3)
                    continue;
                Version version;
                version.name = fields[0];
                version.date = fields[1];
                version.title = fields[2];
                versions.emplace_back(version);
            }
            else if (!versions.empty() && Utils::String::startsWith(line, "* ")) {
                versions.back().changes.emplace_back(trim(line.substr(2)));
            }
            else if (!versions.empty() && Utils::String::startsWith(line, "  ") &&
                     !versions.back().changes.empty()) {
                versions.back().changes.back().append(" " + trim(line));
            }
        }
        return versions;
    }

    void show(Window* window, unsigned int textColor)
    {
        const std::vector<Version> versions {load()};
        auto list {new GuiSettings("VERSIONS")};

        if (versions.empty()) {
            ComponentListRow row;
            row.addElement(std::make_shared<TextComponent>("NO VERSION LIST WAS FOUND",
                                                           Font::get(FONT_SIZE_MEDIUM),
                                                           textColor),
                           true);
            list->addRow(row);
        }

        for (const Version& version : versions) {
            ComponentListRow row;
            row.addElement(std::make_shared<TextComponent>(
                               Utils::String::toUpper(version.name + "  " + version.title),
                               Font::get(FONT_SIZE_MEDIUM), textColor),
                           true);
            row.addElement(list->getMenu().makeArrow(), false);
            row.makeAcceptInputHandler(
                [window, version] { window->pushGui(new VersionDetails(version)); });
            list->addRow(row);
        }
        window->pushGui(list);
    }
} // namespace VersionHistory
