//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonSettings.cpp
//
//  Settings of an installed add-on. The definitions (categories, types, conditions) are read
//  from the add-on's own settings file and the values are stored in its data directory.
//  Also provides the translated strings of the add-on, which the labels refer to.
//

#include "media/AddonSettings.h"

#include "Log.h"
#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"

#include <pugixml.hpp>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

namespace
{
    bool isNumber(const std::string& text)
    {
        if (text.empty())
            return false;
        for (const char character : text) {
            if (character < '0' || character > '9')
                return false;
        }
        return true;
    }

    // Reads a quoted string of a .po file, including continuation lines.
    std::string unquotePo(const std::string& text)
    {
        std::string result;
        bool inQuotes {false};
        for (size_t i {0}; i < text.size(); ++i) {
            const char character {text[i]};
            if (character == '"') {
                inQuotes = !inQuotes;
                continue;
            }
            if (!inQuotes)
                continue;
            if (character == '\\' && i + 1 < text.size()) {
                const char next {text[++i]};
                if (next == 'n')
                    result.push_back('\n');
                else if (next == 't')
                    result.push_back('\t');
                else
                    result.push_back(next);
                continue;
            }
            result.push_back(character);
        }
        return result;
    }

    std::vector<std::string> splitTerms(const std::string& condition, char separator)
    {
        std::vector<std::string> terms;
        std::string current;
        int depth {0};
        for (const char character : condition) {
            if (character == '(')
                ++depth;
            else if (character == ')')
                --depth;
            if (character == separator && depth == 0) {
                terms.emplace_back(Utils::String::trim(current));
                current.clear();
                continue;
            }
            current.push_back(character);
        }
        if (!Utils::String::trim(current).empty())
            terms.emplace_back(Utils::String::trim(current));
        return terms;
    }
} // namespace

std::string stripAddonFormatting(const std::string& text)
{
    std::string result {Utils::String::replace(text, "[CR]", "\n")};
    static const std::regex tags {
        R"(\[/?(B|I|UPPERCASE|LOWERCASE|CAPITALIZE|LIGHT|TABS)\]|\[COLOR [^\]]*\]|\[/COLOR\])",
        std::regex::icase};
    result = std::regex_replace(result, tags, "");
    return result;
}

AddonSettings::AddonSettings(const std::string& addonId,
                             const std::string& addonPath,
                             const std::string& dataPath)
    : mAddonId {addonId}
    , mAddonPath {addonPath}
    , mDataPath {dataPath}
{
}

void AddonSettings::load()
{
    loadStrings();
    loadDefinitions();
    loadValues();
}

void AddonSettings::loadDefinitions()
{
    mCategories.clear();
    mDefaults.clear();

    const std::string path {mAddonPath + "/resources/settings.xml"};
    pugi::xml_document doc;
    if (!doc.load_file(path.c_str()))
        return;

    const pugi::xml_node root {doc.child("settings")};
    for (pugi::xml_node categoryNode {root.child("category")}; categoryNode;
         categoryNode = categoryNode.next_sibling("category")) {
        Category category;
        category.label = categoryNode.attribute("label").as_string();
        for (pugi::xml_node node {categoryNode.child("setting")}; node;
             node = node.next_sibling("setting")) {
            Definition setting;
            setting.id = node.attribute("id").as_string();
            setting.type = node.attribute("type").as_string();
            setting.label = node.attribute("label").as_string();
            setting.defaultValue = node.attribute("default").as_string();
            setting.values = node.attribute("values").as_string();
            setting.lvalues = node.attribute("lvalues").as_string();
            setting.option = node.attribute("option").as_string();
            setting.range = node.attribute("range").as_string();
            setting.action = node.attribute("action").as_string();
            setting.enable = node.attribute("enable").as_string();
            setting.visible = node.attribute("visible").as_string();
            setting.subsetting = std::string(node.attribute("subsetting").as_string()) == "true";
            if (!setting.id.empty() && setting.type != "action" && setting.type != "lsep" &&
                setting.type != "sep")
                mDefaults[setting.id] = setting.defaultValue;
            category.settings.emplace_back(setting);
        }
        mCategories.emplace_back(category);
    }
}

