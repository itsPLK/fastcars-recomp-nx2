"""Milestone 3: settings migrations, the shader translator check, maintenance (against the fake
console), and cleanup (on a temporary work folder; Docker is stubbed, so no real image is ever
touched)."""
import json
import pathlib
import tempfile
import unittest
import zipfile

from installer.core import build, install, maintenance, settings, shaders, storage
from installer.core.console import FtpTarget, SdTarget
from installer.tests.fake_console import FakeConsole
from installer.tests.test_console import APP, make_work
from installer.tests.test_core import Recorder

OLD = """--log_file=logs/fastcars.log --log_level=error
--fastcars_fps_limit=60
--fastcars_native_render_scale=0.9
--fastcars_native_old_option=1
# Options: remove the first # of a line to use it.
"""
NEW = """--log_file=logs/fastcars.log --log_level=error
--fastcars_fps_limit=60
--fastcars_native_render_scale=1
--fastcars_native_new_option=2
# Options: remove the first # of a line to use it.
"""


class MigrateTest(unittest.TestCase):
    def test_untouched_follow_new_defaults(self):
        out = settings.migrate(OLD, OLD, NEW)
        found = settings.parse(out)
        self.assertEqual(found["--fastcars_native_render_scale"], "1")      # default changed
        self.assertNotIn("--fastcars_native_old_option", found)             # dropped, untouched
        self.assertEqual(found["--fastcars_native_new_option"], "2")        # new in the template

    def test_players_own_values_stay(self):
        mine = OLD.replace("render_scale=0.9", "render_scale=0.75").replace("old_option=1", "old_option=5")
        mine += "--fastcars_native_fps=true\n"
        found = settings.parse(settings.migrate(mine, OLD, NEW))
        self.assertEqual(found["--fastcars_native_render_scale"], "0.75")
        self.assertEqual(found["--fastcars_native_old_option"], "5")
        self.assertEqual(found["--fastcars_native_fps"], "true")

    def test_without_old_template_only_the_table(self):
        mine = OLD + "--fastcars_native_renamed_from=7\n--fastcars_native_retired=1\n"
        table = [{"version": "v9", "rename": {"--fastcars_native_renamed_from": "--fastcars_native_renamed_to"},
                  "remove": ["--fastcars_native_retired"]}]
        found = settings.parse(settings.migrate(mine, None, NEW, table))
        self.assertEqual(found["--fastcars_native_renamed_to"], "7")
        self.assertNotIn("--fastcars_native_renamed_from", found)
        self.assertNotIn("--fastcars_native_retired", found)
        self.assertEqual(found["--fastcars_native_render_scale"], "0.9")    # no template: untouched

    def test_install_records_and_applies_the_template(self):
        with tempfile.TemporaryDirectory() as d:
            work = make_work(pathlib.Path(d) / "work")
            (work / "out" / APP / "args.txt").write_text(OLD)
            sd = pathlib.Path(d) / "sd"
            sd.mkdir()
            t = SdTarget(sd)
            install.install(work, t, Recorder())
            self.assertEqual(install.read_installed(t)["args_template"], OLD)
            settings.change(t, {"fps_counter": "on"})                         # the player's change
            (work / "out" / APP / "args.txt").write_text(NEW)                # the update's template
            install.install(work, t, Recorder())
            found = settings.parse((sd / APP / "args.txt").read_text())
            self.assertEqual(found["--fastcars_native_render_scale"], "1")
            self.assertEqual(found["--fastcars_native_fps"], "true")
            self.assertEqual(install.read_installed(t)["args_template"], NEW)


class TranslatorTest(unittest.TestCase):
    def setUp(self):
        self.saved = build.translate_shaders
        self.calls = 0

        def fake_translate(work, reporter):  # stands in for the Docker translation
            self.calls += 1
            spirv = pathlib.Path(work) / "out" / "spirv"
            spirv.mkdir(parents=True, exist_ok=True)
            for b in (pathlib.Path(work) / "shader_cache").glob("*.bin"):
                (spirv / (b.stem + ".spv")).write_bytes(b"spv-" + b.read_bytes())
        build.translate_shaders = fake_translate

    def tearDown(self):
        build.translate_shaders = self.saved

    def test_retranslates_only_when_the_translator_changed(self):
        with tempfile.TemporaryDirectory() as d:
            work = make_work(pathlib.Path(d) / "work")
            (work / "shader_cache").mkdir()
            (work / "shader_cache" / "vs_1.bin").write_bytes(b"microcode")
            sd = pathlib.Path(d) / "sd"
            sd.mkdir()
            t = SdTarget(sd)
            self.assertFalse(shaders.ensure_current(work, t, Recorder()))   # nothing translated yet
            shaders._translate_all(work, Recorder())
            self.assertEqual(shaders.local_translator(work), shaders.translator_id())
            self.assertFalse(shaders.ensure_current(work, t, Recorder()))   # current
            shaders._marker(work).write_text("oldtranslator\n")
            calls = self.calls
            self.assertTrue(shaders.ensure_current(work, t, Recorder()))    # changed: again
            self.assertEqual(self.calls, calls + 1)
            install.install(work, t, Recorder())
            self.assertEqual(install.read_installed(t)["translator"], shaders.translator_id())
            self.assertTrue((sd / APP / "data/spirv/vs_1.spv").is_file())

    def test_unmarked_local_shaders_count_as_current(self):
        with tempfile.TemporaryDirectory() as d:
            work = make_work(pathlib.Path(d) / "work")
            (work / "out" / "spirv").mkdir()
            (work / "out" / "spirv" / "vs_1.spv").write_bytes(b"spv")
            t = SdTarget(pathlib.Path(d))
            self.assertFalse(shaders.ensure_current(work, t, Recorder()))
            self.assertEqual(shaders.local_translator(work), shaders.translator_id())


