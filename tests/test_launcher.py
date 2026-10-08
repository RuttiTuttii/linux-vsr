#!/usr/bin/env python3
# regression checks for launcher argument and configuration precedence

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LauncherTests(unittest.TestCase):
    def run_launcher(self, args, overrides=None):
        # isolate persistent configuration and inherited tuning overrides
        with tempfile.TemporaryDirectory(prefix="vsr-launcher-") as tmp:
            config = Path(tmp) / "config.ini"
            config.write_text("enable=1\ndebug=1\nmode=easu\nsharpness=0.31\nbackend=amd\n")
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith("VSR_") and key != "LD_PRELOAD"}
            env["VSR_CONFIG"] = str(config)
            env["XDG_CONFIG_HOME"] = tmp
            env.update(overrides or {})
            # capture exactly what the launched application receives
            probe = "import json,os,sys; print(json.dumps({'args':sys.argv[1:],'mode':os.getenv('VSR_MODE'),'vaapi':os.getenv('LIBVA_DRIVER_NAME')}))"
            result = subprocess.run(
                [str(ROOT / "bin/linux-vsr"), *args, sys.executable, "-c", probe, "a b", "--flag"],
                env=env, text=True, capture_output=True, check=True,
            )
            return result, json.loads(result.stdout.splitlines()[-1])

    def test_saved_configuration(self):
        # launcher must not overwrite saved file values with environment defaults
        result, payload = self.run_launcher(["launch"])
        self.assertIsNone(payload["mode"])
        self.assertIn("mode: easu, sharpness: 0.31", result.stderr)
        self.assertEqual(payload["args"], ["a b", "--flag"])

    def test_explicit_overrides(self):
        # explicit user environment remains stronger than the saved configuration
        result, payload = self.run_launcher(["launch"], {
            "VSR_MODE": "directional", "LIBVA_DRIVER_NAME": "custom-driver",
        })
        self.assertEqual(payload["mode"], "directional")
        self.assertEqual(payload["vaapi"], "custom-driver")
        self.assertIn("mode: directional", result.stderr)

    def test_shorthand_arguments(self):
        # shorthand invocation must not duplicate the executable as its first argument
        _, payload = self.run_launcher([])
        self.assertEqual(payload["args"], ["a b", "--flag"])


if __name__ == "__main__":
    unittest.main()