void AddonSettings::loadStrings()
{
    mStrings.clear();
    const std::string languageDir {mAddonPath + "/resources/language/"};
    for (const char* name :
         {"resource.language.en_gb", "resource.language.en_us", "English", "english"}) {
        const std::string poPath {languageDir + name + "/strings.po"};
        if (Utils::FileSystem::isRegularFile(poPath)) {
            std::ifstream file {poPath};
            std::string line;
            int currentId {-1};
            std::string msgid;
            bool inMsgid {false};
            auto store = [&] {
                if (currentId >= 0 && !msgid.empty())
                    mStrings[currentId] = msgid;
            };
            while (std::getline(file, line)) {
                const std::string trimmed {Utils::String::trim(line)};
                if (Utils::String::startsWith(trimmed, "msgctxt")) {
                    store();
                    msgid.clear();
                    inMsgid = false;
                    const std::string context {unquotePo(trimmed.substr(7))};
                    currentId = (context.size() > 1 && context[0] == '#') ?
                                    std::atoi(context.c_str() + 1) :
                                    -1;
                }
                else if (Utils::String::startsWith(trimmed, "msgid")) {
                    msgid = unquotePo(trimmed.substr(5));
                    inMsgid = true;
                }
                else if (Utils::String::startsWith(trimmed, "msgstr")) {
                    inMsgid = false;
                }
                else if (inMsgid && !trimmed.empty() && trimmed[0] == '"') {
                    msgid.append(unquotePo(trimmed));
                }
            }
            store();
            return;
        }
        const std::string xmlPath {languageDir + name + "/strings.xml"};
        if (Utils::FileSystem::isRegularFile(xmlPath)) {
            pugi::xml_document doc;
            if (doc.load_file(xmlPath.c_str())) {
                for (pugi::xml_node node {doc.child("strings").child("string")}; node;
                     node = node.next_sibling("string"))
                    mStrings[node.attribute("id").as_int()] = node.text().as_string();
            }
            return;
        }
    }
}

void AddonSettings::loadValues()
{
    std::map<std::string, std::string> values {mDefaults};
    const std::string path {mDataPath + "/settings.xml"};
    pugi::xml_document doc;
    if (Utils::FileSystem::isRegularFile(path) && doc.load_file(path.c_str())) {
        for (pugi::xml_node node {doc.child("settings").child("setting")}; node;
             node = node.next_sibling("setting")) {
            const std::string id {node.attribute("id").as_string()};
            if (id.empty())
                continue;
            // Both the current format (value as text) and the old one (value attribute).
            if (node.attribute("value"))
                values[id] = node.attribute("value").as_string();
            else
                values[id] = node.text().as_string();
        }
    }
    std::unique_lock<std::mutex> lock {mMutex};
    mValues = values;
}

void AddonSettings::reloadValues() { loadValues(); }

void AddonSettings::saveValues() const
{
    // The values can be changed from several threads, the file is written by one at a time.
    std::unique_lock<std::mutex> saveLock {mSaveMutex};
    Utils::FileSystem::createDirectory(mDataPath);
    pugi::xml_document doc;
    pugi::xml_node root {doc.append_child("settings")};
    root.append_attribute("version") = "2";
    {
        std::unique_lock<std::mutex> lock {mMutex};
        for (const auto& [id, value] : mValues) {
            pugi::xml_node node {root.append_child("setting")};
            node.append_attribute("id") = id.c_str();
            const auto it {mDefaults.find(id)};
            if (it != mDefaults.cend() && it->second == value)
                node.append_attribute("default") = "true";
            node.text().set(value.c_str());
        }
    }
    const std::string path {mDataPath + "/settings.xml"};
    const std::string temporary {path + ".tmp"};
    if (!doc.save_file(temporary.c_str(), "    ")) {
        LOG(LogError) << "AddonSettings: Couldn't write \"" << path << "\"";
        return;
    }
    // Note that renameFile() returns true on failure.
    if (Utils::FileSystem::renameFile(temporary, path, true)) {
        LOG(LogError) << "AddonSettings: Couldn't replace \"" << path << "\"";
    }
}

const AddonSettings::Definition* AddonSettings::find(const std::string& id) const
{
    for (const auto& category : mCategories) {
        for (const auto& setting : category.settings) {
            if (setting.id == id)
                return &setting;
        }
    }
    return nullptr;
}

std::string AddonSettings::get(const std::string& id) const
{
    std::unique_lock<std::mutex> lock {mMutex};
    const auto it {mValues.find(id)};
    return it == mValues.cend() ? "" : it->second;
}

void AddonSettings::set(const std::string& id, const std::string& value)
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        const auto it {mValues.find(id)};
        if (it != mValues.cend() && it->second == value)
            return;
        mValues[id] = value;
    }
    saveValues();
}

std::map<std::string, std::string> AddonSettings::getAll() const
{
    std::unique_lock<std::mutex> lock {mMutex};
    return mValues;
}

void AddonSettings::resetToDefaults()
{
    {
        std::unique_lock<std::mutex> lock {mMutex};
        for (const auto& [id, value] : mDefaults)
            mValues[id] = value;
    }
    saveValues();
}

