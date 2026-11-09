#!/usr/bin/env python3
"""将通过测试的桌面程序更新到固定位置，并清理旧 MiniRender 应用包。

先在临时目录部署依赖和执行离屏 GPU 检查，再替换正式应用。
清理范围限于应用目录和本仓库工作树中的构建产物，按 bundle ID 确认归属。
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

BUNDLE_ID = "org.minirender.desktop"
LSREGISTER = Path(
    "/System/Library/Frameworks/CoreServices.framework/Frameworks/"
    "LaunchServices.framework/Support/lsregister"
)


def bundle_info(bundle: Path, require_executable: bool = True) -> dict:
    """读取并校验本项目应用包，避免覆盖或删除同名的其他应用。"""
    info = plistlib.loads((bundle / "Contents/Info.plist").read_bytes())
    if info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise ValueError(f"Bundle does not belong to MiniRender: {bundle}")
    executable = info.get("CFBundleExecutable", "")
    if not executable or Path(executable).name != executable:
        raise ValueError(f"Invalid bundle executable: {bundle}")
    if require_executable and not (bundle / "Contents/MacOS" / executable).is_file():
        raise ValueError(f"Bundle executable is missing: {bundle}")
    return info


def old_bundles(roots: list[Path], app_directories: list[Path]) -> list[Path]:
    """只枚举标准构建输出和安装目录，不遍历用户场景、模型或第三方软件目录。"""
    candidates = []
    for root in roots:
        candidates.extend(root.glob("build/*/src/app/desktop/*.app"))
    for directory in app_directories:
        if directory.is_dir():
            candidates.extend(directory.glob("*.app"))
    owned = []
    for candidate in sorted(set(candidates)):
        try:
            bundle_info(candidate, require_executable=False)
        except (OSError, ValueError, plistlib.InvalidFileException):
            continue
        owned.append(candidate)
    return owned


def remove_bundle(bundle: Path) -> None:
    """删除经过身份校验的应用包；符号链接只解除链接，保留其目标。"""
    bundle_info(bundle, require_executable=False)
    if bundle.is_symlink():
        bundle.unlink()
    else:
        shutil.rmtree(bundle)


def publish_bundle(staged: Path, destination: Path, backup: Path) -> None:
    """在同一文件系统内更名替换；替换失败时恢复原应用。"""
    bundle_info(staged)
    had_previous = destination.exists() or destination.is_symlink()
    if had_previous:
        bundle_info(destination)
        destination.rename(backup)
    try:
        staged.rename(destination)
    except OSError:
        if had_previous:
            backup.rename(destination)
        raise
    if had_previous:
        remove_bundle(backup)


def require_stopped(bundles: list[Path]) -> None:
    """更新前检查旧应用已退出，保留正在编辑的场景。"""
    processes = subprocess.check_output(["ps", "-axo", "comm="], text=True).splitlines()
    for bundle in bundles:
        executable = (
            bundle
            / "Contents/MacOS"
            / bundle_info(bundle, require_executable=False)["CFBundleExecutable"]
        )
        if str(executable) in processes or str(executable.resolve()) in processes:
            raise RuntimeError(f"Close MiniRender before updating: {bundle}")


def require_idle_builds(roots: list[Path]) -> None:
    """构建仍在写入应用包时终止更新，避免清理其他工作树的活动产物。"""
    processes = subprocess.check_output(
        ["ps", "-axo", "pid=,comm="], text=True
    ).splitlines()
    for process in processes:
        pid, command = process.strip().split(maxsplit=1)
        if Path(command).name not in {"cmake", "ninja"}:
            continue
        result = subprocess.run(
            ["/usr/sbin/lsof", "-a", "-p", pid, "-d", "cwd", "-Fn"],
            capture_output=True,
            text=True,
            check=False,
        )
        for line in result.stdout.splitlines():
            if line.startswith("n"):
                directory = Path(line[1:])
                if any(directory.is_relative_to(root / "build") for root in roots):
                    raise RuntimeError(f"Wait for the build to finish: {directory}")


def deploy_vulkan(staged: Path) -> list[str]:
    """复制本机 Homebrew 的 Vulkan 驱动与验证层，清单使用包内相对路径。"""
    additional = []
    for formula, manifest_path, section, library in [
        ("molten-vk", "etc/vulkan/icd.d/MoltenVK_icd.json", "ICD", "libMoltenVK.dylib"),
        (
            "vulkan-validationlayers",
            "share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json",
            "layer",
            "libVkLayer_khronos_validation.dylib",
        ),
    ]:
        prefix = Path(
            subprocess.check_output(["brew", "--prefix", formula], text=True).strip()
        )
        binary = staged / "Contents/Frameworks" / library
        binary.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(prefix / "lib" / library, binary)
        manifest = json.loads((prefix / manifest_path).read_text(encoding="utf-8"))
        manifest[section]["library_path"] = "../../../Frameworks/" + library
        target = (
            staged
            / "Contents/Resources/vulkan"
            / Path(manifest_path).parent.name
            / Path(manifest_path).name
        )
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        additional.append(f"-executable={binary}")
    return additional


def main() -> int:
    """部署、验证、发布和清理；整个流程不打开前台窗口。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "bundle",
        type=Path,
        nargs="?",
        default=Path(
            "build/macos-desktop-release/src/app/desktop/minirender_desktop.app"
        ),
    )
    parser.add_argument(
        "--destination", type=Path, default=Path("/Applications/MiniRender.app")
    )
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This installer requires macOS")
    repository = Path(__file__).resolve().parent.parent
    source = args.bundle.parent.resolve() / args.bundle.name
    destination = args.destination.parent.resolve() / args.destination.name
    bundle_info(source)
    if source.resolve() == destination.resolve() or destination.suffix != ".app":
        parser.error("Use a separate .app installation destination")
    deploy = shutil.which("macdeployqt")
    if not deploy:
        parser.error("macdeployqt is required")
    worktrees = subprocess.check_output(
        ["git", "worktree", "list", "--porcelain"], cwd=repository, text=True
    )
    roots = [
        Path(line[9:])
        for line in worktrees.splitlines()
        if line.startswith("worktree ")
    ]
    previous = old_bundles(
        roots, [Path("/Applications"), Path.home() / "Applications", destination.parent]
    )
    previous = sorted(set([*previous, source]))
    require_idle_builds(roots)
    require_stopped(previous)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".minirender-update-", dir=destination.parent
    ) as temporary:
        staging = Path(temporary)
        staged = staging / "MiniRender.app"
        shutil.copytree(source, staged, symlinks=True)
        commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repository, text=True
        ).strip()
        resources = staged / "Contents/Resources"
        resources.mkdir(exist_ok=True)
        (resources / "minirender-build.json").write_text(
            json.dumps(
                {
                    "commit": commit,
                    "installed_at": datetime.now(timezone.utc).isoformat(),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        # 只部署 Widgets 使用的 Cocoa 和原生样式插件，避免混入其他 Qt 版本的插件。
        qtpaths = str(Path(deploy).with_name("qtpaths"))
        plugins = Path(
            subprocess.check_output(
                [qtpaths, "--query", "QT_INSTALL_PLUGINS"], text=True
            ).strip()
        )
        additional = deploy_vulkan(staged)
        for relative in [
            Path("platforms/libqcocoa.dylib"),
            Path("styles/libqmacstyle.dylib"),
        ]:
            plugin = staged / "Contents/PlugIns" / relative
            plugin.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(plugins / relative, plugin)
            additional.append(f"-executable={plugin}")
        (resources / "qt.conf").write_text(
            "[Paths]\nPlugins=PlugIns\n", encoding="utf-8"
        )
        result = subprocess.run(
            [
                deploy,
                str(staged),
                "-always-overwrite",
                "-no-plugins",
                "-codesign=-",
                *additional,
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if "ERROR:" in result.stdout or "ERROR:" in result.stderr:
            raise RuntimeError("Qt deployment reported an error")
        subprocess.run(
            ["codesign", "--verify", "--deep", "--strict", str(staged)], check=True
        )
        subprocess.run(
            [
                sys.executable,
                str(repository / "scripts/verify_gpu.py"),
                str(staged),
                "--output",
                str(repository / "outputs/install"),
                "--timeout",
                "60",
            ],
            cwd=repository,
            check=True,
        )
        subprocess.run(
            [
                sys.executable,
                str(repository / "scripts/verify_gpu.py"),
                str(staged),
                "--scene",
                str(staged / "Contents/Resources/scenes/studio.json"),
                "--sides-check",
                "--output",
                str(repository / "outputs/install-sides"),
                "--timeout",
                "120",
            ],
            cwd=repository,
            check=True,
        )
        require_idle_builds(roots)
        require_stopped(previous)
        publish_bundle(staged, destination, staging / "previous.app")
        print(f"Installed: {destination} ({commit[:12]})", flush=True)
    for bundle in previous:
        if bundle == destination or not bundle.exists():
            continue
        subprocess.run([str(LSREGISTER), "-u", str(bundle)], check=True)
        remove_bundle(bundle)
        print(f"Removed old bundle: {bundle}", flush=True)
    subprocess.run([str(LSREGISTER), "-f", str(destination)], check=True)
    print("MiniRender installation updated. Previous application bundles removed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"MiniRender installation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
