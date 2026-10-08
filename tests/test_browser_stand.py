#!/usr/bin/env python3
# unit tests for browser stand classification, without launching a browser

import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("browser_stand", ROOT / "tests/browser_stand.py")
STAND = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STAND)


class BrowserStandTests(unittest.TestCase):
    def test_video_and_patch_classification(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.glsl").write_text("ps_quad_yuv sample_luma_cas")
            (root / "b.glsl").write_text("float color = 1.0;")
            payloads, video, patched = STAND.shader_summary(root)
            self.assertEqual(len(payloads), 2)
            self.assertEqual([p.name for p in video], ["a.glsl"])
            self.assertEqual([p.name for p in patched], ["a.glsl"])

    def test_stats_files_are_ignored(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "stats_1.log").write_text("video=1")
            payloads, video, patched = STAND.shader_summary(root)
            self.assertEqual(payloads, [])
            self.assertEqual(video, [])
            self.assertEqual(patched, [])


if __name__ == "__main__":
    unittest.main()
