# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  xbmc.py
#
#  Compatibility module for the add-on scripts, the module name is the one the scripts import.
#

import time

import _host

LOGDEBUG = 0
LOGINFO = 1
LOGNOTICE = 1
LOGWARNING = 2
LOGERROR = 3
LOGSEVERE = 4
LOGFATAL = 4
LOGNONE = 5

ISO_639_1 = 0
ISO_639_2 = 1
ENGLISH_NAME = 2

PLAYLIST_MUSIC = 0
PLAYLIST_VIDEO = 1

_LEVELS = {0: "DEBUG", 1: "INFO", 2: "WARNING", 3: "ERROR", 4: "FATAL", 5: "NONE"}

_LANGUAGES = {
    "en": ("eng", "English"), "et": ("est", "Estonian"), "ru": ("rus", "Russian"),
    "fi": ("fin", "Finnish"), "de": ("ger", "German"), "fr": ("fre", "French"),
    "es": ("spa", "Spanish"), "it": ("ita", "Italian"), "sv": ("swe", "Swedish"),
    "no": ("nor", "Norwegian"), "da": ("dan", "Danish"), "pl": ("pol", "Polish"),
    "lv": ("lav", "Latvian"), "lt": ("lit", "Lithuanian"), "ja": ("jpn", "Japanese"),
    "nl": ("dut", "Dutch"), "pt": ("por", "Portuguese"), "uk": ("ukr", "Ukrainian"),
    "hu": ("hun", "Hungarian"), "cs": ("cze", "Czech"), "tr": ("tur", "Turkish"),
    "zh": ("chi", "Chinese"), "ko": ("kor", "Korean"), "ar": ("ara", "Arabic"),
    "he": ("heb", "Hebrew"), "el": ("gre", "Greek"), "ro": ("rum", "Romanian"),
    "hr": ("hrv", "Croatian"), "sk": ("slo", "Slovak"), "bg": ("bul", "Bulgarian"),
}

_INFO_LABELS = {
    "System.BuildVersion": _host.BUILD_VERSION,
    "System.BuildVersionShort": _host.BUILD_VERSION.split()[0],
    "System.Language": "English",
    "System.FriendlyName": "EmuSPLASH",
}


def log(msg, level=LOGDEBUG):
    _host.log("%s: %s" % (_LEVELS.get(level, "DEBUG"), msg))


def translatePath(path):
    return _host.translate_path(path)


def getInfoLabel(label):
    if label in _INFO_LABELS:
        return _INFO_LABELS[label]
    result = _host.call("Host_InfoLabel", label)
    return result if isinstance(result, str) else ""


def getCondVisibility(condition):
    return False


def getLanguage(format=ENGLISH_NAME, region=False):
    code = (_host.LANGUAGE or "en").lower()
    region_code = ""
    if "-" in code:
        code, region_code = code.split("-", 1)
    entry = _LANGUAGES.get(code, ("eng", "English"))
    if format == ISO_639_1:
        if region:
            return "%s-%s" % (code, (region_code or code).lower())
        return code
    if format == ISO_639_2:
        return entry[0]
    return entry[1]


def convertLanguage(language, format=ENGLISH_NAME):
    language = (language or "").strip()
    for code, (three, name) in _LANGUAGES.items():
        if language.lower() in (code, three, name.lower()):
            if format == ISO_639_1:
                return code
            if format == ISO_639_2:
                return three
            return name
    return ""


def getRegion(id):
    return ""


def sleep(milliseconds):
    time.sleep(milliseconds / 1000.0)


def executebuiltin(function, wait=False):
    _host.call("Host_Builtin", function)


def executeJSONRPC(request):
    result = _host.call("Host_JsonRpc", request)
    return result if isinstance(result, str) else "{}"


def getSkinDir():
    return "skin.emusplash"


def getUserAgent():
    return "EmuSPLASH/" + _host.BUILD_VERSION.split()[0]


def getFreeMem():
    return 1024


def makeLegalFilename(filename, fatX=True):
    return filename


def validatePath(path):
    return path


def audioSuspend():
    pass


def audioResume():
    pass


def enableNavSounds(yesNo):
    pass


def playSFX(filename, useCached=True):
    pass


def stopSFX():
    pass


class Monitor(object):
    def __init__(self):
        pass

    def abortRequested(self):
        return False

    def waitForAbort(self, timeout=None):
        if timeout:
            time.sleep(timeout)
        return False

    def onSettingsChanged(self):
        pass

    def onNotification(self, sender, method, data):
        pass


class Player(object):
    def __init__(self, *args, **kwargs):
        pass

    def isPlaying(self):
        return bool(_host.call("Player_IsPlaying"))

    def isPlayingVideo(self):
        return self.isPlaying()

    def getPlayingFile(self):
        result = _host.call("Player_GetPlayingFile")
        return result if isinstance(result, str) else ""

    def getTime(self):
        times = _host.call("Player_WatchTimes") or {}
        return float(times.get("watchedTime", 0) or 0)

    def getTotalTime(self):
        times = _host.call("Player_WatchTimes") or {}
        return float(times.get("videoDuration", 0) or 0)

    def pause(self):
        pass

    def stop(self):
        pass

    def seekTime(self, seconds):
        _host.call("Player_Seek", seconds)

    def play(self, item=None, listitem=None, windowed=False, startpos=-1):
        pass

    def getAvailableSubtitleStreams(self):
        return _host.call("Player_GetSubtitles") or []

    def setSubtitles(self, path):
        _host.call("Player_SetSubtitles", [path])

    def showSubtitles(self, visible):
        pass


class PlayList(object):
    def __init__(self, playlist=PLAYLIST_VIDEO):
        self._items = []

    def add(self, url, listitem=None, index=-1):
        self._items.append(url)

    def clear(self):
        self._items = []

    def size(self):
        return len(self._items)

    def getposition(self):
        return -1

    def __len__(self):
        return len(self._items)


class Keyboard(object):
    def __init__(self, default="", heading="", hidden=False):
        self._default = default
        self._heading = heading
        self._hidden = hidden
        self._text = ""
        self._confirmed = False

    def setDefault(self, default):
        self._default = default

    def setHeading(self, heading):
        self._heading = heading

    def setHiddenInput(self, hidden):
        self._hidden = hidden

    def doModal(self, autoclose=0):
        result = _host.call("Keyboard", self._default, self._heading, self._hidden)
        self._confirmed = isinstance(result, str)
        self._text = result if self._confirmed else ""

    def isConfirmed(self):
        return self._confirmed

    def getText(self):
        return self._text


class Actor(object):
    def __init__(self, name="", role="", order=-1, thumbnail=""):
        self.name = name
        self.role = role
        self.order = order
        self.thumbnail = thumbnail


class InfoTagVideo(object):
    def __getattr__(self, name):
        return lambda *args, **kwargs: None
