"""Fingerprint ABI-keyed vcpkg archives, not timestamps or unpacked build trees."""
import hashlib
import sys
from pathlib import Path


def snapshot(root: Path) -> str:
    entries = sorted(
        f"{path.relative_to(root).as_posix()}:{path.stat().st_size}"
        for path in root.rglob("*.zip") if path.is_file()
    )
    return hashlib.sha256("\n".join(entries).encode("utf-8")).hexdigest()


if __name__ == "__main__":
    print(f"digest={snapshot(Path(sys.argv[1]))}")
