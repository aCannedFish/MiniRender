"""把 HW2 六张 sRGB 立方体面转换为 RenderBox 使用的经纬图。"""

import math
from pathlib import Path
import subprocess


def cubemap_to_latlong(directory: Path, destination: Path, width: int = 512):
    """按世界方向采样六个面。输出经度从 +X 向 +Z 递增，首行位于北极。"""
    side = width // 2
    faces = []
    for name in ("posx", "negx", "posy", "negy", "posz", "negz"):
        faces.append(
            subprocess.check_output(
                [
                    "ffmpeg",
                    "-loglevel",
                    "error",
                    "-i",
                    directory / (name + ".jpg"),
                    "-vf",
                    f"scale={side}:{side}",
                    "-frames:v",
                    "1",
                    "-pix_fmt",
                    "rgb24",
                    "-f",
                    "rawvideo",
                    "-",
                ]
            )
        )
    output = bytearray()
    for y in range(side):
        theta = math.pi * (y + 0.5) / side
        radial, dy = math.sin(theta), math.cos(theta)
        for x in range(width):
            phi = 2 * math.pi * (x + 0.5) / width
            dx, dz = radial * math.cos(phi), radial * math.sin(phi)
            ax, ay, az = abs(dx), abs(dy), abs(dz)
            if ax >= ay and ax >= az:
                face, u, v = (
                    (0, -dz / ax, -dy / ax) if dx > 0 else (1, dz / ax, -dy / ax)
                )
            elif ay >= az:
                face, u, v = (2, dx / ay, dz / ay) if dy > 0 else (3, dx / ay, -dz / ay)
            else:
                face, u, v = (
                    (4, dx / az, -dy / az) if dz > 0 else (5, -dx / az, -dy / az)
                )
            col = min(side - 1, max(0, int((u * 0.5 + 0.5) * side)))
            row = min(side - 1, max(0, int((v * 0.5 + 0.5) * side)))
            index = 3 * (row * side + col)
            output.extend(faces[face][index : index + 3])
    subprocess.run(
        [
            "ffmpeg",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "rawvideo",
            "-pix_fmt",
            "rgb24",
            "-s",
            f"{width}x{side}",
            "-i",
            "-",
            "-frames:v",
            "1",
            destination,
        ],
        input=output,
        check=True,
    )
