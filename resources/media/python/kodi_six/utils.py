# -*- coding: utf-8 -*-
#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  utils.py
#
#  Compatibility module for the add-on scripts. Only Python 3 is used, so the text conversion
#  helpers return the text unchanged.
#

import sys

PY2 = sys.version_info[0] == 2


def py2_encode(s, encoding="utf-8", errors="strict"):
    return s


def py2_decode(s, encoding="utf-8", errors="strict"):
    return s


def encode_decode(func):
    return func
