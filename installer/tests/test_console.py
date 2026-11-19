"""The installer against a fake console (installer/tests/fake_console.py): FTP operations, a
dropped upload, a full install and an update over it, settings, the network probe."""
import json
import os
import pathlib
import tempfile
import unittest

from installer.core import console, install, settings
from installer.core.console import FtpTarget
from installer.tests.fake_console import FakeConsole
from installer.tests.test_core import Recorder

APP = "switch/fastcars"


def make_work(root, nro=b"NRO1" * 1000):
    """A minimal work folder: a built NRO, default settings, game files and saves."""
    w = pathlib.Path(root)
    out = w / "out" / APP
    out.mkdir(parents=True)
    (out / "fastcars.nro").write_bytes(nro)
    (out / "args.txt").write_text("--fastcars_fps_limit=60\n# Options: remove the first # of a line to use it.\n")
    (w / "out" / "BUILT").write_text("v1.2.3-abcdef12\n")
    for rel, data in (("data/game/default.xex", b"x" * 3000), ("data/game/common/a.rpf", b"a" * 500),
                      ("data/update/update.rpf", b"u" * 700), ("data/user/content/save", b"disc1")):
        p = w / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return w


class FtpTargetTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.sd = pathlib.Path(self.tmp.name) / "sd"
        self.sd.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_operations(self):
        local = pathlib.Path(self.tmp.name) / "f.bin"
        local.write_bytes(b"z" * 5000)
        with FakeConsole(self.sd) as c:
            t = FtpTarget("127.0.0.1", c.port)
            t.put(local, f"{APP}/data/f.bin")
            self.assertEqual(t.size(f"{APP}/data/f.bin"), 5000)
            self.assertFalse((self.sd / APP / "data" / "f.bin.part").exists())
            self.assertTrue(t.exists(f"{APP}/data/f.bin"))
            self.assertEqual(t.listdir(f"{APP}/data"), ["f.bin"])
            self.assertIsNone(t.size(f"{APP}/missing"))
            local.write_bytes(b"y" * 10)  # replacing an existing file (the console can't rename over one)
            t.put(local, f"{APP}/data/f.bin")
            self.assertEqual((self.sd / APP / "data" / "f.bin").read_bytes(), b"y" * 10)
            t.write_text(f"{APP}/x.json", '{"a": 1}\n')
            self.assertEqual(t.read_text(f"{APP}/x.json"), '{"a": 1}\n')
            self.assertIsNone(t.read_text(f"{APP}/nope.json"))
            back = pathlib.Path(self.tmp.name) / "back.bin"
            t.get(f"{APP}/data/f.bin", back)
            self.assertEqual(back.read_bytes(), b"y" * 10)
            t.delete(f"{APP}/data/f.bin")
            self.assertEqual(t.listdir(f"{APP}/data"), [])
            t.close()

    def test_dropped_upload_is_retried(self):
        local = pathlib.Path(self.tmp.name) / "big.nro"
        local.write_bytes(os.urandom(3 << 20))
        with FakeConsole(self.sd, drop_uploads=1) as c:
            t = FtpTarget("127.0.0.1", c.port)
            console.time.sleep, saved = (lambda s: None), console.time.sleep  # no waiting in tests
            try:
                t.put(local, f"{APP}/fastcars.nro")
            finally:
                console.time.sleep = saved
            t.close()
            self.assertEqual(c.server.drop_uploads, 0)  # the drop really happened
        self.assertEqual((self.sd / APP / "fastcars.nro").read_bytes(), local.read_bytes())
        self.assertEqual(list((self.sd / APP).glob("*.part")), [])

    def test_probe(self):
        with FakeConsole(self.sd) as c:
            self.assertEqual(console.probe_ftp("127.0.0.1", c.port), "220 Hello!")
        self.assertIsNone(console.probe_ftp("127.0.0.1", c.port, timeout=0.3))  # closed now


class InstallTest(unittest.TestCase):
    def test_fresh_install_then_update(self):
        with tempfile.TemporaryDirectory() as d:
            work = make_work(pathlib.Path(d) / "work")
            sd = pathlib.Path(d) / "sd"
            sd.mkdir()
            with FakeConsole(sd) as c:
                t = FtpTarget("127.0.0.1", c.port)
                state = install.install(work, t, Recorder())
                self.assertEqual(state["version"], "v1.2.3-abcdef12")
                self.assertFalse(state["shaders_on_console"])
                for rel in ("fastcars.nro", "args.txt", "install.json", "data/game/default.xex",
                            "data/game/common/a.rpf", "data/update/update.rpf", "data/user/content/save"):
                    self.assertTrue((sd / APP / rel).is_file(), rel)
                self.assertEqual(install.read_installed(t)["version"], "v1.2.3-abcdef12")

                # The player plays (saves change) and changes a setting; then an update.
                (sd / APP / "data/user/content/save").write_bytes(b"my progress")
                settings.change(t, {"fps_counter": "on"})
                (work / "out" / APP / "fastcars.nro").write_bytes(b"NRO2" * 1000)
                (work / "out" / "BUILT").write_text("v1.3.0-12345678\n")
                (sd / APP / "data/spirv").mkdir(parents=True)
                (sd / APP / "data/spirv/vs_1.spv").write_bytes(b"spv")
                uploads = c.uploads
                state = install.install(work, t, Recorder())
                # Only the NRO and install.json went up: game files matched, saves and settings kept.
                self.assertEqual(c.uploads - uploads, 2)
                self.assertEqual((sd / APP / "fastcars.nro").read_bytes(), b"NRO2" * 1000)
                self.assertEqual((sd / APP / "data/user/content/save").read_bytes(), b"my progress")
                self.assertIn("--fastcars_native_fps=true", (sd / APP / "args.txt").read_text())
                self.assertTrue(state["shaders_on_console"])
                self.assertEqual(json.loads((sd / APP / "install.json").read_text())["version"],
                                 "v1.3.0-12345678")
                t.close()

    def test_reset_settings(self):
        with tempfile.TemporaryDirectory() as d:
            work = make_work(pathlib.Path(d) / "work")
            sd = pathlib.Path(d) / "sd"
            (sd / APP).mkdir(parents=True)
            (sd / APP / "args.txt").write_text("--my_own_setting=1\n")
            with FakeConsole(sd) as c:
                t = FtpTarget("127.0.0.1", c.port)
                install.install(work, t, Recorder())
                self.assertEqual((sd / APP / "args.txt").read_text(), "--my_own_setting=1\n")
                install.install(work, t, Recorder(), reset_settings=True)
                self.assertIn("--fastcars_fps_limit=60", (sd / APP / "args.txt").read_text())
                t.close()


if __name__ == "__main__":
    unittest.main()
