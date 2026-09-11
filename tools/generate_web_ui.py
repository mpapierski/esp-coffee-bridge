#!/usr/bin/env python3
"""Generate the deterministic, flash-resident gzip web UI header."""

from __future__ import annotations

import gzip
import hashlib
from pathlib import Path
import sys


try:
    Import("env")  # type: ignore[name-defined]  # supplied by PlatformIO/SCons
    PIO_ENV = env  # type: ignore[name-defined]
except NameError:
    PIO_ENV = None

PROJECT_ROOT = (
    Path(PIO_ENV.subst("$PROJECT_DIR"))
    if PIO_ENV is not None
    else Path(__file__).resolve().parents[1]
)
SOURCE = PROJECT_ROOT / "web" / "index.html"


def gzip_source(source: Path) -> tuple[bytes, bytes]:
    raw = source.read_bytes()
    compressed = bytearray(gzip.compress(raw, compresslevel=9, mtime=0))
    # RFC 1952 leaves the OS byte informational. Pin it so output is identical
    # across Python/zlib hosts as well as across build timestamps.
    compressed[9] = 255
    return raw, bytes(compressed)


def render_header(raw: bytes, compressed: bytes) -> str:
    rows = []
    for offset in range(0, len(compressed), 16):
        values = ", ".join(f"0x{value:02x}" for value in compressed[offset : offset + 16])
        rows.append(f"    {values},")
    digest = hashlib.sha256(raw).hexdigest()
    return "\n".join(
        [
            "#pragma once",
            "",
            "#include <Arduino.h>",
            "",
            "namespace web_ui {",
            "",
            "static const uint8_t kPageGzip[] PROGMEM = {",
            *rows,
            "};",
            f"constexpr size_t kPageGzipSize = {len(compressed)};",
            f'constexpr char kPageSourceSha256[] = "{digest}";',
            "",
            "} // namespace web_ui",
            "",
        ]
    )


def generate(output: Path) -> bool:
    raw, compressed = gzip_source(SOURCE)
    expected = render_header(raw, compressed)
    current = output.read_text() if output.exists() else None
    if current == expected:
        return False
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(expected)
    return True


def verify(output: Path) -> None:
    raw, compressed = gzip_source(SOURCE)
    expected = render_header(raw, compressed)
    if not output.exists() or output.read_text() != expected:
        raise SystemExit(f"generated web UI header is stale: {output}")
    if gzip.decompress(compressed) != raw:
        raise SystemExit("generated web UI gzip does not reproduce web/index.html")
    print(
        f"verified {output}: {len(raw)} source bytes -> "
        f"{len(compressed)} deterministic gzip bytes"
    )


def platformio_generate() -> None:
    if PIO_ENV is None:
        raise RuntimeError("PlatformIO environment is unavailable")
    build_dir = Path(PIO_ENV.subst("$BUILD_DIR"))
    output = build_dir / "generated" / "web_ui_gzip.h"
    generate(output)
    PIO_ENV.Append(CPPPATH=[str(output.parent)])


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] != "--verify":
        raise SystemExit("usage: generate_web_ui.py --verify <generated-header>")
    verify(Path(sys.argv[2]))
else:
    platformio_generate()
