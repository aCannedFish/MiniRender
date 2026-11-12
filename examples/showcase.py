#!/usr/bin/env python3
"""把三个核心例程生成的网格组装为可移动、可编辑的展示场景。"""

import argparse
import json
from pathlib import Path
import shutil


def load(path):
    """按场景的 UTF-8 编码读取 JSON，保留普通编辑器可识别的字段。"""
    return json.loads(path.read_text(encoding="utf-8"))


def assemble(outputs, destination, scenes):
    """复制布料、点云环面和位移网格；场景中的资产路径均相对于输出目录。"""
    cloth_path = outputs / "a6.2/linen.json"
    cloth_scene = load(cloth_path)
    source_meshes = {mesh["id"]: mesh["source"] for mesh in cloth_scene["meshes"]}
    meshes = {
        "plane": scenes / "assets/plane.obj",
        "cloth": cloth_path.parent / source_meshes["cloth"],
        "ring": outputs / "a5.1/reconstruction.obj",
        "relief": outputs / "a4.2a/displaced.obj",
    }
    for source in meshes.values():
        if not source.is_file():
            raise FileNotFoundError(f"Missing example mesh: {source}")

    scene = cloth_scene
    scene["camera"].update(
        position=[1.0, 2.05, 4.85], target=[0.0, 0.55, 0.0], vertical_fov=36
    )
    scene["meshes"] = []
    assets = destination / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for name, source in meshes.items():
        relative = f"assets/{name}.obj"
        shutil.copyfile(source, destination / relative)
        scene["meshes"].append({"id": name, "source": relative})

    # 保留冻结时的布料变换。采用重建例程计算的环面离地高度和缩放。
    ring_scene = load(outputs / "a5.1/scene.json")
    ring = next(obj for obj in ring_scene["objects"] if obj["mesh"] == "reconstruction")
    ring.update(id="ring", name="点云重建环面", mesh="ring", material="metal")
    ring["transform"]["translation"][0] -= 1.55
    scene["objects"].append(ring)
    scene["objects"].append(
        {
            "id": "relief",
            "name": "细分位移模型",
            "mesh": "relief",
            "material": "blue",
            "transform": {
                "translation": [1.50, 0.40, 0.0],
                "rotation": [0, 25, 0],
                "scale": [1.25, 1.25, 1.25],
            },
        }
    )
    fog = load(scenes / "volume-bounded.json")["media"][0]
    fog.update(
        minimum=[-2.6, 0.01, -1.3],
        maximum=[2.6, 2.0, 1.3],
        absorption=[0.015, 0.025, 0.035],
        scattering=[0.07, 0.07, 0.07],
    )
    scene["media"] = [fog]
    path = destination / "showcase.json"
    path.write_text(
        json.dumps(scene, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outputs", type=Path, default=Path("outputs"))
    parser.add_argument("--destination", type=Path, default=Path("outputs/a9/showcase"))
    args = parser.parse_args()
    # examples/ 与 scenes/ 在同一仓库，源场景位置与调用时的工作目录无关。
    print(
        assemble(
            args.outputs,
            args.destination,
            Path(__file__).resolve().parents[1] / "scenes",
        )
    )
