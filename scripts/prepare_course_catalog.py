#!/usr/bin/env python3
"""把 GAMES202 本地作业包整理成 RenderBox 可挂载的课程目录。"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from course_cubemap import cubemap_to_latlong
from course_gltf import convert_hw3

ROOT = Path(__file__).resolve().parents[1]
BUILTINS = {
    "cornell": "cornell-box",
    "glass": "cornell-materials",
    "materials": "material-balls",
    "small_light": "cornell-small-light",
    "bounded_fog": "volume-bounded",
    "cornell_fog": "volume-cornell",
    "light_beam": "volume-beam",
    "glass_caustic": "sppm-glass",
    "mirror_caustic": "sppm-mirror",
    "sss_marble": "sss-marble",
    "sss_wax": "sss-wax",
    "sss_marble_edge": "sss-marble-edge",
    "sss_wax_edge": "sss-wax-edge",
    "lightfield": "lightfield-charts",
}


def digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def base_scene():
    return json.loads((ROOT / "scenes/studio.json").read_text(encoding="utf-8"))


def transform(translation=(0, 0, 0), scale=(1, 1, 1)):
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


def export_model(directory, name, obj, texture=None):
    """保留 OBJ 每个材质组的三角形与面角 UV，共用同一归一化变换。"""
    dst = directory / name
    dst.mkdir(parents=True, exist_ok=True)
    vertices = []
    attrs = []
    groups = {}
    group = "surface"
    dropped = 0
    for line in obj.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        parts = line.split()
        if not parts:
            continue
        tag = parts[0]
        if tag in ("v", "vt", "vn"):
            attrs.append(line)
            if tag == "v":
                vertices.append(tuple(map(float, parts[1:4])))
        elif tag == "usemtl":
            group = " ".join(parts[1:])
        elif tag == "f":
            for i in range(2, len(parts) - 1):
                face = (parts[1], parts[i], parts[i + 1])
                indices = [int(c.split("/")[0]) for c in face]
                if any(idx <= 0 for idx in indices):
                    raise ValueError("Expected absolute indices in the course OBJ")
                a, b, c = [vertices[idx - 1] for idx in indices]
                u = [b[j] - a[j] for j in range(3)]
                v = [c[j] - a[j] for j in range(3)]
                if (
                    sum(
                        (
                            u[(j + 1) % 3] * v[(j + 2) % 3]
                            - u[(j + 2) % 3] * v[(j + 1) % 3]
                        )
                        ** 2
                        for j in range(3)
                    )
                    <= 1e-19
                ):
                    dropped += 1
                    continue
                groups.setdefault(group, []).append("f " + " ".join(face))
    lo = [min(p[i] for p in vertices) for i in range(3)]
    hi = [max(p[i] for p in vertices) for i in range(3)]
    scale = 1.8 / max(b - a for a, b in zip(lo, hi))
    offset = [
        -(lo[0] + hi[0]) * scale / 2,
        -lo[1] * scale,
        -(lo[2] + hi[2]) * scale / 2,
    ]
    scene = base_scene()
    scene.update(materials=[], meshes=[], objects=[], textures=[])
    if texture:
        shutil.copyfile(texture, dst / "base.png")
        scene["textures"].append(dict(id="base", source="base.png", encoding="srgb"))
    for i, (group, faces) in enumerate(groups.items()):
        key = f"part{i}"
        mesh = dst / (key + ".obj")
        mesh.write_text("\n".join(attrs + faces) + "\n", encoding="utf-8")
        textured = bool(texture and group == "MC003_Kozakura_Mari")
        scene["materials"].append(
            dict(
                id=key,
                kind="diffuse",
                name=group,
                base_color=[0.270588, 0.552941, 0.874510]
                if texture and not textured
                else [0.8] * 3,
                base_color_texture="base" if textured else "",
                double_sided=True,
            )
        )
        scene["meshes"].append(dict(id=key, source=mesh.name))
        scene["objects"].append(
            dict(
                id=key,
                name=f"{name} · {group}",
                mesh=key,
                material=key,
                transform=transform(offset, [scale] * 3),
                visible=True,
            )
        )
    scene["camera"].update(position=[0, 1.0, 3.8], target=[0, 0.9, 0], vertical_fov=38)
    path = dst / "model.json"
    write_json(path, scene)
    print(name, "groups", len(groups), "degenerate faces removed", dropped)
    return path


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


def classic_models(root, cli):
    """为随包 OBJ 建立单模型文档，物体贴地摆放，相机对准模型中心。"""
    models = []
    for key, name, source, color, scale, base, camera in (
        (
            "spot",
            "Spot 小牛",
            "Keenan Crane",
            [1, 1, 1],
            1,
            0.736784,
            [2.6, 1.55, -2.35],
        ),
        (
            "suzanne",
            "Suzanne 猴头",
            "Norbert Nopper / UX3D · Khronos",
            [0.62, 0.64, 0.67],
            0.75,
            0.974609375,
            [0.9, 1.25, 3.3],
        ),
    ):
        scene = base_scene()
        material = dict(id="surface", name=name, kind="diffuse", base_color=color)
        if key == "spot":
            scene["directional_lights"][0]["direction"] = [-1, -2, 1]
            material["base_color_texture"] = "markings"
            scene["textures"] = [
                dict(
                    id="markings", source="../assets/textures/spot.png", encoding="srgb"
                )
            ]
        scene["camera"].update(position=camera, target=[0, 0.8, 0], vertical_fov=36)
        scene["materials"] = [material]
        scene["meshes"] = [dict(id=key, source=f"../assets/{key}.obj")]
        scene["objects"] = [
            dict(
                id=key,
                name=name,
                mesh=key,
                material="surface",
                transform=transform((0, base * scale, 0), (scale,) * 3),
            )
        ]
        file = root / "catalog" / (key + ".json")
        image = root / "catalog_thumbs" / (key + ".png")
        write_json(file, scene)
        thumbnail(cli, file, image, samples=64)
        description = (
            "带原版斑纹贴图的三角网格。"
            if key == "spot"
            else "灰色漫反射材质，保留猴头的 UV 和着色法线。"
        )
        models.append(
            entry(root, key, name, "model", file, image, description, source, "CC0 1.0")
        )
    return models


def builtin_entries(root, cli):
    """渲染内置缩略图，并从同一场景生成可追加模型和四帧测试输入。"""
    result = []
    meta = root / "catalog"
    thumbs = root / "catalog_thumbs"
    src, own = "RenderBox", "RenderBox 自建资源"
    for id_, stem in BUILTINS.items():
        f = root / (stem + ".json")
        png = thumbs / (id_ + ".png")
        thumbnail(cli, f, png)
        result.append(
            entry(root, id_, stem, "scene", f, png, "", src, own, builtin=True)
        )
    scene = base_scene()
    scene["objects"] = [scene["objects"][1]]
    scene["materials"] = [m for m in scene["materials"] if m["id"] == "blue"]
    scene["meshes"] = [m for m in scene["meshes"] if m["id"] == "cube"]
    scene["meshes"][0]["source"] = "../assets/cube.obj"
    scene["objects"][0]["transform"] = transform((0, 0.5, 0))
    f = meta / "cube.json"
    write_json(f, scene)
    png = thumbs / "cube.png"
    thumbnail(cli, f, png)
    result.append(
        entry(
            root,
            "unit-cube",
            "单位立方体",
            "model",
            f,
            png,
            "追加 1 米的可编辑立方体。",
            src,
            own,
        )
    )
    result.extend(classic_models(root, cli))
    # 小序列来自同一自建场景，低采样、固定相机与对象轨迹可在干净安装中复现。
    frames = []
    with tempfile.TemporaryDirectory(dir=meta) as tmp:
        scene["meshes"][0]["source"] = "../../assets/cube.obj"
        for i in range(4):
            scene["objects"][0]["transform"]["translation"][0] = -0.6 + 0.4 * i
            sf = Path(tmp) / "scene.json"
            write_json(sf, scene)
            prefix = meta / f"motion_{i}"
            subprocess.run(
                [
                    cli,
                    "--render",
                    sf,
                    prefix,
                    "--width",
                    "64",
                    "--height",
                    "48",
                    "--spp",
                    "2",
                    "--depth",
                    "3",
                    "--seed",
                    "101",
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            guidance = meta / "motion_guides"
            subprocess.run(
                [cli, "--write-guides", sf, guidance, str(i), "64", "48"],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            reference = guidance / f"reference_{i}"
            subprocess.run(
                [
                    cli,
                    "--render",
                    sf,
                    reference,
                    "--width",
                    "64",
                    "--height",
                    "48",
                    "--spp",
                    "128",
                    "--depth",
                    "3",
                    "--seed",
                    "202",
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            reference.with_suffix(".png").unlink()
            frames.append(
                frame_entry(
                    root,
                    prefix.with_suffix(".exr"),
                    guidance,
                    reference.with_suffix(".exr"),
                    i,
                )
            )
            prefix.with_suffix(".png").unlink()
    f = meta / "motion.json"
    write_json(f, frames)
    result.append(
        entry(
            root,
            "motion",
            "方块运动 · 4 帧",
            "sequence",
            f,
            png,
            "逐帧查看自建的低采样 EXR。",
            src,
            own,
        )
    )
    shadow_scene = root / "soft-shadows.json"
    shadow_image = thumbs / "soft-shadows.png"
    thumbnail(cli, shadow_scene, shadow_image)
    result.append(
        entry(
            root,
            "shadow-plates",
            "接触软阴影",
            "scene",
            shadow_scene,
            shadow_image,
            "两块离地高度不同的挡板，用于比较接触硬化与固定半径滤波。",
            src,
            own,
            recommended="pcss",
        )
    )
    furnace = root / "white-furnace.json"
    write_json(furnace, furnace_scene())
    preview = thumbs / "white-furnace.png"
    thumbnail(cli, furnace, preview)
    result.append(
        entry(
            root,
            "white-furnace",
            "白炉 · 粗糙金属",
            "scene",
            furnace,
            preview,
            "上排启用多次散射补偿，下排为单次反射。三列粗糙度从 0.25 增至 1。",
            src,
            own,
            recommended="energy",
        )
    )
    # 地面有足够的顶点插值局部遮挡，两个色块可向附近地面反射不同颜色。
    plane = meta / "prt_grid.obj"
    grid = 24
    rows = [
        f"v {2 * x / grid - 1} 0 {2 * z / grid - 1}"
        for z in range(grid + 1)
        for x in range(grid + 1)
    ]
    for z in range(grid):
        for x in range(grid):
            a = 1 + z * (grid + 1) + x
            rows.append(f"f {a} {a + grid + 1} {a + grid + 2} {a + 1}")
    plane.write_text("\n".join(rows) + "\n", encoding="utf-8")
    demo = base_scene()
    for mesh in demo["meshes"]:
        if mesh["id"] == "plane":
            mesh["source"] = "catalog/prt_grid.obj"
    demo["environment"] = [0.8, 0.8, 0.8]
    demo["directional_lights"] = []
    demo["materials"][-1].update(kind="diffuse", base_color=[0.9, 0.15, 0.06])
    demo["objects"][-1]["name"] = "红色挡板"
    file = root / "prt-studio.json"
    write_json(file, demo)
    preview = thumbs / "prt-studio.png"
    thumbnail(cli, file, preview)
    result.append(
        entry(
            root,
            "prt-studio",
            "球谐传输 · 彩色遮挡",
            "scene",
            file,
            preview,
            "网格地面上的静态软遮挡与红、蓝物体的间接反射。",
            src,
            own,
            recommended="prt",
        )
    )
    room = root / "cornell-box.json"
    result.append(
        entry(
            root,
            "screen-room",
            "屏幕间接光 · Cornell",
            "scene",
            room,
            thumbs / "cornell.png",
            "红绿墙和方块，用同一相机比较直接与屏幕间接光。",
            src,
            own,
            recommended="ssgi",
        )
    )
    result.append(
        entry(
            root,
            "cornell-comparison",
            "Cornell · 实时与离线对照",
            "scene",
            room,
            thumbs / "cornell.png",
            "左侧为动态矩形光与时域间接光，右侧用同一相机生成路径追踪参考。",
            src,
            own,
            recommended="comparison",
        )
    )
    return result


def course_entries(root, cli, meta):
    """准备模型与索引。课程 EXR 保存在原位置，只读取和记录摘要。"""
    result = []
    thumbs = meta / "thumbs"
    rights = "课程本地资料"
    fw = root / "frameworks"
    source = "GAMES202 HW"
    for id_, name, obj, tex, hw in [
        (
            "mary",
            "Mary",
            fw / "HW0/homework0/assets/mary/Marry.obj",
            fw / "HW0/homework0/assets/mary/MC003_Kozakura_Mari.png",
            "0",
        ),
        (
            "bunny",
            "Bunny",
            fw / "HW2/Assignment2/homework2/assets/bunny/bunny.obj",
            None,
            "2",
        ),
    ]:
        f = export_model(meta, name, obj, tex)
        png = thumbs / (id_ + ".png")
        thumbnail(cli, f, png)
        result.append(
            entry(
                root,
                id_,
                name + (" 人物" if id_ == "mary" else " 兔子"),
                "model",
                f,
                png,
                "保留人物贴图及两个原始材质组。"
                if id_ == "mary"
                else "保留顶点法线的兔子模型。",
                source + hw,
                rights,
                recommended="blinn-phong" if id_ == "mary" else "",
            )
        )
    for name in ("Skybox", "Indoor", "GraceCathedral", "CornellBox"):
        model = meta / "Bunny"
        original = json.loads((model / "model.json").read_text(encoding="utf-8"))
        env = model / (name + ".png")
        cubemap_to_latlong(fw / "HW2/Assignment2/homework2/assets/cubemap" / name, env)
        original["textures"] = [
            dict(id="environment", source=env.name, encoding="srgb")
        ]
        original["environment_map"] = dict(
            texture="environment", strength=1, rotation_degrees=0
        )
        original["environment"] = [0, 0, 0]
        original["directional_lights"] = []
        file = model / (name + ".json")
        write_json(file, original)
        preview = thumbs / ("bunny-" + name + ".png")
        thumbnail(cli, file, preview)
        result.append(
            entry(
                root,
                "prt-" + name,
                "Bunny · " + name,
                "scene",
                file,
                preview,
                "课程六面环境贴图，适合观察 PRT 烘焙和环境旋转。",
                "GAMES202 HW2",
                rights,
                recommended="prt",
            )
        )
    # 课程材质球采用半径为一的 OBJ；替换白炉环境后可观察有色反射。
    hw4 = fw / "HW4/homework4/homework4/assets"
    lut = meta / "hw4-energy.json"
    charts = []
    for label, filename in (
        ("方向反照率 E", "GGX_E_LUT.png"),
        ("平均反照率 Eavg", "GGX_Eavg_LUT.png"),
    ):
        image = hw4 / "sphere" / filename
        charts.append(
            dict(
                path=image.relative_to(root).as_posix(),
                sha256=digest(image),
                label=label,
            )
        )
    write_json(lut, charts)
    result.append(
        entry(
            root,
            "course-energy-lut",
            "HW4 · GGX 能量表",
            "sequence",
            lut,
            hw4 / "sphere/GGX_E_LUT.png",
            "按页查看原作业提供的 E 与 Eavg，数值按线性编码读取。",
            "GAMES202 HW4",
            rights,
        )
    )
    energy = meta / "HW4"
    energy.mkdir(exist_ok=True)
    mesh = export_model(meta, "HW4-sphere", hw4 / "sphere/sphere.obj")
    prepared = json.loads(mesh.read_text(encoding="utf-8"))
    scene = furnace_scene()
    scene["meshes"][0]["source"] = "../HW4-sphere/" + prepared["meshes"][0]["source"]
    scene["environment"] = [0, 0, 0]
    shutil.copyfile(hw4 / "winter_sky_1k.hdr", energy / "winter.hdr")
    scene["textures"] = [dict(id="winter", encoding="linear", source="winter.hdr")]
    scene["environment_map"] = dict(texture="winter", strength=1, rotation_degrees=0)
    for i, m in enumerate(scene["materials"]):
        m["base_color"] = [[0.95, 0.64, 0.54], [1, 0.78, 0.34], [0.91, 0.92, 0.92]][
            i % 3
        ]
    file = energy / "metals.json"
    write_json(file, scene)
    image = thumbs / "winter-metals.png"
    thumbnail(cli, file, image)
    result.append(
        entry(
            root,
            "winter-metals",
            "冬日环境 · 金属材质球",
            "scene",
            file,
            image,
            "HW4 模型和 HDR 环境。上排启用补偿，下排为单次反射。",
            "GAMES202 HW4",
            rights,
            recommended="energy",
        )
    )
    for name in ("cube1", "cube2", "cave"):
        folder = "cave" if name == "cave" else "cube"
        src = fw / "HW3/homework3/assets" / folder / (name + ".gltf")
        dst = meta / name
        scene = convert_hw3(src, dst, base_scene())
        cave = name == "cave"
        scene["camera"].update(
            position=[4.18927, 1.0313, 2.07331] if cave else [6, 1, 0],
            target=[2.92191, 0.98, 1.55037] if cave else [0, 0, 0],
            vertical_fov=75,
            near=0.01,
            far=100.0,
        )
        scene["directional_lights"][0].update(
            direction=[0.39049, -0.89897, 0.19843] if cave else [0.4, -0.9, -0.2],
            color=[1, 1, 1],
            intensity=20 if cave else 1,
        )
        scene["environment"] = [0.02] * 3
        file = dst / "scene.json"
        write_json(file, scene)
        preview = thumbs / (name + ".png")
        thumbnail(cli, file, preview)
        result.append(
            entry(
                root,
                "hw3-" + name,
                "HW3 · " + name,
                "scene",
                file,
                preview,
                "场景节点、底色与切线空间法线贴图已转换为可编辑资产。",
                "GAMES202 HW3",
                rights,
                recommended="ssgi",
            )
        )
    for name, count in [("box", 20), ("pink-room", 80)]:
        files = [root / f"HW5-data/{name}/input/beauty_{i}.exr" for i in range(count)]
        refs = meta / (name + "-reference")
        refs.mkdir(exist_ok=True)
        video = root / f"HW5-data/{name}/reference/{name.replace('-', '')}-result.mp4"
        subprocess.run(
            [
                "ffmpeg",
                "-loglevel",
                "error",
                "-y",
                "-i",
                video,
                "-start_number",
                "0",
                "-frames:v",
                str(count),
                refs / "frame_%d.png",
            ],
            check=True,
        )
        frames = [
            frame_entry(root, file, file.parent, refs / f"frame_{idx}.png", idx)
            for idx, file in enumerate(files)
        ]
        f = meta / (name + ".json")
        write_json(f, frames)
        # 缩略图由该序列首帧生成；详细数据沿用下载清单及课程目录布局。
        png = thumbs / (name + ".png")
        subprocess.run(
            [
                "ffmpeg",
                "-loglevel",
                "error",
                "-y",
                "-i",
                files[0],
                "-frames:v",
                "1",
                "-vf",
                "scale=96:-1",
                png,
            ],
            check=True,
        )
        result.append(
            entry(
                root,
                name,
                name + " · " + str(count) + " 帧",
                "sequence",
                f,
                png,
                "课程光追输入、引导缓冲、逐帧降噪和课程参考结果。",
                source + "5",
                rights,
            )
        )
        videos = []
        for key, label in (
            ("input", "输入"),
            ("filter", "空间滤波"),
            ("project", "重投影"),
            ("result", "完整结果"),
        ):
            file = root / f"HW5-data/{name}/reference/{name.replace('-', '')}-{key}.mp4"
            videos.append(
                dict(
                    name=label,
                    path=file.relative_to(root).as_posix(),
                    sha256=digest(file),
                )
            )
        result[-1]["videos"] = videos
    return result


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
