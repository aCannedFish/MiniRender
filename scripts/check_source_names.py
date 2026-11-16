#!/usr/bin/env python3
"""检查自有源码文件名，拒绝空格、大小写混用和临时副本名称。

只扫描源码、测试、示例和脚本目录，构建缓存及用户资产不参与检查。
"""

from pathlib import Path
import re
import sys

SOURCE_DIRECTORIES = ("include", "src", "tests", "examples", "shaders", "scripts")
SOURCE_SUFFIXES = {".cpp", ".hpp", ".c", ".h", ".mm", ".py", ".vert", ".frag", ".glsl"}
SOURCE_NAME = re.compile(r"[a-z][a-z0-9_]*\.[a-z]+\Z")


def invalid_source_names(root: Path) -> list[Path]:
    """返回不符合 snake_case 约定的源码路径，包含未提交的本地副本。"""
    invalid = []
    for name in SOURCE_DIRECTORIES:
        directory = root / name
        if not directory.exists():
            continue
        for path in directory.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                if not SOURCE_NAME.fullmatch(path.name):
                    invalid.append(path.relative_to(root))
    return sorted(invalid)


def main() -> int:
    """打印有问题的路径并返回失败码，供开发者和 CI 共用。"""
    root = Path(__file__).resolve().parent.parent
    invalid = invalid_source_names(root)
    for path in invalid:
        print(f"Invalid source filename: {path}", file=sys.stderr)
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
