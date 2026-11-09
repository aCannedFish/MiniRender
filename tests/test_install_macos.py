"""检查安装器的应用归属、清理范围和替换失败恢复，不调用系统部署工具。"""

import importlib.util
import json
from pathlib import Path
import plistlib
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "install_macos", Path(__file__).resolve().parents[1] / "scripts/install_macos.py"
)
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def make_bundle(
    path: Path, identifier: str = installer.BUNDLE_ID, content: bytes = b"old"
) -> Path:
    """构造最小应用包；不同二进制内容用于检查替换与恢复。"""
    (path / "Contents/MacOS").mkdir(parents=True)
    (path / "Contents/MacOS/minirender").write_bytes(content)
    (path / "Contents/Info.plist").write_bytes(
        plistlib.dumps(
            {
                "CFBundleIdentifier": identifier,
                "CFBundleExecutable": "minirender",
            }
        )
    )
    return path


class InstallerTests(unittest.TestCase):
    """只在临时目录执行文件操作，校验正式安装器使用的同一组函数。"""

    def test_cleanup_matches_bundle_id_and_preserves_other_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            old = make_bundle(root / "build/debug/src/app/desktop/old.app")
            foreign = make_bundle(
                root / "Applications/MiniRender.app", "other.application"
            )
            unrelated = root / "models.obj"
            unrelated.write_text("model", encoding="utf-8")
            self.assertEqual(installer.old_bundles([root], [foreign.parent]), [old])
            with self.assertRaises(ValueError):
                installer.remove_bundle(foreign)
            installer.remove_bundle(old)
            self.assertFalse(old.exists())
            self.assertTrue(foreign.exists())
            self.assertEqual(unrelated.read_text(encoding="utf-8"), "model")

    def test_incomplete_owned_bundle_is_removed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = make_bundle(root / "build/debug/src/app/desktop/partial.app")
            (bundle / "Contents/MacOS/minirender").unlink()
            self.assertEqual(installer.old_bundles([root], []), [bundle])
            installer.remove_bundle(bundle)
            self.assertFalse(bundle.exists())

    def test_vulkan_manifests_remain_valid_after_bundle_rename(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            staged = root / "staged.app"
            prefixes = []
            for formula, relative, section, library in [
                (
                    "molten",
                    "etc/vulkan/icd.d/MoltenVK_icd.json",
                    "ICD",
                    "libMoltenVK.dylib",
                ),
                (
                    "layers",
                    "share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json",
                    "layer",
                    "libVkLayer_khronos_validation.dylib",
                ),
            ]:
                prefix = root / formula
                (prefix / "lib").mkdir(parents=True)
                (prefix / "lib" / library).write_bytes(b"runtime")
                manifest = prefix / relative
                manifest.parent.mkdir(parents=True)
                manifest.write_text(
                    json.dumps({section: {"library_path": library}}), encoding="utf-8"
                )
                prefixes.append(str(prefix))
            with patch.object(
                installer.subprocess, "check_output", side_effect=prefixes
            ):
                self.assertEqual(len(installer.deploy_vulkan(staged)), 2)
            destination = root / "MiniRender.app"
            staged.rename(destination)
            for manifest in (destination / "Contents/Resources/vulkan").glob(
                "*/*.json"
            ):
                content = json.loads(manifest.read_text(encoding="utf-8"))
                settings = content.get("ICD", content.get("layer"))
                library = manifest.parent / settings["library_path"]
                self.assertEqual(library.read_bytes(), b"runtime")
                self.assertTrue(library.resolve().is_relative_to(destination.resolve()))

    def test_symlink_cleanup_preserves_the_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = make_bundle(root / "target.app")
            link = root / "old.app"
            try:
                link.symlink_to(target, target_is_directory=True)
            except OSError:
                self.skipTest("Directory symlinks require host permission")
            installer.remove_bundle(link)
            self.assertFalse(link.is_symlink())
            self.assertTrue(target.exists())

    def test_publication_replaces_only_the_validated_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = make_bundle(root / "MiniRender.app")
            staged = make_bundle(root / "new.app", content=b"new")
            backup = root / "previous.app"
            installer.publish_bundle(staged, destination, backup)
            self.assertEqual(
                (destination / "Contents/MacOS/minirender").read_bytes(), b"new"
            )
            self.assertFalse(backup.exists())
            self.assertFalse(staged.exists())

    def test_running_application_prevents_update(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = make_bundle(Path(temporary) / "MiniRender.app")
            executable = bundle / "Contents/MacOS/minirender"
            with patch.object(
                installer.subprocess,
                "check_output",
                return_value=str(executable) + "\n",
            ):
                with self.assertRaises(RuntimeError):
                    installer.require_stopped([bundle])
            self.assertTrue(bundle.exists())

    def test_active_worktree_build_prevents_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with patch.object(
                installer.subprocess,
                "check_output",
                return_value="123 /usr/bin/ninja\n",
            ):
                result = SimpleNamespace(
                    stdout="p123\nfcwd\nn" + str(root / "build/release") + "\n"
                )
                with patch.object(installer.subprocess, "run", return_value=result):
                    with self.assertRaises(RuntimeError):
                        installer.require_idle_builds([root])

    def test_failed_publication_restores_previous_application(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            destination = make_bundle(root / "MiniRender.app")
            staged = make_bundle(root / "new.app", content=b"new")
            backup = root / "previous.app"
            rename = Path.rename

            def fail_staged(path, target):
                if path == staged:
                    raise OSError("Simulated publication failure")
                return rename(path, target)

            with patch.object(Path, "rename", fail_staged):
                with self.assertRaises(OSError):
                    installer.publish_bundle(staged, destination, backup)
            self.assertEqual(
                (destination / "Contents/MacOS/minirender").read_bytes(), b"old"
            )
            self.assertTrue(staged.exists())
            self.assertFalse(backup.exists())


if __name__ == "__main__":
    unittest.main()
