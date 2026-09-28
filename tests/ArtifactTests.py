import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("artifact", Path(__file__).resolve().parents[1] / "scripts/artifact.py")
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / "Holder.exe"
        self.binary.write_bytes(b"fake build output")
        self.symbols = self.root / "Holder.pdb"
        self.symbols.write_bytes(b"debug symbols")
        self.record = self.root / "built.json"
        self.metadata = {"source": {"commit": "a" * 40, "dirty": False},
                         "binary_sha256": artifact.digest(self.binary)}
        self.record.write_text(json.dumps(self.metadata))
        self.output = self.root / "artifact"

    def create(self, allow_dirty=False):
        artifact.create(self.binary, self.record, self.symbols, self.output, allow_dirty)

    def test_round_trip_and_symbols(self):
        self.create()
        info = artifact.verify(self.output)
        self.assertEqual(info["source"]["commit"], "a" * 40)
        self.assertIn("Holder.pdb", info["files"])

    def test_directory_symbols(self):
        self.symbols = self.root / "Holder.dSYM"
        (self.symbols / "Contents" / "Resources" / "DWARF").mkdir(parents=True)
        (self.symbols / "Contents" / "Resources" / "DWARF" / "Holder").write_bytes(b"DWARF")
        self.create()
        self.assertIn("Holder.dSYM/Contents/Resources/DWARF/Holder", artifact.verify(self.output)["files"])

    def test_changed_binary_rejected(self):
        self.binary.write_bytes(b"different build")
        with self.assertRaisesRegex(ValueError, "changed since linking"):
            self.create()

    def test_dirty_source_requires_explicit_override(self):
        self.metadata["source"]["dirty"] = True
        self.record.write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError, "clean, identified"):
            self.create()
        self.create(True)
        self.assertTrue(artifact.verify(self.output)["source"]["dirty"])

    def test_unknown_source_rejected(self):
        self.metadata["source"]["commit"] = "unknown"
        self.record.write_text(json.dumps(self.metadata))
        with self.assertRaisesRegex(ValueError, "clean, identified"):
            self.create()

    def test_tampering_rejected(self):
        self.create()
        (self.output / "Holder.exe").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "checksums"):
            artifact.verify(self.output)

    def test_extra_file_rejected(self):
        self.create()
        (self.output / "extra").write_text("extra")
        with self.assertRaisesRegex(ValueError, "inventory"):
            artifact.verify(self.output)

    def test_existing_artifact_not_overwritten(self):
        self.create()
        with self.assertRaisesRegex(ValueError, "never overwritten"):
            self.create()

    def test_missing_symbols_rejected(self):
        self.symbols.unlink()
        with self.assertRaisesRegex(ValueError, "Missing debug symbols"):
            self.create()


if __name__ == "__main__":
    unittest.main(verbosity=2)
