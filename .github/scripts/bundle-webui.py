"""Assemble independent V1/V2 builds. Never replace an existing output tree."""
import argparse
import json
import re
import shutil
from pathlib import Path


def bundle(v1, v2, output, v1_sha=None, v2_sha=None):
    for version, root in (("v1", v1), ("v2", v2)):
        entry = root / "index.html"
        if not entry.is_file():
            raise ValueError(f"Missing {version} entry: {entry}")
        html = entry.read_text(encoding="utf-8")
        if re.search(r'''(?:src|href)=["']/assets/''', html):
            raise ValueError(f"{version} must be built with Vite base './' for isolated assets")
        if not (root / "assets").is_dir():
            raise ValueError(f"Missing {version} assets")
        for sha in (v1_sha, v2_sha):
            if sha is not None and not re.fullmatch(r"[0-9a-f]{40}", sha):
                raise ValueError("Expected a full Git SHA")
    if output.exists():
        raise ValueError(f"Output already exists; choose a fresh staging directory: {output}")
    output.mkdir(parents=True)
    shutil.copytree(v1, output / "v1")
    shutil.copytree(v2, output / "v2")
    # Old bookmarks/PWA installations can still resolve root-level public files.
    for name in ("icons", "favicon.svg", "manifest.json"):
        source = v1 / name
        if source.is_dir():
            shutil.copytree(source, output / name)
        elif source.is_file():
            shutil.copy2(source, output / name)
    (output / "webui-versions.json").write_text(json.dumps({
        "schema": 1, "default": "v1", "versions": {
            "v1": {"branch": "main", "sha": v1_sha, "preview": False},
            "v2": {"branch": "webui-v2", "sha": v2_sha, "preview": True},
        }}, indent=2) + "\n", encoding="utf-8")
    # The new backend selects the saved version at /. This entry also keeps
    # old package validators and static-only preview servers usable.
    (output / "index.html").write_text('''<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Dice!Next WebUI</title><body><a href="./v1/">打开 Dice!Next WebUI V1</a>
<script>location.replace('./v1/' + location.search + location.hash);</script></body></html>
''', encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("v1", "v2", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--v1-sha")
    parser.add_argument("--v2-sha")
    args = parser.parse_args()
    try:
        bundle(args.v1.resolve(), args.v2.resolve(), args.output.resolve(), args.v1_sha, args.v2_sha)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")
