#!/usr/bin/env python3
"""核对 README 架构章节的文件树，报告未列入的源码、重复条目和失效路径。"""

from collections import Counter
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = {"cmake", "examples", "include", "scripts", "shaders", "src", "tests"}
SOURCE_SUFFIXES = {
    ".cmake",
    ".cpp",
    ".frag",
    ".glsl",
    ".h",
    ".hpp",
    ".in",
    ".mm",
    ".py",
    ".qss",
    ".vert",
}


def source_paths():
    """读取 Git 中的源码及尚未提交的新文件，排除构建目录和本地课程包。"""
    paths = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    )
    result = set()
    for item in paths.decode("utf-8").split("\0"):
        if not item:
            continue
        path = Path(item)
        if path.parts[0] in SOURCE_DIRS and (
            path.suffix in SOURCE_SUFFIXES or path.name == "CMakeLists.txt"
        ):
            result.add(path)
    return result


def documented_paths(text):
    """以每个 text 文件树的首行为目录，按文件名读取带用途说明的条目。"""
    entries = Counter()
    for block in re.findall(r"```text\n(.*?)\n```", text, re.S):
        folder, *lines = block.splitlines()
        for line in lines:
            match = re.fullmatch(r"[├└]── (.+?)\s+#\s+(.+)", line)
            if match:
                entries[Path(folder) / match[1]] += 1
    return entries


def main():
    """输出需要修改的具体路径，任一问题都返回非零退出码。"""
    text = (ROOT / "README.md").read_text(encoding="utf-8")
    heading = "\n## 架构\n"
    if text.count(heading) != 1:
        print("Expected one architecture section in README.md", file=sys.stderr)
        return 1
    section = text.split(heading, 1)[1].split("\n## ", 1)[0]
    entries = documented_paths(section)
    missing = source_paths() - entries.keys()
    errors = [f"Missing source entry: {path}" for path in sorted(missing)]
    for path, count in sorted(entries.items()):
        if not (ROOT / path).is_file():
            errors.append(f"File does not exist: {path}")
        if count > 1:
            errors.append(f"Duplicate entry: {path}")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Verified {len(entries)} architecture entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
