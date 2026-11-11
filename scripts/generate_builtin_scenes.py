#!/usr/bin/env python3
"""生成自有几何和 Cornell 风格示例；所有尺寸以米表示，墙面正面朝房间内部。"""

import copy
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "scenes"


def material(name, color, kind="diffuse", roughness=0.5, emission=None):
    """每个对象创建独立材质，避免编辑一个物体时连带修改其他对象。"""
    result = {
        "id": "mat_" + name,
        "name": name,
        "kind": kind,
        "base_color": color,
        "roughness": roughness,
        "double_sided": kind == "dielectric",
    }
    if emission is not None:
        result["emission"] = emission
    if kind == "dielectric":
        result["ior"] = 1.5
    return result


def add(
    scene,
    name,
    label,
    mesh,
    color,
    position,
    scale,
    rotation=None,
    kind="diffuse",
    roughness=0.5,
    emission=None,
):
    """添加可编辑实例及其材质引用，变换沿用引擎的局部米制坐标。"""
    scene["materials"].append(material(name, color, kind, roughness, emission))
    scene["objects"].append(
        {
            "id": name,
            "name": label,
            "mesh": mesh,
            "material": "mat_" + name,
            "visible": True,
            "transform": {
                "translation": position,
                "rotation": rotation or [0, 0, 0],
                "scale": scale,
            },
        }
    )


def room():
    """五面开放房间，面积光与天花板朝下，其余墙面朝向内部。"""
    scene = {
        "version": 1,
        "camera": {
            "position": [0, 1.02, 3.4],
            "target": [0, 1, 0],
            "up": [0, 1, 0],
            "vertical_fov": 39,
            "near": 0.03,
            "far": 100,
        },
        "environment": [0, 0, 0],
        "materials": [],
        "meshes": [
            {"id": "plane", "source": "assets/plane.obj"},
            {"id": "cube", "source": "assets/cube.obj"},
            {"id": "sphere", "source": "assets/demo-sphere.obj"},
        ],
        "objects": [],
        "directional_lights": [],
    }
    white = [0.73, 0.73, 0.73]
    add(scene, "floor", "地面", "plane", white, [0, 0, 0], [1, 1, 1])
    add(scene, "ceiling", "天花板", "plane", white, [0, 2, 0], [1, 1, 1], [180, 0, 0])
    add(scene, "back", "后墙", "plane", white, [0, 1, -1], [1, 1, 1], [90, 0, 0])
    add(
        scene,
        "left",
        "红色左墙",
        "plane",
        [0.65, 0.05, 0.04],
        [-1, 1, 0],
        [1, 1, 1],
        [0, 0, -90],
    )
    add(
        scene,
        "right",
        "绿色右墙",
        "plane",
        [0.12, 0.45, 0.15],
        [1, 1, 0],
        [1, 1, 1],
        [0, 0, 90],
    )
    add(
        scene,
        "lamp",
        "顶部面积光",
        "plane",
        [0, 0, 0],
        [0, 1.985, -0.15],
        [0.32, 1, 0.27],
        [180, 0, 0],
        emission=[14, 13.5, 12],
    )
    return scene


