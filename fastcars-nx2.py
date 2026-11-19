#!/usr/bin/env python3
"""The previous name of fastcars.py, kept for one release so existing instructions keep working."""
import sys

from installer.cli import main

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["gui"]))
