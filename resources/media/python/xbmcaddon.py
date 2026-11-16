# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  xbmcaddon.py
#
#  Compatibility module for the add-on scripts, the module name is the one the scripts import.
#  The settings, strings and information of the add-ons are read from EmuSPLASH.
#

import threading

import _host

_cache_lock = threading.Lock()
_settings = {}
_strings = {}
_info = {}


def _load(cache, method, addon_id):
    with _cache_lock:
        if addon_id not in cache:
            result = _host.call(method, addon_id)
            cache[addon_id] = result if isinstance(result, dict) else {}
        return cache[addon_id]


class Addon(object):
    def __init__(self, id=None):
        self._id = id or _host.ADDON_ID

    def getSetting(self, id):
        value = _load(_settings, "Addon_GetSettings", self._id).get(id, "")
        return value if isinstance(value, str) else str(value)

    def getSettingBool(self, id):
        return self.getSetting(id).lower() == "true"

    def getSettingInt(self, id):
        try:
            return int(float(self.getSetting(id)))
        except ValueError:
            return 0

    def getSettingNumber(self, id):
        try:
            return float(self.getSetting(id))
        except ValueError:
            return 0.0

    def getSettingString(self, id):
        return self.getSetting(id)

    def setSetting(self, id, value):
        value = value if isinstance(value, str) else str(value)
        _host.call("Addon_SetSetting", self._id, id, value)
        with _cache_lock:
            if self._id in _settings:
                _settings[self._id][id] = value

    def setSettingBool(self, id, value):
        self.setSetting(id, "true" if value else "false")

    def setSettingInt(self, id, value):
        self.setSetting(id, str(int(value)))

    def setSettingNumber(self, id, value):
        self.setSetting(id, str(value))

    def setSettingString(self, id, value):
        self.setSetting(id, value)

    def getLocalizedString(self, id):
        return _load(_strings, "Addon_GetStrings", self._id).get(str(id), "")

    def getAddonInfo(self, id):
        info = _load(_info, "Addon_GetInfo", self._id)
        if id in ("profile", "path"):
            return _host.translate_path(info.get(id, ""))
        return info.get(id, "")

    def openSettings(self):
        _host.call("AddonSettings", self._id)

    def getSettings(self):
        return self
