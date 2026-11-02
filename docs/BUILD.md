# 构建与运行

## 本机环境

A0 使用 Apple M3 / macOS，Apple Clang 21.0.0、CMake 4.1.1、Ninja 1.12.1、Qt 6.11.2、MoltenVK 1.4.2、Vulkan Loader/Headers/Validation Layers 1.4.363、glslang 16.6.0。这里记录实际验证组合，后续升级需要重新运行检查。

通过 Homebrew 安装桌面所需依赖：

```sh
brew install cmake ninja qt glslang molten-vk vulkan-loader vulkan-headers vulkan-validationlayers vulkan-tools
```

GLM 1.0.3、Catch2 3.16.1、nlohmann/json 3.12.0 优先使用本机同版本包。缺失时 CMake 下载固定归档并验证 SHA256，定义见 `cmake/Dependencies.cmake`。tinyobjloader 2.0.0rc13 同样通过固定归档与 SHA256 下载。CPU-only 配置不发现 Qt、Vulkan、CUDA 或 OptiX。

先执行 `vulkaninfo --summary`，确认真实 Apple GPU 与 `VK_LAYER_KHRONOS_validation`。如果受运行环境限制而无法访问 Metal，必须在允许访问 GPU 的本机进程中测试，不能用软件/模拟 ICD 代替。

## 核心程序

```sh
cmake --preset macos-core-debug
cmake --build --preset macos-core-debug
ctest --preset macos-core-debug
build/macos-core-debug/minirender_cli --capabilities
build/macos-core-debug/minirender_cli --test-pattern /tmp/minirender-pattern.ppm
```

当前 CLI 的 `--test-pattern` 是用于检查线性 RGB 和 sRGB 输出的诊断图，不是场景渲染结果。A2 将加入路径追踪入口。

## 桌面程序

```sh
cmake --preset macos-desktop-debug
cmake --build --preset macos-desktop-debug
build/macos-desktop-debug/src/app/desktop/minirender_desktop.app/Contents/MacOS/minirender_desktop --validation
```

Qt 提供工作台和原生窗口，项目的 `VulkanViewport` 管理设备与交换链。默认打开 studio 场景与编辑工作台，使用方法见 VIEWPORT.md。A0 棋盘/深度诊断仍由 --smoke 参数运行。

GPU 验证使用实际窗口、缩放和交换链回读：

```sh
python3 scripts/verify_gpu.py build/macos-desktop-debug/src/app/desktop/minirender_desktop.app
```

结果写入 `outputs/a0/gpu-smoke.png` 和同目录日志。脚本检查退出码、像素检查成功和销毁之后的零 Validation 错误。日志缺失、超时、不能回读或缺少 Validation Layer 都是失败，不是跳过。

着色器在构建时编译并嵌入程序，因此可从其他工作目录运行。场景编辑、相机交互和材质参数面板已在 A1.2 加入；阴影与 HDR 后处理继续在后续检查点实现。

## Windows

CPU CI 使用 Windows 2022 runner 和 MSVC，桌面及 NVIDIA 运行仍按计划留到 B 阶段。在 VS Developer PowerShell 中可以使用 `windows-core-debug` 等 presets。Qt 与 Vulkan SDK 的私有位置通过不入库的 `CMakeUserPresets.json` 配置。

OptiX 开关当前会明确报未实现错误，而非构建空后端；到 B2 才加入 CUDA 语言、SDK 检测与设备实现。

## 提交检查

```sh
clang-format --dry-run --Werror include/minirender/core/*.hpp src/core/*.cpp src/app/*.cpp src/app/desktop/*.hpp src/app/desktop/*.cpp tests/*.cpp
ctest --preset macos-core-debug
git diff --check
```

每完成一个可验证 CKPT，提交相关代码、文档和验收记录并 push 到远端分支。不要把构建目录、SDK、机器路径或本地生成日志提交到普通 Git 历史。