def sphere():
    """共享几何位置与径向法线，UV 接缝独立保存面角索引。"""
    latitude = 24
    longitude = 48
    positions = [(0, 1, 0)]
    for j in range(1, latitude):
        theta = math.pi * j / latitude
        for k in range(longitude):
            phi = 2 * math.pi * k / longitude
            positions.append(
                (
                    math.sin(theta) * math.cos(phi),
                    math.cos(theta),
                    math.sin(theta) * math.sin(phi),
                )
            )
    bottom = len(positions)
    positions.append((0, -1, 0))
    faces = []
    for k in range(longitude):
        u = k / longitude
        v = (k + 1) / longitude
        faces.append(
            [
                (0, ((u + v) / 2, 1)),
                (1 + (k + 1) % longitude, (v, 1 - 1 / latitude)),
                (1 + k, (u, 1 - 1 / latitude)),
            ]
        )
        for j in range(latitude - 2):
            a = 1 + j * longitude + k
            b = 1 + j * longitude + (k + 1) % longitude
            c = b + longitude
            d = a + longitude
            top = 1 - (j + 1) / latitude
            low = 1 - (j + 2) / latitude
            faces.extend(
                [
                    [(a, (u, top)), (b, (v, top)), (c, (v, low))],
                    [(a, (u, top)), (c, (v, low)), (d, (u, low))],
                ]
            )
        a = 1 + (latitude - 2) * longitude + k
        b = 1 + (latitude - 2) * longitude + (k + 1) % longitude
        faces.append(
            [(a, (u, 1 / latitude)), (b, (v, 1 / latitude)), (bottom, ((u + v) / 2, 0))]
        )
    lines = ["# MiniRender generated unit sphere; outward winding, face-varying UV."]
    lines += ["v " + " ".join(f"{x:.9g}" for x in p) for p in positions]
    lines += ["vn " + " ".join(f"{x:.9g}" for x in p) for p in positions]
    for face in faces:
        lines += [f"vt {uv[0]:.9g} {uv[1]:.9g}" for _, uv in face]
    for index, face in enumerate(faces):
        lines.append(
            "f "
            + " ".join(
                f"{vertex + 1}/{index * 3 + corner + 1}/{vertex + 1}"
                for corner, (vertex, _) in enumerate(face)
            )
        )
    (ROOT / "assets/demo-sphere.obj").write_text("\n".join(lines) + "\n")


def volume_scenes(classic):
    """复用工作室与 Cornell 场景，添加介质和带透光孔的顶板。"""

    def medium(low, high, absorption, scattering, g):
        return {
            "id": "fog",
            "name": "均匀体积",
            "minimum": low,
            "maximum": high,
            "absorption": absorption,
            "scattering": scattering,
            "anisotropy": g,
            "enabled": True,
        }

    studio = json.loads((ROOT / "studio.json").read_text(encoding="utf-8"))
    bounded = copy.deepcopy(studio)
    bounded["media"] = [
        medium([-2, 0.01, -2], [2, 2.8, 2], [0.06, 0.12, 0.18], [0.3] * 3, 0.6)
    ]
    cornell = copy.deepcopy(classic)
    cornell["media"] = [
        medium(
            [-0.99, 0.01, -0.99], [0.99, 1.99, 0.99], [0.08, 0.1, 0.13], [0.4] * 3, 0.25
        )
    ]
    beam = copy.deepcopy(studio)
    beam["objects"] = [o for o in beam["objects"] if o["id"] == "ground"]
    beam["environment"] = [0.001] * 3
    beam["directional_lights"] = [
        {
            "id": "sun",
            "direction": [0.2, -1, 0.3],
            "color": [1, 0.93, 0.8],
            "intensity": 12,
        }
    ]
    beam["camera"]["position"] = [3, 1.8, 4.5]
    beam["camera"]["target"] = [0, 1.2, 0]
    beam["materials"].append(
        {
            "id": "aperture",
            "name": "透光顶板",
            "kind": "diffuse",
            "base_color": [0.08] * 3,
            "double_sided": True,
        }
    )
    beam["meshes"].append({"id": "aperture", "source": "assets/volume-aperture.obj"})
    beam["objects"].append(
        {
            "id": "aperture",
            "name": "透光顶板",
            "mesh": "aperture",
            "material": "aperture",
            "transform": {
                "translation": [0, 0, 0],
                "rotation": [0, 0, 0],
                "scale": [1, 1, 1],
            },
            "visible": True,
        }
    )
    beam["media"] = [
        medium([-2, 0.01, -2], [2, 2.5, 2], [0.05, 0.08, 0.13], [0.55] * 3, 0.15)
    ]
    quads = [
        (-2, -0.35, -2, 2),
        (0.35, 2, -2, 2),
        (-0.35, 0.35, -2, -0.35),
        (-0.35, 0.35, 0.35, 2),
    ]
    lines = ["# Four ceiling panels around a 0.7 m square aperture.", "vn 0 1 0"]
    for i, (x0, x1, z0, z1) in enumerate(quads):
        for x, z in [(x0, z0), (x0, z1), (x1, z1), (x1, z0)]:
            lines.append(f"v {x} 2.6 {z}")
        start = 4 * i + 1
        lines.append(f"f {start}//1 {start + 1}//1 {start + 2}//1")
        lines.append(f"f {start}//1 {start + 2}//1 {start + 3}//1")
    (ROOT / "assets/volume-aperture.obj").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )
    return {"volume-bounded": bounded, "volume-cornell": cornell, "volume-beam": beam}


