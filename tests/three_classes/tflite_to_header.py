#!/usr/bin/env python3
"""Convert a .tflite file to a C/C++ header for flash embedding (TFLite Micro)."""

import argparse
from datetime import datetime, timezone
from pathlib import Path


def bytes_per_line() -> int:
    return 12


def to_c_array(data: bytes) -> str:
    line_width = bytes_per_line()
    lines = []
    for i in range(0, len(data), line_width):
        chunk = data[i : i + line_width]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"  {hexes}")
    return ",\n".join(lines)


def emit_header(
    model_bytes: bytes,
    array_name: str,
    len_name: str,
    source_name: str,
) -> str:
    generated = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    guard = array_name.upper() + "_H"
    body = to_c_array(model_bytes)
    return f"""// Auto-generated — do not edit.
// Source: {source_name}
// Size: {len(model_bytes)} bytes
// Generated: {generated}

#ifndef {guard}
#define {guard}

#include <cstddef>
#include <cstdint>

alignas(8) const unsigned char {array_name}[] = {{
{body}
}};

const unsigned int {len_name} = {len(model_bytes)};

#endif  // {guard}
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tflite", type=Path, help="Input .tflite file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="Output .h path (default: firmware/include/<stem>_model.h)",
    )
    parser.add_argument(
        "--array-name",
        default=None,
        help="C array symbol (default: <stem with _ replaced> + _model)",
    )
    args = parser.parse_args()

    tflite_path: Path = args.tflite.resolve()
    if not tflite_path.is_file():
        raise SystemExit(f"Not found: {tflite_path}")

    stem = tflite_path.stem.replace("-", "_")
    array_name = args.array_name or f"{stem}_model"
    len_name = f"{array_name}_len"

    if args.output:
        out_path = args.output.resolve()
    else:
        repo_root = tflite_path.parents[2]
        out_path = repo_root / "firmware" / "include" / f"{stem}_model.h"

    model_bytes = tflite_path.read_bytes()
    header = emit_header(model_bytes, array_name, len_name, tflite_path.name)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(header, encoding="utf-8")
    print(f"Wrote {out_path} ({len(model_bytes)} bytes, symbols {array_name}, {len_name})")


if __name__ == "__main__":
    main()
