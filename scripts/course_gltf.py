"""将 HW3 的 cube1、cube2 和 cave 离线转换为可编辑的 RenderBox 网格与材质。"""

import base64
import json
from pathlib import Path


def convert_hw3(path: Path, destination: Path, scene: dict):
    """读取这三个场景使用的 glTF 2.0 三角形、节点 TRS 和嵌入或外置纹理。"""
    import numpy as np

    asset = json.loads(path.read_text(encoding="utf-8"))
    destination.mkdir(parents=True, exist_ok=True)

    def file_bytes(uri):
        if uri.startswith("data:"):
            return base64.b64decode(uri.split(",", 1)[1], validate=True)
        return (path.parent / uri).read_bytes()

    buffers = [file_bytes(b["uri"]) for b in asset["buffers"]]

    def accessor(index):
        entry = asset["accessors"][index]
        view = asset["bufferViews"][entry["bufferView"]]
        dtype = {5126: "<f4", 5125: "<u4", 5123: "<u2", 5121: "u1"}[
            entry["componentType"]
        ]
        n = {"VEC2": 2, "VEC3": 3, "SCALAR": 1}[entry["type"]]
        item = np.dtype(dtype).itemsize
        return np.ndarray(
            (entry["count"], n),
            dtype,
            buffer=buffers[view["buffer"]],
            offset=view.get("byteOffset", 0) + entry.get("byteOffset", 0),
            strides=(view.get("byteStride", item * n), item),
        )

    scene.update(materials=[], textures=[], meshes=[], objects=[])
    textures = {}

    def texture(index, encoding):
        key = (index, encoding)
        if key in textures:
            return textures[key]
        image = asset["images"][asset["textures"][index]["source"]]
        if "uri" in image:
            data = file_bytes(image["uri"])
        else:
            view = asset["bufferViews"][image["bufferView"]]
            start = view.get("byteOffset", 0)
            data = buffers[view["buffer"]][start : start + view["byteLength"]]
        suffix = ".png" if data.startswith(b"\x89PNG") else ".jpg"
        id_ = f"texture_{index}_{encoding}"
        (destination / (id_ + suffix)).write_bytes(data)
        scene["textures"].append(dict(id=id_, source=id_ + suffix, encoding=encoding))
        textures[key] = id_
        return id_

    for index, material in enumerate(asset["materials"]):
        pbr = material.get("pbrMetallicRoughness", {})
        entry = dict(
            id=f"material_{index}",
            name=material.get("name", str(index)),
            kind="conductor" if pbr.get("metallicFactor", 1) >= 0.5 else "diffuse",
            base_color=pbr.get("baseColorFactor", [1, 1, 1, 1])[:3],
            roughness=pbr.get("roughnessFactor", 1),
            double_sided=material.get("doubleSided", False),
        )
        if "baseColorTexture" in pbr:
            entry["base_color_texture"] = texture(
                pbr["baseColorTexture"]["index"], "srgb"
            )
        if "normalTexture" in material:
            normal = material["normalTexture"]
            entry.update(
                normal_texture=texture(normal["index"], "linear"),
                normal_scale=normal.get("scale", 1),
                normal_flip_y=True,
            )
        scene["materials"].append(entry)

    def visit(node_id, parent):
        node = asset["nodes"][node_id]
        if "matrix" in node:
            local = np.asarray(node["matrix"]).reshape(4, 4).T
        else:
            x, y, z, w = node.get("rotation", [0, 0, 0, 1])
            rotation = np.array(
                [
                    [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                    [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                    [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
                ]
            )
            local = np.eye(4)
            local[:3, :3] = rotation @ np.diag(node.get("scale", [1, 1, 1]))
            local[:3, 3] = node.get("translation", [0, 0, 0])
        world = parent @ local
        for child in node.get("children", []):
            visit(child, world)
        if "mesh" not in node:
            return
        for j, prim in enumerate(asset["meshes"][node["mesh"]]["primitives"]):
            if prim.get("mode", 4) != 4:
                raise ValueError("HW3 conversion expects triangles")
            attrs = prim["attributes"]
            points = accessor(attrs["POSITION"]) @ world[:3, :3].T + world[:3, 3]
            normals = accessor(attrs["NORMAL"]) @ np.linalg.inv(world[:3, :3])
            normals /= np.maximum(1e-20, np.linalg.norm(normals, axis=1))[:, None]
            uv = accessor(attrs["TEXCOORD_0"]).copy()
            uv[:, 1] = 1 - uv[:, 1]
            faces = accessor(prim["indices"]).reshape(-1, 3).astype(np.int64)
            if np.linalg.det(world[:3, :3]) < 0:
                faces = faces[:, [0, 2, 1]]
            tri = points[faces]
            valid = (
                np.linalg.norm(
                    np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]), axis=1
                )
                > 1e-9
            )
            faces = faces[valid] + 1
            if not len(faces):
                continue
            # 把节点变换烘焙到几何，再以各对象自己的包围盒中心作旋转轴。
            center = (np.max(points, axis=0) + np.min(points, axis=0)) * 0.5
            id_ = f"mesh_{node_id}_{j}"
            mesh = destination / (id_ + ".obj")
            with mesh.open("w", encoding="ascii", newline="\n") as stream:
                np.savetxt(stream, points - center, fmt="v %.9g %.9g %.9g")
                np.savetxt(stream, uv, fmt="vt %.9g %.9g")
                np.savetxt(stream, normals, fmt="vn %.9g %.9g %.9g")
                for a, b, c in faces:
                    stream.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")
            scene["meshes"].append(dict(id=id_, source=mesh.name))
            scene["objects"].append(
                dict(
                    id=id_,
                    name=node.get("name", id_),
                    mesh=id_,
                    material=f"material_{prim['material']}",
                    visible=True,
                    transform=dict(
                        translation=center.tolist(), rotation=[0, 0, 0], scale=[1, 1, 1]
                    ),
                )
            )

    for node in asset["scenes"][asset.get("scene", 0)]["nodes"]:
        visit(node, np.eye(4))
    return scene
