#!/usr/bin/env python3
"""生成并原子发布 RenderBox 目录索引，课程包内的派生文件按内容版本存储。"""

import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from builtin_catalog import builtin_entries
from course_catalog import course_entries
from catalog_assets import digest, write_json


def dependencies(root, result):
    """将模型清单中的每个网格和纹理加入内容校验。"""
    for item in result:
        if item["kind"] != "sequence":
            sf = root / item["path"]
            doc = json.loads(sf.read_text(encoding="utf-8"))
            deps = []
            for asset in doc["meshes"] + doc.get("textures", []):
                p = (sf.parent / asset["source"]).resolve()
                deps.append(dict(path=p.relative_to(root).as_posix(), sha256=digest(p)))
            item["dependencies"] = deps


def build(root, cli, builtin):
    """先写完并校验新资源，再以一次原子替换发布索引。"""
    with tempfile.TemporaryDirectory(dir=root, prefix=".catalog-") as tmp:
        staging = Path(tmp)
        if builtin:
            result = builtin_entries(root, cli)
            dependencies(root, result)
        else:
            result = course_entries(root, cli, staging)
            dependencies(root, result)
            # 每份内容有独立目录，转换失败或再次运行时仍可使用先前挂载的包。
            # 帧清单也引用暂存区中的参考图，先规范化这些路径以计算稳定版本号。
            sequences = []
            for item in result:
                if item["kind"] == "sequence":
                    file = root / item["path"]
                    template = file.read_text(encoding="utf-8").replace(
                        staging.name + "/", "@revision@/"
                    )
                    sequences.append((item, file, template))
                    item["sha256"] = hashlib.sha256(template.encode()).hexdigest()
            payload = json.dumps(result, ensure_ascii=False).replace(
                staging.name + "/", ""
            )
            revision = hashlib.sha256(payload.encode()).hexdigest()[:20]
            directory = root / "prepared" / revision
            directory.parent.mkdir(exist_ok=True)
            prefix = directory.relative_to(root).as_posix()
            for item, file, template in sequences:
                file.write_text(
                    template.replace("@revision@/", prefix + "/"), encoding="utf-8"
                )
                item["sha256"] = digest(file)
            result = json.loads(
                json.dumps(result, ensure_ascii=False).replace(
                    staging.name + "/", prefix + "/"
                )
            )
            if not directory.exists():
                staging.rename(directory)
            staging.mkdir(exist_ok=True)
        pending = staging / "resource_catalog.json"
        write_json(pending, dict(version=1, entries=result))
        pending.replace(root / "resource_catalog.json")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--builtin", action="store_true")
    a = parser.parse_args()
    build(a.directory.resolve(), a.cli.resolve(), a.builtin)
