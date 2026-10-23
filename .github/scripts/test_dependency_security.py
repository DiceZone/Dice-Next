"""Keep the Brotli backport and the minimum secure Node runtime in every build."""
import json
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class DependencySecurityTests(unittest.TestCase):
    def test_manifest_uses_the_security_overlay_without_upgrading_other_ports(self):
        manifest = json.loads((ROOT / "server/vcpkg.json").read_text(encoding="utf-8"))
        configuration = json.loads((ROOT / "server/vcpkg-configuration.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["builtin-baseline"], "44819aa2a6c10e56065e2b0330e7d6c89d1d2574")
        self.assertIn("brotli", manifest["dependencies"])
        self.assertIn("vcpkg-overlays", configuration["overlay-ports"])
        overlay = ROOT / "server/vcpkg-overlays/brotli"
        port = json.loads((overlay / "vcpkg.json").read_text(encoding="utf-8"))
        self.assertEqual(port["version"], "1.2.0")
        self.assertGreaterEqual(port["port-version"], 1)
        portfile = (overlay / "portfile.cmake").read_text(encoding="utf-8")
        self.assertIn("block-switch-security.patch", portfile)
        patch = (overlay / "block-switch-security.patch").read_text(encoding="utf-8")
        self.assertIn("0ed4268a4401abcb4472096340477cf4af7d1a2b", patch)
        self.assertIn("+    BROTLI_SAFE_WITH_STATUS(DecodeCommandBlockSwitch(s));", patch)
        for patch_path in overlay.glob("*.patch"):
            result = subprocess.run(["git", "apply", "--numstat", str(patch_path)],
                                    cwd=ROOT, capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_cache_identity_includes_the_configuration_and_every_patch(self):
        action = (ROOT / ".github/actions/setup-vcpkg/action.yml").read_text(encoding="utf-8")
        cache_key = next(line for line in action.splitlines() if "CACHE_FAMILY:" in line)
        self.assertIn("main/server/vcpkg-configuration.json", cache_key)
        self.assertIn("main/server/vcpkg-overlays/**", cache_key)

    def test_classic_build_entry_points_cannot_bypass_the_overlay(self):
        for filename in ["build-linux.sh", "build-macos.sh"]:
            source = (ROOT / "cross-compile/scripts" / filename).read_text(encoding="utf-8")
            self.assertIn('--overlay-ports="$SERVER_DIR/vcpkg-overlays"', source)
        for arch in ["amd64", "arm64"]:
            source = (ROOT / f"cross-compile/docker/Dockerfile.linux-{arch}").read_text(encoding="utf-8")
            self.assertIn("COPY dice-next/server/vcpkg-overlays /opt/dice-next-overlays", source)
            self.assertIn("--overlay-ports=/opt/dice-next-overlays", source)
        windows = (ROOT / "cross-compile/scripts/build-windows.ps1").read_text(encoding="utf-8")
        self.assertIn('--overlay-ports=$serverDir/vcpkg-overlays', windows)

    def test_configuration_and_tests_guard_headers_and_linked_decoder(self):
        cmake = (ROOT / "server/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("BROTLI_DECODER_ERROR_FORMAT_BLOCK_SWITCH == -17", cmake)
        self.assertIn('message(FATAL_ERROR "Brotli is missing', cmake)
        test_cmake = (ROOT / "server/tests/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("test_brotli_security.cpp", test_cmake)
        self.assertIn("unofficial::brotli::brotlidec", test_cmake)

    def test_ci_follows_the_latest_node_major_line_but_has_a_security_floor(self):
        self.assertEqual((ROOT / ".node-version").read_text(encoding="utf-8").strip(), "26.12.1")
        for filename in ["release.yml", "update-checks.yml"]:
            workflow = (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            self.assertRegex(workflow, r"(?m)^  NODE_VERSION: '26\.x'$",
                             filename)
            steps = re.split(r"(?m)^      - ", workflow)
            node_steps = [step for step in steps if "uses: actions/setup-node@" in step]
            self.assertTrue(node_steps, filename)
            for step in node_steps:
                self.assertIn("node-version: ${{ env.NODE_VERSION }}", step)
                self.assertIn("check-latest: true", step)
            self.assertIn("node scripts/check-node-version.mjs", workflow)
            self.assertIn("node tools/check-node-version.mjs", workflow)


if __name__ == "__main__":
    unittest.main()
