"""Keep selective packaging builds from dropping required executables."""
import pathlib
import re
import unittest


class ReleaseTargetsTests(unittest.TestCase):
    def test_every_platform_checks_the_actual_package_before_upload(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/release.yml").read_text(encoding="utf-8")
        for job, following in (("linux", "windows"), ("windows", "macos-arm64"),
                               ("macos-arm64", "release")):
            with self.subTest(job=job):
                steps = workflow.split(f"  {job}:", 1)[1].split(f"  {following}:", 1)[0]
                smoke = steps.index("node tools/test-webui-package.mjs release")
                self.assertLess(steps.index("- name: Package"), smoke)
                self.assertLess(smoke, steps.index("uses: actions/upload-artifact@v4"))
                self.assertIn("uses: actions/setup-node@", steps[:smoke])

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
