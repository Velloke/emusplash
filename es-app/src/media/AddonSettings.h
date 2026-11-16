//  SPDX-License-Identifier: MIT
//
//  EmuSPLASH
//  AddonSettings.h
//
//  Settings of an installed add-on. The definitions (categories, types, conditions) are read
//  from the add-on's own settings file and the values are stored in its data directory.
//  Also provides the translated strings of the add-on, which the labels refer to.
//

#ifndef ES_APP_MEDIA_ADDON_SETTINGS_H
#define ES_APP_MEDIA_ADDON_SETTINGS_H

#include <map>
#include <mutex>
#include <string>
#include <vector>

class AddonSettings
{
public:
    struct Definition {
        std::string id;
        std::string type; // bool, text, number, slider, enum, labelenum, select, folder, file,
                          // action, lsep, sep, ipaddress.
        std::string label;
        std::string defaultValue;
        std::string values;  // "a|b|c" for select and labelenum.
        std::string lvalues; // String IDs for enum.
        std::string option;  // E.g. "hidden" for passwords, "int" for sliders.
        std::string range;   // "min,step,max" for sliders.
        std::string action;
        std::string enable;
        std::string visible;
        bool subsetting {false};
    };

    struct Category {
        std::string label;
        std::vector<Definition> settings;
    };

    AddonSettings(const std::string& addonId,
                  const std::string& addonPath,
                  const std::string& dataPath);

    // Reads the definitions, the strings and the stored values.
    void load();

    const std::string& getAddonId() const { return mAddonId; }
    const std::string& getAddonPath() const { return mAddonPath; }
    const std::vector<Category>& getCategories() const { return mCategories; }
    const Definition* find(const std::string& id) const;

    // The values can be read and changed from any thread.
    std::string get(const std::string& id) const;
    void set(const std::string& id, const std::string& value);
    std::map<std::string, std::string> getAll() const;
    // Restores the default values of all settings.
    void resetToDefaults();
    // Called after the values have been changed by someone else, e.g. the add-on itself.
    void reloadValues();

    // Translated string of the add-on, empty if it doesn't exist.
    std::string getString(int id) const;
    std::map<std::string, std::string> getStrings() const;
    // A label is either a string ID or the text itself.
    std::string translateLabel(const std::string& label) const;

    // Evaluates the enable and visible conditions of a setting, which refer to other
    // settings in the same category by their relative position.
    bool isVisible(size_t category, size_t index) const;
    bool isEnabled(size_t category, size_t index) const;

    // Choices of enum, labelenum and select settings, as value and label.
    std::vector<std::pair<std::string, std::string>> getChoices(const Definition& setting) const;

private:
    void loadDefinitions();
    void loadStrings();
    void loadValues();
    void saveValues() const;
    bool evaluate(const std::string& condition, size_t category, size_t index) const;

    std::string mAddonId;
    std::string mAddonPath;
    std::string mDataPath;
    std::vector<Category> mCategories;
    std::map<std::string, std::string> mDefaults;
    std::map<int, std::string> mStrings;

    mutable std::mutex mMutex;
    mutable std::mutex mSaveMutex;
    std::map<std::string, std::string> mValues;
};

// Removes the formatting tags used in add-on texts, e.g. [B] and [COLOR red], and turns
// the line break tag into a new line.
std::string stripAddonFormatting(const std::string& text);

#endif // ES_APP_MEDIA_ADDON_SETTINGS_H