std::string AddonSettings::getString(int id) const
{
    const auto it {mStrings.find(id)};
    return it == mStrings.cend() ? "" : it->second;
}

std::map<std::string, std::string> AddonSettings::getStrings() const
{
    std::map<std::string, std::string> strings;
    for (const auto& [id, text] : mStrings)
        strings[std::to_string(id)] = text;
    return strings;
}

std::string AddonSettings::translateLabel(const std::string& label) const
{
    std::string text {label};
    if (isNumber(text)) {
        const std::string translated {getString(std::atoi(text.c_str()))};
        text = translated.empty() ? text : translated;
    }
    // References to strings, e.g. "$ADDON[script.example 32111]" or "$LOCALIZE[30001]".
    static const std::regex addonReference {R"(\$ADDON\[([^ \]]+) (\d+)\])"};
    std::smatch match;
    std::string result;
    std::string rest {text};
    while (std::regex_search(rest, match, addonReference)) {
        result.append(match.prefix());
        if (match[1].str() == mAddonId)
            result.append(getString(std::atoi(match[2].str().c_str())));
        rest = match.suffix();
    }
    result.append(rest);
    static const std::regex localizeReference {R"(\$LOCALIZE\[(\d+)\])"};
    result = std::regex_replace(result, localizeReference, "");
    return stripAddonFormatting(result);
}

std::vector<std::pair<std::string, std::string>> AddonSettings::getChoices(
    const Definition& setting) const
{
    std::vector<std::pair<std::string, std::string>> choices;
    if (setting.type == "enum") {
        // The value is the index of the choice.
        const std::string source {setting.lvalues.empty() ? setting.values : setting.lvalues};
        const std::vector<std::string> items {
            Utils::String::delimitedStringToVector(source, "|")};
        for (size_t i {0}; i < items.size(); ++i) {
            choices.emplace_back(std::to_string(i), setting.lvalues.empty() ?
                                                        items[i] :
                                                        translateLabel(items[i]));
        }
    }
    else {
        // The value is the text of the choice.
        const std::string source {setting.lvalues.empty() ? setting.values : setting.lvalues};
        for (const auto& item : Utils::String::delimitedStringToVector(source, "|")) {
            const std::string label {setting.lvalues.empty() ? item : translateLabel(item)};
            choices.emplace_back(setting.lvalues.empty() ? item : label, label);
        }
    }
    return choices;
}

bool AddonSettings::isVisible(size_t category, size_t index) const
{
    if (category >= mCategories.size() || index >= mCategories[category].settings.size())
        return false;
    const std::string& condition {mCategories[category].settings[index].visible};
    return condition.empty() || evaluate(condition, category, index);
}

bool AddonSettings::isEnabled(size_t category, size_t index) const
{
    if (category >= mCategories.size() || index >= mCategories[category].settings.size())
        return false;
    const std::string& condition {mCategories[category].settings[index].enable};
    return condition.empty() || evaluate(condition, category, index);
}

bool AddonSettings::evaluate(const std::string& condition, size_t category, size_t index) const
{
    const std::string trimmed {Utils::String::trim(condition)};
    if (trimmed == "true")
        return true;
    if (trimmed == "false")
        return false;

    // "|" separates alternatives and "+" conditions that all have to be true.
    const std::vector<std::string> alternatives {splitTerms(trimmed, '|')};
    if (alternatives.size() > 1) {
        for (const auto& alternative : alternatives) {
            if (evaluate(alternative, category, index))
                return true;
        }
        return false;
    }
    const std::vector<std::string> terms {splitTerms(trimmed, '+')};
    if (terms.size() > 1) {
        for (const auto& term : terms) {
            if (!evaluate(term, category, index))
                return false;
        }
        return true;
    }

    static const std::regex comparison {R"((!?)(eq|lt|gt)\(\s*(-?\d+)\s*,\s*([^)]*)\))",
                                        std::regex::icase};
    std::smatch match;
    if (!std::regex_match(trimmed, match, comparison))
        return true;

    const bool negate {match[1].str() == "!"};
    const std::string op {Utils::String::toLower(match[2].str())};
    const int offset {std::atoi(match[3].str().c_str())};
    const std::string expected {Utils::String::toLower(Utils::String::trim(match[4].str()))};

    const auto& settings {mCategories[category].settings};
    const int target {static_cast<int>(index) + offset};
    bool result {false};
    if (target >= 0 && target < static_cast<int>(settings.size())) {
        const std::string actual {Utils::String::toLower(get(settings[target].id))};
        if (op == "eq") {
            result = actual == expected;
        }
        else {
            const double left {std::atof(actual.c_str())};
            const double right {std::atof(expected.c_str())};
            result = op == "lt" ? left < right : left > right;
        }
    }
    return negate ? !result : result;
}