class MaintenanceTest(unittest.TestCase):
    def _console(self, root):
        app = pathlib.Path(root) / APP
        for rel, data in (("fastcars.nro", b"n"), ("args.txt", b"--log_level=error\n"), ("install.json", b"{}"),
                          ("logs/fastcars.log", b"log"), ("logs/old-name.log", b"other app"),
                          ("data/game/default.xex", b"x"), ("data/spirv/vs.spv", b"s"),
                          ("data/user/0000000000000000/545408A7/00000002/545408A700000000/a.rpf", b"i" * 1000),
                          ("data/user/B13EBABEBABEBABE/545408A7/00000001/SAVE/save", b"progress"),
                          ("data/user/545408A7/profile/User", b"p")):
            p = app / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        return app

    def test_backup_logs_uninstall_over_ftp(self):
        with tempfile.TemporaryDirectory() as d:
            sd = pathlib.Path(d) / "sd"
            app = self._console(sd)
            with FakeConsole(sd) as c:
                t = FtpTarget("127.0.0.1", c.port)
                dest = maintenance.backup_saves(t, pathlib.Path(d) / "backups", Recorder())
                got = sorted(p.relative_to(dest).as_posix() for p in dest.rglob("*") if p.is_file())
                self.assertEqual(got, ["545408A7/profile/User", "B13EBABEBABEBABE/545408A7/00000001/SAVE/save"])
                z = maintenance.collect_logs(t, pathlib.Path(d) / "logs", Recorder())
                self.assertEqual(sorted(zipfile.ZipFile(z).namelist()), ["args.txt", "install.json", "logs/fastcars.log"])
                maintenance.uninstall(t, Recorder())
                left = sorted(p.relative_to(app).as_posix() for p in app.rglob("*") if p.is_file())
                self.assertTrue(left and all(p.startswith("data/user/") for p in left))
                maintenance.uninstall(t, Recorder(), delete_saves=True)
                self.assertFalse(app.exists())
                t.close()


class StorageTest(unittest.TestCase):
    def setUp(self):
        self.saved = storage.images
        storage.images = lambda: [("fastcars-builder:old", 10, False), ("fastcars-builder:cur", 20, True)]
        self.removed = []
        self.saved_run = storage.subprocess.run
        storage.subprocess.run = lambda cmd, **kw: self.removed.append(cmd[-1])

    def tearDown(self):
        storage.images = self.saved
        storage.subprocess.run = self.saved_run

    def _work(self, d):
        work = make_work(pathlib.Path(d) / "work")
        for rel in ("build/x.o", "codegen/y.cpp", "src/z.h", "out/spirv/vs.spv", "shader_cache/vs.bin"):
            p = work / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"1234")
        (work / "out" / "spirv.translator").write_text("t\n")
        return work

    def test_levels(self):
        with tempfile.TemporaryDirectory() as d:
            work = self._work(d)
            storage.clean(work, "tidy", Recorder())
            self.assertEqual(self.removed, ["fastcars-builder:old"])
            self.assertTrue((work / "build").exists())
            storage.clean(work, "build", Recorder())
            for gone in ("build", "codegen", "src", "out/switch", "out/BUILT"):
                self.assertFalse((work / gone).exists(), gone)
            for kept in ("data", "out/spirv", "out/spirv.translator", "shader_cache"):
                self.assertTrue((work / kept).exists(), kept)
            storage.clean(work, "all", Recorder())
            self.assertFalse((work / "data").exists())
            self.assertFalse((work / "out").exists())
            self.assertTrue((work / "shader_cache" / "vs.bin").exists())
            storage.clean(work, "all", Recorder(), delete_shaders=True)
            self.assertFalse((work / "shader_cache").exists())


if __name__ == "__main__":
    unittest.main()
