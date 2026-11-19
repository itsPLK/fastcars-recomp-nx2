"""Progress reporting. Core functions take a Reporter; the CLI prints, the wizard streams to the page."""
import sys


class InstallError(Exception):
    """A failure the player can act on; its message is shown as is."""


class Reporter:
    """Prints to stdout. PROGRESS lines are machine-readable (the browser page parses them)."""

    def log(self, msg):
        print(msg, flush=True)

    def stage(self, name):
        print(f"STAGE {name}", flush=True)

    def progress(self, fraction):
        print(f"PROGRESS {int(max(0.0, min(fraction, 1.0)) * 100)}", flush=True)

    def warn(self, msg):
        print(f"warning: {msg}", file=sys.stderr, flush=True)
