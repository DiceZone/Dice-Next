"""Keep selective packaging builds from dropping required executables."""
import pathlib
import re
import unittest


class ReleaseTargetsTests(unittest.TestCase):
    def test_macos_builds_manager_core_and_mcp_probe(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/release.yml").read_text(encoding="utf-8")
        macos = workflow.split("  macos-arm64:", 1)[1].split("  release:", 1)[0]
        command = re.search(r"cmake --build server/build --target (.*?) --parallel", macos)
        self.assertIsNotNone(command)
        self.assertTrue({"dice-next-server", "dice-next-launcher", "dice-next-mcp-probe"}
                        <= set(command.group(1).split()))


if __name__ == "__main__":
    unittest.main()