def main():
    sphere()
    classic = room()
    add(
        classic,
        "short_box",
        "矮方块",
        "cube",
        [0.73] * 3,
        [-0.43, 0.3, 0.35],
        [0.58, 0.6, 0.58],
        [0, -18, 0],
    )
    add(
        classic,
        "tall_box",
        "高方块",
        "cube",
        [0.73] * 3,
        [0.38, 0.62, -0.28],
        [0.58, 1.24, 0.58],
        [0, 17, 0],
    )
    glass = room()
    add(
        glass,
        "glass_ball",
        "玻璃球",
        "sphere",
        [0.98, 1, 0.99],
        [-0.43, 0.4, 0.3],
        [0.4] * 3,
        kind="dielectric",
    )
    add(
        glass,
        "gold_ball",
        "金属球",
        "sphere",
        [0.95, 0.67, 0.3],
        [0.43, 0.48, -0.25],
        [0.48] * 3,
        kind="conductor",
        roughness=0.17,
    )
    small = copy.deepcopy(classic)
    next(o for o in small["objects"] if o["id"] == "lamp")["transform"]["scale"] = [
        0.065,
        1,
        0.065,
    ]
    next(m for m in small["materials"] if m["id"] == "mat_lamp")["emission"] = [
        280,
        270,
        240,
    ]
    balls = room()
    balls["objects"] = [o for o in balls["objects"] if o["id"] in ["floor", "lamp"]]
    balls["environment"] = [0.08] * 3
    balls["camera"]["position"] = [2.7, 2.2, 4.8]
    balls["camera"]["target"] = [0, 0.4, 0]
    balls["camera"]["vertical_fov"] = 35
    next(o for o in balls["objects"] if o["id"] == "floor")["transform"]["scale"] = [
        3,
        1,
        3,
    ]
    next(o for o in balls["objects"] if o["id"] == "lamp")["transform"][
        "translation"
    ] = [0, 3.5, 0.6]
    next(o for o in balls["objects"] if o["id"] == "lamp")["transform"]["scale"] = [
        1.2,
        1,
        1.0,
    ]
    balls["materials"] = [
        m for m in balls["materials"] if m["id"] in ["mat_floor", "mat_lamp"]
    ]
    for row, kind in enumerate(["diffuse", "conductor"]):
        for column, roughness in enumerate([0.08, 0.3, 0.65]):
            index = row * 3 + column
            color = [0.12, 0.35, 0.65] if row == 0 else [0.95, 0.67, 0.3]
            add(
                balls,
                f"ball_{index}",
                f"{('漫反射', '金属')[row]} {roughness}",
                "sphere",
                color,
                [(column - 1) * 0.9, 0.38, (row - 0.5) * 0.95],
                [0.38] * 3,
                kind=kind,
                roughness=roughness,
            )
    for name, scene in [
        ("cornell-box", classic),
        ("cornell-materials", glass),
        ("cornell-small-light", small),
        ("material-balls", balls),
        *volume_scenes(classic).items(),
    ]:
        (ROOT / f"{name}.json").write_text(
            json.dumps(scene, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )


if __name__ == "__main__":
    main()
