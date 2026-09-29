import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "cache_snapshot", Path(__file__).with_name("cache-snapshot.py"))
cache_snapshot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache_snapshot)


class CacheSnapshotTests(unittest.TestCase):
    def test_missing_cache_matches_empty_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(cache_snapshot.snapshot(root / "absent"),
                             cache_snapshot.snapshot(root))

    def test_new_abi_changes_snapshot_but_timestamp_does_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package = root / "first-abi.zip"
            package.write_bytes(b"binary package")
            before = cache_snapshot.snapshot(root)
            os.utime(package, (100, 100))
            self.assertEqual(before, cache_snapshot.snapshot(root))
            (root / "second-abi.zip").write_bytes(b"new compiler package")
            self.assertNotEqual(before, cache_snapshot.snapshot(root))

    def test_order_and_unrelated_files_do_not_change_snapshot(self):
        with tempfile.TemporaryDirectory() as left, tempfile.TemporaryDirectory() as right:
            first, second = Path(left), Path(right)
            for name in ("a.zip", "b.zip"):
                (first / name).write_bytes(b"package")
            for name in ("b.zip", "a.zip"):
                (second / name).write_bytes(b"package")
            (second / "diagnostic.log").write_text("ignored", encoding="utf-8")
            self.assertEqual(cache_snapshot.snapshot(first),
                             cache_snapshot.snapshot(second))


if __name__ == "__main__":
    unittest.main()
