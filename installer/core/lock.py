"""A lock on the work folder, so two actions (a build and a shader fetch, say) never run at once."""
import contextlib
import os
import pathlib
import sys

from .report import InstallError


@contextlib.contextmanager
def work_lock(work):
    work = pathlib.Path(work)
    work.mkdir(parents=True, exist_ok=True)
    f = open(work / ".lock", "a+")
    try:
        try:
            if sys.platform == "win32":
                import msvcrt
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise InstallError("Another action is still running on this work folder. Wait for it "
                               "to finish (or close the other window) and try again.")
        f.seek(0)
        f.truncate()
        f.write(str(os.getpid()))
        f.flush()
        yield
    finally:
        f.close()
