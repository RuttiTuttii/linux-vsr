#!/usr/bin/env python3
# integration checks for per-run diagnostic paths and disabled mode

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # keep the fake driver and probe separate from application tests
        cls.temp = tempfile.TemporaryDirectory(prefix="vsr-hook-fixture-")
        cls.directory = Path(cls.temp.name)
        compiler = shlex.split(os.environ.get("CC", "cc"))
        driver = cls.directory / "libdriver.so"
        subprocess.run([*compiler, "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(ROOT / "tests/fixtures/gl_driver.c"), "-o", str(driver)], check=True)
        cls.probe = cls.directory / "probe"
        subprocess.run([*compiler, "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests/fixtures/hook_probe.c"), str(driver),
                        "-o", str(cls.probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, directory, mode="cas", fail=False):
        # remove inherited overrides and write only into the private run directory
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("VSR_") and key != "LD_PRELOAD"}
        env.update({"LD_PRELOAD": str(ROOT / "libvsr.so"), "VSR_CONFIG": str(directory / "absent.ini"),
                    "VSR_MODE": mode, "VSR_DUMP": "1", "VSR_BACKEND": "generic",
                    "VSR_DUMP_DIR": str(directory / "shaders"), "VSR_HIT_LOG": str(directory / "hits.log"),
                    "VSR_COMPILE_LOG": str(directory / "compile.log"), "VSR_TEST_SOURCE": str(directory / "source.glsl")})
        if fail:
            env["VSR_TEST_FAIL"] = "1"
        subprocess.run([str(self.probe)], env=env, capture_output=True, check=True)

    def test_isolated_patch_and_dump(self):
        with tempfile.TemporaryDirectory(prefix="vsr-diagnostics-") as tmp:
            directory = Path(tmp)
            self.run_probe(directory)
            self.assertIn("sample_luma_cas", (directory / "source.glsl").read_text())
            self.assertIn("backend=generic", (directory / "hits.log").read_text())
            self.assertEqual(len(list((directory / "shaders").glob("*.glsl"))), 1)
            self.assertFalse((directory / "compile.log").exists())

    def test_compile_error_path(self):
        with tempfile.TemporaryDirectory(prefix="vsr-diagnostics-") as tmp:
            directory = Path(tmp)
            self.run_probe(directory, fail=True)
            self.assertIn("fixture compile failure", (directory / "compile.log").read_text())

    def test_off_bypasses_patch(self):
        with tempfile.TemporaryDirectory(prefix="vsr-diagnostics-") as tmp:
            directory = Path(tmp)
            self.run_probe(directory, mode="off")
            self.assertNotIn("sample_luma_cas", (directory / "source.glsl").read_text())
            self.assertFalse((directory / "hits.log").exists())


if __name__ == "__main__":
    unittest.main()
