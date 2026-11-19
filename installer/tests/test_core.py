"""Unit tests for the installer core (no Docker, no console). Run from the repository root:
    python3 -m unittest discover -s installer/tests -t .
"""
import os
import pathlib
import tempfile
import unittest

from installer.core import build, settings, source
from installer.core.console import SdTarget
from installer.core.lock import work_lock
from installer.core.report import InstallError

ARGS = """# fastcars-recomp-nx2: the game's settings
--log_file=logs/fastcars.log --log_level=info --log_max_file_size_mb=16
--fastcars_fps_limit=60
--fastcars_native_render_scale=0.9   # my own comment
# Options: remove the first # of a line to use it.
# --fastcars_fps_limit=30       # frame rate cap
"""


class Recorder:
    def __init__(self):
        self.lines, self.stages, self.fractions = [], [], []

    def log(self, msg):
        self.lines.append(msg)

    def stage(self, name):
        self.stages.append(name)

    def progress(self, f):
        self.fractions.append(f)

    def warn(self, msg):
        self.lines.append(msg)


class SettingsTest(unittest.TestCase):
    def test_parse_ignores_comments(self):
        found = settings.parse(ARGS)
        self.assertEqual(found["--fastcars_fps_limit"], "60")
        self.assertEqual(found["--log_level"], "info")
        self.assertEqual(found["--fastcars_native_render_scale"], "0.9")

    def test_read_settings(self):
        s = settings.read_settings(ARGS)
        self.assertEqual(s["resolution"], "90")
        self.assertEqual(s["log"], "info")
        self.assertEqual(s["fps_counter"], "off")

    def test_apply_keeps_other_words_and_comments(self):
        out = settings.apply_setting(ARGS, "--log_level", "debug")
        self.assertIn("--log_file=logs/fastcars.log --log_max_file_size_mb=16 --log_level=debug", out)
        out = settings.apply_setting(ARGS, "--fastcars_native_render_scale", "1")
        self.assertIn("--fastcars_native_render_scale=1  # my own comment", out)

    def test_apply_adds_before_options_and_removes(self):
        out = settings.apply_setting(ARGS, "--fastcars_native_fps", "true")
        lines = out.splitlines()
        self.assertLess(lines.index("--fastcars_native_fps=true"),
                        lines.index("# Options: remove the first # of a line to use it."))
        out = settings.apply_setting(ARGS, "--fastcars_fps_limit", None)
        self.assertNotIn("--fastcars_fps_limit=60", out)
        self.assertIn("# --fastcars_fps_limit=30", out)  # the commented example stays

    def test_value_checks(self):
        self.assertEqual(settings.value_for("resolution", "85"), ("--fastcars_native_render_scale", "0.85"))
        with self.assertRaises(InstallError):
            settings.value_for("resolution", "40")
        with self.assertRaises(InstallError):
            settings.value_for("taa", "maybe")


class SdTargetTest(unittest.TestCase):
    def test_put_replaces_atomically(self):
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d) / "sd"
            root.mkdir()
            local = pathlib.Path(d) / "game.nro"
            local.write_bytes(b"x" * 1000)
            t = SdTarget(root)
            t.put(local, "switch/fastcars/fastcars.nro")
            self.assertEqual(t.size("switch/fastcars/fastcars.nro"), 1000)
            self.assertFalse((root / "switch/fastcars/fastcars.nro.part").exists())
            local.write_bytes(b"y" * 10)
            t.put(local, "switch/fastcars/fastcars.nro")
            self.assertEqual((root / "switch/fastcars/fastcars.nro").read_bytes(), b"y" * 10)
            t.write_text("switch/fastcars/install.json", "{}\n")
            self.assertEqual(t.read_text("switch/fastcars/install.json"), "{}\n")
            self.assertEqual(t.listdir("switch/fastcars"), ["fastcars.nro", "install.json"])

    def test_missing_sd(self):
        with self.assertRaises(InstallError):
            SdTarget("/nonexistent/sd/card")


class LockTest(unittest.TestCase):
    def test_second_lock_fails(self):
        with tempfile.TemporaryDirectory() as d:
            with work_lock(d):
                if os.name == "nt":
                    self.skipTest("Windows locks are per process")
                # flock is per open file description: a second open in this process conflicts too.
                with self.assertRaises(InstallError):
                    with work_lock(d):
                        pass
            with work_lock(d):  # free again
                pass


class StageParserTest(unittest.TestCase):
    def test_stages_and_progress(self):
        r = Recorder()
        on_line = build._stage_parser(r)
        self.assertFalse(on_line("== Recompiling the game executable to C++ (several minutes)"))
        self.assertIn("Recompiling the game code", r.stages)
        self.assertTrue(on_line("PROGRESS 50"))  # consumed
        on_line("== Building fastcars.nro (the first build takes a while)")
        on_line("[500/1000] Building CXX object x.cpp.obj")
        self.assertAlmostEqual(r.fractions[-1], 0.5 + 0.5 * 0.5)


class SourceTest(unittest.TestCase):
    def test_files_and_version(self):
        files = source.source_files()
        self.assertIn("fastcars.py", files)
        self.assertIn("switch2/builder/build.sh", files)
        self.assertFalse(any(f.startswith(("rexglue-sdk/", "thirdparty/", "logs/", ".git/")) for f in files))
        self.assertTrue(source.version().startswith("v"))


if __name__ == "__main__":
    unittest.main()
