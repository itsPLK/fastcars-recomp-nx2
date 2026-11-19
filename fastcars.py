#!/usr/bin/env python3
"""fastcars: builds the game from your own Xbox 360 discs and installs it on a Switch 2 (kaiHEN).
Needs Python 3.9+ and Docker. Without arguments it opens the builder in your web browser; see
`python3 fastcars.py --help` for the command line.

Everything it makes stays in the work folder (python3 fastcars.py status shows where). It is yours
alone: never share it, nor the fastcars.nro it produces (they contain the publisher's copyrighted
code).
"""
import sys

if sys.version_info < (3, 9):
    sys.exit("Python 3.9 or newer is needed (https://www.python.org/downloads/).")

from installer.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["gui"]))
