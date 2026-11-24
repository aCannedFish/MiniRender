#!/usr/bin/env python3
"""在实际 Vulkan 设备上依次运行 GAMES202 隐藏窗口回归，保存设备、耗时与验证层结果。"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "outputs" / "games202")
    parser.add_argument(
        "--course-root", type=Path, help="包含 resource_catalog.json 的可选课程资源包"
    )
    parser.add_argument(
        "--require-nvidia", action="store_true", help="检查实际使用的 GPU 厂商"
    )
    options = parser.parse_args()
    scenes = ROOT / "scenes"
    cases = [
        ("catalog", "catalog", scenes / "studio.json", False),
        ("preview", "preview", scenes / "studio.json", False),
        ("directional", "shadow", scenes / "soft-shadows.json", False),
        ("rectangle", "rectangle", scenes / "cornell-box.json", False),
        ("prt", "prt", scenes / "prt-studio.json", False),
        ("screen", "transport", scenes / "cornell-box.json", False),
        ("denoise", "denoise", scenes / "cornell-box.json", False),
        ("cornell", "integration", scenes / "studio.json", False),
    ]
    if options.course_root:
        course = options.course_root.resolve()
        data = json.loads(
            (course / "resource_catalog.json").read_text(encoding="utf-8")
        )
        paths = {entry["id"]: course / entry["path"] for entry in data["entries"]}
        cases.extend(
            [
                ("course-catalog", "catalog", scenes / "studio.json", True),
                ("course-bunny", "prt", paths["prt-Skybox"], True),
                ("course-cave", "transport", paths["hw3-cave"], True),
                ("course-box", "denoise", scenes / "cornell-box.json", True),
            ]
        )
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = []
    for name, flag, scene, local in cases:
        env = os.environ.copy()
        env.pop("MINIRENDER_TEST_COURSE_ROOT", None)
        if local:
            env["MINIRENDER_TEST_COURSE_ROOT"] = str(course)
        directory = output / name
        started = time.monotonic()
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts/verify_gpu.py"),
                str(options.executable.resolve()),
                "--scene",
                str(scene),
                "--" + flag + "-check",
                "--sync-validation",
                "--output",
                str(directory),
                "--timeout",
                "300",
            ],
            cwd=ROOT,
            env=env,
        )
        log = directory / "gpu-smoke.log"
        lines = (
            log.read_text(encoding="utf-8", errors="replace").splitlines()
            if log.exists()
            else []
        )
        device = next(
            (
                s.partition("Vulkan device:")[2].strip()
                for s in lines
                if "Vulkan device:" in s
            ),
            "",
        )
        ok = result.returncode == 0 and (
            not options.require_nvidia or "nvidia" in device.lower()
        )
        evidence.append(
            dict(
                check=name,
                device=device,
                seconds=round(time.monotonic() - started, 3),
                passed=ok,
                log=str(log.relative_to(output)),
            )
        )
        (output / "results.json").write_text(
            json.dumps(evidence, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        if not ok:
            print(f"{name} failed · {device} · {log}", file=sys.stderr)
            return 1
    print(f"{len(evidence)} checks passed · {output / 'results.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
