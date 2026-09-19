#!/usr/bin/env python3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = HERE.parent / "src" / "modkit" / "sora1_tbl_schemas.json"
DEFAULT_DST = HERE.parent / "src" / "modkit" / "sora1_tbl_schemas_embedded.h"


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_DST

    data = src.read_bytes()

    try:
        import json
        json.loads(data.decode("utf-8"))
    except Exception as e:
        print(f"[错误] 源不是合法 JSON: {e}", file=sys.stderr)
        return 1

    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace ed9loader { namespace modkit {")
    lines.append("")
    lines.append("static const unsigned char kSora1TblSchemasJson[] = {")

    per_line = 16
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",")

    lines.append("};")
    lines.append(f"static const unsigned int kSora1TblSchemasJsonLen = {len(data)}u;")
    lines.append("")
    lines.append("} }")
    lines.append("")

    dst.write_text("\n".join(lines), encoding="utf-8")
    print(f"已生成 {dst}")
    print(f"  内嵌 {len(data)} 字节")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
