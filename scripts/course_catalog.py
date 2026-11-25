"""按作业转换本地课程资产，保留模型材质、纹理编码和时间序列索引。"""

import json
import shutil
import subprocess
from catalog_assets import (
    base_scene,
    digest,
    entry,
    frame_entry,
    furnace_scene,
    thumbnail,
    transform,
    write_json,
)
from course_cubemap import cubemap_to_latlong
from course_gltf import convert_hw3

COURSE_RIGHTS = "课程本地资料"


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


def models_and_environments(root, cli, meta):
    """转换分材质 OBJ，并为同一 Bunny 网格生成四组环境场景。"""
    result = []
    thumbs = meta / "thumbs"
    fw = root / "frameworks"
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
                "GAMES202 HW" + hw,
                COURSE_RIGHTS,
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
                COURSE_RIGHTS,
                recommended="prt",
            )
        )
    return result


def course_metals(root, cli, meta):
    """保留课程能量表与冬季 HDR，生成有色导体的对照场景。"""
    result = []
    thumbs = meta / "thumbs"
    fw = root / "frameworks"
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
            COURSE_RIGHTS,
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
            COURSE_RIGHTS,
            recommended="energy",
        )
    )
    return result


def screen_scenes(root, cli, meta):
    """转换 HW3 场景，按课程示例设置相机和方向光。"""
    result = []
    thumbs = meta / "thumbs"
    fw = root / "frameworks"
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
                COURSE_RIGHTS,
                recommended="ssgi",
            )
        )
    return result


def frame_sequences(root, meta):
    """核对 HW5 帧与引导图，索引四个步骤的视频和逐帧参考。"""
    result = []
    thumbs = meta / "thumbs"
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
                "GAMES202 HW5",
                COURSE_RIGHTS,
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


def course_entries(root, cli, meta):
    """按原目录顺序收集条目，源模型和课程帧均保留在用户的课程包中。"""
    return [
        *models_and_environments(root, cli, meta),
        *course_metals(root, cli, meta),
        *screen_scenes(root, cli, meta),
        *frame_sequences(root, meta),
    ]
