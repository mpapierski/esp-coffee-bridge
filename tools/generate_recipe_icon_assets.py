#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
from textwrap import wrap

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT / "output" / "imagegen"
OUT_CPP = ROOT / "src" / "recipe_icon_assets.cpp"

ICON_ORDER = [
    "americano",
    "caffe-latte",
    "cappuccino",
    "chilled-americano",
    "chilled-espresso",
    "chilled-lungo",
    "cloud",
    "coffee",
    "creme",
    "espresso",
    "flower",
    "frothy-milk",
    "heart",
    "hot-milk",
    "lungo",
    "macchiato",
    "milk",
    "my-coffee",
    "smily",
    "star",
    "sun",
    "undefined",
    "warm-milk",
    "water",
]


def c_identifier(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def emit_array(name: str, data: bytes) -> str:
    values = ", ".join(f"0x{byte:02X}" for byte in data)
    lines = wrap(values, width=100)
    body = "\n".join(f"    {line}" for line in lines)
    ident = c_identifier(name)
    return f"const uint8_t k_{ident}[] PROGMEM = {{\n{body}\n}};\n"


def main() -> None:
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    chunks: list[str] = [
        '#include "recipe_icons.h"\n',
        "\n",
        "#include <pgmspace.h>\n",
        "\n",
        "namespace recipe_icons {\n",
        "namespace {\n\n",
    ]

    table_rows: list[str] = []
    for icon_key in ICON_ORDER:
        source = SRC_DIR / f"{icon_key}.png"
        if not source.exists():
            raise SystemExit(f"Missing source icon: {source}")

        image = Image.open(source).convert("RGBA")
        image.thumbnail((96, 96), Image.Resampling.LANCZOS)
        width, height = image.size

        from io import BytesIO

        buf = BytesIO()
        image.save(buf, "WEBP", quality=70, method=6)
        payload = buf.getvalue()

        chunks.append(emit_array(icon_key, payload))
        ident = c_identifier(icon_key)
        table_rows.append(
            f'    {{"{icon_key}", k_{ident}, sizeof(k_{ident}), {width}, {height}}},'
        )
        chunks.append("\n")

    chunks.append("const Asset kEmbeddedAssets[] = {\n")
    chunks.extend(f"{row}\n" for row in table_rows)
    chunks.append("};\n\n")
    chunks.append("} // namespace\n\n")
    chunks.append("const Asset* embeddedAssets(size_t& countOut) {\n")
    chunks.append("    countOut = sizeof(kEmbeddedAssets) / sizeof(kEmbeddedAssets[0]);\n")
    chunks.append("    return kEmbeddedAssets;\n")
    chunks.append("}\n\n")
    chunks.append("} // namespace recipe_icons\n")

    OUT_CPP.write_text("".join(chunks), encoding="utf-8")


if __name__ == "__main__":
    main()
