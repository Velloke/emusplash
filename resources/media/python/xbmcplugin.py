# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  xbmcplugin.py
#
#  Compatibility module for the add-on scripts, the module name is the one the scripts import.
#  Directory listings are not used by the scripts run by EmuSPLASH.
#

SORT_METHOD_NONE = 0
SORT_METHOD_LABEL = 1
SORT_METHOD_LABEL_IGNORE_THE = 2
SORT_METHOD_DATE = 3
SORT_METHOD_TITLE = 9
SORT_METHOD_TITLE_IGNORE_THE = 10
SORT_METHOD_VIDEO_YEAR = 18
SORT_METHOD_VIDEO_RATING = 19
SORT_METHOD_GENRE = 16
SORT_METHOD_UNSORTED = 40


def addDirectoryItem(handle, url, listitem, isFolder=False, totalItems=0):
    return True


def addDirectoryItems(handle, items, totalItems=0):
    return True


def endOfDirectory(handle, succeeded=True, updateListing=False, cacheToDisc=True):
    pass


def setResolvedUrl(handle, succeeded, listitem):
    pass


def addSortMethod(handle, sortMethod, label2Mask=""):
    pass


def setContent(handle, content):
    pass


def setPluginCategory(handle, category):
    pass


def setProperty(handle, key, value):
    pass
