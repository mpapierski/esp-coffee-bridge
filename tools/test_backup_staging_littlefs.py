#!/usr/bin/env python3
"""Exercise staging and growth against the runtime-pinned LittleFS source.

Downloads only upstream test dependencies into a temporary directory. Requires
network access plus a host C/C++ compiler; does not access any bridge device.
"""

import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
LITTLEFS_COMMIT = "f53a0cc961a8acac85f868b431d2f3e58e447ba3"
SOURCES = {
    "lfs.c": "0b9845f350c33aa448a916847d2246b76c8e296eed8e7e296ae0430242cfda6c",
    "lfs.h": "a8c8d70f0863fbbc46ce17c5dc7673b40f1b3c9e7e10f3bf33fd28f03dc67703",
    "lfs_util.c": "f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec",
    "lfs_util.h": "03e912a6e9894c9d10c61f5da22b89ebe0bb778af67972d7b67a5f160731bf72",
}


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="bridge-lfs-test-") as temporary:
        work = Path(temporary)
        for name, expected in SOURCES.items():
            url = (
                "https://raw.githubusercontent.com/littlefs-project/littlefs/"
                f"{LITTLEFS_COMMIT}/{name}"
            )
            with urlopen(url, timeout=30) as response:
                data = response.read()
            if sha256(data).hexdigest() != expected:
                raise RuntimeError(
                    f"Upstream test dependency checksum mismatch: {name}"
                )
            (work / name).write_bytes(data)
        subprocess.run(["cc", "-O2", "-c", "lfs.c", "lfs_util.c"], cwd=work, check=True)
        subprocess.run([
            "c++", "-std=c++17", "-O2", f"-I{ROOT / 'include'}", f"-I{work}",
            str(ROOT / "tools/tests/backup_staging_littlefs.cpp"),
            "lfs.o", "lfs_util.o", "-o", "staging-test",
        ], cwd=work, check=True)
        subprocess.run([str(work / "staging-test")], check=True)


if __name__ == "__main__":
    main()
