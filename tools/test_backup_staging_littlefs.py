#!/usr/bin/env python3
"""Exercise the production staging helper against hash-pinned LittleFS 2.5.

Downloads only upstream test dependencies into a temporary directory. Requires
network access plus a host C/C++ compiler; does not access any bridge device.
"""

import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "lfs.c": "c4de0850d8629f511b91217334c2cc2e75e89cd24f1f7b40b483df3d5762a918",
    "lfs.h": "6c747c5b51813beb7979e632ba3076b2d674b5419541a450e3b02a27d7bee310",
    "lfs_util.c": "8e1376a90e923a2897388a54ebaa756ad3ea07a0e2a87e73c0045515c0a82685",
    "lfs_util.h": "3c8b6799cb057c3243015e7a09c501f1c8db859898f6e40dbce587538274cd3f",
}


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="bridge-lfs-test-") as temporary:
        work = Path(temporary)
        for name, expected in SOURCES.items():
            url = f"https://raw.githubusercontent.com/littlefs-project/littlefs/v2.5.0/{name}"
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
