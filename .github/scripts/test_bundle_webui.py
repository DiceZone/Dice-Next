import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("bundle_webui", Path(__file__).with_name("bundle-webui.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class BundleTest(unittest.TestCase):
    def test_both_bundles_assets_sources_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for version in ("v1", "v2"):
                (root / version / "assets").mkdir(parents=True)
                (root / version / "index.html").write_text('<script src="./assets/app.js"></script>')
                (root / version / "assets/app.js").write_text(version)
            module.bundle(root / "v1", root / "v2", root / "out", "a" * 40, "b" * 40)
            for version in ("v1", "v2"):
                self.assertEqual((root / "out" / version / "assets/app.js").read_text(), version)
            record = json.loads((root / "out/webui-versions.json").read_text())
            self.assertTrue(record["versions"]["v2"]["preview"])
            self.assertEqual(record["versions"]["v2"]["sha"], "b" * 40)
            with self.assertRaises(ValueError):
                module.bundle(root / "v1", root / "v2", root / "out")
            (root / "v2/index.html").write_text('<script src="/assets/app.js"></script>')
            with self.assertRaises(ValueError):
                module.bundle(root / "v1", root / "v2", root / "invalid")
            (root / "v2/index.html").unlink()
            with self.assertRaises(ValueError):
                module.bundle(root / "v1", root / "v2", root / "missing")


if __name__ == "__main__":
    unittest.main()
