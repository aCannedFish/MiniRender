# Windows 迁移

本机基线标签为 `macos-baseline`，对应提交 [`57d0919c066958db88e525c6d28b507ba5355d96`](https://github.com/aCannedFish/MiniRender/commit/57d0919c066958db88e525c6d28b507ba5355d96)。基线覆盖 CPU 核心和 macOS/MoltenVK 桌面。Windows 托管 CI 已验证 MSVC 的 CPU 构建与测试，NVIDIA 驱动、Windows 桌面和 OptiX 需要在目标设备上验证。

## 获取源码

在 VS 2022 的 x64 Developer PowerShell 中运行：

```powershell
git clone https://github.com/aCannedFish/MiniRender.git
cd MiniRender
git fetch origin --tags
git show -s --format="%H %s" "macos-baseline^{commit}"
git switch -c codex/b0-windows macos-baseline
```

安装 MSVC C++ 工具链、CMake 和 Ninja。先构建核心：

```powershell
cmake --preset windows-core-release
cmake --build --preset windows-core-release --parallel 4
ctest --preset windows-core-release --output-on-failure
.\build\windows-core-release\minirender_cli.exe --capabilities
```

## 桌面检查

安装与 MSVC 匹配的 Qt 6 和 Vulkan SDK（含 glslangValidator），确认 `vulkaninfo --summary` 列出实际 NVIDIA GPU 与验证层。

用 `CMAKE_PREFIX_PATH` 指向 Qt 的 MSVC 目录。使用 [VS Code 配置](VSCODE.md)中的 desktop preset 为 IntelliSense 提供编译参数。

```powershell
cmake --preset windows-desktop-debug
cmake --build --preset windows-desktop-debug --parallel 4
ctest --preset windows-desktop-debug --output-on-failure
```

运行桌面程序前确保 Qt 的 bin 目录在 PATH 中。用“快速导入”打开 Cornell Box，检查视口拖拽、物体移动/旋转、参数修改和不同显示缩放比例。先跑离屏 Vulkan 回归，再单独检查可见窗口与交换链。用中文和空格路径保存、重开场景和光场。可从[基线报告](checkpoints/A9.md)取得参考图和演示场景，对照 EXR 的线性值。

## NVIDIA 阶段

按 [PLAN](../PLAN.md#nvidia-阶段) 执行 Windows 平台回归和打包，再接入 CUDA/OptiX。基线构建保持 OptiX 关闭；当前仓库还没有可启用的 OptiX 后端。记录 GPU 型号、驱动、CUDA/OptiX SDK、编译器及 preset。用相同场景、种子、样本预算与 CPU 对照，分开报告首次构建、稳定渲染和显存用量。Windows 安装包沿用固定目录更新策略。
