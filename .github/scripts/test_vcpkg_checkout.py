"""Guard the Git history required to resolve pinned vcpkg port versions."""
import re
import unittest
from pathlib import Path


class VcpkgCheckoutTests(unittest.TestCase):
    def test_pinned_vcpkg_checkout_fetches_complete_history(self):
        action = (Path(__file__).resolve().parents[1] /
                  "actions/setup-vcpkg/action.yml").read_text(encoding="utf-8")
        steps = re.split(r"(?m)^    - ", action)
        checkouts = [step for step in steps
                     if re.search(r"(?m)^        repository: microsoft/vcpkg\s*$", step)]
        self.assertEqual(len(checkouts), 1)
        self.assertRegex(checkouts[0], r"(?m)^      uses: actions/checkout@")
        self.assertRegex(checkouts[0], r"(?m)^        ref: \$\{\{ inputs.baseline \}\}\s*$")
        # Version overrides (e.g. Lua 5.4.8) reference trees from older commits.
        # checkout's default depth=1 contains only the current port definitions.
        self.assertRegex(checkouts[0], r"(?m)^        fetch-depth: 0\s*$",
                         "Pinned vcpkg ports require full Git history, not a shallow checkout")


if __name__ == "__main__":
    unittest.main()
