"""Keep MCP cancellation compatible with the release runner's Xcode 15.4 SDK."""
import re
import unittest
from pathlib import Path


UNSUPPORTED_STOP = re.compile(
    r"\bstd\s*::\s*(?:stop_token|stop_source|stop_callback|jthread)\b"
    r"|#\s*include\s*<stop_token>"
)


class McpPortabilityTests(unittest.TestCase):
    def test_guard_catches_unavailable_types_and_header(self):
        for source in ("std::stop_token stop;", "std::stop_source shutdown;",
                       "std::stop_callback callback;", "std::jthread worker;",
                       "std ::\n stop_token stop;", "#include <stop_token>"):
            self.assertRegex(source, UNSUPPORTED_STOP)
        self.assertNotRegex("std::condition_variable changed;", UNSUPPORTED_STOP)

    def test_mcp_does_not_require_unavailable_cxx20_cancellation(self):
        root = Path(__file__).resolve().parents[2]
        for name in ("mcp_service.h", "mcp_service.cpp"):
            with self.subTest(source=name):
                source = (root / "server/src/service" / name).read_text(encoding="utf-8")
                self.assertNotRegex(source, UNSUPPORTED_STOP,
                                    "Keep MCP cancellation compatible with Xcode 15.4")


if __name__ == "__main__":
    unittest.main()
