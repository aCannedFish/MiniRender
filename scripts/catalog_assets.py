"""资源包的路径、摘要、场景模板和 CLI 缩略图生成。"""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    """分块计算文件摘要，供导入时校验资产内容。"""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, data):
    """创建父目录并以稳定的 UTF-8 缩进格式写入元数据。"""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def base_scene():
    """独立读取默认场景，供每个示例修改相机、灯光与资产。"""
    return json.loads((ROOT / "scenes/studio.json").read_text(encoding="utf-8"))


def transform(translation=(0, 0, 0), scale=(1, 1, 1)):
    """构造无旋转的局部到世界变换，平移单位为米。"""
    return {
        "translation": list(translation),
        "scale": list(scale),
        "rotation": [0, 0, 0],
    }


def thumbnail(cli, scene, png, samples=12):
    """缩略图由项目积分器生成，与场景放在同一个包内。"""
    png.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        prefix = Path(tmp) / "image"
        subprocess.run(
            [
                cli,
                "--render",
                scene,
                prefix,
                "--width",
                "96",
                "--height",
                "72",
                "--spp",
                str(samples),
                "--depth",
                "4",
                "--seed",
                "101",
                "--threads",
                "4",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        shutil.copyfile(prefix.with_suffix(".png"), png)


def entry(root, id_, name, kind, file, image, description, source, license_, **extra):
    """记录相对于包根目录的路径、来源、许可及入口文件摘要。"""
    return dict(
        id=id_,
        name=name,
        kind=kind,
        path=file.relative_to(root).as_posix(),
        sha256=digest(file),
        thumbnail=image.relative_to(root).as_posix(),
        description=description,
        source=source,
        rights=license_,
        **extra,
    )


def furnace_scene():
    """六个同半径的测试球，按行切换补偿，按列增大粗糙度。"""
    scene = base_scene()
    scene.update(
        environment=[1, 1, 1],
        directional_lights=[],
        meshes=[dict(id="sphere", source="assets/demo-sphere.obj")],
        objects=[],
        materials=[],
    )
    scene["camera"].update(position=[0, 1, 4.4], target=[0, 1, 0], vertical_fov=32)
    for row in range(2):
        for column, roughness in enumerate((0.25, 0.65, 1.0)):
            key = f"ball_{row}_{column}"
            label = f"{'补偿' if row else '单次'} · r={roughness}"
            scene["materials"].append(
                dict(
                    id=key,
                    name=label,
                    kind="conductor",
                    base_color=[1, 1, 1],
                    roughness=roughness,
                    compensate_energy=bool(row),
                )
            )
            scene["objects"].append(
                dict(
                    id=key,
                    name=label,
                    mesh="sphere",
                    material=key,
                    visible=True,
                    transform=transform(
                        ((column - 1) * 0.95, 0.55 + row * 0.9, 0), [0.38] * 3
                    ),
                )
            )
    return scene


def frame_entry(root, beauty, guides, reference, frame):
    """登记每一张独立的 EXR 和矩阵，播放器按当前帧及所选滤波方式验证。"""

    def record(file):
        return dict(path=file.relative_to(root).as_posix(), sha256=digest(file))

    item = record(beauty)
    item["guides"] = {
        name: record(guides / f"{name}_{frame}.exr")
        for name in ("normal", "position", "depth", "ID")
    }
    item["guides"]["matrix"] = record(guides / f"matrix_{frame}.mat")
    item["reference"] = record(reference)
    return item
