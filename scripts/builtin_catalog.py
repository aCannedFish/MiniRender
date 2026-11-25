"""生成随应用发布的模型、场景预设及四帧降噪参考。"""

from pathlib import Path
import subprocess
import tempfile
from catalog_assets import (
    base_scene,
    entry,
    frame_entry,
    furnace_scene,
    thumbnail,
    transform,
    write_json,
)

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
