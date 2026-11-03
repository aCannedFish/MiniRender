#!/usr/bin/env python3
"""生成参数响应测试场景。密集平面替换 studio 地面，保留其他参数用于前后对照。"""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--resolution", type=int, default=320)
    args = parser.parse_args()
    if args.resolution < 1:
        parser.error("Resolution must be positive")
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    n = args.resolution
    with (output / "dense.obj").open("w", encoding="utf-8") as file:
        for y in range(n + 1):
            for x in range(n + 1):
                file.write(f"v {x / n * 2 - 1} 0 {y / n * 2 - 1}\n")
        file.write("vn 0 1 0\n")
        for y in range(n):
            for x in range(n):
                a = y * (n + 1) + x + 1
                b, c, d = a + 1, a + n + 1, a + n + 2
                file.write(f"f {a}//1 {c}//1 {b}//1\nf {b}//1 {c}//1 {d}//1\n")
    scene = json.loads((root / "scenes/studio.json").read_text(encoding="utf-8"))
    for mesh in scene["meshes"]:
        source = output / "dense.obj" if mesh["id"] == "plane" else root / "scenes" / mesh["source"]
        mesh["source"] = str(source)
    destination = output / "dense.json"
    destination.write_text(json.dumps(scene, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"{destination}: {n * n * 2 + 24} triangles")


if __name__ == "__main__":
    main()
