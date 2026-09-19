#!/usr/bin/env python3
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC_DIR = HERE.parent / "src" / "rva"
OUT = HERE.parent / "src" / "rva_tables_embedded.h"

def carr(data: bytes, indent="    ") -> str:
    lines, row = [], []
    for i, b in enumerate(data):
        row.append(f"0x{b:02x},")
        if len(row) == 16:
            lines.append(indent + "".join(row)); row = []
    if row: lines.append(indent + "".join(row))
    return "\n".join(lines)

def main():
    files = sorted(SRC_DIR.glob("*.json"))
    if not files:
        print(f"没有找到表:{SRC_DIR}"); return 1
    parts = ["#pragma once", "", "#include <cstddef>", "", "namespace ed9loader {", ""]
    names = []
    for f in files:
        stem = f.stem
        sym = stem.replace("-", "_")
        data = f.read_bytes()
        parts.append(f"static const unsigned char kRvaTable_{sym}[] = {{")
        parts.append(carr(data))
        parts.append("};")
        parts.append(f"static const size_t kRvaTable_{sym}_Len = sizeof(kRvaTable_{sym});")
        parts.append("")
        names.append((stem, sym, len(data)))
    parts.append("struct EmbeddedRvaTable { const char* name; const unsigned char* data; size_t len; };")
    parts.append("static const EmbeddedRvaTable kEmbeddedRvaTables[] = {")
    for stem, sym, n in names:
        parts.append(f'    {{ "{stem}", kRvaTable_{sym}, kRvaTable_{sym}_Len }},')
    parts.append("};")
    parts.append(f"static const int kEmbeddedRvaTableCount = {len(names)};")
    parts.append("")
    parts.append("}")
    parts.append("")
    OUT.write_text("\n".join(parts), encoding="utf-8")
    print(f"生成 {OUT}")
    for stem, sym, n in names: print(f"  内置 {stem}.json  {n} 字节")
    return 0

if __name__ == "__main__":
    sys.exit(main())
