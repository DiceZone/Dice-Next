"""Keep application snapshots compatible with the pinned macOS libc++ SDK."""
import re
import unittest
from pathlib import Path


UNSUPPORTED_ATOMIC = re.compile(r"\bstd\s*::\s*atomic\s*<\s*std\s*::\s*shared_ptr\s*<")


class SharedPtrPortabilityTests(unittest.TestCase):
    def test_guard_catches_multiline_specializations_not_supported_by_xcode_15(self):
        self.assertRegex("std::atomic<std::shared_ptr<State>> state;", UNSUPPORTED_ATOMIC)
        self.assertRegex("std::atomic<\n std::shared_ptr<const Table>\n> snapshot;", UNSUPPORTED_ATOMIC)
        self.assertNotRegex("std::atomic<bool> active;", UNSUPPORTED_ATOMIC)
        self.assertNotRegex("std::shared_ptr<State> state; std::atomic_load(&state);", UNSUPPORTED_ATOMIC)

    def test_application_does_not_require_cxx20_atomic_shared_ptr_specialization(self):
        source = Path(__file__).resolve().parents[2] / "server/src"
        files = sorted(path for path in source.rglob("*") if path.suffix in {".h", ".cpp"})
        self.assertTrue(files, "application sources must be checked")
        unsupported = [path.relative_to(source).as_posix() for path in files
                       if UNSUPPORTED_ATOMIC.search(path.read_text(encoding="utf-8"))]
        self.assertEqual(unsupported, [], "Use shared_ptr atomic free functions for Xcode 15.4 compatibility")


if __name__ == "__main__":
    unittest.main()
