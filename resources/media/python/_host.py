# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  _host.py
#
#  Connection from the add-on scripts to EmuSPLASH, which runs a JSON-RPC server on the local
#  host. Also translates the special:// paths used by the add-ons into real paths.
#

import json
import os
import socket
import sys
import threading

ADDON_ID = os.environ.get("EMUSPLASH_ADDON_ID", "")
HOME = os.environ.get("EMUSPLASH_ADDON_HOME", os.path.expanduser("~/EmuSPLASH/Elementum"))
RPC_PORT = int(os.environ.get("EMUSPLASH_RPC_PORT", "65221"))
LANGUAGE = os.environ.get("EMUSPLASH_LANGUAGE", "en")
BUILD_VERSION = "21.2 (21.2.0) Git:20250101-emusplash"


class _Connection(object):
    def __init__(self):
        self._lock = threading.Lock()
        self._socket = None
        self._buffer = b""
        self._id = 0

    def _connect(self):
        self._socket = socket.create_connection(("127.0.0.1", RPC_PORT), timeout=None)
        self._buffer = b""

    def call(self, method, *params):
        with self._lock:
            for attempt in range(2):
                try:
                    if self._socket is None:
                        self._connect()
                    self._id += 1
                    request = {"jsonrpc": "2.0", "method": method, "params": list(params),
                               "id": self._id}
                    self._socket.sendall(json.dumps(request).encode("utf-8") + b"\n")
                    while b"\n" not in self._buffer:
                        chunk = self._socket.recv(65536)
                        if not chunk:
                            raise IOError("Connection closed")
                        self._buffer += chunk
                    line, self._buffer = self._buffer.split(b"\n", 1)
                    response = json.loads(line.decode("utf-8"))
                    return response.get("result")
                except Exception as e:
                    try:
                        self._socket.close()
                    except Exception:
                        pass
                    self._socket = None
                    if attempt == 1:
                        sys.stderr.write("RPC call %s failed: %s\n" % (method, e))
                        return None


_connection = _Connection()


def call(method, *params):
    return _connection.call(method, *params)


def log(message):
    try:
        sys.stderr.write("[%s] %s\n" % (ADDON_ID, message))
        sys.stderr.flush()
    except Exception:
        pass


def translate_path(path):
    if not path:
        return path
    if not isinstance(path, str):
        try:
            path = path.decode("utf-8")
        except Exception:
            path = str(path)
    if not path.startswith("special://"):
        return path
    rest = path[len("special://"):]
    name, _, tail = rest.partition("/")
    addon_data = os.path.join(HOME, "addons-data")
    if name in ("profile", "masterprofile", "userdata"):
        if tail.startswith("addon_data/"):
            return os.path.join(addon_data, tail[len("addon_data/"):])
        if tail == "addon_data":
            return addon_data
        base = os.path.join(HOME, "userdata")
    elif name == "home":
        base = HOME
    elif name == "temp":
        base = os.path.join(HOME, "temp")
    elif name == "logpath":
        base = os.path.join(HOME, "logs")
    elif name in ("xbmc", "xbmcbin", "xbmcbinaddons"):
        base = HOME
    elif name == "skin":
        base = os.path.join(HOME, "skin")
    else:
        base = HOME
    return os.path.join(base, tail) if tail else base + os.sep
