"""Verify identity refresh without committing or changing the developer repository."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

CMAKE, GIT = sys.argv[1:3]
del sys.argv[1:3]
SOURCE = Path(__file__).resolve().parents[1]


class BuildIdentityTests(unittest.TestCase):
    def test_clean_dirty_new_commit_and_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo, output = root / "repo", root / "build"
            (repo / "cmake").mkdir(parents=True)
            for name in ("BuildIdentity.h.in", "source.json.in"):
                shutil.copy2(SOURCE / "cmake" / name, repo / "cmake" / name)

            def git(*args):
                return subprocess.check_output([GIT, "-C", str(repo), *args], text=True, stderr=subprocess.STDOUT).strip()

            def capture(git_executable=GIT):
                subprocess.run([CMAKE, f"-DSOURCE_DIR={repo}", f"-DOUTPUT_DIR={output}",
                                f"-DGIT_EXECUTABLE={git_executable}", "-P",
                                str(SOURCE / "cmake" / "WriteBuildIdentity.cmake")], check=True)
                return json.loads((output / "source.json").read_text())

            git("init")
            git("add", ".")
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-m", "fixture")
            initial = git("rev-parse", "HEAD")
            self.assertEqual(capture(), {"commit": initial, "dirty": False})
            header = output / "BuildIdentity.h"
            timestamp = header.stat().st_mtime_ns
            capture()
            self.assertEqual(header.stat().st_mtime_ns, timestamp, "unchanged identity must not trigger rebuilds")
            (repo / "new-file").write_text("untracked change")
            self.assertTrue(capture()["dirty"])
            self.assertIn(initial + "-dirty", header.read_text())
            git("add", ".")
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-m", "next")
            next_commit = git("rev-parse", "HEAD")
            self.assertNotEqual(initial, next_commit)
            self.assertEqual(capture(), {"commit": next_commit, "dirty": False})
            self.assertIn(next_commit, header.read_text())
            self.assertEqual(capture(""), {"commit": "unknown", "dirty": True})


if __name__ == "__main__":
    unittest.main(verbosity=2)
