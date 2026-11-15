#!/usr/bin/env python3
"""Run a command with transparent huge pages disabled for it and its children (attribution runs only)."""

import ctypes
import os
import sys

PR_SET_THP_DISABLE = 41

if len(sys.argv) < 2:
    sys.exit("usage: thp_off.py COMMAND [ARGS...]")
libc = ctypes.CDLL(None, use_errno=True)
if libc.prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0) != 0:
    sys.exit(f"prctl failed: {os.strerror(ctypes.get_errno())}")
os.execvp(sys.argv[1], sys.argv[1:])
