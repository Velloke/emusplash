# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  xbmcgui.py
#
#  Compatibility module for the add-on scripts, the module name is the one the scripts import.
#  Dialogs are shown by EmuSPLASH.
#

import _host

NOTIFICATION_INFO = "info"
NOTIFICATION_WARNING = "warning"
NOTIFICATION_ERROR = "error"

INPUT_ALPHANUM = 0
INPUT_NUMERIC = 1
INPUT_DATE = 2
INPUT_TIME = 3
INPUT_IPADDRESS = 4
INPUT_PASSWORD = 5

ALPHANUM_HIDE_INPUT = 2


class Dialog(object):
    def ok(self, heading, message="", *args, **kwargs):
        _host.call("Dialog", heading, message)
        return True

    def yesno(self, heading, message="", *args, **kwargs):
        return bool(_host.call("Dialog_Confirm", heading, message))

    def select(self, heading, list, autoclose=0, preselect=-1, useDetails=False):
        labels = [item if isinstance(item, str) else getattr(item, "_label", str(item))
                  for item in list]
        result = _host.call("Dialog_Select_With_Options", heading, labels, autoclose, preselect)
        return result if isinstance(result, int) else -1

    def multiselect(self, heading, options, autoclose=0, preselect=None, useDetails=False):
        labels = [item if isinstance(item, str) else getattr(item, "_label", str(item))
                  for item in options]
        result = _host.call("Dialog_MultiSelect", heading, labels, preselect or [])
        return result if isinstance(result, list) else None

    def notification(self, heading, message, icon="", time=5000, sound=True):
        _host.call("Notify", heading, message, icon or "")

    def input(self, heading, defaultt="", type=INPUT_ALPHANUM, option=0, autoclose=0):
        result = _host.call("Keyboard", defaultt, heading, type == INPUT_PASSWORD or
                            bool(option & ALPHANUM_HIDE_INPUT))
        return result if isinstance(result, str) else ""

    def browse(self, type, heading, shares, mask="", useThumbs=False, treatAsFolder=False,
               defaultt="", enableMultiple=False):
        result = _host.call("Dialog_Browse_Single", type, heading, shares, mask, useThumbs,
                            treatAsFolder, defaultt)
        return result if isinstance(result, str) else defaultt

    def browseSingle(self, type, heading, shares, mask="", useThumbs=False, treatAsFolder=False,
                     defaultt=""):
        return self.browse(type, heading, shares, mask, useThumbs, treatAsFolder, defaultt)

    def textviewer(self, heading, text, usemono=False):
        _host.call("Dialog_Text", heading, text)

    def info(self, item):
        return False


class DialogProgress(object):
    def __init__(self):
        self._handle = None
        self._canceled = False

    def create(self, heading, message=""):
        self._handle = _host.call("DialogProgress_Create", heading, message)

    def update(self, percent, message=""):
        if self._handle is not None:
            _host.call("DialogProgress_Update", self._handle, percent, message)

    def iscanceled(self):
        if self._handle is None:
            return False
        return bool(_host.call("DialogProgress_IsCanceled", self._handle))

    def close(self):
        if self._handle is not None:
            _host.call("DialogProgress_Close", self._handle)
            self._handle = None


class DialogProgressBG(object):
    def __init__(self):
        self._handle = None

    def create(self, heading, message=""):
        self._handle = _host.call("DialogProgressBG_Create", heading, message)

    def update(self, percent=0, heading=None, message=None):
        if self._handle is not None:
            _host.call("DialogProgressBG_Update", self._handle, int(percent or 0),
                       heading or "", message or "")

    def isFinished(self):
        return self._handle is None

    def close(self):
        if self._handle is not None:
            _host.call("DialogProgressBG_Close", self._handle)
            self._handle = None


class ListItem(object):
    def __init__(self, label="", label2="", iconImage="", thumbnailImage="", path="",
                 offscreen=False):
        self._label = label
        self._label2 = label2
        self._path = path
        self._properties = {}
        self._art = {}

    def getLabel(self):
        return self._label

    def setLabel(self, label):
        self._label = label

    def getLabel2(self):
        return self._label2

    def setLabel2(self, label):
        self._label2 = label

    def setArt(self, art):
        self._art.update(art)

    def getArt(self, key):
        return self._art.get(key, "")

    def setProperty(self, key, value):
        self._properties[key.lower()] = value

    def getProperty(self, key):
        return self._properties.get(key.lower(), "")

    def setPath(self, path):
        self._path = path

    def getPath(self):
        return self._path

    def __getattr__(self, name):
        return lambda *args, **kwargs: None


class Window(object):
    def __init__(self, existingWindowId=-1):
        self._id = existingWindowId

    def getProperty(self, key):
        result = _host.call("GetWindowProperty", key)
        return result if isinstance(result, str) else ""

    def setProperty(self, key, value):
        _host.call("SetWindowProperty", key, value)

    def clearProperty(self, key):
        _host.call("SetWindowProperty", key, "")

    def show(self):
        pass

    def close(self):
        pass

    def doModal(self):
        pass


class WindowDialog(Window):
    pass


class WindowXML(Window):
    def __init__(self, *args, **kwargs):
        Window.__init__(self)


class WindowXMLDialog(WindowXML):
    pass


def getCurrentWindowId():
    return 10000


def getCurrentWindowDialogId():
    return 9999


def getScreenHeight():
    return 1080


def getScreenWidth():
    return 1920
