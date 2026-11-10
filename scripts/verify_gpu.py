#!/usr/bin/env python3
"""运行离屏 Vulkan GPU 验证并保存日志，超时或验证错误返回失败。

独立进程超时不依赖 Qt event loop，能够捕获渲染提交阻塞主线程的情况。
默认隐藏 Qt 窗口；--onscreen 供人工检查呈现，窗口仍不主动激活。
"""

import argparse
import os
import plistlib
from pathlib import Path
import subprocess
import sys


def main() -> int:
    """从用户指定的可执行文件运行检查；输出路径与工作目录无关。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--output", type=Path, default=Path("outputs/a0"))
    parser.add_argument("--timeout", type=float, default=35)
    parser.add_argument(
        "--onscreen",
        action="store_true",
        help="Show a non-activating window for manual checks",
    )
    parser.add_argument(
        "--sync-validation",
        action="store_true",
        help="Enable synchronization validation in the Vulkan layer",
    )
    parser.add_argument(
        "--timing-only",
        action="store_true",
        help="Measure responsiveness without Vulkan validation instrumentation",
    )
    parser.add_argument(
        "--scene", type=Path, help="Run scene editor checks instead of A0 diagnostics"
    )
    checks = parser.add_mutually_exclusive_group()
    checks.add_argument(
        "--texture-check",
        action="store_true",
        help="Verify texture import and filtering",
    )
    checks.add_argument(
        "--interaction-check",
        action="store_true",
        help="Verify repeated drags and layout",
    )
    checks.add_argument(
        "--parameter-check",
        action="store_true",
        help="Verify edit latency and background progress",
    )
    checks.add_argument(
        "--wire-check",
        action="store_true",
        help="Verify wireframe visibility and HDR isolation",
    )
    checks.add_argument(
        "--offline-check",
        action="store_true",
        help="Verify CPU task controls and Vulkan display",
    )
    checks.add_argument(
        "--ssao-check", action="store_true", help="Verify SSAO and lighting isolation"
    )
    checks.add_argument(
        "--ibl-check", action="store_true", help="Verify IBL tasks and CPU/GPU sampling"
    )
    checks.add_argument(
        "--geometry-check",
        action="store_true",
        help="Verify geometry tasks and CPU/GPU updates",
    )
    checks.add_argument(
        "--gizmo-check",
        action="store_true",
        help="Verify object translation and rotation",
    )
    checks.add_argument(
        "--sides-check",
        action="store_true",
        help="Verify single and double sided surfaces",
    )
    checks.add_argument(
        "--presets-check",
        action="store_true",
        help="Verify built-in scene replacement and editing",
    )
    checks.add_argument(
        "--adaptive-check",
        action="store_true",
        help="Verify adaptive geometry and projection",
    )
    checks.add_argument(
        "--points-check",
        action="store_true",
        help="Verify point cloud editing and preview",
    )
    args = parser.parse_args()
    if args.timing_only and args.sync_validation:
        parser.error(
            "Timing measurements and synchronization validation must run separately"
        )
    scene_check = (
        args.points_check
        or args.adaptive_check
        or args.presets_check
        or args.sides_check
        or args.texture_check
        or args.interaction_check
        or args.parameter_check
        or args.wire_check
        or args.offline_check
        or args.ssao_check
        or args.ibl_check
        or args.geometry_check
        or args.gizmo_check
    )
    if scene_check and not args.scene:
        parser.error("Scene checks require --scene")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    executable = args.executable.resolve()
    if executable.is_dir() and executable.suffix == ".app":
        with (executable / "Contents/Info.plist").open("rb") as file:
            name = plistlib.load(file)["CFBundleExecutable"]
        executable = executable / "Contents/MacOS" / name
    mode = "GPU"
    option = "--smoke"
    for selected, candidate in [
        (args.points_check, "Points"),
        (args.adaptive_check, "Adaptive"),
        (args.presets_check, "Presets"),
        (args.sides_check, "Sides"),
        (args.gizmo_check, "Gizmo"),
        (args.geometry_check, "Geometry"),
        (args.ibl_check, "IBL"),
        (args.ssao_check, "SSAO"),
        (args.offline_check, "Offline"),
        (args.parameter_check, "Parameter"),
        (args.interaction_check, "Interaction"),
        (args.texture_check, "Texture"),
        (args.wire_check, "Wire"),
    ]:
        if selected:
            mode = candidate
    if scene_check and mode == "GPU":
        parser.error("Selected check has no executable mode")
    if args.scene:
        mode = "Scene" if mode == "GPU" else mode
        option = f"--{mode.lower()}-smoke"
        arguments = [
            str(executable),
            "--scene",
            str(args.scene.resolve()),
            option,
            str(output),
        ]
    else:
        arguments = [str(executable), option, str(output / "gpu-smoke.png")]
    if args.timing_only:
        arguments.append("--timing-only")
    if args.onscreen:
        arguments.append("--onscreen-regression")
    with (output / "gpu-smoke.log").open("w", encoding="utf-8") as log:
        environment = os.environ.copy()
        if args.sync_validation:
            environment["VK_LAYER_VALIDATE_SYNC"] = "1"
            log.write("Synchronization validation: VK_LAYER_VALIDATE_SYNC=1\n")
            log.flush()
        try:
            result = subprocess.run(
                arguments,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
                env=environment,
                check=False,
            )
            code = result.returncode
        except subprocess.TimeoutExpired:
            log.write("\nExternal watchdog: GPU smoke timed out.\n")
            code = 124
    text = (output / "gpu-smoke.log").read_text(encoding="utf-8")
    print(text, end="")
    marker = f"{mode} smoke PASS"
    validation_mode = (
        "Vulkan validation mode: disabled"
        if args.timing_only
        else "Vulkan validation mode: enabled"
    )
    if code == 0 and (
        marker not in text
        or "Vulkan validation errors: 0" not in text
        or validation_mode not in text
        or f"Vulkan rendering target: {'window' if args.onscreen else 'offscreen'}"
        not in text
        or f"Regression window policy: {'visible' if args.onscreen else 'hidden'}, no activation"
        not in text
    ):
        print("Missing verification evidence.", file=sys.stderr)
        return 1
    return code


if __name__ == "__main__":
    raise SystemExit(main())
