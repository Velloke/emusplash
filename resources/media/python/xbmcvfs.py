# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  xbmcvfs.py
#
#  Compatibility module for the add-on scripts, the module name is the one the scripts import.
#

import os
import shutil

import _host


def translatePath(path):
    return _host.translate_path(path)


def exists(path):
    return os.path.exists(translatePath(path))


def mkdir(path):
    try:
        os.mkdir(translatePath(path))
        return True
    except OSError:
        return False


def mkdirs(path):
    try:
        os.makedirs(translatePath(path), exist_ok=True)
        return True
    except OSError:
        return False


def rmdir(path, force=False):
    try:
        if force:
            shutil.rmtree(translatePath(path))
        else:
            os.rmdir(translatePath(path))
        return True
    except OSError:
        return False


def delete(path):
    try:
        os.remove(translatePath(path))
        return True
    except OSError:
        return False


def rename(source, destination):
    try:
        os.rename(translatePath(source), translatePath(destination))
        return True
    except OSError:
        return False


def copy(source, destination):
    try:
        shutil.copyfile(translatePath(source), translatePath(destination))
        return True
    except (OSError, IOError):
        return False


def listdir(path):
    path = translatePath(path)
    folders = []
    files = []
    try:
        for name in os.listdir(path):
            if os.path.isdir(os.path.join(path, name)):
                folders.append(name)
            else:
                files.append(name)
    except OSError:
        pass
    return folders, files


def makeLegalFilename(filename):
    return filename


def validatePath(path):
    return path


class File(object):
    def __init__(self, path, mode="r"):
        binary = "b" if "b" not in mode else ""
        self._file = open(translatePath(path), mode + binary if mode in ("r", "w") else mode)

    def read(self, size=-1):
        data = self._file.read(size)
        return data.decode("utf-8", "ignore") if isinstance(data, bytes) else data

    def readBytes(self, size=-1):
        return self._file.read(size)

    def write(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self._file.write(data)
        return True

    def size(self):
        position = self._file.tell()
        self._file.seek(0, os.SEEK_END)
        size = self._file.tell()
        self._file.seek(position)
        return size

    def seek(self, offset, whence=0):
        return self._file.seek(offset, whence)

    def close(self):
        self._file.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Stat(object):
    def __init__(self, path):
        self._stat = os.stat(translatePath(path))

    def st_size(self):
        return self._stat.st_size

    def st_mtime(self):
        return self._stat.st_mtime
