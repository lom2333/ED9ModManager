#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 sora1_tbl_schemas.json 内嵌进 C++ 头文件,作为编译期单一可信源。

用法:
    python embed_schema.py
    python embed_schema.py <源 json> <目标 .h>

默认:
    源 = ED9Loader/src/modkit/sora1_tbl_schemas.json
    目标 = ED9Loader/src/modkit/sora1_tbl_schemas_embedded.h

schema 改动后重跑本脚本,再重新编译即可。生成的字节数组与原 json 字节完全一致
(原样嵌入,不重新格式化,避免引入解析差异)。
"""
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = HERE.parent / "src" / "modkit" / "sora1_tbl_schemas.json"
DEFAULT_DST = HERE.parent / "src" / "modkit" / "sora1_tbl_schemas_embedded.h"


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_SRC
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_DST

    data = src.read_bytes()

    # 校验是合法 json(防止把损坏文件嵌进去)
    try:
        import json
        json.loads(data.decode("utf-8"))
    except Exception as e:  # noqa: BLE001
        print(f"[错误] 源不是合法 JSON: {e}", file=sys.stderr)
        return 1

    lines = []
    lines.append("// 自动生成 —— 请勿手改。改 schema 请编辑 sora1_tbl_schemas.json 后重跑 tools/embed_schema.py")
    lines.append(f"// 源: {src.name}  字节数: {len(data)}")
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace ed9loader { namespace modkit {")
    lines.append("")
    lines.append("// sora1_tbl_schemas.json 原始字节(UTF-8,未重格式化)")
    lines.append("static const unsigned char kSora1TblSchemasJson[] = {")

    per_line = 16
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",")

    lines.append("};")
    lines.append(f"static const unsigned int kSora1TblSchemasJsonLen = {len(data)}u;")
    lines.append("")
    lines.append("} } // namespace ed9loader::modkit")
    lines.append("")

    dst.write_text("\n".join(lines), encoding="utf-8")
    print(f"已生成 {dst}")
    print(f"  内嵌 {len(data)} 字节")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
